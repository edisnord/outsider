/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/renderer.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h> /* for fprintf in FBO error path */
#include <math.h>  /* for floorf, ceilf in scissor transform */

#ifdef RMMZ_HAS_GL
#include "rendering/gl_loader.h"
#include "engine/error_handler.h"
#endif

struct Renderer {
    SpriteBatch *batch;
    int          width;       /* game logical width (projection) */
    int          height;      /* game logical height (projection) */
    int          vp_x;        /* screen viewport X offset (letterbox) */
    int          vp_y;        /* screen viewport Y offset (letterbox) */
    int          vp_w;        /* screen viewport width in pixels */
    int          vp_h;        /* screen viewport height in pixels */
    uint32_t     bound_fbo;   /* currently bound FBO, 0 = default */

    /* The frame is composed here and blitted to the window on present, so a
       screenshot reads the same pixels on every backend and rendering does
       not depend on the window being visible. */
    uint32_t     screen_fbo;
    uint32_t     screen_tex;
    int          out_w;
    int          out_h;
    uint8_t     *readback;    /* renderer_read_screen() buffer */
    size_t       readback_size;
    int          pres_w;      /* window size the frame is finally shown in (0: same as out) */
    int          pres_h;
};

Renderer *renderer_create(int width, int height)
{
    Renderer *r = calloc(1, sizeof(Renderer));
    if (!r) return NULL;

    r->width = width;
    r->height = height;
    r->vp_x = 0;
    r->vp_y = 0;
    r->vp_w = width;
    r->vp_h = height;

    r->batch = sprite_batch_create(4096);
    if (!r->batch) {
        free(r);
        return NULL;
    }

    sprite_batch_set_projection(r->batch, (float)width, (float)height);

#ifdef RMMZ_HAS_GL
    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    fprintf(stderr, "[GL] Version: %s\n", glGetString(GL_VERSION));
    fprintf(stderr, "[GL] Renderer: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "[GL] Vendor: %s\n", glGetString(GL_VENDOR));
    fprintf(stderr, "[GL] GLSL: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
#endif

    return r;
}

void renderer_destroy(Renderer *r)
{
    if (!r) return;
#ifdef RMMZ_HAS_GL
    if (r->screen_fbo) renderer_delete_fbo(r->screen_fbo, r->screen_tex);
#endif
    sprite_batch_destroy(r->batch);
    free(r);
}

#ifdef RMMZ_HAS_GL
static void ensure_screen_fbo(Renderer *r)
{
    if (r->out_w <= 0 || r->out_h <= 0) return;
    if (r->screen_fbo) return;
    r->screen_fbo = renderer_create_fbo(r->out_w, r->out_h, &r->screen_tex);
}
#endif

void renderer_set_output_size(Renderer *r, int width, int height)
{
    if (!r || width <= 0 || height <= 0) return;
    if (r->out_w == width && r->out_h == height) return;
    r->out_w = width;
    r->out_h = height;
#ifdef RMMZ_HAS_GL
    if (r->screen_fbo) {
        renderer_delete_fbo(r->screen_fbo, r->screen_tex);
        r->screen_fbo = 0;
        r->screen_tex = 0;
    }
    ensure_screen_fbo(r);
#endif
}

void renderer_resize(Renderer *r, int width, int height)
{
    if (!r) return;
    r->width = width;
    r->height = height;
    sprite_batch_set_projection(r->batch, (float)width, (float)height);

    /* Reset to a full viewport; the JS side re-applies letterboxing afterwards. */
    r->vp_x = 0;
    r->vp_y = 0;
    r->vp_w = width;
    r->vp_h = height;

#ifdef RMMZ_HAS_GL
    glViewport(0, 0, width, height);
#endif
}

void renderer_get_size(const Renderer *r, int *out_w, int *out_h)
{
    if (!r) return;
    if (out_w) *out_w = r->width;
    if (out_h) *out_h = r->height;
}

void renderer_set_screen_viewport(Renderer *r, int x, int y, int w, int h)
{
    if (!r) return;
    r->vp_x = x;
    r->vp_y = y;
    r->vp_w = w;
    r->vp_h = h;

    /* Apply immediately if we're rendering to the default framebuffer. */
    if (r->bound_fbo == 0) {
#ifdef RMMZ_HAS_GL
        glViewport(x, y, w, h);
#endif
    }
}

#ifdef RMMZ_HAS_GL
/* Clear the whole target even while a scissor is active. A filter pass may
   start inside a scissor-clipped window; if its (pooled) framebuffer were
   only cleared within the clip, stale pixels from an earlier pass would be
   composited back over the screen. */
static void clear_unscissored(void)
{
    GLboolean scissored = glIsEnabled(GL_SCISSOR_TEST);
    if (scissored) glDisable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT);
    if (scissored) glEnable(GL_SCISSOR_TEST);
}
#endif

static void run_pending_deletes(void);

void renderer_begin_frame(Renderer *r)
{
    if (!r) return;
    run_pending_deletes();

#ifdef RMMZ_HAS_GL
    /* When an FBO is bound, renderer_bind_fbo already set its viewport. */
    if (r->bound_fbo == 0) {
        ensure_screen_fbo(r);
        glBindFramebuffer(GL_FRAMEBUFFER, r->screen_fbo);
        glViewport(r->vp_x, r->vp_y, r->vp_w, r->vp_h);
    }
    clear_unscissored();
#endif

    sprite_batch_begin(r->batch);
    sprite_batch_set_pristine(r->batch, 1);
}

void renderer_begin_frame_transparent(Renderer *r)
{
    if (!r) return;
    run_pending_deletes();

#ifdef RMMZ_HAS_GL
    if (r->bound_fbo == 0) {
        ensure_screen_fbo(r);
        glBindFramebuffer(GL_FRAMEBUFFER, r->screen_fbo);
        glViewport(r->vp_x, r->vp_y, r->vp_w, r->vp_h);
    }
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    clear_unscissored();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
#endif

    sprite_batch_begin(r->batch);
}

void renderer_end_frame(Renderer *r)
{
    if (!r) return;
    sprite_batch_end(r->batch);

}

const uint8_t *renderer_read_screen(Renderer *r, int *out_w, int *out_h)
{
#ifdef RMMZ_HAS_GL
    if (!r || !r->screen_fbo || r->out_w <= 0 || r->out_h <= 0) return NULL;
    size_t need = (size_t)r->out_w * (size_t)r->out_h * 4;
    if (need > r->readback_size) {
        uint8_t *bigger = realloc(r->readback, need);
        if (!bigger) return NULL;
        r->readback = bigger;
        r->readback_size = need;
    }
    sprite_batch_flush(r->batch);
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->screen_fbo);
    glReadPixels(0, 0, r->out_w, r->out_h, GL_RGBA, GL_UNSIGNED_BYTE, r->readback);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    if (out_w) *out_w = r->out_w;
    if (out_h) *out_h = r->out_h;
    return r->readback;
#else
    (void)r; (void)out_w; (void)out_h;
    return NULL;
#endif
}

