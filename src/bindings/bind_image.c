/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "engine/js_engine.h"
#include "bindings/bind_image.h"
#include "rendering/image_loader.h"
#include "rendering/apng.h"
#include "io/file_io.h"

#include <quickjs.h>
#include <stdlib.h>
#include <string.h>

#define PATH_BUF_SIZE 4096

/* Resolve a JS string argument to an absolute path. Returns the JS C-string
   (caller frees) or NULL with a pending exception. */
static const char *resolve_path_arg(JSContext *ctx, JSValueConst arg,
                                    char *out, size_t out_size)
{
    const char *raw = JS_ToCString(ctx, arg);
    if (!raw) return NULL;

    if (!file_io_resolve_path(raw, out, out_size)) {
        JS_ThrowTypeError(ctx, "path resolution failed: %s", raw);
        JS_FreeCString(ctx, raw);
        return NULL;
    }
    return raw;
}

/* Build a {width, height, handle, glTexture} JS object from an ImageHandle. */
static JSValue make_image_result(JSContext *ctx, ImageHandle handle)
{
    ImageInfo info;
    if (!image_get_info(handle, &info)) {
        return JS_ThrowInternalError(ctx, "failed to query image info");
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width",     JS_NewInt32(ctx, info.width));
    JS_SetPropertyStr(ctx, obj, "height",    JS_NewInt32(ctx, info.height));
    JS_SetPropertyStr(ctx, obj, "handle",    JS_NewUint32(ctx, handle));
    JS_SetPropertyStr(ctx, obj, "glTexture", JS_NewUint32(ctx, info.gl_texture));
    return obj;
}

/* loadImage(path) -> {width, height, handle, glTexture} | null */
static JSValue js_load_image(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("image.load_image");
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "loadImage requires a path argument");

    char resolved[PATH_BUF_SIZE];
    const char *raw = resolve_path_arg(ctx, argv[0], resolved, sizeof(resolved));
    if (!raw) {
        /* A path outside the game root is a load failure, not a JS error: the
           image shim expects null so it can fire onerror. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_NULL;
    }
    JS_FreeCString(ctx, raw);

    ImageHandle handle = image_load(resolved);
    if (handle == IMAGE_HANDLE_INVALID) {
        return JS_NULL;
    }

    return make_image_result(ctx, handle);
}

/* loadImageFromMemory(key, arrayBuffer) -> {width, height, handle, glTexture} | null */
static JSValue js_load_image_from_memory(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("image.load_image_from_memory");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "loadImageFromMemory requires key and ArrayBuffer");

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    size_t size = 0;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &size, argv[1]);
    if (!buf) {
        JS_FreeCString(ctx, key);
        return JS_ThrowTypeError(ctx, "second argument must be an ArrayBuffer");
    }

    ImageHandle handle = image_load_from_memory(key, buf, size);
    JS_FreeCString(ctx, key);

    if (handle == IMAGE_HANDLE_INVALID) {
        return JS_NULL;
    }

    return make_image_result(ctx, handle);
}

/* getImageInfo(handle) -> {width, height, glTexture} | null */
static JSValue js_get_image_info(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("image.get_image_info");
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getImageInfo requires a handle argument");

    uint32_t handle;
    if (JS_ToUint32(ctx, &handle, argv[0]))
        return JS_EXCEPTION;

    ImageInfo info;
    if (!image_get_info(handle, &info)) {
        return JS_NULL;
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width",     JS_NewInt32(ctx, info.width));
    JS_SetPropertyStr(ctx, obj, "height",    JS_NewInt32(ctx, info.height));
    JS_SetPropertyStr(ctx, obj, "glTexture", JS_NewUint32(ctx, info.gl_texture));
    return obj;
}

/* getImagePixels(handle) -> ArrayBuffer (RGBA) | null */
static JSValue js_get_image_pixels(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("image.get_image_pixels");
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getImagePixels requires a handle argument");

    uint32_t handle;
    if (JS_ToUint32(ctx, &handle, argv[0]))
        return JS_EXCEPTION;

    ImageInfo info;
    if (!image_get_info(handle, &info)) {
        return JS_NULL;
    }

    const uint8_t *pixels = image_get_pixels(handle);
    if (!pixels) {
        return JS_NULL;
    }

    size_t byte_size = (size_t)info.width * (size_t)info.height * 4;
    return JS_NewArrayBufferCopy(ctx, pixels, byte_size);
}

/* Image tokens, like the canvas and texture tokens: createImageToken(handle) returns an object that frees the image when the
   engine frees it; releaseImageToken(token) frees it right away and disarms the token. */
static JSClassID image_token_class_id;

static void image_token_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    uint32_t handle = (uint32_t)(uintptr_t)JS_GetOpaque(val, image_token_class_id);
    if (handle) image_free(handle);
}

static JSValue js_create_image_token(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    uint32_t handle = 0;
    if (argc < 1 || JS_ToUint32(ctx, &handle, argv[0])) return JS_EXCEPTION;
    JSValue obj = JS_NewObjectClass(ctx, (int)image_token_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, (void *)(uintptr_t)handle);
    return obj;
}

