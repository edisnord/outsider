/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include <time.h>
#include "engine/js_engine.h"
#include "bindings/bind_canvas2d.h"
#include "rendering/canvas2d.h"
#include "rendering/image_loader.h"
#include "io/file_io.h"

#include <quickjs.h>
#include <stdlib.h>
#include <string.h>

#define PATH_BUF_SIZE 4096

/* Cap on pixel dimensions so w*h*4 cannot overflow size_t: 16384 (GL max) is
   safe on 64-bit; on 32-bit 16384^2*4 == 2^32 wraps, so cap at 8192 there. */
#include <stdint.h>
#if SIZE_MAX > 0xFFFFFFFFu
#define MAX_PIXEL_DIM 16384
#else
#define MAX_PIXEL_DIM 8192
#endif

/* Lifecycle */

/* create(width, height) -> handle */
static JSClassID c2d_token_class_id;

static void c2d_token_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    uint32_t handle = (uint32_t)(uintptr_t)JS_GetOpaque(val, c2d_token_class_id);
    if (handle) canvas2d_destroy(handle);
}

static JSValue js_c2d_create_token(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    uint32_t handle = 0;
    if (argc < 1 || JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    JSValue obj = JS_NewObjectClass(ctx, (int)c2d_token_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, (void *)(uintptr_t)handle);
    return obj;
}

/* The garbage collector runs on its own schedule, which counts script memory only; canvas pixels are far bigger than the
   objects that own them. Collect early when a lot of canvas memory (or many canvases) piled up. */
static size_t s_collect_mark;

/* Canvas memory guard. A game that makes too many or too big canvases (Domina REMAKE's weather plugin made 2.2 GB of
   them at start) would run the console out of memory and take it down. Past this limit a canvas is refused with an
   ordinary script error, which a game can show or survive, instead. RMMZ_CANVAS_LIMIT_MB overrides it. */
static size_t canvas_limit_bytes(void)
{
    static size_t limit;
    if (!limit) {
        const char *env = getenv("RMMZ_CANVAS_LIMIT_MB");
        long mb = env ? atol(env) : 0;
#ifdef RMMZ_PS5_HOST
        size_t def = (size_t)3072 * 1024 * 1024;   /* a PC has the memory; only a runaway game is stopped */
#else
        size_t def = (size_t)1024 * 1024 * 1024;
#endif
        limit = mb > 0 ? (size_t)mb * 1024 * 1024 : def;
    }
    return limit;
}

/* Runs the cycle collector on behalf of canvas creation and logs it when it is slow or common: on the console a
   collection of a game with a large heap takes long enough to be a stall of its own, and which of the two conditions
   below asked for it is the first thing to know. */
static void c2d_run_gc(JSContext *ctx, const char *why)
{
    static unsigned runs;
    static double total_ms;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    JS_RunGC(JS_GetRuntime(ctx));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    runs++;
    total_ms += ms;
    if (ms > 50.0 || runs <= 3 || runs % 100 == 0)
        printf("canvas2d: forced GC #%u (%s) took %.0f ms; %zu canvases, %zu MB of pixels; %.1f s in forced GCs so far\n",
               runs, why, ms, canvas2d_context_count(), canvas2d_bytes_alive() / (1024 * 1024), total_ms / 1000.0);
}

static void c2d_collect_if_needed(JSContext *ctx)
{
    size_t alive = canvas2d_bytes_alive();
    if (alive > s_collect_mark + (size_t)32 * 1024 * 1024) {
        c2d_run_gc(ctx, "pixel memory grew by 32 MB");
        s_collect_mark = canvas2d_bytes_alive();
    } else if (canvas2d_context_count() > 3000) {
        c2d_run_gc(ctx, "more than 3000 canvases");
        s_collect_mark = canvas2d_bytes_alive();
    }
}

static JSValue js_c2d_create(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    c2d_collect_if_needed(ctx);
    RMMZ_PROF_SCOPE("canvas2d.c2d_create");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "create requires width and height");

    int w, h;
    if (JS_ToInt32(ctx, &w, argv[0]) || JS_ToInt32(ctx, &h, argv[1]))
        return JS_EXCEPTION;

    if (w <= 0 || h <= 0 || w > MAX_PIXEL_DIM || h > MAX_PIXEL_DIM)
        return JS_ThrowRangeError(ctx, "create: dimensions out of range %dx%d", w, h);

    size_t need = (size_t)w * (size_t)h * 4;
    if (canvas2d_bytes_alive() + need > canvas_limit_bytes()) {
        c2d_run_gc(ctx, "canvas memory limit");   /* unreferenced canvases are freed by the garbage collector */
        if (canvas2d_bytes_alive() + need > canvas_limit_bytes()) {
            fprintf(stderr, "[canvas2d] memory limit: %zu MB in use, refusing a %dx%d canvas\n",
                    canvas2d_bytes_alive() / (1024 * 1024), w, h);
            return JS_ThrowRangeError(ctx, "out of canvas memory (%zu MB in use): a game made more or bigger images than "
                                           "this console can hold", canvas2d_bytes_alive() / (1024 * 1024));
        }
    }

    Canvas2DHandle handle = canvas2d_create(w, h);
    if (handle == CANVAS2D_HANDLE_INVALID)
        return JS_NULL;

    return JS_NewUint32(ctx, handle);
}