#define PENDING_DELETES 4096
static uint32_t s_pending_delete[PENDING_DELETES];
static int      s_pending_delete_count;

void renderer_queue_texture_delete(uint32_t texture)
{
    if (!texture) return;
    if (s_pending_delete_count >= PENDING_DELETES) {
        renderer_delete_texture(texture);   /* the list is full: better now than never */
        return;
    }
    s_pending_delete[s_pending_delete_count++] = texture;
}

static void run_pending_deletes(void)
{
    for (int i = 0; i < s_pending_delete_count; i++) renderer_delete_texture(s_pending_delete[i]);
    s_pending_delete_count = 0;
}

void renderer_set_present_size(Renderer *r, int width, int height)
{
    if (!r) return;
    r->pres_w = width;
    r->pres_h = height;
}

void renderer_present(Renderer *r)
{
    if (!r) return;
#ifdef RMMZ_HAS_GL
    if (!r->screen_fbo || r->out_w <= 0 || r->out_h <= 0) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, r->screen_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    {
        /* present_log: what is shown where, once per change (a log line says more than a photo of the TV) */
        static int last[4];
        if (last[0] != r->out_w || last[1] != r->out_h || last[2] != r->pres_w || last[3] != r->pres_h) {
            last[0] = r->out_w; last[1] = r->out_h; last[2] = r->pres_w; last[3] = r->pres_h;
            error_handler_log(LOG_INFO, "present: game picture %dx%d, screen %dx%d", r->out_w, r->out_h, r->pres_w, r->pres_h);
        }
    }
    if (r->pres_w > 0 && r->pres_h > 0 && (r->pres_w != r->out_w || r->pres_h != r->out_h)) {
        /* The frame was drawn at the game's own size (much less work for the software renderer than drawing
           it at the size of the screen); scale it up once, keeping the aspect ratio. */
        float sx = (float)r->pres_w / (float)r->out_w, sy = (float)r->pres_h / (float)r->out_h;
        float scale = sx < sy ? sx : sy;
        int dw = (int)((float)r->out_w * scale + 0.5f), dh = (int)((float)r->out_h * scale + 0.5f);
        int dx = (r->pres_w - dw) / 2, dy = (r->pres_h - dh) / 2;
        static int filter_mode = -1;   /* -1: undecided, 0: pick by scale, 1: nearest, 2: smooth */
        if (filter_mode < 0) {
            const char *e = getenv("RMMZ_SCALE");
            filter_mode = (e && strcmp(e, "nearest") == 0) ? 1 : ((e && (strcmp(e, "smooth") == 0 || strcmp(e, "linear") == 0)) ? 2 : 0);
        }
        int exact = fabsf(scale - roundf(scale)) < 0.001f;
        GLenum filter = filter_mode == 1 ? GL_NEAREST : (filter_mode == 2 ? GL_LINEAR : (exact ? GL_NEAREST : GL_LINEAR));
        if (dw != r->pres_w || dh != r->pres_h) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glBlitFramebuffer(0, 0, r->out_w, r->out_h, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, filter);
    } else {
        /* A plain blit: the offscreen surface is already the window's size, so there is no scaling and the
           letterboxing baked into it is preserved. */
        glBlitFramebuffer(0, 0, r->out_w, r->out_h,
                          0, 0, r->out_w, r->out_h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
}

SpriteBatch *renderer_get_batch(Renderer *r)
{
    return r ? r->batch : NULL;
}

/* Texture management */

uint32_t renderer_create_texture(int width, int height)
{
#ifdef RMMZ_HAS_GL
    GLuint tex;
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glTextureStorage2D(tex, 1, GL_RGBA8, width, height);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return (uint32_t)tex;
#else
    (void)width; (void)height;
    return 0;
#endif
}

void renderer_update_texture(uint32_t texture, int width, int height,
                             const uint8_t *pixels)
{
#ifdef RMMZ_HAS_GL
    if (!texture) return;

    glTextureSubImage2D(texture, 0, 0, 0, width, height,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
#else
    (void)texture; (void)width; (void)height; (void)pixels;
#endif
}

void renderer_set_texture_filter(uint32_t texture, bool linear)
{
#ifdef RMMZ_HAS_GL
    if (texture == 0) return;
    GLint f = linear ? GL_LINEAR : GL_NEAREST;
    glTextureParameteri((GLuint)texture, GL_TEXTURE_MIN_FILTER, f);
    glTextureParameteri((GLuint)texture, GL_TEXTURE_MAG_FILTER, f);
#else
    (void)texture; (void)linear;
#endif
}

void renderer_delete_texture(uint32_t texture)
{
#ifdef RMMZ_HAS_GL
    if (texture) {
        GLuint t = texture;
        glDeleteTextures(1, &t);
    }
#else
    (void)texture;
#endif
}

/* Framebuffer objects */

uint32_t renderer_create_fbo(int width, int height, uint32_t *out_texture)
{
#ifdef RMMZ_HAS_GL
    GLuint tex;
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glTextureStorage2D(tex, 1, GL_RGBA8, width, height);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLuint fbo;
    glCreateFramebuffers(1, &fbo);
    glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, tex, 0);

    GLenum status = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        compat_report("render", "framebuffer incomplete", "status 0x%X", status);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        return 0;
    }

    /* Clear to transparent; DSA needs no bind. */
    float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    glClearNamedFramebufferfv(fbo, GL_COLOR, 0, clear_color);

    if (out_texture) *out_texture = (uint32_t)tex;
    return (uint32_t)fbo;
#else
    (void)width; (void)height;
    if (out_texture) *out_texture = 0;
    return 0;
#endif
}

void renderer_delete_fbo(uint32_t fbo, uint32_t texture)
{
#ifdef RMMZ_HAS_GL
    if (fbo) {
        GLuint f = fbo;
        glDeleteFramebuffers(1, &f);
    }
    if (texture) {
        GLuint t = texture;
        glDeleteTextures(1, &t);
    }
#else
    (void)fbo; (void)texture;
#endif
}

void renderer_bind_fbo(Renderer *r, uint32_t fbo, int width, int height)
{
    if (!r) return;

    /* Flush any pending draws before switching targets. */
    sprite_batch_flush(r->batch);
    sprite_batch_set_pristine(r->batch, 0);
    r->bound_fbo = fbo;

#ifdef RMMZ_HAS_GL
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
#else
    (void)width; (void)height;
#endif

    sprite_batch_set_projection(r->batch, (float)width, (float)height);
}

void renderer_unbind_fbo(Renderer *r)
{
    if (!r) return;

    sprite_batch_flush(r->batch);
    r->bound_fbo = 0;

#ifdef RMMZ_HAS_GL
    glBindFramebuffer(GL_FRAMEBUFFER, r->screen_fbo);
    glViewport(r->vp_x, r->vp_y, r->vp_w, r->vp_h);
#endif

    sprite_batch_set_projection(r->batch, (float)r->width, (float)r->height);
}

uint8_t *renderer_read_pixels(Renderer *r, uint32_t fbo, int width, int height)
{
    if (!r || width <= 0 || height <= 0) return NULL;

    /* Guard against size_t overflow on 32-bit targets. */
    uint64_t alloc_size = (uint64_t)width * (uint64_t)height * 4;
    if (alloc_size > SIZE_MAX) return NULL;

    uint8_t *pixels = malloc((size_t)alloc_size);
    if (!pixels) return NULL;

#ifdef RMMZ_HAS_GL
    /* Flush pending draws before reading. */
    sprite_batch_flush(r->batch);

    /* 0 means "the screen", which is the offscreen surface the frame was
       composed into rather than the window itself. */
    if (fbo == 0 && r->screen_fbo) fbo = r->screen_fbo;

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    if ((uint32_t)prev_fbo != fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    }

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    /* OpenGL reads bottom-up; flip to top-down. */
    size_t row_bytes = (size_t)width * 4;
    uint8_t *tmp = malloc(row_bytes);
    if (!tmp) {
        free(pixels);
        if ((uint32_t)prev_fbo != fbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        }
        return NULL;
    }
    for (int y = 0; y < height / 2; y++) {
        uint8_t *top = pixels + y * row_bytes;
        uint8_t *bot = pixels + (height - 1 - y) * row_bytes;
        memcpy(tmp, top, row_bytes);
        memcpy(top, bot, row_bytes);
        memcpy(bot, tmp, row_bytes);
    }
    free(tmp);

    /* Restore previous FBO. */
    if ((uint32_t)prev_fbo != fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    }
#else
    memset(pixels, 0, (size_t)width * (size_t)height * 4);
#endif

    return pixels;
}

void renderer_rebind_batch(Renderer *r)
{
    if (r && r->batch) sprite_batch_rebind(r->batch);
}

void renderer_set_scissor(Renderer *r, int x, int y, int w, int h)
{
#ifdef RMMZ_HAS_GL
    if (!r) return;

    /* On the default framebuffer, map game-space to the letterbox viewport.
       Floor the origin and ceil the far edge so fractional scaling never
       clips boundary pixels. */
    if (r->bound_fbo == 0 && r->width > 0 && r->height > 0) {
        float sx = (float)r->vp_w / (float)r->width;
        float sy = (float)r->vp_h / (float)r->height;
        int x1 = (int)floorf(x * sx + r->vp_x);
        int y1 = (int)floorf(y * sy + r->vp_y);
        int x2 = (int)ceilf((x + w) * sx + r->vp_x);
        int y2 = (int)ceilf((y + h) * sy + r->vp_y);
        x = x1;
        y = y1;
        w = x2 - x1;
        h = y2 - y1;
    }

    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
#else
    (void)r; (void)x; (void)y; (void)w; (void)h;
#endif
}

void renderer_clear_scissor(Renderer *r)
{
#ifdef RMMZ_HAS_GL
    (void)r;
    glDisable(GL_SCISSOR_TEST);
#else
    (void)r;
#endif
}
