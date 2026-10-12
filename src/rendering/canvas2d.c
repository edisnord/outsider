/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/canvas2d.h"
#include "rendering/woff.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stb_truetype implementation lives in this translation unit. */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

typedef struct {
    uint8_t fill_r, fill_g, fill_b, fill_a;
    uint8_t stroke_r, stroke_g, stroke_b, stroke_a;
    float   global_alpha;      /* 0.0 .. 1.0 */
    int     composite_op;
    float   line_width;
    int     text_align;
    int     text_baseline;
    char   *font_name;
    float   font_size;
} DrawState;

#define STATE_STACK_MAX 64

#define MAX_FONTS 512

typedef struct {
    char            *name;
    uint8_t         *data;       /* TTF file data (owned). */
    size_t           data_size;
    stbtt_fontinfo   info;
    bool             valid;
} FontEntry;

static FontEntry s_fonts[MAX_FONTS];
static int       s_font_count = 0;

/* --- Glyph fallback ---------------------------------------------------
   A game's font rarely covers every script the game prints: a Korean font
   with the database's Japanese parameter names, or symbols such as arrows.
   Browsers substitute another installed font per glyph; so does this
   renderer: first the other fonts the game loaded, then well-known system
   fonts, read from disk the first time a glyph is missing. */
static const char *const SYSTEM_FALLBACK_FILES[] = {
#ifdef _WIN32
    "YuGothM.ttc", "meiryo.ttc", "msgothic.ttc", "malgun.ttf", "msyh.ttc",
    "simsun.ttc", "seguisym.ttf", "segoeui.ttf", "arial.ttf",
#elif defined(__APPLE__)
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/AppleSDGothicNeo.ttc",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/System/Library/Fonts/Apple Symbols.ttf",
#else
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
};
#define SYSTEM_FALLBACK_COUNT (sizeof(SYSTEM_FALLBACK_FILES) / sizeof(SYSTEM_FALLBACK_FILES[0]))
static FontEntry   s_fallback_fonts[SYSTEM_FALLBACK_COUNT];
static signed char s_fallback_state[SYSTEM_FALLBACK_COUNT];   /* 0 untried, 1 loaded, -1 unavailable */

typedef struct {
    Canvas2DHandle handle;
    uint8_t       *pixels;       /* RGBA pixel buffer (owned). */
    int            width;
    int            height;
    DrawState      state;
    DrawState      state_stack[STATE_STACK_MAX];
    int            state_depth;
    bool           active;
} Canvas2DCtx;

/* A handle is (generation << 12 | slot): looking a canvas up is one array access, and a stale handle of a canvas
   that was freed never reaches a newer canvas that took its slot. */
#define CONTEXT_SLOT_BITS 12
#define MAX_CONTEXTS (1u << CONTEXT_SLOT_BITS)

static Canvas2DCtx    s_contexts[MAX_CONTEXTS];
static Canvas2DHandle s_next_generation = 1;
static size_t         s_next_slot   = 0;    /* where the search for a free slot starts */
static size_t         s_bytes_alive = 0;    /* pixel memory of all canvases */
static size_t         s_ctx_count   = 0;

static Canvas2DCtx *find_ctx(Canvas2DHandle h)
{
    Canvas2DCtx *ctx = &s_contexts[h & (MAX_CONTEXTS - 1)];
    return (ctx->active && ctx->handle == h) ? ctx : NULL;
}

static FontEntry *find_font(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < s_font_count; i++) {
        if (s_fonts[i].valid && strcmp(s_fonts[i].name, name) == 0) {
            return &s_fonts[i];
        }
    }
    return NULL;
}

static inline int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Operators whose result is "nothing" wherever the source is empty (spec: they clear the destination there, also
   outside the drawn shape): copy, source-in, source-out, destination-in, destination-atop. */
static inline int comp_clears_dst(int comp_op)
{
    return comp_op == CANVAS2D_COMP_COPY || comp_op == CANVAS2D_COMP_SOURCE_IN || comp_op == CANVAS2D_COMP_SOURCE_OUT ||
           comp_op == CANVAS2D_COMP_DEST_IN || comp_op == CANVAS2D_COMP_DEST_ATOP;
}

/* Porter-Duff operators that depend on both alphas (straight-alpha pixels): result = source * Fa + dest * Fb. */
static void blend_porter_duff(uint8_t *dst, uint8_t sr, uint8_t sg, uint8_t sb, int sa, int comp_op)
{
    int da = dst[3];
    int fa, fb; /* 0..255 */
    switch (comp_op) {
    case CANVAS2D_COMP_SOURCE_ATOP: fa = da;       fb = 255 - sa; break;
    case CANVAS2D_COMP_SOURCE_IN:   fa = da;       fb = 0;        break;
    case CANVAS2D_COMP_SOURCE_OUT:  fa = 255 - da; fb = 0;        break;
    case CANVAS2D_COMP_DEST_OVER:   fa = 255 - da; fb = 255;      break;
    case CANVAS2D_COMP_DEST_IN:     fa = 0;        fb = sa;       break;
    case CANVAS2D_COMP_DEST_OUT:    fa = 0;        fb = 255 - sa; break;
    case CANVAS2D_COMP_DEST_ATOP:   fa = 255 - da; fb = sa;       break;
    default:                        fa = 255 - da; fb = 255 - sa; break; /* xor */
    }
    int ws = sa * fa, wd = da * fb, tot = ws + wd;
    int out_a = (tot + 127) / 255;
    if (out_a <= 0) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0;
        return;
    }
    dst[0] = (uint8_t)((sr * ws + dst[0] * wd + tot / 2) / tot);
    dst[1] = (uint8_t)((sg * ws + dst[1] * wd + tot / 2) / tot);
    dst[2] = (uint8_t)((sb * ws + dst[2] * wd + tot / 2) / tot);
    dst[3] = (uint8_t)(out_a > 255 ? 255 : out_a);
}

static inline float blend_lum(const float *c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }

/* "difference" and "saturation" blend modes (W3C compositing: the blended colour replaces the source colour in
   proportion to the backdrop alpha, then the usual source-over). */