/* Canvases live as long as the script object that owns them. createToken(handle) returns an object that frees the
   canvas when the engine frees it (the context object holds one), because games never free their canvases
   themselves: RPG Maker MV makes dozens of window bitmaps per scene and leaves them to the garbage collector. */
/* destroy(handle) */
static JSValue js_c2d_destroy(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_destroy");
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "destroy requires a handle");
    uint32_t h;
    if (JS_ToUint32(ctx, &h, argv[0])) return JS_EXCEPTION;
    canvas2d_destroy(h);
    return JS_UNDEFINED;
}

/* contextCount() -> number of canvases alive */
static JSValue js_c2d_adjust_tone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_adjust_tone");
    (void)this_val;
    if (argc < 4) return JS_ThrowTypeError(ctx, "adjustTone requires handle, r, g, b");
    uint32_t handle;
    double r, g, b;
    if (JS_ToUint32(ctx, &handle, argv[0]) || JS_ToFloat64(ctx, &r, argv[1]) ||
        JS_ToFloat64(ctx, &g, argv[2]) || JS_ToFloat64(ctx, &b, argv[3]))
        return JS_EXCEPTION;
    canvas2d_adjust_tone((Canvas2DHandle)handle, r, g, b);
    return JS_UNDEFINED;
}

static JSValue js_c2d_context_count(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, (int32_t)canvas2d_context_count());
}

/* bytesAlive() -> pixel bytes held by all canvases (diagnostics) */
static JSValue js_c2d_bytes_alive(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, (double)canvas2d_bytes_alive());
}

/* contextSizes() -> [[w, h], ...] of the live canvases (diagnostics) */
static JSValue js_c2d_context_sizes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    static int w[4096], h[4096];
    size_t n = canvas2d_list_sizes(w, h, 4096);
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < n; i++) {
        JSValue pair = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, pair, 0, JS_NewInt32(ctx, w[i]));
        JS_SetPropertyUint32(ctx, pair, 1, JS_NewInt32(ctx, h[i]));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, pair);
    }
    return arr;
}

/* resize(handle, width, height) */
static JSValue js_c2d_resize(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_resize");
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "resize requires handle, width, height");
    uint32_t handle;
    int w, h;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[2])) return JS_EXCEPTION;

    if (w <= 0 || h <= 0 || w > MAX_PIXEL_DIM || h > MAX_PIXEL_DIM)
        return JS_ThrowRangeError(ctx, "resize: dimensions out of range %dx%d", w, h);

    canvas2d_resize(handle, w, h);
    return JS_UNDEFINED;
}

/* getSize(handle) -> {width, height} */
static JSValue js_c2d_get_size(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_get_size");
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getSize requires a handle");
    uint32_t handle;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    int w, h;
    if (!canvas2d_get_size(handle, &w, &h)) return JS_NULL;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, h));
    return obj;
}

