/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "engine/js_engine.h"
#include "bindings/bind_filters.h"
#include "rendering/filters.h"
#include "engine/error_handler.h"

#include <quickjs.h>
#include <stdlib.h>
#include <string.h>

/* Built-in shader IDs, compiled during init(). */

static uint32_t s_color_matrix_shader = 0;
static uint32_t s_blur_shader = 0;
static uint32_t s_alpha_shader = 0;
static uint32_t s_color_filter_shader = 0;
static uint32_t s_mask_shader = 0;

/* Lifecycle */

/* init() -> bool */
static JSValue js_filters_init(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("filters.filters_init");
    (void)this_val; (void)argc; (void)argv;
    filters_init();

    /* Compile built-in filter shaders. */
    if (!s_color_matrix_shader)
        s_color_matrix_shader = filter_compile_shader(NULL, filter_color_matrix_frag_src());
    if (!s_blur_shader)
        s_blur_shader = filter_compile_shader(NULL, filter_blur_frag_src());
    if (!s_alpha_shader)
        s_alpha_shader = filter_compile_shader(NULL, filter_alpha_frag_src());
    if (!s_color_filter_shader)
        s_color_filter_shader = filter_compile_shader(NULL, filter_color_filter_frag_src());
    if (!s_mask_shader)
        s_mask_shader = filter_compile_shader(NULL, filter_mask_frag_src());

    if (!s_color_matrix_shader || !s_blur_shader ||
        !s_alpha_shader || !s_color_filter_shader) {
        error_handler_log(LOG_ERROR, "Built-in shader compilation failed:"
                          " colorMatrix=%u blur=%u alpha=%u colorFilter=%u",
                          s_color_matrix_shader, s_blur_shader,
                          s_alpha_shader, s_color_filter_shader);
        return JS_FALSE;
    }

    /* Expose shader IDs as properties on __native_filters. */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue obj = JS_GetPropertyStr(ctx, global, "__native_filters");
    if (!JS_IsUndefined(obj)) {
        JS_SetPropertyStr(ctx, obj, "colorMatrixShader",
                          JS_NewUint32(ctx, s_color_matrix_shader));
        JS_SetPropertyStr(ctx, obj, "blurShader",
                          JS_NewUint32(ctx, s_blur_shader));
        JS_SetPropertyStr(ctx, obj, "alphaShader",
                          JS_NewUint32(ctx, s_alpha_shader));
        JS_SetPropertyStr(ctx, obj, "colorFilterShader",
                          JS_NewUint32(ctx, s_color_filter_shader));
        JS_SetPropertyStr(ctx, obj, "maskShader",
                          JS_NewUint32(ctx, s_mask_shader));
    }
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, global);

    return JS_TRUE;
}

/* shutdown() */
static JSValue js_filters_shutdown(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("filters.filters_shutdown");
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (s_color_matrix_shader) { filter_delete_shader(s_color_matrix_shader); s_color_matrix_shader = 0; }
    if (s_blur_shader) { filter_delete_shader(s_blur_shader); s_blur_shader = 0; }
    if (s_alpha_shader) { filter_delete_shader(s_alpha_shader); s_alpha_shader = 0; }
    if (s_color_filter_shader) { filter_delete_shader(s_color_filter_shader); s_color_filter_shader = 0; }
    if (s_mask_shader) { filter_delete_shader(s_mask_shader); s_mask_shader = 0; }
    filters_shutdown();
    return JS_UNDEFINED;
}

/* Shader compilation */

/* compileShader(vertSrc_or_null, fragSrc) -> shaderId */
static JSValue js_filters_compile(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("filters.filters_compile");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "compileShader requires vertSrc and fragSrc");

    const char *vert_src = NULL;
    const char *frag_src = NULL;

    if (!JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0]))
        vert_src = JS_ToCString(ctx, argv[0]);
    frag_src = JS_ToCString(ctx, argv[1]);
    if (!frag_src) {
        if (vert_src) JS_FreeCString(ctx, vert_src);
        return JS_ThrowTypeError(ctx, "compileShader: fragSrc required");
    }

    uint32_t id = filter_compile_shader(vert_src, frag_src);

    if (vert_src) JS_FreeCString(ctx, vert_src);
    JS_FreeCString(ctx, frag_src);
    return JS_NewUint32(ctx, id);
}