static void blend_special(uint8_t *dst, uint8_t sr, uint8_t sg, uint8_t sb, int sa, int comp_op)
{
    float as = sa / 255.0f, ab = dst[3] / 255.0f;
    float cb[3] = { dst[0] / 255.0f, dst[1] / 255.0f, dst[2] / 255.0f };
    float cs[3] = { sr / 255.0f, sg / 255.0f, sb / 255.0f };
    float bl[3];
    if (comp_op == CANVAS2D_COMP_DIFFERENCE) {
        for (int i = 0; i < 3; i++) bl[i] = fabsf(cb[i] - cs[i]);
    } else { /* saturation: backdrop hue and luminosity, source saturation */
        float smax = fmaxf(cs[0], fmaxf(cs[1], cs[2])), smin = fminf(cs[0], fminf(cs[1], cs[2]));
        float sat = smax - smin;
        float bmax = fmaxf(cb[0], fmaxf(cb[1], cb[2])), bmin = fminf(cb[0], fminf(cb[1], cb[2]));
        float t[3] = { 0.0f, 0.0f, 0.0f };
        if (bmax > bmin)
            for (int i = 0; i < 3; i++) t[i] = (cb[i] - bmin) * sat / (bmax - bmin);
        float d = blend_lum(cb) - blend_lum(t);
        for (int i = 0; i < 3; i++) bl[i] = t[i] + d;
        float l = blend_lum(bl), n = fminf(bl[0], fminf(bl[1], bl[2])), x = fmaxf(bl[0], fmaxf(bl[1], bl[2]));
        if (n < 0.0f)
            for (int i = 0; i < 3; i++) bl[i] = l + (bl[i] - l) * l / (l - n);
        if (x > 1.0f)
            for (int i = 0; i < 3; i++) bl[i] = l + (bl[i] - l) * (1.0f - l) / (x - l);
    }
    float ao = as + ab - as * ab;
    if (ao <= 0.0f) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0;
        return;
    }
    for (int i = 0; i < 3; i++) {
        float csp = (1.0f - ab) * cs[i] + ab * bl[i];
        float co = (as * csp + (1.0f - as) * ab * cb[i]) / ao;
        int v = (int)(co * 255.0f + 0.5f);
        dst[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    dst[3] = (uint8_t)(int)(ao * 255.0f + 0.5f);
}

/* Clears everything outside the rectangle [x0,x1) x [y0,y1) (already clamped to the canvas). */
static void clear_outside(Canvas2DCtx *ctx, int x0, int y0, int x1, int y1)
{
    int W = ctx->width, H = ctx->height;
    if (x1 <= x0 || y1 <= y0) {
        memset(ctx->pixels, 0, (size_t)W * H * 4);
        return;
    }
    for (int y = 0; y < H; y++) {
        uint8_t *row = ctx->pixels + (size_t)y * W * 4;
        if (y < y0 || y >= y1) {
            memset(row, 0, (size_t)W * 4);
        } else {
            if (x0 > 0) memset(row, 0, (size_t)x0 * 4);
            if (x1 < W) memset(row + (size_t)x1 * 4, 0, (size_t)(W - x1) * 4);
        }
    }
}

/* Composite a source RGBA pixel onto a destination pixel with the given
   composite operation and global alpha. */
static inline void blend_pixel(uint8_t *dst,
                                uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa,
                                float global_alpha, int comp_op)
{
    int src_a = (int)(sa * global_alpha + 0.5f);
    if (src_a <= 0) {
        if (comp_clears_dst(comp_op)) dst[0] = dst[1] = dst[2] = dst[3] = 0;
        return;
    }

    if (comp_op == CANVAS2D_COMP_COPY) {
        dst[0] = sr;
        dst[1] = sg;
        dst[2] = sb;
        dst[3] = (uint8_t)src_a;
        return;
    }

    int dst_a = dst[3];

    if (comp_op == CANVAS2D_COMP_SOURCE_OVER) {
        if (src_a >= 255) {
            dst[0] = sr;
            dst[1] = sg;
            dst[2] = sb;
            dst[3] = 255;
            return;
        }

        int inv = 255 - src_a;
        int out_a = src_a + ((dst_a * inv + 127) / 255);
        if (out_a == 0) {
            dst[0] = dst[1] = dst[2] = dst[3] = 0;
            return;
        }
        dst[0] = (uint8_t)((sr * src_a + dst[0] * dst_a * inv / 255 + out_a / 2) / out_a);
        dst[1] = (uint8_t)((sg * src_a + dst[1] * dst_a * inv / 255 + out_a / 2) / out_a);
        dst[2] = (uint8_t)((sb * src_a + dst[2] * dst_a * inv / 255 + out_a / 2) / out_a);
        dst[3] = (uint8_t)out_a;
        return;
    }

    if (comp_op == CANVAS2D_COMP_LIGHTER) {
        int out_a = clampi(src_a + dst_a, 0, 255);
        dst[0] = (uint8_t)clampi(((int)sr * src_a + (int)dst[0] * dst_a + 127) / 255, 0, 255);
        dst[1] = (uint8_t)clampi(((int)sg * src_a + (int)dst[1] * dst_a + 127) / 255, 0, 255);
        dst[2] = (uint8_t)clampi(((int)sb * src_a + (int)dst[2] * dst_a + 127) / 255, 0, 255);
        dst[3] = (uint8_t)out_a;
        return;
    }

    if (comp_op == CANVAS2D_COMP_MULTIPLY) {
        int inv = 255 - src_a;
        int out_a = src_a + ((dst_a * inv + 127) / 255);
        if (out_a == 0) {
            dst[0] = dst[1] = dst[2] = dst[3] = 0;
            return;
        }
        int mr = ((int)sr * (int)dst[0] + 127) / 255;
        int mg = ((int)sg * (int)dst[1] + 127) / 255;
        int mb = ((int)sb * (int)dst[2] + 127) / 255;
        dst[0] = (uint8_t)clampi((mr * src_a + dst[0] * dst_a * inv / 255 + out_a / 2) / out_a, 0, 255);
        dst[1] = (uint8_t)clampi((mg * src_a + dst[1] * dst_a * inv / 255 + out_a / 2) / out_a, 0, 255);
        dst[2] = (uint8_t)clampi((mb * src_a + dst[2] * dst_a * inv / 255 + out_a / 2) / out_a, 0, 255);
        dst[3] = (uint8_t)out_a;
        return;
    }

    if (comp_op == CANVAS2D_COMP_SCREEN) {
        int inv = 255 - src_a;
        int out_a = src_a + ((dst_a * inv + 127) / 255);
        if (out_a == 0) {
            dst[0] = dst[1] = dst[2] = dst[3] = 0;
            return;
        }
        int mr = (int)sr + (int)dst[0] - ((int)sr * (int)dst[0] + 127) / 255;
        int mg = (int)sg + (int)dst[1] - ((int)sg * (int)dst[1] + 127) / 255;
        int mb = (int)sb + (int)dst[2] - ((int)sb * (int)dst[2] + 127) / 255;
        dst[0] = (uint8_t)clampi((mr * src_a + dst[0] * dst_a * inv / 255 + out_a / 2) / out_a, 0, 255);
        dst[1] = (uint8_t)clampi((mg * src_a + dst[1] * dst_a * inv / 255 + out_a / 2) / out_a, 0, 255);
        dst[2] = (uint8_t)clampi((mb * src_a + dst[2] * dst_a * inv / 255 + out_a / 2) / out_a, 0, 255);
        dst[3] = (uint8_t)out_a;
        return;
    }

    if (comp_op == CANVAS2D_COMP_DIFFERENCE || comp_op == CANVAS2D_COMP_SATURATION) {
        blend_special(dst, sr, sg, sb, src_a, comp_op);
        return;
    }

    if (comp_op >= CANVAS2D_COMP_SOURCE_ATOP && comp_op <= CANVAS2D_COMP_DEST_OUT) {
        blend_porter_duff(dst, sr, sg, sb, src_a, comp_op);
        return;
    }
    if (comp_op == CANVAS2D_COMP_XOR) {
        blend_porter_duff(dst, sr, sg, sb, src_a, comp_op);
        return;
    }

    /* Anything else falls back to source-over. */
    {
        int inv = 255 - src_a;
        int out_a = src_a + ((dst_a * inv + 127) / 255);
        if (out_a == 0) {
            dst[0] = dst[1] = dst[2] = dst[3] = 0;
            return;
        }
        dst[0] = (uint8_t)((sr * src_a + dst[0] * dst_a * inv / 255 + out_a / 2) / out_a);
        dst[1] = (uint8_t)((sg * src_a + dst[1] * dst_a * inv / 255 + out_a / 2) / out_a);
        dst[2] = (uint8_t)((sb * src_a + dst[2] * dst_a * inv / 255 + out_a / 2) / out_a);
        dst[3] = (uint8_t)out_a;
    }
}

static void init_state(DrawState *s)
{
    s->fill_r = 0;   s->fill_g = 0;   s->fill_b = 0;   s->fill_a = 255;
    s->stroke_r = 0; s->stroke_g = 0; s->stroke_b = 0; s->stroke_a = 255;
    s->global_alpha = 1.0f;
    s->composite_op = CANVAS2D_COMP_SOURCE_OVER;
    s->line_width = 1.0f;
    s->text_align = CANVAS2D_ALIGN_START;
    s->text_baseline = CANVAS2D_BASELINE_ALPHABETIC;
    s->font_name = NULL;
    s->font_size = 10.0f;
}

static void copy_state(DrawState *dst, const DrawState *src)
{
    *dst = *src;
    if (src->font_name) {
        dst->font_name = strdup(src->font_name);
    }
}

static void free_state(DrawState *s)
{
    free(s->font_name);
    s->font_name = NULL;
}

/* Lifecycle */

void canvas2d_init(void)
{
    memset(s_contexts, 0, sizeof(s_contexts));
    s_next_generation = 1;
    s_next_slot = 0;
    s_bytes_alive = 0;
    s_ctx_count = 0;
}

void canvas2d_shutdown(void)
{
    for (size_t i = 0; i < MAX_CONTEXTS; i++) {
        if (s_contexts[i].active) {
            free(s_contexts[i].pixels);
            free_state(&s_contexts[i].state);
            for (int j = 0; j < s_contexts[i].state_depth; j++) {
                free_state(&s_contexts[i].state_stack[j]);
            }
            s_contexts[i].active = false;
        }
    }
    s_ctx_count = 0;

    for (int i = 0; i < s_font_count; i++) {
        free(s_fonts[i].name);
        free(s_fonts[i].data);
        s_fonts[i].valid = false;
    }
    s_font_count = 0;

    for (size_t i = 0; i < SYSTEM_FALLBACK_COUNT; i++) {
        if (s_fallback_state[i] == 1) {
            free(s_fallback_fonts[i].name);
            free(s_fallback_fonts[i].data);
        }
        memset(&s_fallback_fonts[i], 0, sizeof(FontEntry));
        s_fallback_state[i] = 0;
    }
}

Canvas2DHandle canvas2d_create(int width, int height)
{
    if (width <= 0 || height <= 0) return CANVAS2D_HANDLE_INVALID;

    /* Guard against size_t overflow on 32-bit targets. */
    uint64_t alloc_check = (uint64_t)width * (uint64_t)height * 4;
    if (alloc_check > SIZE_MAX) return CANVAS2D_HANDLE_INVALID;

    for (size_t n = 0; n < MAX_CONTEXTS; n++) {
        size_t i = (s_next_slot + n) & (MAX_CONTEXTS - 1);
        if (!s_contexts[i].active) {
            Canvas2DCtx *ctx = &s_contexts[i];
            size_t buf_size = (size_t)alloc_check;
            ctx->pixels = calloc(1, buf_size);
            if (!ctx->pixels) return CANVAS2D_HANDLE_INVALID;

            ctx->width = width;
            ctx->height = height;
            Canvas2DHandle gen = s_next_generation++;
            if ((gen << CONTEXT_SLOT_BITS) >> CONTEXT_SLOT_BITS != gen || gen == 0) {   /* wrapped around: start again */
                s_next_generation = 2;
                gen = 1;
            }
            ctx->handle = (gen << CONTEXT_SLOT_BITS) | (Canvas2DHandle)i;
            ctx->active = true;
            ctx->state_depth = 0;
            init_state(&ctx->state);

            s_next_slot = i + 1;
            s_ctx_count++;
            s_bytes_alive += buf_size;
            return ctx->handle;
        }
    }
    return CANVAS2D_HANDLE_INVALID;
}

void canvas2d_destroy(Canvas2DHandle handle)
{
    Canvas2DCtx *ctx = find_ctx(handle);
    if (!ctx) return;

    s_bytes_alive -= (size_t)ctx->width * (size_t)ctx->height * 4;
    free(ctx->pixels);
    free_state(&ctx->state);
    for (int j = 0; j < ctx->state_depth; j++) {
        free_state(&ctx->state_stack[j]);
    }
    ctx->active = false;
    ctx->pixels = NULL;
    s_ctx_count--;
}

void canvas2d_resize(Canvas2DHandle handle, int width, int height)
{
    Canvas2DCtx *ctx = find_ctx(handle);
    if (!ctx || width <= 0 || height <= 0) return;

    /* Guard against size_t overflow on 32-bit targets. */
    uint64_t alloc_check = (uint64_t)width * (uint64_t)height * 4;
    if (alloc_check > SIZE_MAX) return;

    size_t buf_size = (size_t)alloc_check;
    uint8_t *new_pixels = calloc(1, buf_size);
    if (!new_pixels) return;

    s_bytes_alive = s_bytes_alive - (size_t)ctx->width * (size_t)ctx->height * 4 + buf_size;
    free(ctx->pixels);
    ctx->pixels = new_pixels;
    ctx->width = width;
    ctx->height = height;
}

bool canvas2d_get_size(Canvas2DHandle handle, int *out_w, int *out_h)
{
    Canvas2DCtx *ctx = find_ctx(handle);
    if (!ctx) return false;
    if (out_w) *out_w = ctx->width;
    if (out_h) *out_h = ctx->height;
    return true;
}

uint8_t *canvas2d_get_pixels(Canvas2DHandle handle)
{
    Canvas2DCtx *ctx = find_ctx(handle);
    return ctx ? ctx->pixels : NULL;
}

size_t canvas2d_context_count(void)
{
    return s_ctx_count;
}

size_t canvas2d_list_sizes(int *w, int *h, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < MAX_CONTEXTS && n < max; i++) {
        if (s_contexts[i].active) { w[n] = s_contexts[i].width; h[n] = s_contexts[i].height; n++; }
    }
    return n;
}