/* State setters */

/* setFillColor(handle, r, g, b, a) */
static JSValue js_c2d_set_fill_color(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_set_fill_color");
    (void)this_val;
    if (argc < 5)
        return JS_ThrowTypeError(ctx, "setFillColor requires handle,r,g,b,a");
    uint32_t handle; int r, g, b, a;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &r, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &g, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &b, argv[3])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &a, argv[4])) return JS_EXCEPTION;
    canvas2d_set_fill_color(handle, (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a);
    return JS_UNDEFINED;
}

/* setStrokeColor(handle, r, g, b, a) */
static JSValue js_c2d_set_stroke_color(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_set_stroke_color");
    (void)this_val;
    if (argc < 5)
        return JS_ThrowTypeError(ctx, "setStrokeColor requires handle,r,g,b,a");
    uint32_t handle; int r, g, b, a;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &r, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &g, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &b, argv[3])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &a, argv[4])) return JS_EXCEPTION;
    canvas2d_set_stroke_color(handle, (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a);
    return JS_UNDEFINED;
}

/* setGlobalAlpha(handle, alpha) */
static JSValue js_c2d_set_global_alpha(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_set_global_alpha");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "setGlobalAlpha requires handle, alpha");
    uint32_t handle; double alpha;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &alpha, argv[1])) return JS_EXCEPTION;
    canvas2d_set_global_alpha(handle, (float)alpha);
    return JS_UNDEFINED;
}

/* setCompositeOp(handle, op) */
static JSValue js_c2d_set_composite_op(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_set_composite_op");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "setCompositeOp requires handle, op");
    uint32_t handle; int op;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &op, argv[1])) return JS_EXCEPTION;
    canvas2d_set_composite_op(handle, op);
    return JS_UNDEFINED;
}

/* setLineWidth(handle, width) */
static JSValue js_c2d_set_line_width(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_set_line_width");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "setLineWidth requires handle, width");
    uint32_t handle; double w;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &w, argv[1])) return JS_EXCEPTION;
    canvas2d_set_line_width(handle, (float)w);
    return JS_UNDEFINED;
}

/* setTextAlign(handle, align) */
static JSValue js_c2d_set_text_align(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_set_text_align");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "setTextAlign requires handle, align");
    uint32_t handle; int align;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &align, argv[1])) return JS_EXCEPTION;
    canvas2d_set_text_align(handle, align);
    return JS_UNDEFINED;
}

/* setTextBaseline(handle, baseline) */
static JSValue js_c2d_set_text_baseline(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_set_text_baseline");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "setTextBaseline requires handle, baseline");
    uint32_t handle; int baseline;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &baseline, argv[1])) return JS_EXCEPTION;
    canvas2d_set_text_baseline(handle, baseline);
    return JS_UNDEFINED;
}

/* State stack */

/* save(handle) */
static JSValue js_c2d_save(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_save");
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "save requires a handle");
    uint32_t handle;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    canvas2d_save(handle);
    return JS_UNDEFINED;
}

/* restore(handle) */
static JSValue js_c2d_restore(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_restore");
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "restore requires a handle");
    uint32_t handle;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    canvas2d_restore(handle);
    return JS_UNDEFINED;
}

/* Drawing operations */

/* fillRect(handle, x, y, w, h) */
static JSValue js_c2d_fill_rect(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_fill_rect");
    (void)this_val;
    if (argc < 5)
        return JS_ThrowTypeError(ctx, "fillRect requires handle,x,y,w,h");
    uint32_t handle; int x, y, w, h;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &x, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[3])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[4])) return JS_EXCEPTION;
    canvas2d_fill_rect(handle, x, y, w, h);
    return JS_UNDEFINED;
}

/* fillGradient(handle, x, y, w, h, radial, p0, p1, p2, p3, stops) where stops is a flat array
   [offset, r, g, b, a, offset, r, g, b, a, ...] sorted by offset */
