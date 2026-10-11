/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/canvas2d.h"
#include "engine/js_engine.h"
#include "bindings/bind_renderer.h"
#include "rendering/renderer.h"
#include "rendering/sprite_batch.h"

#include <quickjs.h>
#include <stdlib.h>

#ifdef RMMZ_HAS_GL
#include "rendering/gl_loader.h"
#endif

/* Cap on pixel dimensions so w*h*4 cannot overflow size_t: 16384 (GL max) is
   safe on 64-bit; on 32-bit 16384^2*4 == 2^32 wraps, so cap at 8192 there. */
#include <stdint.h>
#include "engine/error_handler.h"
#if SIZE_MAX > 0xFFFFFFFFu
#define MAX_PIXEL_DIM 16384
#else
#define MAX_PIXEL_DIM 8192
#endif

/* Single global renderer instance; also accessed by bind_tilemap.c. */
Renderer *s_renderer = NULL;

/* Lifecycle */

/* init(width, height) -> bool */
static JSValue js_renderer_init(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_init");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "init requires width and height");

    int w, h;
    if (JS_ToInt32(ctx, &w, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[1])) return JS_EXCEPTION;

    if (s_renderer) {
        renderer_resize(s_renderer, w, h);
        return JS_TRUE;
    }

    s_renderer = renderer_create(w, h);
    return JS_NewBool(ctx, s_renderer != NULL);
}

/* shutdown() */
static JSValue js_renderer_shutdown(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_shutdown");
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (s_renderer) {
        renderer_destroy(s_renderer);
        s_renderer = NULL;
    }
    return JS_UNDEFINED;
}

/* resize(width, height) */
static JSValue js_renderer_resize(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_resize");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "resize requires width and height");
    if (!s_renderer) return JS_UNDEFINED;

    int w, h;
    if (JS_ToInt32(ctx, &w, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[1])) return JS_EXCEPTION;
    renderer_resize(s_renderer, w, h);
    return JS_UNDEFINED;
}

/* getSize() -> {width, height} */
static JSValue js_renderer_get_size(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_get_size");
    (void)this_val; (void)argc; (void)argv;
    if (!s_renderer) return JS_NULL;

    int w, h;
    renderer_get_size(s_renderer, &w, &h);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, h));
    return obj;
}

/* Frame management */

/* beginFrame() */
static JSValue js_renderer_begin_frame(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_begin_frame");
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (s_renderer) renderer_begin_frame(s_renderer);
    return JS_UNDEFINED;
}

/* beginFrameTransparent() - clear to transparent black (for FBO rendering) */
static JSValue js_renderer_begin_frame_transparent(JSContext *ctx, JSValueConst this_val,
                                                    int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_begin_frame_transparent");
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (s_renderer) renderer_begin_frame_transparent(s_renderer);
    return JS_UNDEFINED;
}

/* endFrame() */
static JSValue js_renderer_end_frame(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_end_frame");
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (s_renderer) renderer_end_frame(s_renderer);
    return JS_UNDEFINED;
}

/* Texture management */

/* createTexture(width, height) -> textureId */
static JSValue js_renderer_create_texture(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_create_texture");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "createTexture requires width and height");

    int w, h;
    if (JS_ToInt32(ctx, &w, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[1])) return JS_EXCEPTION;

    if (w <= 0 || h <= 0 || w > MAX_PIXEL_DIM || h > MAX_PIXEL_DIM)
        return JS_ThrowRangeError(ctx, "createTexture: dimensions out of range %dx%d", w, h);

    uint32_t tex = renderer_create_texture(w, h);
    return JS_NewUint32(ctx, tex);
}

/* updateTexture(textureId, width, height, pixelsArrayBuffer) */
static JSValue js_renderer_update_texture(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_update_texture");
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "updateTexture requires texId, w, h, pixels");

    uint32_t tex;
    int w, h;
    if (JS_ToUint32(ctx, &tex, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[2])) return JS_EXCEPTION;

    size_t buf_size = 0;
    uint8_t *pixels = JS_GetArrayBuffer(ctx, &buf_size, argv[3]);
    if (!pixels)
        return JS_ThrowTypeError(ctx, "updateTexture: fourth arg must be ArrayBuffer");

    if (w <= 0 || h <= 0 || w > MAX_PIXEL_DIM || h > MAX_PIXEL_DIM ||
        buf_size < (size_t)w * (size_t)h * 4)
        return JS_ThrowRangeError(ctx, "updateTexture: buffer too small for %dx%d", w, h);

    renderer_update_texture(tex, w, h, pixels);
    return JS_UNDEFINED;
}