size_t canvas2d_bytes_alive(void)
{
    return s_bytes_alive;
}

/* State setters */

void canvas2d_set_fill_color(Canvas2DHandle h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx) return;
    ctx->state.fill_r = r;
    ctx->state.fill_g = g;
    ctx->state.fill_b = b;
    ctx->state.fill_a = a;
}

void canvas2d_set_stroke_color(Canvas2DHandle h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx) return;
    ctx->state.stroke_r = r;
    ctx->state.stroke_g = g;
    ctx->state.stroke_b = b;
    ctx->state.stroke_a = a;
}

void canvas2d_set_global_alpha(Canvas2DHandle h, float alpha)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx) return;
    ctx->state.global_alpha = (alpha < 0.0f) ? 0.0f : (alpha > 1.0f) ? 1.0f : alpha;
}

void canvas2d_set_composite_op(Canvas2DHandle h, int op)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx) return;
    ctx->state.composite_op = op;
}

void canvas2d_set_line_width(Canvas2DHandle h, float width)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx) return;
    ctx->state.line_width = width;
}

void canvas2d_set_text_align(Canvas2DHandle h, int align)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx) return;
    ctx->state.text_align = align;
}

void canvas2d_set_text_baseline(Canvas2DHandle h, int baseline)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx) return;
    ctx->state.text_baseline = baseline;
}

/* State stack */

void canvas2d_save(Canvas2DHandle h)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx || ctx->state_depth >= STATE_STACK_MAX) return;
    copy_state(&ctx->state_stack[ctx->state_depth], &ctx->state);
    ctx->state_depth++;
}

void canvas2d_restore(Canvas2DHandle h)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx || ctx->state_depth <= 0) return;
    ctx->state_depth--;
    free_state(&ctx->state);
    ctx->state = ctx->state_stack[ctx->state_depth];
    /* The popped slot no longer owns font_name. */
    ctx->state_stack[ctx->state_depth].font_name = NULL;
}

/* Drawing operations */

#define FILL_DIV255(x) ((uint32_t)(((uint64_t)(x) * 0x80808081ULL) >> 39))
typedef uint32_t __attribute__((may_alias)) pixel32_t;

#if defined(__x86_64__)
#include <immintrin.h>
#include <cpuid.h>
#define CANVAS2D_HAVE_X86_SIMD 1

/* AVX2 is used for the hot loops (lights) when the CPU and the OS support it; both the console and any PC from
   the last decade do. RMMZ_NO_AVX2=1 turns it off (to compare). */
static int have_avx2(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;
    int ok = 0;
    unsigned a, b, c, d;
    if (__get_cpuid(1, &a, &b, &c, &d) && (c & (1u << 27)) && (c & (1u << 28))) {   /* OSXSAVE, AVX */
        unsigned xlo, xhi;
        __asm__ volatile("xgetbv" : "=a"(xlo), "=d"(xhi) : "c"(0));
        if ((xlo & 6) == 6 && __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & (1u << 5))) ok = 1;
    }
    if (getenv("RMMZ_NO_AVX2")) ok = 0;
    cached = ok;
    return ok;
}

/* Additive fill of an opaque destination: result = min(255, dst + (colour*alpha + 127) / 255) per channel, which is
   exactly what the general formula gives when the destination alpha is 255, and one saturating byte add. Returns
   the number of pixels done (a multiple of 8, stopping at the first block that is not fully opaque); the caller's
   general loop continues where this stops. */
__attribute__((target("avx2")))
static int fill_lighter_avx2(uint8_t *dst, int n, uint32_t r8, uint32_t g8, uint32_t b8)
{
    const __m256i add = _mm256_set1_epi32((int)(r8 | (g8 << 8) | (b8 << 16) | (255u << 24)));
    const __m256i full = _mm256_set1_epi32(255);
    int i = 0;
    for (; i + 8 <= n; i += 8, dst += 32) {
        __m256i d = _mm256_loadu_si256((const __m256i *)dst);
        int opaque = _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_srli_epi32(d, 24), full)));
        if (opaque != 0xff) break;
        _mm256_storeu_si256((__m256i *)dst, _mm256_adds_epu8(d, add));
    }
    return i;
}
#endif

/* Blends one colour into n pixels exactly like blend_pixel() does, with everything that only depends on the
   colour and the drawing state worked out once instead of per pixel. blend_pixel() is a long chain of
   branches; on the console's compiler it costs about 20 cycles a pixel even for an opaque fill, and the
   lighting plugin clears whole-screen canvases with it every frame. */
static void fill_span(uint8_t *dst, int n, uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa, float ga, int comp)
{
    if (n <= 0) return;
    int src_a = (int)(sa * ga + 0.5f);
    if (src_a <= 0) {
        if (comp_clears_dst(comp)) memset(dst, 0, (size_t)n * 4);
        return;
    }

    if (comp == CANVAS2D_COMP_COPY || (comp == CANVAS2D_COMP_SOURCE_OVER && src_a >= 255)) {
        uint32_t a = comp == CANVAS2D_COMP_COPY ? (uint32_t)src_a : 255u;
        uint32_t v = (uint32_t)sr | ((uint32_t)sg << 8) | ((uint32_t)sb << 16) | (a << 24);
        pixel32_t *px = (pixel32_t *)dst;
        for (int i = 0; i < n; i++) px[i] = v;
        return;
    }

    if (comp == CANVAS2D_COMP_LIGHTER) {
        uint32_t pr = (uint32_t)sr * (uint32_t)src_a, pg = (uint32_t)sg * (uint32_t)src_a;
        uint32_t pb = (uint32_t)sb * (uint32_t)src_a, sa32 = (uint32_t)src_a;
#ifdef CANVAS2D_HAVE_X86_SIMD
        if (n >= 8 && have_avx2()) {
            int done = fill_lighter_avx2(dst, n, FILL_DIV255(pr + 127), FILL_DIV255(pg + 127), FILL_DIV255(pb + 127));
            dst += (size_t)done * 4;
            n -= done;
        }
#endif
        for (int i = 0; i < n; i++, dst += 4) {
            uint32_t da = dst[3];
            uint32_t v = FILL_DIV255(pr + dst[0] * da + 127);
            dst[0] = (uint8_t)(v > 255 ? 255 : v);
            v = FILL_DIV255(pg + dst[1] * da + 127);
            dst[1] = (uint8_t)(v > 255 ? 255 : v);
            v = FILL_DIV255(pb + dst[2] * da + 127);
            dst[2] = (uint8_t)(v > 255 ? 255 : v);
            v = sa32 + da;
            dst[3] = (uint8_t)(v > 255 ? 255 : v);
        }
        return;
    }

    for (int i = 0; i < n; i++, dst += 4)
        blend_pixel(dst, sr, sg, sb, sa, ga, comp);
}

void canvas2d_fill_rect(Canvas2DHandle h, int x, int y, int w, int h2)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx || w <= 0 || h2 <= 0) return;

    int x0 = clampi(x, 0, ctx->width);
    int y0 = clampi(y, 0, ctx->height);
    int x1 = clampi((int)((int64_t)x + w), 0, ctx->width);
    int y1 = clampi((int)((int64_t)y + h2), 0, ctx->height);

    uint8_t fr = ctx->state.fill_r;
    uint8_t fg = ctx->state.fill_g;
    uint8_t fb = ctx->state.fill_b;
    uint8_t fa = ctx->state.fill_a;
    float ga = ctx->state.global_alpha;
    int comp = ctx->state.composite_op;

    for (int py = y0; py < y1; py++)
        fill_span(ctx->pixels + ((size_t)py * ctx->width + x0) * 4, x1 - x0, fr, fg, fb, fa, ga, comp);
    if (comp_clears_dst(comp)) clear_outside(ctx, x0, y0, x1, y1);
}

#define GRADIENT_LUT_SIZE 1024

/* ---- helper threads for big fills ------------------------------------------------------------------
   Lighting plugins fill hundreds of thousands of pixels with gradients many times per frame. Rows are
   independent, so large fills are split between the calling thread and a few helper threads. */

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

typedef void (*ParRowsFn)(void *arg, int y0, int y1);

#define PAR_MAX_WORKERS 6

static int par_worker_count = -1;   /* -1: not started yet */
static pthread_mutex_t par_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t par_wake = PTHREAD_COND_INITIALIZER;
static pthread_cond_t par_idle = PTHREAD_COND_INITIALIZER;
static struct {
    ParRowsFn fn;
    void     *arg;
    int       y1;
    int       chunk;
    int       open;         /* workers may still join this job */
    int       active;       /* workers currently inside it */
    unsigned  generation;
    atomic_int next;
} par_job;

static void par_run_chunks(ParRowsFn fn, void *arg, int y1, int chunk)
{
    for (;;) {
        int y = atomic_fetch_add(&par_job.next, chunk);
        if (y >= y1) break;
        int e = y + chunk;
        if (e > y1) e = y1;
        fn(arg, y, e);
    }
}

static void *par_worker(void *unused)
{
    (void)unused;
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&par_mutex);
        while (par_job.generation == seen || !par_job.open)
            pthread_cond_wait(&par_wake, &par_mutex);
        seen = par_job.generation;
        ParRowsFn fn = par_job.fn;
        void *arg = par_job.arg;
        int y1 = par_job.y1, chunk = par_job.chunk;
        par_job.active++;
        pthread_mutex_unlock(&par_mutex);

        par_run_chunks(fn, arg, y1, chunk);

        pthread_mutex_lock(&par_mutex);
        if (--par_job.active == 0) pthread_cond_signal(&par_idle);
        pthread_mutex_unlock(&par_mutex);
    }
    return NULL;
}