static JSValue js_c2d_fill_gradient(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_fill_gradient");
    (void)this_val;
    if (argc < 11)
        return JS_ThrowTypeError(ctx, "fillGradient requires handle,x,y,w,h,radial,p0..p3,stops");
    uint32_t handle; int x, y, w, h, radial;
    double p[4];
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &x, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[3])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[4])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &radial, argv[5])) return JS_EXCEPTION;
    for (int i = 0; i < 4; i++)
        if (JS_ToFloat64(ctx, &p[i], argv[6 + i])) return JS_EXCEPTION;

    JSValue len_v = JS_GetPropertyStr(ctx, argv[10], "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_v);
    JS_FreeValue(ctx, len_v);
    int n = len / 5;
    if (n <= 0 || n > 64) return JS_UNDEFINED;

    double offsets[64];
    uint8_t rgba[64 * 4];
    for (int i = 0; i < n; i++) {
        double v[5];
        for (int k = 0; k < 5; k++) {
            JSValue e = JS_GetPropertyUint32(ctx, argv[10], (uint32_t)(i * 5 + k));
            int rc = JS_ToFloat64(ctx, &v[k], e);
            JS_FreeValue(ctx, e);
            if (rc) return JS_EXCEPTION;
        }
        offsets[i] = v[0];
        for (int k = 0; k < 4; k++) {
            int c = (int)(v[1 + k] + 0.5);
            rgba[i * 4 + k] = (uint8_t)(c < 0 ? 0 : (c > 255 ? 255 : c));
        }
    }
    canvas2d_fill_gradient_rect(handle, x, y, w, h, radial != 0, p[0], p[1], p[2], p[3],
                                offsets, rgba, n);
    return JS_UNDEFINED;
}

/* fillPath(handle, pts, counts, clipX, clipY, clipW, clipH, hasClip, gradKind, p0, p1, p2, p3, stops)
   pts: flat [x, y, ...] of all subpaths; counts: points per subpath; gradKind -1/0/1; stops as in
   fillGradient (ignored for -1) */
static JSValue js_c2d_fill_path(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_fill_path");
    (void)this_val;
    if (argc < 14)
        return JS_ThrowTypeError(ctx, "fillPath requires 14 arguments");
    uint32_t handle; int clip[4], has_clip, kind;
    double p[4];
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    for (int i = 0; i < 4; i++)
        if (JS_ToInt32(ctx, &clip[i], argv[3 + i])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &has_clip, argv[7])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &kind, argv[8])) return JS_EXCEPTION;
    for (int i = 0; i < 4; i++)
        if (JS_ToFloat64(ctx, &p[i], argv[9 + i])) return JS_EXCEPTION;

    int32_t npts_len = 0, nsub = 0, stops_len = 0;
    JSValue v = JS_GetPropertyStr(ctx, argv[1], "length"); JS_ToInt32(ctx, &npts_len, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[2], "length"); JS_ToInt32(ctx, &nsub, v); JS_FreeValue(ctx, v);
    if (kind >= 0) {
        v = JS_GetPropertyStr(ctx, argv[13], "length"); JS_ToInt32(ctx, &stops_len, v); JS_FreeValue(ctx, v);
    }
    if (npts_len < 6 || nsub <= 0 || nsub > 4096 || npts_len > 4 * 1024 * 1024) return JS_UNDEFINED;

    double *pts = malloc((size_t)npts_len * sizeof(double));
    int *counts = malloc((size_t)nsub * sizeof(int));
    if (!pts || !counts) { free(pts); free(counts); return JS_UNDEFINED; }
    JSValue result = JS_UNDEFINED;
    for (int i = 0; i < npts_len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
        int rc = JS_ToFloat64(ctx, &pts[i], e);
        JS_FreeValue(ctx, e);
        if (rc) { result = JS_EXCEPTION; goto done; }
    }
    for (int i = 0; i < nsub; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[2], (uint32_t)i);
        int rc = JS_ToInt32(ctx, &counts[i], e);
        JS_FreeValue(ctx, e);
        if (rc) { result = JS_EXCEPTION; goto done; }
    }

    double offsets[64];
    uint8_t rgba[64 * 4];
    int n = stops_len / 5;
    if (kind >= 0) {
        if (n <= 0 || n > 64) goto done;
        for (int i = 0; i < n; i++) {
            double sv[5];
            for (int k = 0; k < 5; k++) {
                JSValue e = JS_GetPropertyUint32(ctx, argv[13], (uint32_t)(i * 5 + k));
                int rc = JS_ToFloat64(ctx, &sv[k], e);
                JS_FreeValue(ctx, e);
                if (rc) { result = JS_EXCEPTION; goto done; }
            }
            offsets[i] = sv[0];
            for (int k = 0; k < 4; k++) {
                int c = (int)(sv[1 + k] + 0.5);
                rgba[i * 4 + k] = (uint8_t)(c < 0 ? 0 : (c > 255 ? 255 : c));
            }
        }
    }
    canvas2d_fill_path(handle, pts, counts, nsub, has_clip != 0, clip[0], clip[1], clip[2], clip[3],
                       kind, p[0], p[1], p[2], p[3], offsets, rgba, n);