/* updateTextureFromCanvas(texId, w, h, canvasHandle) -> bool: uploads a canvas's pixels straight from its own
   buffer. getPixels() + updateTexture() copies the whole canvas into a fresh ArrayBuffer first, which for the
   screen-sized canvases of lighting plugins is several megabytes per upload. False: sizes do not match. */
static JSValue js_renderer_update_texture_from_canvas(JSContext *ctx, JSValueConst this_val,
                                                      int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_update_texture");
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "updateTextureFromCanvas requires texId, w, h, canvasHandle");

    uint32_t tex, handle;
    int w, h;
    if (JS_ToUint32(ctx, &tex, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[2])) return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &handle, argv[3])) return JS_EXCEPTION;

    int cw = 0, ch = 0;
    if (!canvas2d_get_size(handle, &cw, &ch) || cw != w || ch != h) return JS_FALSE;
    if (w <= 0 || h <= 0 || w > MAX_PIXEL_DIM || h > MAX_PIXEL_DIM) return JS_FALSE;
    uint8_t *pixels = canvas2d_get_pixels(handle);
    if (!pixels) return JS_FALSE;
    renderer_update_texture(tex, w, h, pixels);
    return JS_TRUE;
}

/* Texture tokens: createTextureToken(textureId) returns an object that deletes the texture when the engine
   frees it (at the start of the next frame), so textures of script objects nobody frees explicitly (RPG Maker MV
   leaves its bitmaps to the garbage collector) do not pile up. releaseTextureToken(token) deletes the texture right
   away and disarms the token. */
static JSClassID texture_token_class_id;

static void texture_token_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    uint32_t tex = (uint32_t)(uintptr_t)JS_GetOpaque(val, texture_token_class_id);
    if (tex) renderer_queue_texture_delete(tex);
}

static JSValue js_renderer_create_texture_token(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    uint32_t tex = 0;
    if (argc < 1 || JS_ToUint32(ctx, &tex, argv[0])) return JS_EXCEPTION;
    JSValue obj = JS_NewObjectClass(ctx, (int)texture_token_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, (void *)(uintptr_t)tex);
    return obj;
}

static JSValue js_renderer_release_texture_token(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)ctx;
    if (argc < 1) return JS_UNDEFINED;
    uint32_t tex = (uint32_t)(uintptr_t)JS_GetOpaque(argv[0], texture_token_class_id);
    if (tex) {
        JS_SetOpaque(argv[0], NULL);
        renderer_delete_texture(tex);
    }
    return JS_UNDEFINED;
}

/* deleteTexture(textureId) */
static JSValue js_renderer_delete_texture(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_delete_texture");
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "deleteTexture requires textureId");

    uint32_t tex;
    if (JS_ToUint32(ctx, &tex, argv[0])) return JS_EXCEPTION;
    renderer_delete_texture(tex);
    return JS_UNDEFINED;
}

/* Render textures (FBO) */

/* createRenderTexture(width, height) -> {fbo, texture} */
static JSValue js_renderer_create_rt(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_create_rt");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "createRenderTexture requires width and height");

    int w, h;
    if (JS_ToInt32(ctx, &w, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[1])) return JS_EXCEPTION;

    if (w <= 0 || h <= 0 || w > MAX_PIXEL_DIM || h > MAX_PIXEL_DIM)
        return JS_ThrowRangeError(ctx, "createRenderTexture: dimensions out of range %dx%d", w, h);

    uint32_t tex = 0;
    uint32_t fbo = renderer_create_fbo(w, h, &tex);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "fbo", JS_NewUint32(ctx, fbo));
    JS_SetPropertyStr(ctx, obj, "texture", JS_NewUint32(ctx, tex));
    return obj;
}

/* deleteRenderTexture(fbo, texture) */
static JSValue js_renderer_delete_rt(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_delete_rt");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "deleteRenderTexture requires fbo and texture");

    uint32_t fbo, tex;
    if (JS_ToUint32(ctx, &fbo, argv[0])) return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &tex, argv[1])) return JS_EXCEPTION;
    renderer_delete_fbo(fbo, tex);
    return JS_UNDEFINED;
}