static void par_init(void)
{
    par_worker_count = 0;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    int want = n >= 4 ? (int)(n - 1 > 4 ? 4 : n - 1) : 0;
    const char *env = getenv("RMMZ_CANVAS_THREADS");
    if (env && *env) want = atoi(env);
    if (want > PAR_MAX_WORKERS) want = PAR_MAX_WORKERS;
    for (int i = 0; i < want; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, par_worker, NULL) != 0) break;
        pthread_detach(t);
        par_worker_count++;
    }
    /* Without this line a console that reports few CPUs silently runs every fill on one thread (about four times
       slower in a lighting-heavy map), which looks like a slow rasteriser rather than a missing thread pool. */
    printf("canvas2d: %ld online CPUs, %d of %d fill worker threads started%s\n", n, par_worker_count, want,
           env && *env ? " (RMMZ_CANVAS_THREADS)" : "");
    fflush(stdout);
}

/* Runs fn over rows [y0, y1). work_px is a rough size of the whole job; small jobs stay on this thread. */
static void par_rows(ParRowsFn fn, void *arg, int y0, int y1, long work_px)
{
    if (par_worker_count < 0) par_init();
    if (par_worker_count == 0 || work_px < 60000 || y1 - y0 < 8) {
        fn(arg, y0, y1);
        return;
    }
    int chunk = (y1 - y0) / ((par_worker_count + 1) * 4);
    if (chunk < 2) chunk = 2;

    pthread_mutex_lock(&par_mutex);
    par_job.fn = fn;
    par_job.arg = arg;
    par_job.y1 = y1;
    par_job.chunk = chunk;
    atomic_store(&par_job.next, y0);
    par_job.open = 1;
    par_job.generation++;
    pthread_cond_broadcast(&par_wake);
    pthread_mutex_unlock(&par_mutex);

    par_run_chunks(fn, arg, y1, chunk);

    pthread_mutex_lock(&par_mutex);
    par_job.open = 0;
    while (par_job.active > 0)
        pthread_cond_wait(&par_idle, &par_mutex);
    pthread_mutex_unlock(&par_mutex);
}

/* ---- gradients ---------------------------------------------------------------------------------- */

/* Colour of a gradient at t (0..1), the same interpolation the JS shim used. */
static void gradient_color(const double *offsets, const uint8_t *rgba, int n, double t, uint8_t out[4])
{
    if (t <= offsets[0]) { memcpy(out, rgba, 4); return; }
    if (t >= offsets[n - 1]) { memcpy(out, rgba + 4 * (n - 1), 4); return; }
    for (int i = 0; i < n - 1; i++) {
        if (t >= offsets[i] && t <= offsets[i + 1]) {
            double range = offsets[i + 1] - offsets[i];
            double lt = range > 0 ? (t - offsets[i]) / range : 0;
            const uint8_t *c0 = rgba + 4 * i, *c1 = rgba + 4 * (i + 1);
            for (int k = 0; k < 4; k++)
                out[k] = (uint8_t)clampi((int)(c0[k] + (c1[k] - c0[k]) * lt + 0.5), 0, 255);
            return;
        }
    }
    memcpy(out, rgba + 4 * (n - 1), 4);
}

#define DIV255(x) ((uint32_t)(((uint64_t)(x) * 0x80808081ULL) >> 39))

/* One entry of the square-distance table used by the fast additive radial path. */
typedef struct { uint32_t a, pr, pg, pb; } FastEntry;

typedef struct {
    bool       radial;
    double     p[4];        /* linear: x0,y0,x1,y1; radial: cx,cy,r0,r1 */
    double     len_sq;      /* linear only */
    bool       outside_clear;   /* radial: nothing is drawn beyond the outer radius (last stop transparent) */
    uint8_t    first[4];    /* colour of the first stop (degenerate linear gradients) */
    /* fast additive radial path: colours indexed by squared distance, no square root per pixel */
    FastEntry *fast;
    uint32_t  *fast_packed;   /* the same as bytes for opaque destinations: min(255, (colour*alpha + 127) / 255), alpha 255 */
    int        fast_bins;
    float      fast_inv;
    uint8_t    lut[GRADIENT_LUT_SIZE][4];
} GradientPaint;

static bool gradient_is_uniform(const GradientPaint *g)
{
    for (int i = 1; i < GRADIENT_LUT_SIZE; i++)
        if (memcmp(g->lut[i], g->lut[0], 4) != 0) return false;
    return true;
}

static void gradient_init(GradientPaint *g, bool radial, double p0, double p1, double p2, double p3,
                          const double *offsets, const uint8_t *rgba, int n)
{
    g->radial = radial;
    g->p[0] = p0; g->p[1] = p1; g->p[2] = p2; g->p[3] = p3;
    g->len_sq = (p2 - p0) * (p2 - p0) + (p3 - p1) * (p3 - p1);
    memcpy(g->first, rgba, 4);
    for (int i = 0; i < GRADIENT_LUT_SIZE; i++)
        gradient_color(offsets, rgba, n, (double)i / (GRADIENT_LUT_SIZE - 1), g->lut[i]);
    g->outside_clear = false;
    g->fast = NULL;
    g->fast_packed = NULL;
    g->fast_bins = 0;
    g->fast_inv = 0;
}

static void gradient_free(GradientPaint *g)
{
    if (!g) return;
    free(g->fast);
    free(g->fast_packed);
    free(g);
}

/* Called once the drawing state is known: notes whether the far side of a radial gradient is invisible and
   prepares the fast path for additive ("lighter") radial gradients, which is what lights are. */
static void gradient_finish(const Canvas2DCtx *ctx, GradientPaint *g)
{
    if (!g->radial) return;
    float ga = ctx->state.global_alpha;
    int outer_a = (int)(g->lut[GRADIENT_LUT_SIZE - 1][3] * ga + 0.5f);
    g->outside_clear = outer_a <= 0;

    double r0 = g->p[2], r1 = g->p[3], denom = r1 - r0;
    if (g->outside_clear && denom > 0 && r1 >= 1 && ctx->state.composite_op == CANVAS2D_COMP_LIGHTER) {
        int bins = (int)(r1 * 2.5);
        if (bins < 64) bins = 64;
        if (bins > 1024) bins = 1024;
        FastEntry *fast = malloc((size_t)bins * sizeof(FastEntry));
        if (!fast) return;
        double inv = bins / (r1 * r1);
        for (int i = 0; i < bins; i++) {
            double d = sqrt((i + 0.5) / inv);
            double t = (d - r0) / denom;
            if (t < 0) t = 0; else if (t > 1) t = 1;
            const uint8_t *c = g->lut[(int)(t * (GRADIENT_LUT_SIZE - 1) + 0.5)];
            uint32_t a = (uint32_t)(c[3] * ga + 0.5f);
            fast[i].a = a;
            fast[i].pr = c[0] * a;
            fast[i].pg = c[1] * a;
            fast[i].pb = c[2] * a;
        }
        uint32_t *packed = malloc((size_t)bins * sizeof(uint32_t));
        if (packed) {
            for (int i = 0; i < bins; i++) {
                if (fast[i].a == 0) { packed[i] = 0; continue; }
                packed[i] = DIV255(fast[i].pr + 127) | (DIV255(fast[i].pg + 127) << 8) |
                            (DIV255(fast[i].pb + 127) << 16) | (255u << 24);
            }
        }
        g->fast = fast;
        g->fast_packed = packed;
        g->fast_bins = bins;
        g->fast_inv = (float)inv;
    }
}

#ifdef CANVAS2D_HAVE_X86_SIMD
/* Eight pixels at a time for the additive radial gradient: the table index of each pixel is worked out in a
   vector, and where all eight destination pixels are opaque (the lighting plugin's canvases always are) the
   result is a saturating byte add of the table's bytes (see fill_lighter_avx2). Returns how many pixels were done
   (a multiple of 8, stopping at the first block with a not fully opaque destination); the scalar loop continues. */
__attribute__((target("avx2")))
static int gradient_blocks_avx2(uint8_t *dst, int px0, int x1, float fcx, float dy2, float inv, int last,
                                const uint32_t *packed)
{
    const __m256 vinv = _mm256_set1_ps(inv), vdy2 = _mm256_set1_ps(dy2);
    const __m256i vlast = _mm256_set1_epi32(last), full = _mm256_set1_epi32(255);
    __m256 vx = _mm256_sub_ps(_mm256_add_ps(_mm256_add_ps(_mm256_set1_ps((float)px0), _mm256_setr_ps(0, 1, 2, 3, 4, 5, 6, 7)),
                                            _mm256_set1_ps(0.5f)), _mm256_set1_ps(fcx));
    const __m256 v8 = _mm256_set1_ps(8.0f);
    int px = px0;
    for (; px + 8 <= x1; px += 8, dst += 32) {
        __m256i d = _mm256_loadu_si256((const __m256i *)dst);
        if (_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_srli_epi32(d, 24), full))) != 0xff) break;
        __m256 d2 = _mm256_add_ps(_mm256_mul_ps(vx, vx), vdy2);
        __m256i idx = _mm256_min_epi32(_mm256_cvttps_epi32(_mm256_mul_ps(d2, vinv)), vlast);
#ifdef __SCE__
        /* Zen 2 gathers are slow: eight plain loads are quicker on the console */
        uint32_t i[8] __attribute__((aligned(32)));
        _mm256_store_si256((__m256i *)i, idx);
        __m256i add = _mm256_setr_epi32((int)packed[i[0]], (int)packed[i[1]], (int)packed[i[2]], (int)packed[i[3]],
                                        (int)packed[i[4]], (int)packed[i[5]], (int)packed[i[6]], (int)packed[i[7]]);
#else
        __m256i add = _mm256_i32gather_epi32((const int *)packed, idx, 4);
#endif
        _mm256_storeu_si256((__m256i *)dst, _mm256_adds_epu8(d, add));
        vx = _mm256_add_ps(vx, v8);
    }
    return px - px0;
}
#endif

/* Additive radial gradient: same result as blend_pixel(..., CANVAS2D_COMP_LIGHTER), minus the divisions by a
   variable and the square root. Only the chord of the circle on this row is visited. */