done:
    free(pts);
    free(counts);
    return result;
}

/* clearRect(handle, x, y, w, h) */
static JSValue js_c2d_clear_rect(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_clear_rect");
    (void)this_val;
    if (argc < 5)
        return JS_ThrowTypeError(ctx, "clearRect requires handle,x,y,w,h");
    uint32_t handle; int x, y, w, h;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &x, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[3])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[4])) return JS_EXCEPTION;
    canvas2d_clear_rect(handle, x, y, w, h);
    return JS_UNDEFINED;
}

/* strokeRect(handle, x, y, w, h) */
static JSValue js_c2d_stroke_rect(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_stroke_rect");
    (void)this_val;
    if (argc < 5)
        return JS_ThrowTypeError(ctx, "strokeRect requires handle,x,y,w,h");
    uint32_t handle; int x, y, w, h;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &x, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[3])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[4])) return JS_EXCEPTION;
    canvas2d_stroke_rect(handle, x, y, w, h);
    return JS_UNDEFINED;
}

/* drawImage(handle, srcPixels, srcW, srcH, sx, sy, sw, sh, dx, dy, dw, dh) */
static JSValue js_c2d_draw_image(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_draw_image");
    (void)this_val;
    if (argc < 12)
        return JS_ThrowTypeError(ctx, "drawImage requires 12 arguments");

    uint32_t handle;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;

    int src_w, src_h, sx, sy, sw, sh, dx, dy, dw, dh;
    if (JS_ToInt32(ctx, &src_w, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &src_h, argv[3])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &sx, argv[4])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &sy, argv[5])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &sw, argv[6])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &sh, argv[7])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &dx, argv[8])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &dy, argv[9])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &dw, argv[10])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &dh, argv[11])) return JS_EXCEPTION;

    /* Fetch the pixel ArrayBuffer (argv[1]) only after the conversions above:
       they can run user JS (valueOf) that detaches it and leaves a stale pointer. */
    size_t buf_size = 0;
    uint8_t *src_pixels = JS_GetArrayBuffer(ctx, &buf_size, argv[1]);
    if (!src_pixels)
        return JS_ThrowTypeError(ctx, "drawImage: second arg must be ArrayBuffer");

    if (src_w <= 0 || src_h <= 0 || src_w > MAX_PIXEL_DIM || src_h > MAX_PIXEL_DIM ||
        buf_size < (size_t)src_w * (size_t)src_h * 4)
        return JS_ThrowRangeError(ctx, "drawImage: buffer size mismatch for %dx%d", src_w, src_h);

    canvas2d_draw_image(handle, src_pixels, src_w, src_h,
                         sx, sy, sw, sh, dx, dy, dw, dh);
    return JS_UNDEFINED;
}

/* drawImageHandle(handle, imageHandle, sx, sy, sw, sh, dx, dy, dw, dh)
   Like drawImage but takes an image_loader handle instead of pixel ArrayBuffer. */