/* bindRenderTexture(fbo, width, height) */
static JSValue js_renderer_bind_rt(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_bind_rt");
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "bindRenderTexture requires fbo, width, height");
    if (!s_renderer) return JS_UNDEFINED;

    uint32_t fbo;
    int w, h;
    if (JS_ToUint32(ctx, &fbo, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[2])) return JS_EXCEPTION;
    renderer_bind_fbo(s_renderer, fbo, w, h);
    return JS_UNDEFINED;
}

/* unbindRenderTexture() */
static JSValue js_renderer_unbind_rt(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_unbind_rt");
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (s_renderer) renderer_unbind_fbo(s_renderer);
    return JS_UNDEFINED;
}

/* Sprite batch drawing */

/* drawQuad(texture, x, y, w, h, u0, v0, u1, v1, tint, alpha) */
static JSValue js_renderer_draw_quad(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_draw_quad");
    (void)this_val;
    if (argc < 11)
        return JS_ThrowTypeError(ctx, "drawQuad requires 11 arguments");
    if (!s_renderer) return JS_UNDEFINED;

    uint32_t texture;
    double x, y, w, h, u0, v0, u1, v1, alpha;
    uint32_t tint;

    if (JS_ToUint32(ctx, &texture, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &x, argv[1])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[2])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &w, argv[3])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &h, argv[4])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &u0, argv[5])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &v0, argv[6])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &u1, argv[7])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &v1, argv[8])) return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &tint, argv[9])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &alpha, argv[10])) return JS_EXCEPTION;

    SpriteBatch *batch = renderer_get_batch(s_renderer);
    if (batch) {
        sprite_batch_draw(batch, texture,
                          (float)x, (float)y, (float)w, (float)h,
                          (float)u0, (float)v0, (float)u1, (float)v1,
                          tint, (float)alpha);
    }
    return JS_UNDEFINED;
}

/* drawQuadVertices(texture, x0,y0, x1,y1, x2,y2, x3,y3, u0,v0,u1,v1, tint, alpha)
   Corners in TL, TR, BR, BL order; preserves rotation/skew/mirroring. */
static JSValue js_renderer_draw_quad_verts(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_draw_quad_verts");
    (void)this_val;
    if (argc < 15)
        return JS_ThrowTypeError(ctx, "drawQuadVertices requires 15 arguments");
    if (!s_renderer) return JS_UNDEFINED;

    uint32_t texture, tint;
    double p[8]; /* x0,y0,x1,y1,x2,y2,x3,y3 */
    double u0, v0, u1, v1, alpha;

    if (JS_ToUint32(ctx, &texture, argv[0])) return JS_EXCEPTION;
    for (int i = 0; i < 8; i++) {
        if (JS_ToFloat64(ctx, &p[i], argv[1 + i])) return JS_EXCEPTION;
    }
    if (JS_ToFloat64(ctx, &u0, argv[9]))  return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &v0, argv[10])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &u1, argv[11])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &v1, argv[12])) return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &tint, argv[13])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &alpha, argv[14])) return JS_EXCEPTION;

    SpriteBatch *batch = renderer_get_batch(s_renderer);
    if (batch) {
        sprite_batch_draw_verts(batch, texture,
                                (float)p[0], (float)p[1], (float)p[2], (float)p[3],
                                (float)p[4], (float)p[5], (float)p[6], (float)p[7],
                                (float)u0, (float)v0, (float)u1, (float)v1,
                                tint, (float)alpha);
    }
    return JS_UNDEFINED;
}

/* drawQuadVerticesUV(texture, x0,y0, x1,y1, x2,y2, x3,y3,
                      u0,v0, u1,v1, u2,v2, u3,v3, tint, alpha)
   Corners and their UVs in TL, TR, BR, BL order. */
static JSValue js_renderer_draw_quad_verts_uv(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_draw_quad_verts_uv");
    (void)this_val;
    if (argc < 19)
        return JS_ThrowTypeError(ctx, "drawQuadVerticesUV requires 19 arguments");
    if (!s_renderer) return JS_UNDEFINED;

    uint32_t texture, tint;
    float xy[8], uv[8];
    double d, alpha;

    if (JS_ToUint32(ctx, &texture, argv[0])) return JS_EXCEPTION;
    for (int i = 0; i < 8; i++) {
        if (JS_ToFloat64(ctx, &d, argv[1 + i])) return JS_EXCEPTION;
        xy[i] = (float)d;
    }
    for (int i = 0; i < 8; i++) {
        if (JS_ToFloat64(ctx, &d, argv[9 + i])) return JS_EXCEPTION;
        uv[i] = (float)d;
    }
    if (JS_ToUint32(ctx, &tint, argv[17])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &alpha, argv[18])) return JS_EXCEPTION;

    SpriteBatch *batch = renderer_get_batch(s_renderer);
    if (batch) {
        sprite_batch_draw_verts_uv(batch, texture, xy, uv, tint, (float)alpha);
    }
    return JS_UNDEFINED;
}