static void gradient_span_fast(Canvas2DCtx *ctx, const GradientPaint *g, int y, int x0, int x1)
{
    double cx = g->p[0], cy = g->p[1], r1 = g->p[3];
    double ddy = (y + 0.5) - cy;
    double rem = r1 * r1 - ddy * ddy;
    if (rem < 0) return;
    double half = sqrt(rem);
    int cxa = (int)ceil(cx - half - 0.5), cxb = (int)floor(cx + half - 0.5) + 1;
    if (cxa > x0) x0 = cxa;
    if (cxb < x1) x1 = cxb;
    if (x0 >= x1) return;

    float fcx = (float)cx, dy2 = (float)(ddy * ddy), inv = g->fast_inv;
    int last = g->fast_bins - 1;
    const FastEntry *fast = g->fast;
    uint8_t *dst = ctx->pixels + ((size_t)y * ctx->width + x0) * 4;
    int px = x0;
#ifdef CANVAS2D_HAVE_X86_SIMD
    if (g->fast_packed && x1 - x0 >= 8 && have_avx2()) {
        int done = gradient_blocks_avx2(dst, x0, x1, fcx, dy2, inv, last, g->fast_packed);
        px += done;
        dst += (size_t)done * 4;
    }
#endif
    for (; px < x1; px++, dst += 4) {
        float dx = (px + 0.5f) - fcx;
        int idx = (int)((dx * dx + dy2) * inv);
        if (idx > last) idx = last;
        const FastEntry *e = &fast[idx];
        uint32_t a = e->a;
        if (a == 0) continue;
        uint32_t da = dst[3];
        uint32_t v = DIV255(e->pr + dst[0] * da + 127);
        dst[0] = (uint8_t)(v > 255 ? 255 : v);
        v = DIV255(e->pg + dst[1] * da + 127);
        dst[1] = (uint8_t)(v > 255 ? 255 : v);
        v = DIV255(e->pb + dst[2] * da + 127);
        dst[2] = (uint8_t)(v > 255 ? 255 : v);
        v = a + da;
        dst[3] = (uint8_t)(v > 255 ? 255 : v);
    }
}

/* Blend the gradient into pixels [x0, x1) of row y (already clipped to the canvas). */
static void gradient_span(Canvas2DCtx *ctx, const GradientPaint *g, int y, int x0, int x1)
{
    if (g->fast) {
        gradient_span_fast(ctx, g, y, x0, x1);
        return;
    }
    float ga = ctx->state.global_alpha;
    int comp = ctx->state.composite_op;
    uint8_t *dst = ctx->pixels + ((size_t)y * ctx->width + x0) * 4;

    if (g->radial) {
        double cx = g->p[0], cy = g->p[1], r0 = g->p[2], denom = g->p[3] - g->p[2];
        double ddy = (y + 0.5) - cy;
        if (g->outside_clear && denom > 0) {
            /* Beyond the outer radius the colour is transparent, and drawing transparent pixels changes
               nothing: only the chord of the circle on this row needs work. */
            double r1 = g->p[3];
            double rem = r1 * r1 - ddy * ddy;
            if (rem < 0) return;
            double half = sqrt(rem);
            int cxa = (int)ceil(cx - half - 0.5), cxb = (int)floor(cx + half - 0.5) + 1;
            if (cxa > x0) { dst += (size_t)(cxa - x0) * 4; x0 = cxa; }
            if (cxb < x1) x1 = cxb;
            if (x0 >= x1) return;
        }
        for (int px = x0; px < x1; px++, dst += 4) {
            double ddx = (px + 0.5) - cx;
            double dist = sqrt(ddx * ddx + ddy * ddy);
            double rt = denom != 0 ? (dist - r0) / denom : (dist <= r0 ? 0 : 1);
            if (rt < 0) rt = 0; else if (rt > 1) rt = 1;
            const uint8_t *c = g->lut[(int)(rt * (GRADIENT_LUT_SIZE - 1) + 0.5)];
            blend_pixel(dst, c[0], c[1], c[2], c[3], ga, comp);
        }
        return;
    }

    if (g->len_sq == 0) {
        for (int px = x0; px < x1; px++, dst += 4)
            blend_pixel(dst, g->first[0], g->first[1], g->first[2], g->first[3], ga, comp);
        return;
    }
    double dx = g->p[2] - g->p[0], dy = g->p[3] - g->p[1];
    double row_t = (((y + 0.5) - g->p[1]) * dy - g->p[0] * dx) / g->len_sq;
    double step = dx / g->len_sq;
    for (int px = x0; px < x1; px++, dst += 4) {
        double t = row_t + (px + 0.5) * step;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        const uint8_t *c = g->lut[(int)(t * (GRADIENT_LUT_SIZE - 1) + 0.5)];
        blend_pixel(dst, c[0], c[1], c[2], c[3], ga, comp);
    }
}

typedef struct {
    Canvas2DCtx        *ctx;
    const GradientPaint *g;
    int                 x0, x1;
} GradRectJob;

static void gradient_rect_rows(void *arg, int y0, int y1)
{
    const GradRectJob *j = arg;
    for (int y = y0; y < y1; y++)
        gradient_span(j->ctx, j->g, y, j->x0, j->x1);
}

void canvas2d_parallel_rows(void (*fn)(void *, int, int), void *arg, int y0, int y1, long work_px)
{
    par_rows(fn, arg, y0, y1, work_px);
}

typedef struct {
    Canvas2DCtx *ctx;
    int          x0, x1;
    uint8_t      rgba[4];
    float        ga;
    int          comp;
} SolidJob;

static void solid_rows(void *arg, int y0, int y1)
{
    const SolidJob *j = arg;
    for (int y = y0; y < y1; y++)
        fill_span(j->ctx->pixels + ((size_t)y * j->ctx->width + j->x0) * 4, j->x1 - j->x0,
                  j->rgba[0], j->rgba[1], j->rgba[2], j->rgba[3], j->ga, j->comp);
}

void canvas2d_fill_gradient_rect(Canvas2DHandle h, int x, int y, int w, int h2, bool radial,
                                 double p0, double p1, double p2, double p3,
                                 const double *offsets, const uint8_t *rgba, int n)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx || w <= 0 || h2 <= 0 || n <= 0) return;

    int x0 = clampi(x, 0, ctx->width);
    int y0 = clampi(y, 0, ctx->height);
    int x1 = clampi((int)((int64_t)x + w), 0, ctx->width);
    int y1 = clampi((int)((int64_t)y + h2), 0, ctx->height);
    if (x0 >= x1 || y0 >= y1) return;

    GradientPaint *g = malloc(sizeof(GradientPaint));
    if (!g) return;
    gradient_init(g, radial, p0, p1, p2, p3, offsets, rgba, n);
    gradient_finish(ctx, g);
    if (gradient_is_uniform(g)) {
        /* One colour everywhere (a single colour stop, as the lighting plugin uses for its flat fills): no need
           to work out the position of every pixel. */
        SolidJob sj = { ctx, x0, x1, { g->lut[0][0], g->lut[0][1], g->lut[0][2], g->lut[0][3] },
                        ctx->state.global_alpha, ctx->state.composite_op };
        par_rows(solid_rows, &sj, y0, y1, (long)(x1 - x0) * (y1 - y0));
        gradient_free(g);
        return;
    }
    GradRectJob job = { ctx, g, x0, x1 };
    par_rows(gradient_rect_rows, &job, y0, y1, (long)(x1 - x0) * (y1 - y0));
    gradient_free(g);
}

typedef struct { double y0, y1, x0, dxdy; int dir; } PathEdge;

static int path_x_cmp(const void *a, const void *b)
{
    double d = ((const double *)a)[0] - ((const double *)b)[0];
    return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

typedef struct {
    Canvas2DCtx        *ctx;
    const PathEdge     *edges;
    int                 ne;
    int                 xa_lim, xb_lim;
    const GradientPaint *grad;   /* NULL: solid colour */
} PathJob;

/* Fills rows [ya, yb) of a polygon set (see fill_path_impl). */
static void path_rows(void *arg, int ya, int yb)
{
    const PathJob *j = arg;
    Canvas2DCtx *ctx = j->ctx;
    double *xs = malloc((size_t)j->ne * 2 * sizeof(double));
    if (!xs) return;

    uint8_t fr = ctx->state.fill_r, fg = ctx->state.fill_g, fb = ctx->state.fill_b, fa = ctx->state.fill_a;
    float ga = ctx->state.global_alpha;
    int comp = ctx->state.composite_op;

    for (int y = ya; y < yb; y++) {
        double sy = y + 0.5;
        int nx = 0;
        for (int e = 0; e < j->ne; e++) {
            if (sy >= j->edges[e].y0 && sy < j->edges[e].y1) {
                xs[2 * nx] = j->edges[e].x0 + (sy - j->edges[e].y0) * j->edges[e].dxdy;
                xs[2 * nx + 1] = j->edges[e].dir;
                nx++;
            }
        }
        if (nx < 2) continue;
        qsort(xs, (size_t)nx, 2 * sizeof(double), path_x_cmp);
        int wind = 0;
        double span_start = 0;
        for (int k = 0; k < nx; k++) {
            int prev = wind;
            wind += (int)xs[2 * k + 1];
            if (prev == 0 && wind != 0) {
                span_start = xs[2 * k];
            } else if (prev != 0 && wind == 0) {
                int xa = (int)lround(span_start), xb = (int)lround(xs[2 * k]);
                if (xa < j->xa_lim) xa = j->xa_lim;
                if (xb > j->xb_lim) xb = j->xb_lim;
                if (xb <= xa) continue;
                if (j->grad) {
                    gradient_span(ctx, j->grad, y, xa, xb);
                } else {
                    fill_span(ctx->pixels + ((size_t)y * ctx->width + xa) * 4, xb - xa, fr, fg, fb, fa, ga, comp);
                }
            }
        }
    }
    free(xs);
}

/* Scanline fill of polygons (nonzero winding, every subpath implicitly closed), like the JS rasteriser
   this replaces: pixel rows are sampled at their centre, span ends are rounded, no anti-aliasing.
   pts holds x,y pairs, counts the number of points of each subpath. With grad == NULL the current fill
   colour is used. */
static void fill_path_impl(Canvas2DCtx *ctx, const double *pts, const int *counts, int nsub,
                           bool has_clip, int clip_x, int clip_y, int clip_w, int clip_h,
                           const GradientPaint *grad)
{
    size_t total = 0;
    for (int s = 0; s < nsub; s++) if (counts[s] >= 3) total += (size_t)counts[s];
    if (total == 0) return;
    PathEdge *edges = malloc(total * sizeof(PathEdge));
    if (!edges) return;

    int ne = 0;
    double min_y = 1e300, max_y = -1e300, min_x = 1e300, max_x = -1e300;
    const double *p = pts;
    for (int s = 0; s < nsub; s++) {
        int n = counts[s];
        if (n < 3) { p += 2 * (size_t)n; continue; }
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            double x0 = p[2 * i], y0 = p[2 * i + 1], x1 = p[2 * j], y1 = p[2 * j + 1];
            if (x0 < min_x) min_x = x0;
            if (x0 > max_x) max_x = x0;
            if (y0 == y1) continue;
            int dir = 1;
            if (y0 > y1) {
                double t = x0; x0 = x1; x1 = t;
                t = y0; y0 = y1; y1 = t;
                dir = -1;
            }
            edges[ne].y0 = y0; edges[ne].y1 = y1; edges[ne].x0 = x0;
            edges[ne].dxdy = (x1 - x0) / (y1 - y0); edges[ne].dir = dir;
            ne++;
            if (y0 < min_y) min_y = y0;
            if (y1 > max_y) max_y = y1;
        }
        p += 2 * (size_t)n;
    }
    if (ne == 0) { free(edges); return; }

    int ch = ctx->height, cw = ctx->width;
    int ya = 0, yb = ch, xa_lim = 0, xb_lim = cw;
    if (has_clip) {
        if (clip_y > ya) ya = clip_y;
        if (clip_y + clip_h < yb) yb = clip_y + clip_h;
        if (clip_x > xa_lim) xa_lim = clip_x;
        if (clip_x + clip_w < xb_lim) xb_lim = clip_x + clip_w;
    }
    if (floor(min_y) > ya) ya = (int)floor(min_y);
    if (ceil(max_y) < yb) yb = (int)ceil(max_y);

    PathJob job = { ctx, edges, ne, xa_lim, xb_lim, grad };
    if (ya < yb) {
        double wpx = (max_x - min_x) * (double)(yb - ya);
        par_rows(path_rows, &job, ya, yb, wpx > 1e9 ? 1000000000L : (long)wpx);
    }
    free(edges);
}