static JSValue js_c2d_draw_image_handle(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_draw_image_handle");
    (void)this_val;
    if (argc < 10)
        return JS_ThrowTypeError(ctx, "drawImageHandle requires 10 arguments");

    uint32_t handle, img_handle;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &img_handle, argv[1])) return JS_EXCEPTION;

    ImageInfo info;
    if (!image_get_info(img_handle, &info)) return JS_UNDEFINED;
    const uint8_t *pixels = image_get_pixels(img_handle);
    if (!pixels) return JS_UNDEFINED;

    int sx, sy, sw, sh, dx, dy, dw, dh;
    if (JS_ToInt32(ctx, &sx, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &sy, argv[3])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &sw, argv[4])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &sh, argv[5])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &dx, argv[6])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &dy, argv[7])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &dw, argv[8])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &dh, argv[9])) return JS_EXCEPTION;

    canvas2d_draw_image(handle, pixels, info.width, info.height,
                         sx, sy, sw, sh, dx, dy, dw, dh);
    return JS_UNDEFINED;
}

/* Pixel manipulation */

/* getImageData(handle, x, y, w, h) -> ArrayBuffer (RGBA) */
static JSValue js_c2d_get_image_data(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_get_image_data");
    (void)this_val;
    if (argc < 5)
        return JS_ThrowTypeError(ctx, "getImageData requires handle,x,y,w,h");
    uint32_t handle; int x, y, w, h;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &x, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[3])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[4])) return JS_EXCEPTION;

    if (w <= 0 || h <= 0) return JS_NULL;
    if (w > MAX_PIXEL_DIM || h > MAX_PIXEL_DIM)
        return JS_ThrowRangeError(ctx, "getImageData: dimensions too large %dx%d", w, h);

    size_t buf_size = (size_t)w * (size_t)h * 4;
    if (buf_size > 64 * 1024 * 1024)
        return JS_ThrowRangeError(ctx, "getImageData: total size too large (%zu bytes)", buf_size);
    /* calloc: an invalid handle or out-of-range rect yields zeroed pixels, not uninitialized heap. */
    uint8_t *buf = calloc(buf_size, 1);
    if (!buf) return JS_ThrowInternalError(ctx, "out of memory");

    canvas2d_get_image_data(handle, x, y, w, h, buf);

    JSValue ab = JS_NewArrayBufferCopy(ctx, buf, buf_size);
    free(buf);
    return ab;
}

/* putImageData(handle, arrayBuffer, x, y, w, h) */
static JSValue js_c2d_put_image_data(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_put_image_data");
    (void)this_val;
    if (argc < 6)
        return JS_ThrowTypeError(ctx, "putImageData requires handle,data,x,y,w,h");
    uint32_t handle;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;

    int x, y, w, h;
    if (JS_ToInt32(ctx, &x, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[3])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[4])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[5])) return JS_EXCEPTION;

    /* Fetch the ArrayBuffer only after the conversions above: they can run user
       JS (valueOf) that detaches it and leaves a stale pointer. */
    size_t buf_size = 0;
    uint8_t *data = JS_GetArrayBuffer(ctx, &buf_size, argv[1]);
    if (!data)
        return JS_ThrowTypeError(ctx, "putImageData: second arg must be ArrayBuffer");

    if (w <= 0 || h <= 0 || w > MAX_PIXEL_DIM || h > MAX_PIXEL_DIM ||
        buf_size < (size_t)w * (size_t)h * 4)
        return JS_ThrowRangeError(ctx, "putImageData: buffer size mismatch for %dx%d", w, h);

    canvas2d_put_image_data(handle, data, x, y, w, h);
    return JS_UNDEFINED;
}

/* getPixels(handle) -> ArrayBuffer (full canvas RGBA) */
static JSValue js_c2d_get_pixels(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_get_pixels");
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "getPixels requires a handle");
    uint32_t handle;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;

    int w, h;
    if (!canvas2d_get_size(handle, &w, &h)) return JS_NULL;
    uint8_t *pixels = canvas2d_get_pixels(handle);
    if (!pixels) return JS_NULL;

    if (w <= 0 || h <= 0 || w > MAX_PIXEL_DIM || h > MAX_PIXEL_DIM)
        return JS_ThrowRangeError(ctx, "getPixels: canvas dimensions out of range %dx%d", w, h);

    return JS_NewArrayBufferCopy(ctx, pixels, (size_t)w * (size_t)h * 4);
}