/* setTextureFilter(texture, linear) */
static JSValue js_renderer_set_texture_filter(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_set_texture_filter");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "setTextureFilter requires texture and linear");
    uint32_t texture;
    if (JS_ToUint32(ctx, &texture, argv[0])) return JS_EXCEPTION;
    int linear = JS_ToBool(ctx, argv[1]);
    if (linear < 0) return JS_EXCEPTION;
    if (s_renderer) renderer_set_texture_filter(texture, linear != 0);
    return JS_UNDEFINED;
}

/* flush() */
static JSValue js_renderer_flush(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_flush");
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (s_renderer) {
        SpriteBatch *batch = renderer_get_batch(s_renderer);
        if (batch) sprite_batch_flush(batch);
    }
    return JS_UNDEFINED;
}

/* setBlendMode(mode) */
static JSValue js_renderer_set_blend(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_set_blend");
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "setBlendMode requires mode");
    if (!s_renderer) return JS_UNDEFINED;

    int mode;
    if (JS_ToInt32(ctx, &mode, argv[0])) return JS_EXCEPTION;
    if (mode < BLEND_MODE_NORMAL || mode > BLEND_MODE_NORMAL_PREMULT)
        compat_report("render", "blend mode", "mode %d requested; only normal, add, multiply and screen exist", mode);

    SpriteBatch *batch = renderer_get_batch(s_renderer);
    if (batch) sprite_batch_set_blend_mode(batch, mode);
    return JS_UNDEFINED;
}

/* getDrawCalls() -> number */
static JSValue js_renderer_draw_calls(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_draw_calls");
    (void)this_val; (void)argc; (void)argv;
    if (!s_renderer) return JS_NewInt32(ctx, 0);
    SpriteBatch *batch = renderer_get_batch(s_renderer);
    return JS_NewInt32(ctx, sprite_batch_get_draw_calls(batch));
}

/* getQuadCount() -> number */
static JSValue js_renderer_quad_count(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_quad_count");
    (void)this_val; (void)argc; (void)argv;
    if (!s_renderer) return JS_NewInt32(ctx, 0);
    SpriteBatch *batch = renderer_get_batch(s_renderer);
    return JS_NewInt32(ctx, sprite_batch_get_quad_count(batch));
}

/* rebindBatch() - restore sprite batch GL state after filter rendering */
static JSValue js_renderer_rebind_batch(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_rebind_batch");
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (s_renderer) renderer_rebind_batch(s_renderer);
    return JS_UNDEFINED;
}

/* setScissor(x, y, w, h) - enable scissor test with the given rect (GL pixel coords) */
static JSValue js_renderer_set_scissor(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_set_scissor");
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "setScissor requires x, y, w, h");
    if (!s_renderer) return JS_UNDEFINED;

    int x, y, w, h;
    if (JS_ToInt32(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[3])) return JS_EXCEPTION;

    /* Flush pending draws before changing scissor state. */
    SpriteBatch *batch = renderer_get_batch(s_renderer);
    if (batch) sprite_batch_flush(batch);

    renderer_set_scissor(s_renderer, x, y, w, h);
    return JS_UNDEFINED;
}

/* clearScissor() - disable scissor test */
static JSValue js_renderer_clear_scissor(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_clear_scissor");
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (!s_renderer) return JS_UNDEFINED;

    SpriteBatch *batch = renderer_get_batch(s_renderer);
    if (batch) sprite_batch_flush(batch);

    renderer_clear_scissor(s_renderer);
    return JS_UNDEFINED;
}

/* setScreenViewport(x, y, w, h) - letterbox viewport for the default framebuffer */
static JSValue js_renderer_set_screen_viewport(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_set_screen_viewport");
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "setScreenViewport requires x, y, w, h");
    if (!s_renderer) return JS_UNDEFINED;

    int x, y, w, h;
    if (JS_ToInt32(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &y, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[3])) return JS_EXCEPTION;
    renderer_set_screen_viewport(s_renderer, x, y, w, h);
    return JS_UNDEFINED;
}