void canvas2d_fill_path(Canvas2DHandle h, const double *pts, const int *counts, int nsub,
                        bool has_clip, int clip_x, int clip_y, int clip_w, int clip_h,
                        int gradient_kind, double p0, double p1, double p2, double p3,
                        const double *offsets, const uint8_t *rgba, int n)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx || nsub <= 0) return;
    if (gradient_kind < 0 || n <= 0) {
        fill_path_impl(ctx, pts, counts, nsub, has_clip, clip_x, clip_y, clip_w, clip_h, NULL);
        return;
    }
    GradientPaint *g = malloc(sizeof(GradientPaint));
    if (!g) return;
    gradient_init(g, gradient_kind == 1, p0, p1, p2, p3, offsets, rgba, n);
    gradient_finish(ctx, g);
    fill_path_impl(ctx, pts, counts, nsub, has_clip, clip_x, clip_y, clip_w, clip_h, g);
    gradient_free(g);
}

void canvas2d_clear_rect(Canvas2DHandle h, int x, int y, int w, int h2)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx || w <= 0 || h2 <= 0) return;

    int x0 = clampi(x, 0, ctx->width);
    int y0 = clampi(y, 0, ctx->height);
    int x1 = clampi((int)((int64_t)x + w), 0, ctx->width);
    int y1 = clampi((int)((int64_t)y + h2), 0, ctx->height);

    for (int py = y0; py < y1; py++) {
        uint8_t *row = ctx->pixels + (size_t)py * ctx->width * 4;
        memset(row + x0 * 4, 0, (size_t)(x1 - x0) * 4);
    }
}

void canvas2d_stroke_rect(Canvas2DHandle h, int x, int y, int w, int h2)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx || w <= 0 || h2 <= 0) return;

    uint8_t sr = ctx->state.stroke_r;
    uint8_t sg = ctx->state.stroke_g;
    uint8_t sb = ctx->state.stroke_b;
    uint8_t sa = ctx->state.stroke_a;
    float ga = ctx->state.global_alpha;
    int comp = ctx->state.composite_op;
    int lw = (int)(ctx->state.line_width + 0.5f);
    if (lw < 1) lw = 1;

    /* Clamped edges; int64 avoids overflow. */
    int top0 = clampi(y, 0, ctx->height);
    int top1 = clampi((int)((int64_t)y + lw), 0, ctx->height);
    int bot0 = clampi((int)((int64_t)y + h2 - lw), 0, ctx->height);
    int bot1 = clampi((int)((int64_t)y + h2), 0, ctx->height);
    int lft0 = clampi(x, 0, ctx->width);
    int lft1 = clampi((int)((int64_t)x + lw), 0, ctx->width);
    int rgt0 = clampi((int)((int64_t)x + w - lw), 0, ctx->width);
    int rgt1 = clampi((int)((int64_t)x + w), 0, ctx->width);

    /* Top edge */
    for (int py = top0; py < top1; py++) {
        for (int px = lft0; px < rgt1; px++) {
            blend_pixel(ctx->pixels + ((size_t)py * ctx->width + px) * 4,
                        sr, sg, sb, sa, ga, comp);
        }
    }
    /* Bottom edge */
    for (int py = bot0; py < bot1; py++) {
        for (int px = lft0; px < rgt1; px++) {
            blend_pixel(ctx->pixels + ((size_t)py * ctx->width + px) * 4,
                        sr, sg, sb, sa, ga, comp);
        }
    }
    /* Left edge */
    for (int py = top1; py < bot0; py++) {
        for (int px = lft0; px < lft1; px++) {
            blend_pixel(ctx->pixels + ((size_t)py * ctx->width + px) * 4,
                        sr, sg, sb, sa, ga, comp);
        }
    }
    /* Right edge */
    for (int py = top1; py < bot0; py++) {
        for (int px = rgt0; px < rgt1; px++) {
            blend_pixel(ctx->pixels + ((size_t)py * ctx->width + px) * 4,
                        sr, sg, sb, sa, ga, comp);
        }
    }
}

void canvas2d_draw_image(Canvas2DHandle h,
                          const uint8_t *src_pixels, int src_w, int src_h,
                          int sx, int sy, int sw, int sh,
                          int dx, int dy, int dw, int dh)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx || !src_pixels) return;
    /* Zero-size blits are legal no-ops (RPG Maker routinely draws 0-width
       window-frame slices). */
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    float ga = ctx->state.global_alpha;
    int comp = ctx->state.composite_op;
    int clears = comp_clears_dst(comp);

    /* Nearest-neighbour sampling. The source column of each destination column is worked out once (it used to be
       an int64 multiply and divide per pixel), and the common cases skip the general blend: an opaque source
       pixel over, a transparent one under an operator that leaves the destination alone, and 'copy'. */
    int px0 = dx < 0 ? -dx : 0;
    int px1 = dw;
    if (dx + px1 > ctx->width) px1 = ctx->width - dx;
    if (px1 <= px0) goto clear_part;
    int span = px1 - px0;
    int xs_stack[1024];
    int *xs = span <= 1024 ? xs_stack : malloc((size_t)span * sizeof(int));
    if (!xs) return;
    for (int i = 0; i < span; i++) {
        int sxp = sx + (int)(((int64_t)(px0 + i) * sw) / dw);
        xs[i] = (sxp < 0 || sxp >= src_w) ? -1 : sxp;
    }
    const int full_alpha = ga >= 1.0f;
    for (int py = 0; py < dh; py++) {
        int dst_y = dy + py;
        if (dst_y < 0 || dst_y >= ctx->height) continue;
        int src_y = sy + (int)(((int64_t)py * sh) / dh);
        uint8_t *drow = ctx->pixels + ((size_t)dst_y * ctx->width + (dx + px0)) * 4;
        if (src_y < 0 || src_y >= src_h) {
            if (clears) memset(drow, 0, (size_t)span * 4);
            continue;
        }
        const uint8_t *srow = src_pixels + (size_t)src_y * src_w * 4;
        for (int i = 0; i < span; i++) {
            uint8_t *d = drow + (size_t)i * 4;
            int xx = xs[i];
            if (xx < 0) {
                if (clears) d[0] = d[1] = d[2] = d[3] = 0;     /* nothing to draw here: an empty source */
                continue;
            }
            const uint8_t *s = srow + (size_t)xx * 4;
            if (full_alpha) {
                if (s[3] == 255 && (comp == CANVAS2D_COMP_SOURCE_OVER || comp == CANVAS2D_COMP_COPY)) {
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
                    continue;
                }
                if (s[3] == 0) {
                    if (clears) d[0] = d[1] = d[2] = d[3] = 0;
                    continue;
                }
            }
            blend_pixel(d, s[0], s[1], s[2], s[3], ga, comp);
        }
    }
    if (xs != xs_stack) free(xs);
clear_part:
    if (clears) {
        /* destination-in, copy and friends also clear everything outside the drawn rectangle */
        int cx0 = clampi(dx, 0, ctx->width), cy0 = clampi(dy, 0, ctx->height);
        int cx1 = clampi((int)((int64_t)dx + dw), 0, ctx->width), cy1 = clampi((int)((int64_t)dy + dh), 0, ctx->height);
        clear_outside(ctx, cx0, cy0, cx1, cy1);
    }
}

void canvas2d_draw_canvas(Canvas2DHandle h, Canvas2DHandle src,
                          int sx, int sy, int sw, int sh,
                          int dx, int dy, int dw, int dh)
{
    Canvas2DCtx *d = find_ctx(h);
    Canvas2DCtx *s = find_ctx(src);
    if (!d || !s || !s->pixels || s->width <= 0 || s->height <= 0) return;
    if (d == s) {
        size_t n = (size_t)s->width * (size_t)s->height * 4;
        uint8_t *copy = malloc(n);
        if (!copy) return;
        memcpy(copy, s->pixels, n);
        canvas2d_draw_image(h, copy, s->width, s->height, sx, sy, sw, sh, dx, dy, dw, dh);
        free(copy);
        return;
    }
    canvas2d_draw_image(h, s->pixels, s->width, s->height, sx, sy, sw, sh, dx, dy, dw, dh);
}