static JSValue js_release_image_token(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)ctx;
    if (argc < 1) return JS_UNDEFINED;
    uint32_t handle = (uint32_t)(uintptr_t)JS_GetOpaque(argv[0], image_token_class_id);
    if (handle) {
        JS_SetOpaque(argv[0], NULL);
        image_free(handle);
    }
    return JS_UNDEFINED;
}

/* freeImage(handle) */
static JSValue js_free_image(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("image.free_image");
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "freeImage requires a handle argument");

    uint32_t handle;
    if (JS_ToUint32(ctx, &handle, argv[0]))
        return JS_EXCEPTION;

    image_free(handle);
    return JS_UNDEFINED;
}

/* cacheCount() -> number */
static JSValue js_cache_count(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("image.cache_count");
    (void)this_val; (void)argc; (void)argv;
    return JS_NewUint32(ctx, (uint32_t)image_cache_count());
}

/* clearCache() */
static JSValue js_clear_cache(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("image.clear_cache");
    (void)this_val; (void)argc; (void)argv;
    image_cache_clear();
    return JS_UNDEFINED;
}

/* decodeApng(ArrayBuffer) -> {width, height, numPlays,
                               frames: [{delay, glTexture}]} | null
   Frames are composed RGBA textures owned by the caller (delete them through
   __native_renderer.deleteTexture). null when the buffer is not an APNG or
   fails to decode, so the caller can fall back to its own decoder. */
static JSValue js_decode_apng(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("image.decode_apng");
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "decodeApng requires an ArrayBuffer");

    size_t size = 0;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &size, argv[0]);
    if (!buf) {
        /* Accept a typed array view as well. */
        size_t byte_offset = 0, byte_length = 0, bytes_per_element = 0;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_offset, &byte_length, &bytes_per_element);
        if (JS_IsException(ab)) return JS_ThrowTypeError(ctx, "decodeApng requires an ArrayBuffer");
        uint8_t *base = JS_GetArrayBuffer(ctx, &size, ab);
        JS_FreeValue(ctx, ab);
        if (!base) return JS_ThrowTypeError(ctx, "decodeApng requires an ArrayBuffer");
        buf = base + byte_offset;
        size = byte_length;
    }

    ApngImage *img = apng_decode(buf, size);
    if (!img) return JS_NULL;

    JSValue frames = JS_NewArray(ctx);
    for (int i = 0; i < img->num_frames; i++) {
        JSValue f = JS_NewObject(ctx);
        uint32_t tex = image_upload_rgba_texture(img->frames[i].rgba, img->width, img->height);
        JS_SetPropertyStr(ctx, f, "delay",     JS_NewInt32(ctx, img->frames[i].delay_ms));
        JS_SetPropertyStr(ctx, f, "glTexture", JS_NewUint32(ctx, tex));
        JS_SetPropertyUint32(ctx, frames, (uint32_t)i, f);
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width",    JS_NewInt32(ctx, img->width));
    JS_SetPropertyStr(ctx, obj, "height",   JS_NewInt32(ctx, img->height));
    JS_SetPropertyStr(ctx, obj, "numPlays", JS_NewInt32(ctx, img->num_plays));
    JS_SetPropertyStr(ctx, obj, "frames",   frames);
    apng_free(img);
    return obj;
}

/* Registration */

static const JSCFunctionListEntry js_image_funcs[] = {
    JS_CFUNC_DEF("decodeApng",          1, js_decode_apng),
    JS_CFUNC_DEF("loadImage",           1, js_load_image),
    JS_CFUNC_DEF("loadImageFromMemory", 2, js_load_image_from_memory),
    JS_CFUNC_DEF("getImageInfo",        1, js_get_image_info),
    JS_CFUNC_DEF("getImagePixels",      1, js_get_image_pixels),
    JS_CFUNC_DEF("freeImage",           1, js_free_image),
    JS_CFUNC_DEF("createImageToken",    1, js_create_image_token),
    JS_CFUNC_DEF("releaseImageToken",   1, js_release_image_token),
    JS_CFUNC_DEF("cacheCount",          0, js_cache_count),
    JS_CFUNC_DEF("clearCache",          0, js_clear_cache),
};

void bind_image_register(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (!image_token_class_id) {
        static const JSClassDef token_def = { "ImageToken", image_token_finalizer, NULL, NULL, NULL };
        JS_NewClassID(rt, &image_token_class_id);
        JS_NewClass(rt, image_token_class_id, &token_def);
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue img_obj = JS_NewObject(ctx);

    JS_SetPropertyFunctionList(ctx, img_obj, js_image_funcs,
                               sizeof(js_image_funcs) / sizeof(js_image_funcs[0]));

    JS_SetPropertyStr(ctx, global, "__native_image", img_obj);
    JS_FreeValue(ctx, global);
}