/* Font and text */

/* loadFont(name, arrayBuffer) -> bool */
static JSValue js_c2d_load_font(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_load_font");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "loadFont requires name and ArrayBuffer");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    size_t buf_size = 0;
    uint8_t *data = JS_GetArrayBuffer(ctx, &buf_size, argv[1]);
    if (!data) {
        JS_FreeCString(ctx, name);
        return JS_ThrowTypeError(ctx, "loadFont: second arg must be ArrayBuffer");
    }

    bool ok = canvas2d_load_font(name, data, buf_size);
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, ok);
}

/* loadFontFile(name, path) -> bool */
static JSValue js_c2d_load_font_file(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_load_font_file");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "loadFontFile requires name and path");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    const char *path = JS_ToCString(ctx, argv[1]);
    if (!path) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }

    char resolved[PATH_BUF_SIZE];
    if (!file_io_resolve_path(path, resolved, sizeof(resolved))) {
        JS_FreeCString(ctx, path);
        JS_FreeCString(ctx, name);
        return JS_NewBool(ctx, false);
    }
    JS_FreeCString(ctx, path);

    size_t file_size = 0;
    uint8_t *data = file_io_read_binary(resolved, &file_size);
    if (!data) {
        JS_FreeCString(ctx, name);
        return JS_NewBool(ctx, false);
    }

    bool ok = canvas2d_load_font(name, data, file_size);
    free(data);
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, ok);
}

/* setFont(handle, name, pixelSize) */
static JSValue js_c2d_set_font(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_set_font");
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "setFont requires handle, name, pixelSize");
    uint32_t handle; double size;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    const char *name = JS_ToCString(ctx, argv[1]);
    if (!name) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &size, argv[2])) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    canvas2d_set_font(handle, name, (float)size);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

/* fillText(handle, text, x, y) */
static JSValue js_c2d_fill_text(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_fill_text");
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "fillText requires handle,text,x,y");
    uint32_t handle; double x, y;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    const char *text = JS_ToCString(ctx, argv[1]);
    if (!text) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &x, argv[2])) { JS_FreeCString(ctx, text); return JS_EXCEPTION; }
    if (JS_ToFloat64(ctx, &y, argv[3])) { JS_FreeCString(ctx, text); return JS_EXCEPTION; }
    canvas2d_fill_text(handle, text, (float)x, (float)y);
    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

/* strokeText(handle, text, x, y) */
static JSValue js_c2d_stroke_text(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_stroke_text");
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "strokeText requires handle,text,x,y");
    uint32_t handle; double x, y;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    const char *text = JS_ToCString(ctx, argv[1]);
    if (!text) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &x, argv[2])) { JS_FreeCString(ctx, text); return JS_EXCEPTION; }
    if (JS_ToFloat64(ctx, &y, argv[3])) { JS_FreeCString(ctx, text); return JS_EXCEPTION; }
    canvas2d_stroke_text(handle, text, (float)x, (float)y);
    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

/* measureText(handle, text) -> number (width in pixels) */
static JSValue js_c2d_measure_text(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("canvas2d.c2d_measure_text");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "measureText requires handle and text");
    uint32_t handle;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    const char *text = JS_ToCString(ctx, argv[1]);
    if (!text) return JS_EXCEPTION;
    float w = canvas2d_measure_text(handle, text);
    JS_FreeCString(ctx, text);
    return JS_NewFloat64(ctx, (double)w);
}

/* measureTextBounds(handle, text) -> [leftReach, right, ascent, descent] */
static JSValue js_c2d_measure_text_bounds(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "measureTextBounds requires handle and text");
    uint32_t handle;
    if (JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    const char *text = JS_ToCString(ctx, argv[1]);
    if (!text) return JS_EXCEPTION;
    float b[4];
    canvas2d_measure_text_bounds(handle, text, b);
    JS_FreeCString(ctx, text);
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < 4; i++) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewFloat64(ctx, (double)b[i]));
    return arr;
}