/* Pixel manipulation */

/* Adds r, g, b to the colour channels of every pixel, clamped to 0..255 (alpha untouched): what RPG Maker MV's
   Bitmap.adjustTone does with a getImageData / put-back loop, which is a typed-array pass of about a million
   steps for a window background in an interpreter. Uint8ClampedArray stores round to nearest, ties to even. */
void canvas2d_adjust_tone(Canvas2DHandle h, double r, double g, double b)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx || !ctx->pixels || ctx->width <= 0 || ctx->height <= 0) return;
    uint8_t lut[3][256];
    const double add[3] = { r, g, b };
    for (int c = 0; c < 3; c++) {
        for (int v = 0; v < 256; v++) {
            double x = (double)v + add[c];
            if (x != x) x = 0;                  /* NaN stores as 0 */
            if (x < 0) x = 0;
            if (x > 255) x = 255;
            lut[c][v] = (uint8_t)nearbyint(x);  /* default rounding mode: to nearest, ties to even */
        }
    }
    uint8_t *p = ctx->pixels;
    size_t n = (size_t)ctx->width * (size_t)ctx->height;
    for (size_t i = 0; i < n; i++, p += 4) {
        p[0] = lut[0][p[0]];
        p[1] = lut[1][p[1]];
        p[2] = lut[2][p[2]];
    }
}

void canvas2d_get_image_data(Canvas2DHandle h, int x, int y, int w, int h2,
                              uint8_t *out)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx || !out || w <= 0 || h2 <= 0) return;

    for (int py = 0; py < h2; py++) {
        int src_y = y + py;
        uint8_t *dst_row = out + (size_t)py * w * 4;
        if (src_y < 0 || src_y >= ctx->height) {
            memset(dst_row, 0, (size_t)w * 4);
            continue;
        }
        for (int px = 0; px < w; px++) {
            int src_x = x + px;
            uint8_t *dst = dst_row + px * 4;
            if (src_x < 0 || src_x >= ctx->width) {
                dst[0] = dst[1] = dst[2] = dst[3] = 0;
            } else {
                const uint8_t *src = ctx->pixels + ((size_t)src_y * ctx->width + src_x) * 4;
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            }
        }
    }
}

void canvas2d_put_image_data(Canvas2DHandle h, const uint8_t *data,
                              int x, int y, int w, int h2)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx || !data || w <= 0 || h2 <= 0) return;

    for (int py = 0; py < h2; py++) {
        int dst_y = y + py;
        if (dst_y < 0 || dst_y >= ctx->height) continue;
        for (int px = 0; px < w; px++) {
            int dst_x = x + px;
            if (dst_x < 0 || dst_x >= ctx->width) continue;

            const uint8_t *src = data + ((size_t)py * w + px) * 4;
            uint8_t *dst = ctx->pixels + ((size_t)dst_y * ctx->width + dst_x) * 4;
            /* putImageData writes raw pixels without blending. */
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = src[3];
        }
    }
}

/* Font loading and text rendering */

static bool load_sfnt(const char *name, const uint8_t *data, size_t size);

bool canvas2d_load_font(const char *name, const uint8_t *data, size_t size)
{
    if (woff_is_woff(data, size)) {
        size_t sfnt_size = 0;
        uint8_t *sfnt = woff_to_sfnt(data, size, &sfnt_size);
        if (!sfnt) return false;
        bool ok = load_sfnt(name, sfnt, sfnt_size);
        free(sfnt);
        return ok;
    }
    return load_sfnt(name, data, size);
}

static bool load_sfnt(const char *name, const uint8_t *data, size_t size)
{
    /* stb_truetype needs at least the TrueType offset table (12 bytes). */
    if (!name || !data || size < 12) return false;

    /* Replacing an existing entry reuses its slot, so it works on a full table. */
    FontEntry *existing = find_font(name);
    if (existing) {
        uint8_t *new_data = malloc(size);
        if (!new_data) return false;   /* keep the old font intact on OOM */
        memcpy(new_data, data, size);
        free(existing->data);
        existing->data = new_data;
        existing->data_size = size;
        existing->valid = stbtt_InitFont(&existing->info, existing->data, 0);
        if (!existing->valid) {
            /* Unparseable font: release and compact rather than keep a dead entry. */
            free(existing->data);
            free(existing->name);
            int idx = (int)(existing - s_fonts);
            s_font_count--;
            if (idx != s_font_count) s_fonts[idx] = s_fonts[s_font_count];
            memset(&s_fonts[s_font_count], 0, sizeof(FontEntry));
            return false;
        }
        return true;
    }

    if (s_font_count >= MAX_FONTS) return false;

    FontEntry *fe = &s_fonts[s_font_count];
    fe->name = strdup(name);
    fe->data = malloc(size);
    if (!fe->data) { free(fe->name); return false; }
    memcpy(fe->data, data, size);
    fe->data_size = size;
    fe->valid = stbtt_InitFont(&fe->info, fe->data, 0);
    if (!fe->valid) {
        free(fe->name);
        free(fe->data);
        return false;
    }
    s_font_count++;
    return true;
}

void canvas2d_set_font(Canvas2DHandle h, const char *name, float pixel_size)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx) return;
    free(ctx->state.font_name);
    ctx->state.font_name = name ? strdup(name) : NULL;
    ctx->state.font_size = pixel_size > 0.0f ? pixel_size : 10.0f;
}

/* Vertical offset from the requested baseline to the alphabetic baseline. */
static float get_baseline_offset(stbtt_fontinfo *fi, float scale, int baseline)
{
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(fi, &ascent, &descent, &lineGap);

    float a = ascent * scale;
    float d = descent * scale;

    switch (baseline) {
        case CANVAS2D_BASELINE_TOP:
            return a;
        case CANVAS2D_BASELINE_HANGING:
            return a * 0.8f;
        case CANVAS2D_BASELINE_MIDDLE:
            return (a + d) * 0.5f;
        case CANVAS2D_BASELINE_ALPHABETIC:
            return 0.0f;
        case CANVAS2D_BASELINE_IDEOGRAPHIC:
            return d;
        case CANVAS2D_BASELINE_BOTTOM:
            return d;
        default:
            return 0.0f;
    }
}

/* Decode one UTF-8 sequence, advancing *p. Invalid bytes become '?'. */
static int utf8_next(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    int cp;
    if ((*p & 0x80) == 0) {
        cp = *p; p += 1;
    } else if ((*p & 0xE0) == 0xC0 && p[1]) {
        cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2;
    } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
        cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
    } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4;
    } else {
        cp = '?'; p += 1;
    }
    *pp = (const char *)p;
    return cp;
}

static int utf8_peek(const char *p)
{
    return *p ? utf8_next(&p) : 0;
}

/* Read a whole file; NULL when it cannot be opened. */
static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len <= 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *data = malloc((size_t)len);
    if (!data) { fclose(f); return NULL; }
    size_t got = fread(data, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) { free(data); return NULL; }
    *out_size = (size_t)len;
    return data;
}

static FontEntry *load_fallback_font(size_t i)
{
    if (s_fallback_state[i] == 1) return &s_fallback_fonts[i];
    if (s_fallback_state[i] == -1) return NULL;
    s_fallback_state[i] = -1;

    const char *file = SYSTEM_FALLBACK_FILES[i];
    uint8_t *data = NULL;
    size_t size = 0;
#ifdef _WIN32
    const char *dirs[2] = { getenv("WINDIR"), getenv("LOCALAPPDATA") };
    const char *subs[2] = { "\\Fonts\\", "\\Microsoft\\Windows\\Fonts\\" };
    for (int d = 0; d < 2 && !data; d++) {
        if (!dirs[d]) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s%s%s", dirs[d], subs[d], file);
        data = read_file(path, &size);
    }
#else
    data = read_file(file, &size);
#endif
    if (!data) return NULL;

    /* .ttc collections: use the first face. */
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    FontEntry *fe = &s_fallback_fonts[i];
    if (offset < 0 || !stbtt_InitFont(&fe->info, data, offset)) {
        free(data);
        return NULL;
    }
    fe->name = strdup(file);
    fe->data = data;
    fe->data_size = size;
    fe->valid = true;
    s_fallback_state[i] = 1;
    return fe;
}

typedef struct {
    FontEntry *fe;
    int        glyph;     /* 0 = notdef in fe */
    int        blank;     /* whitespace/control character that no font has a glyph for: draw nothing */
} GlyphRef;

/* The font that can draw `cp`: the requested font, another game font, or a
   system fallback. Whitespace and control characters stay with the
   requested font so their advances match its metrics. */
static GlyphRef resolve_glyph(FontEntry *primary, int cp)
{
    GlyphRef r = { primary, stbtt_FindGlyphIndex(&primary->info, cp), 0 };
    if (r.glyph) return r;
    int whitespace = cp <= 0x20 || cp == 0x3000 || cp == 0xA0 || (cp >= 0x7F && cp < 0xA0);
    if (whitespace) r.blank = 1; /* if nothing below finds a glyph, the .notdef box must not be drawn */
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return r;  /* control characters: nothing to look for */

    for (int i = 0; i < s_font_count; i++) {
        FontEntry *fe = &s_fonts[i];
        if (!fe->valid || fe == primary) continue;
        int g = stbtt_FindGlyphIndex(&fe->info, cp);
        if (g) { r.fe = fe; r.glyph = g; r.blank = 0; return r; }
    }
    for (size_t i = 0; i < SYSTEM_FALLBACK_COUNT; i++) {
        FontEntry *fe = load_fallback_font(i);
        if (!fe) continue;
        int g = stbtt_FindGlyphIndex(&fe->info, cp);
        if (g) { r.fe = fe; r.glyph = g; r.blank = 0; return r; }
    }
    return r;
}

/* Advance of `cp` at pixel_size, including kerning with next_cp when both
   come from the same font. */