/* readPixels(fbo, width, height) -> ArrayBuffer */
static JSValue js_renderer_read_pixels(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("renderer.renderer_read_pixels");
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "readPixels requires fbo, width, height");
    if (!s_renderer) return JS_NULL;

    uint32_t fbo;
    int w, h;
    if (JS_ToUint32(ctx, &fbo, argv[0])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[2])) return JS_EXCEPTION;

    if (w <= 0 || h <= 0 || w > MAX_PIXEL_DIM || h > MAX_PIXEL_DIM)
        return JS_ThrowRangeError(ctx, "readPixels: dimensions too large %dx%d", w, h);

    uint8_t *pixels = renderer_read_pixels(s_renderer, fbo, w, h);
    if (!pixels) return JS_NULL;

    size_t len = (size_t)w * (size_t)h * 4;
    JSValue ab = JS_NewArrayBufferCopy(ctx, pixels, len);
    free(pixels);
    return ab;
}

/* Registration */

static const JSCFunctionListEntry js_renderer_funcs[] = {
    JS_CFUNC_DEF("init",                2, js_renderer_init),
    JS_CFUNC_DEF("shutdown",            0, js_renderer_shutdown),
    JS_CFUNC_DEF("resize",              2, js_renderer_resize),
    JS_CFUNC_DEF("getSize",             0, js_renderer_get_size),
    JS_CFUNC_DEF("beginFrame",          0, js_renderer_begin_frame),
    JS_CFUNC_DEF("beginFrameTransparent", 0, js_renderer_begin_frame_transparent),
    JS_CFUNC_DEF("endFrame",            0, js_renderer_end_frame),
    JS_CFUNC_DEF("createTexture",       2, js_renderer_create_texture),
    JS_CFUNC_DEF("updateTexture",       4, js_renderer_update_texture),
    JS_CFUNC_DEF("updateTextureFromCanvas", 4, js_renderer_update_texture_from_canvas),
    JS_CFUNC_DEF("deleteTexture",       1, js_renderer_delete_texture),
    JS_CFUNC_DEF("createTextureToken",  1, js_renderer_create_texture_token),
    JS_CFUNC_DEF("releaseTextureToken", 1, js_renderer_release_texture_token),
    JS_CFUNC_DEF("setTextureFilter",    2, js_renderer_set_texture_filter),
    JS_CFUNC_DEF("createRenderTexture", 2, js_renderer_create_rt),
    JS_CFUNC_DEF("deleteRenderTexture", 2, js_renderer_delete_rt),
    JS_CFUNC_DEF("bindRenderTexture",   3, js_renderer_bind_rt),
    JS_CFUNC_DEF("unbindRenderTexture", 0, js_renderer_unbind_rt),
    JS_CFUNC_DEF("drawQuad",           11, js_renderer_draw_quad),
    JS_CFUNC_DEF("drawQuadVertices",   15, js_renderer_draw_quad_verts),
    JS_CFUNC_DEF("drawQuadVerticesUV", 19, js_renderer_draw_quad_verts_uv),
    JS_CFUNC_DEF("flush",               0, js_renderer_flush),
    JS_CFUNC_DEF("setBlendMode",        1, js_renderer_set_blend),
    JS_CFUNC_DEF("getDrawCalls",        0, js_renderer_draw_calls),
    JS_CFUNC_DEF("getQuadCount",        0, js_renderer_quad_count),
    JS_CFUNC_DEF("readPixels",          3, js_renderer_read_pixels),
    JS_CFUNC_DEF("rebindBatch",         0, js_renderer_rebind_batch),
    JS_CFUNC_DEF("setScissor",          4, js_renderer_set_scissor),
    JS_CFUNC_DEF("clearScissor",        0, js_renderer_clear_scissor),
    JS_CFUNC_DEF("setScreenViewport",   4, js_renderer_set_screen_viewport),
};

void bind_renderer_register(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (!texture_token_class_id) {
        static const JSClassDef token_def = { "TextureToken", texture_token_finalizer, NULL, NULL, NULL };
        JS_NewClassID(rt, &texture_token_class_id);
        JS_NewClass(rt, texture_token_class_id, &token_def);
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue obj = JS_NewObject(ctx);

    JS_SetPropertyFunctionList(ctx, obj, js_renderer_funcs,
                               sizeof(js_renderer_funcs) / sizeof(js_renderer_funcs[0]));

    JS_SetPropertyStr(ctx, global, "__native_renderer", obj);
    JS_FreeValue(ctx, global);
}