/* Registration */

static const JSCFunctionListEntry js_c2d_funcs[] = {
    JS_CFUNC_DEF("create",           2, js_c2d_create),
    JS_CFUNC_DEF("destroy",          1, js_c2d_destroy),
    JS_CFUNC_DEF("createToken",      1, js_c2d_create_token),
    JS_CFUNC_DEF("contextCount",     0, js_c2d_context_count),
    JS_CFUNC_DEF("bytesAlive",       0, js_c2d_bytes_alive),
    JS_CFUNC_DEF("contextSizes",     0, js_c2d_context_sizes),
    JS_CFUNC_DEF("resize",           3, js_c2d_resize),
    JS_CFUNC_DEF("getSize",          1, js_c2d_get_size),
    JS_CFUNC_DEF("setFillColor",     5, js_c2d_set_fill_color),
    JS_CFUNC_DEF("setStrokeColor",   5, js_c2d_set_stroke_color),
    JS_CFUNC_DEF("setGlobalAlpha",   2, js_c2d_set_global_alpha),
    JS_CFUNC_DEF("setCompositeOp",   2, js_c2d_set_composite_op),
    JS_CFUNC_DEF("setLineWidth",     2, js_c2d_set_line_width),
    JS_CFUNC_DEF("setTextAlign",     2, js_c2d_set_text_align),
    JS_CFUNC_DEF("setTextBaseline",  2, js_c2d_set_text_baseline),
    JS_CFUNC_DEF("save",             1, js_c2d_save),
    JS_CFUNC_DEF("restore",          1, js_c2d_restore),
    JS_CFUNC_DEF("fillRect",         5, js_c2d_fill_rect),
    JS_CFUNC_DEF("fillGradient",    11, js_c2d_fill_gradient),
    JS_CFUNC_DEF("fillPath",        14, js_c2d_fill_path),
    JS_CFUNC_DEF("clearRect",        5, js_c2d_clear_rect),
    JS_CFUNC_DEF("strokeRect",       5, js_c2d_stroke_rect),
    JS_CFUNC_DEF("drawImage",       12, js_c2d_draw_image),
    JS_CFUNC_DEF("drawImageHandle", 10, js_c2d_draw_image_handle),
    JS_CFUNC_DEF("adjustTone",       4, js_c2d_adjust_tone),
    JS_CFUNC_DEF("getImageData",     5, js_c2d_get_image_data),
    JS_CFUNC_DEF("putImageData",     6, js_c2d_put_image_data),
    JS_CFUNC_DEF("getPixels",        1, js_c2d_get_pixels),
    JS_CFUNC_DEF("loadFont",         2, js_c2d_load_font),
    JS_CFUNC_DEF("loadFontFile",     2, js_c2d_load_font_file),
    JS_CFUNC_DEF("setFont",          3, js_c2d_set_font),
    JS_CFUNC_DEF("fillText",         4, js_c2d_fill_text),
    JS_CFUNC_DEF("strokeText",       4, js_c2d_stroke_text),
    JS_CFUNC_DEF("measureText",      2, js_c2d_measure_text),
    JS_CFUNC_DEF("measureTextBounds", 2, js_c2d_measure_text_bounds),
};

void bind_canvas2d_register(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (!c2d_token_class_id) {
        static const JSClassDef token_def = { "Canvas2DToken", c2d_token_finalizer, NULL, NULL, NULL };
        JS_NewClassID(rt, &c2d_token_class_id);
        JS_NewClass(rt, c2d_token_class_id, &token_def);
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue obj = JS_NewObject(ctx);

    JS_SetPropertyFunctionList(ctx, obj, js_c2d_funcs,
                               sizeof(js_c2d_funcs) / sizeof(js_c2d_funcs[0]));

    JS_SetPropertyStr(ctx, global, "__native_canvas2d", obj);
    JS_FreeValue(ctx, global);
}