static float glyph_advance(FontEntry *primary, float pixel_size, int cp, int next_cp, GlyphRef *out)
{
    GlyphRef g = resolve_glyph(primary, cp);
    float scale = stbtt_ScaleForMappingEmToPixels(&g.fe->info, pixel_size);
    int advance, lsb;
    stbtt_GetGlyphHMetrics(&g.fe->info, g.glyph, &advance, &lsb);
    float width = advance * scale;
    if (g.blank && !g.glyph) width = cp < 0x20 ? 0.0f : pixel_size * 0.28f; /* no glyph anywhere: a plain gap */
    if (next_cp && !(g.blank && !g.glyph)) {
        GlyphRef n = resolve_glyph(primary, next_cp);
        if (n.fe == g.fe) {
            width += stbtt_GetGlyphKernAdvance(&g.fe->info, g.glyph, n.glyph) * scale;
        }
    }
    if (out) *out = g;
    return width;
}

static float measure_text_internal(FontEntry *fe, float pixel_size, const char *text)
{
    if (!fe || !text || !*text) return 0.0f;

    float width = 0.0f;
    const char *p = text;
    while (*p) {
        int codepoint = utf8_next(&p);
        width += glyph_advance(fe, pixel_size, codepoint, utf8_peek(p), NULL);
    }
    return width;
}

float canvas2d_measure_text(Canvas2DHandle h, const char *text)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx || !text) return 0.0f;

    FontEntry *fe = find_font(ctx->state.font_name);
    if (!fe) {
        /* Fall back to the first loaded font. */
        for (int i = 0; i < s_font_count; i++) {
            if (s_fonts[i].valid) { fe = &s_fonts[i]; break; }
        }
    }
    if (!fe) {
        /* No font loaded: rough estimate of 0.6 * size per character. */
        int len = 0;
        const char *p = text;
        while (*p) { len++; p++; }
        return len * ctx->state.font_size * 0.6f;
    }

    return measure_text_internal(fe, ctx->state.font_size, text);
}

/* Bounding box of the drawn glyphs of `text`, relative to the start of the text on the baseline: out[0] = how far it
   reaches to the left of the start, out[1] = right edge, out[2] = ascent above the baseline, out[3] = descent below it
   (the actualBoundingBox* values of measureText). Returns false when there is no font. */
bool canvas2d_measure_text_bounds(Canvas2DHandle h, const char *text, float out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx || !text) return false;

    FontEntry *fe = find_font(ctx->state.font_name);
    for (int i = 0; !fe && i < s_font_count; i++) {
        if (s_fonts[i].valid) fe = &s_fonts[i];
    }
    if (!fe) return false;

    float pen = 0.0f, min_x = 0.0f, max_x = 0.0f, ascent = 0.0f, descent = 0.0f;
    bool any = false;
    const char *p = text;
    while (*p) {
        int cp = utf8_next(&p);
        GlyphRef g;
        float adv = glyph_advance(fe, ctx->state.font_size, cp, utf8_peek(p), &g);
        float scale = stbtt_ScaleForMappingEmToPixels(&g.fe->info, ctx->state.font_size);
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&g.fe->info, g.glyph, scale, scale, &x0, &y0, &x1, &y1);
        if (!g.blank && x1 > x0 && y1 > y0) {
            if (!any || pen + x0 < min_x) min_x = pen + x0;
            if (!any || pen + x1 > max_x) max_x = pen + x1;
            if (-y0 > ascent) ascent = (float)-y0;
            if (y1 > descent) descent = (float)y1;
            any = true;
        }
        pen += adv;
    }
    if (any) {
        out[0] = -min_x;
        out[1] = max_x;
        out[2] = ascent;
        out[3] = descent;
    }
    return true;
}

/* Browser text rasterizers (Skia/DirectWrite) boost partial coverage so
   glyphs read heavier than linear anti-aliasing; without it, text here is
   visibly lighter than in the NW.js player. Coverage is mapped through
   c^(1/TEXT_GAMMA), built once. */
#define TEXT_GAMMA 2.2f
static uint8_t s_text_gamma[256];
static bool    s_text_gamma_ready = false;

static void init_text_gamma(void)
{
    if (s_text_gamma_ready) return;
    for (int i = 0; i < 256; i++) {
        float c = powf((float)i / 255.0f, 1.0f / TEXT_GAMMA);
        int v = (int)(c * 255.0f + 0.5f);
        s_text_gamma[i] = (uint8_t)(v > 255 ? 255 : v);
    }
    s_text_gamma_ready = true;
}

/* Grow glyph coverage outward by `radius` pixels (max over a disc), the
   way a stroke of width 2*radius spreads beyond the glyph outline. Returns a
   malloc'd (gw + 2*margin) x (gh + 2*margin) bitmap. */
static uint8_t *dilate_coverage(const uint8_t *bitmap, int gw, int gh,
                                float radius, int *margin_out)
{
    int m = (int)(radius + 0.999f);
    int dw = gw + 2 * m, dh = gh + 2 * m;
    uint8_t *out = calloc((size_t)dw * (size_t)dh, 1);
    if (!out) return NULL;
    float r2 = radius * radius + 0.01f;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            uint8_t best = 0;
            for (int dy = -m; dy <= m && best < 255; dy++) {
                int sy = y - m + dy;
                if (sy < 0 || sy >= gh) continue;
                for (int dx = -m; dx <= m; dx++) {
                    if ((float)(dx * dx + dy * dy) > r2) continue;
                    int sx = x - m + dx;
                    if (sx < 0 || sx >= gw) continue;
                    uint8_t v = bitmap[sy * gw + sx];
                    if (v > best) best = v;
                }
            }
            out[y * dw + x] = best;
        }
    }
    *margin_out = m;
    return out;
}

/* Draw text in the given colour. stroke_radius > 0 draws the glyphs
   expanded by that many pixels (a stroke of width 2 * stroke_radius, as
   Canvas strokeText spreads half the line width outside the outline). */
static void draw_text(Canvas2DCtx *ctx, const char *text, float x, float y,
                      uint8_t fr, uint8_t fg, uint8_t fb, uint8_t fa,
                      float stroke_radius)
{
    if (!ctx || !text || !*text) return;
    init_text_gamma();

    FontEntry *fe = find_font(ctx->state.font_name);
    if (!fe) {
        /* Fall back to the first loaded font. */
        for (int i = 0; i < s_font_count; i++) {
            if (s_fonts[i].valid) { fe = &s_fonts[i]; break; }
        }
        if (!fe) return;
    }

    float pixel_size = ctx->state.font_size;
    float scale = stbtt_ScaleForMappingEmToPixels(&fe->info, pixel_size);

    float text_width = 0.0f;
    int align = ctx->state.text_align;
    if (align == CANVAS2D_ALIGN_CENTER || align == CANVAS2D_ALIGN_RIGHT ||
        align == CANVAS2D_ALIGN_END) {
        text_width = measure_text_internal(fe, pixel_size, text);
        if (align == CANVAS2D_ALIGN_CENTER) {
            x -= text_width * 0.5f;
        } else {
            x -= text_width;
        }
    }

    float base_offset = get_baseline_offset(&fe->info, scale, ctx->state.text_baseline);
    float draw_y = y + base_offset;

    float ga = ctx->state.global_alpha;
    int comp = ctx->state.composite_op;

    float cursor_x = x;
    const char *p = text;
    while (*p) {
        int codepoint = utf8_next(&p);
        GlyphRef g;
        float advance = glyph_advance(fe, pixel_size, codepoint, utf8_peek(p), &g);
        float gscale = stbtt_ScaleForMappingEmToPixels(&g.fe->info, pixel_size);

        int gw, gh, gx, gy;
        uint8_t *bitmap = g.blank ? NULL : stbtt_GetGlyphBitmap(&g.fe->info, 0, gscale,
                                                g.glyph, &gw, &gh, &gx, &gy);
        if (bitmap && stroke_radius > 0.0f) {
            int margin = 0;
            uint8_t *fat = dilate_coverage(bitmap, gw, gh, stroke_radius, &margin);
            stbtt_FreeBitmap(bitmap, NULL);
            bitmap = fat;           /* freed with free() below */
            gw += 2 * margin;
            gh += 2 * margin;
            gx -= margin;
            gy -= margin;
        }
        if (bitmap) {
            int bx = (int)(cursor_x + 0.5f) + gx;
            int by = (int)(draw_y + 0.5f) + gy;

            for (int row = 0; row < gh; row++) {
                int dst_y = by + row;
                if (dst_y < 0 || dst_y >= ctx->height) continue;
                for (int col = 0; col < gw; col++) {
                    int dst_x = bx + col;
                    if (dst_x < 0 || dst_x >= ctx->width) continue;

                    uint8_t coverage = s_text_gamma[bitmap[row * gw + col]];
                    if (coverage == 0) continue;

                    /* Modulate fill alpha by glyph coverage. */
                    uint8_t glyph_a = (uint8_t)((fa * coverage + 127) / 255);
                    uint8_t *dst = ctx->pixels + ((size_t)dst_y * ctx->width + dst_x) * 4;
                    blend_pixel(dst, fr, fg, fb, glyph_a, ga, comp);
                }
            }
            if (stroke_radius > 0.0f) free(bitmap);
            else stbtt_FreeBitmap(bitmap, NULL);
        }

        cursor_x += advance;
    }
}

void canvas2d_fill_text(Canvas2DHandle h, const char *text, float x, float y)
{
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx) return;
    draw_text(ctx, text, x, y, ctx->state.fill_r, ctx->state.fill_g,
              ctx->state.fill_b, ctx->state.fill_a, 0.0f);
}

void canvas2d_stroke_text(Canvas2DHandle h, const char *text, float x, float y)
{
    /* The stroke straddles the outline: half the line width lies outside
       the glyph, which is what shows once fillText paints over the inside
       (RPG Maker draws every string as outline then fill). */
    Canvas2DCtx *ctx = find_ctx(h);
    if (!ctx) return;
    float radius = ctx->state.line_width * 0.5f;
    if (radius < 0.5f) radius = 0.5f;
    draw_text(ctx, text, x, y, ctx->state.stroke_r, ctx->state.stroke_g,
              ctx->state.stroke_b, ctx->state.stroke_a, radius);
}