/* deleteShader(shaderId) */
static JSValue js_filters_delete(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("filters.filters_delete");
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    uint32_t id;
    if (JS_ToUint32(ctx, &id, argv[0])) return JS_EXCEPTION;
    filter_delete_shader(id);
    return JS_UNDEFINED;
}

/* Filter rendering */

/* beginFilter(shaderId, inputTexture, width, height) */
static JSValue js_filters_begin(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("filters.filters_begin");
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "beginFilter requires shaderId, inputTexture, w, h");

    uint32_t shader, texture;
    int w, h;
    if (JS_ToUint32(ctx, &shader, argv[0])) return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &texture, argv[1])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &w, argv[2])) return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &h, argv[3])) return JS_EXCEPTION;

    filter_begin(shader, texture, w, h);
    return JS_UNDEFINED;
}

/* setUniform1f(shader, name, value) */
static JSValue js_filters_uniform_1f(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("filters.filters_uniform_1f");
    (void)this_val;
    if (argc < 3) return JS_UNDEFINED;

    uint32_t shader;
    if (JS_ToUint32(ctx, &shader, argv[0])) return JS_EXCEPTION;
    const char *name = JS_ToCString(ctx, argv[1]);
    if (!name) return JS_EXCEPTION;
    double val;
    if (JS_ToFloat64(ctx, &val, argv[2])) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }

    filter_set_uniform_1f(shader, name, (float)val);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

/* setUniformTexture(shader, name, texture, unit) */
static JSValue js_filters_uniform_texture(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("filters.filters_uniform_texture");
    (void)this_val;
    if (argc < 4) return JS_UNDEFINED;
    uint32_t shader, texture;
    int unit;
    if (JS_ToUint32(ctx, &shader, argv[0])) return JS_EXCEPTION;
    const char *name = JS_ToCString(ctx, argv[1]);
    if (!name) return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &texture, argv[2]) || JS_ToInt32(ctx, &unit, argv[3])) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    filter_set_uniform_texture(shader, name, texture, unit);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

/* setUniform2f(shader, name, x, y) */
static JSValue js_filters_uniform_2f(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("filters.filters_uniform_2f");
    (void)this_val;
    if (argc < 4) return JS_UNDEFINED;

    uint32_t shader;
    if (JS_ToUint32(ctx, &shader, argv[0])) return JS_EXCEPTION;
    const char *name = JS_ToCString(ctx, argv[1]);
    if (!name) return JS_EXCEPTION;
    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[2])) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    if (JS_ToFloat64(ctx, &y, argv[3])) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }

    filter_set_uniform_2f(shader, name, (float)x, (float)y);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

/* setUniform4f(shader, name, x, y, z, w) */
static JSValue js_filters_uniform_4f(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("filters.filters_uniform_4f");
    (void)this_val;
    if (argc < 6) return JS_UNDEFINED;

    uint32_t shader;
    if (JS_ToUint32(ctx, &shader, argv[0])) return JS_EXCEPTION;
    const char *name = JS_ToCString(ctx, argv[1]);
    if (!name) return JS_EXCEPTION;
    double x, y, z, w;
    if (JS_ToFloat64(ctx, &x, argv[2])) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    if (JS_ToFloat64(ctx, &y, argv[3])) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    if (JS_ToFloat64(ctx, &z, argv[4])) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    if (JS_ToFloat64(ctx, &w, argv[5])) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }

    filter_set_uniform_4f(shader, name, (float)x, (float)y, (float)z, (float)w);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

/* setUniformMat4(shader, name, Float32Array_16 | ArrayBuffer | Array_16) */
static JSValue js_filters_uniform_mat4(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("filters.filters_uniform_mat4");
    (void)this_val;
    if (argc < 3) return JS_UNDEFINED;

    uint32_t shader;
    if (JS_ToUint32(ctx, &shader, argv[0])) return JS_EXCEPTION;
    const char *name = JS_ToCString(ctx, argv[1]);
    if (!name) return JS_EXCEPTION;

    /* Accept an ArrayBuffer, a typed-array view, or a plain 16-element Array
       (the generic Filter uniform path in pixi_shim passes plain arrays). */
    size_t buf_size = 0;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &buf_size, argv[2]);
    JSValue ab = JS_UNDEFINED;
    float arr[16];
    if (!buf) {
        /* Clear the pending TypeError before trying the next representation. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        size_t offset, view_size, elem_size;
        ab = JS_GetTypedArrayBuffer(ctx, argv[2], &offset, &view_size, &elem_size);
        if (!JS_IsException(ab)) {
            size_t ab_size;
            buf = JS_GetArrayBuffer(ctx, &ab_size, ab);
            if (buf) buf += offset;
            buf_size = view_size; /* Use view size, not total ArrayBuffer size. */
        } else {
            /* Not a typed array either; clear that exception and try a plain Array. */
            JS_FreeValue(ctx, JS_GetException(ctx));
            ab = JS_UNDEFINED;
            if (JS_IsArray(argv[2])) {
                for (int i = 0; i < 16; i++) {
                    JSValue elem = JS_GetPropertyUint32(ctx, argv[2], (uint32_t)i);
                    double d = 0;
                    int rc = JS_ToFloat64(ctx, &d, elem);
                    JS_FreeValue(ctx, elem);
                    if (rc) {
                        JS_FreeCString(ctx, name);
                        return JS_EXCEPTION;
                    }
                    arr[i] = (float)d;
                }
                buf = (uint8_t *)arr;
                buf_size = sizeof(arr);
            }
        }
    }

    if (buf && buf_size >= 16 * sizeof(float)) {
        filter_set_uniform_mat4(shader, name, (const float *)buf);
    }

    /* Release the typed-array buffer reference only after the uniform is uploaded. */
    if (!JS_IsUndefined(ab)) JS_FreeValue(ctx, ab);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

/* drawQuad() */
static JSValue js_filters_draw_quad(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("filters.filters_draw_quad");
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    filter_draw_quad();
    return JS_UNDEFINED;
}

/* endFilter() */
static JSValue js_filters_end(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("filters.filters_end");
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    filter_end();
    return JS_UNDEFINED;
}

/* Registration */

static const JSCFunctionListEntry js_filters_funcs[] = {
    JS_CFUNC_DEF("init",            0, js_filters_init),
    JS_CFUNC_DEF("shutdown",        0, js_filters_shutdown),
    JS_CFUNC_DEF("compileShader",   2, js_filters_compile),
    JS_CFUNC_DEF("deleteShader",    1, js_filters_delete),
    JS_CFUNC_DEF("beginFilter",     4, js_filters_begin),
    JS_CFUNC_DEF("setUniform1f",    3, js_filters_uniform_1f),
    JS_CFUNC_DEF("setUniform2f",    4, js_filters_uniform_2f),
    JS_CFUNC_DEF("setUniformTexture", 4, js_filters_uniform_texture),
    JS_CFUNC_DEF("setUniform4f",    6, js_filters_uniform_4f),
    JS_CFUNC_DEF("setUniformMat4",  3, js_filters_uniform_mat4),
    JS_CFUNC_DEF("drawQuad",        0, js_filters_draw_quad),
    JS_CFUNC_DEF("endFilter",       0, js_filters_end),
};

void bind_filters_register(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue obj = JS_NewObject(ctx);

    JS_SetPropertyFunctionList(ctx, obj, js_filters_funcs,
                               sizeof(js_filters_funcs) / sizeof(js_filters_funcs[0]));

    /* Shader ID properties start at 0 and are filled in by init(). */
    JS_SetPropertyStr(ctx, obj, "colorMatrixShader", JS_NewUint32(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "maskShader", JS_NewUint32(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "blurShader", JS_NewUint32(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "alphaShader", JS_NewUint32(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "colorFilterShader", JS_NewUint32(ctx, 0));

    JS_SetPropertyStr(ctx, global, "__native_filters", obj);
    JS_FreeValue(ctx, global);
}

void bind_filters_shutdown(void)
{
    if (s_color_matrix_shader) { filter_delete_shader(s_color_matrix_shader); s_color_matrix_shader = 0; }
    if (s_blur_shader) { filter_delete_shader(s_blur_shader); s_blur_shader = 0; }
    if (s_alpha_shader) { filter_delete_shader(s_alpha_shader); s_alpha_shader = 0; }
    if (s_color_filter_shader) { filter_delete_shader(s_color_filter_shader); s_color_filter_shader = 0; }
}
