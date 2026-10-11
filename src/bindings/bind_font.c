/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "engine/js_engine.h"
#include "bindings/bind_font.h"
#include "rendering/font_manager.h"
#include "rendering/canvas2d.h"
#include "io/file_io.h"

#include <quickjs.h>
#include <stdlib.h>
#include <string.h>

#define PATH_BUF_SIZE 4096

/* Font loading */

/* loadFont(family, arrayBuffer) -> bool */
static JSValue js_font_load(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("font.font_load");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "loadFont requires family and ArrayBuffer");

    const char *family = JS_ToCString(ctx, argv[0]);
    if (!family) return JS_EXCEPTION;

    size_t buf_size = 0;
    uint8_t *data = JS_GetArrayBuffer(ctx, &buf_size, argv[1]);
    if (!data) {
        JS_FreeCString(ctx, family);
        return JS_ThrowTypeError(ctx, "loadFont: second arg must be ArrayBuffer");
    }

    /* Load into both font_manager and canvas2d font caches. */
    bool ok = font_manager_load(family, data, buf_size);
    if (ok) {
        canvas2d_load_font(family, data, buf_size);
    }

    JS_FreeCString(ctx, family);
    return JS_NewBool(ctx, ok);
}

/* loadFontFile(family, path) -> bool */
static JSValue js_font_load_file(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("font.font_load_file");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "loadFontFile requires family and path");

    const char *family = JS_ToCString(ctx, argv[0]);
    if (!family) return JS_EXCEPTION;

    const char *path = JS_ToCString(ctx, argv[1]);
    if (!path) {
        JS_FreeCString(ctx, family);
        return JS_EXCEPTION;
    }

    char resolved[PATH_BUF_SIZE];
    if (!file_io_resolve_path(path, resolved, sizeof(resolved))) {
        JS_FreeCString(ctx, path);
        JS_FreeCString(ctx, family);
        return JS_NewBool(ctx, false);
    }
    JS_FreeCString(ctx, path);

    size_t file_size = 0;
    uint8_t *data = file_io_read_binary(resolved, &file_size);
    if (!data) {
        JS_FreeCString(ctx, family);
        return JS_NewBool(ctx, false);
    }

    /* Load into both font_manager and canvas2d font caches. */
    bool ok = font_manager_load(family, data, file_size);
    if (ok) {
        canvas2d_load_font(family, data, file_size);
    }

    free(data);
    JS_FreeCString(ctx, family);
    return JS_NewBool(ctx, ok);
}

/* Font queries */

/* hasFont(family) -> bool */
static JSValue js_font_has(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("font.font_has");
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "hasFont requires family name");

    const char *family = JS_ToCString(ctx, argv[0]);
    if (!family) return JS_EXCEPTION;

    bool has = font_manager_has_font(family);
    JS_FreeCString(ctx, family);
    return JS_NewBool(ctx, has);
}

/* getMetrics(family, pixelSize) -> {ascent, descent, lineGap, lineHeight} | null */
static JSValue js_font_get_metrics(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("font.font_get_metrics");
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "getMetrics requires family and pixelSize");

    const char *family = JS_ToCString(ctx, argv[0]);
    if (!family) return JS_EXCEPTION;

    double size;
    if (JS_ToFloat64(ctx, &size, argv[1])) {
        JS_FreeCString(ctx, family);
        return JS_EXCEPTION;
    }

    FontMetrics m;
    if (!font_manager_get_metrics(family, (float)size, &m)) {
        JS_FreeCString(ctx, family);
        return JS_NULL;
    }

    JS_FreeCString(ctx, family);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ascent", JS_NewFloat64(ctx, m.ascent));
    JS_SetPropertyStr(ctx, obj, "descent", JS_NewFloat64(ctx, m.descent));
    JS_SetPropertyStr(ctx, obj, "lineGap", JS_NewFloat64(ctx, m.line_gap));
    JS_SetPropertyStr(ctx, obj, "lineHeight", JS_NewFloat64(ctx, m.line_height));
    return obj;
}

/* measureText(family, pixelSize, text) -> number (width) */
static JSValue js_font_measure_text(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("font.font_measure_text");
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "measureText requires family, pixelSize, text");

    const char *family = JS_ToCString(ctx, argv[0]);
    if (!family) return JS_EXCEPTION;

    double size;
    if (JS_ToFloat64(ctx, &size, argv[1])) {
        JS_FreeCString(ctx, family);
        return JS_EXCEPTION;
    }

    const char *text = JS_ToCString(ctx, argv[2]);
    if (!text) {
        JS_FreeCString(ctx, family);
        return JS_EXCEPTION;
    }

    float w = font_manager_measure_text(family, (float)size, text);
    JS_FreeCString(ctx, text);
    JS_FreeCString(ctx, family);
    return JS_NewFloat64(ctx, (double)w);
}

/* fontCount() -> number */
static JSValue js_font_count(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("font.font_count");
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, font_manager_font_count());
}

/* fontNames() -> string[] */
static JSValue js_font_names(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("font.font_names");
    (void)this_val;
    (void)argc;
    (void)argv;

    int count = font_manager_font_count();
    JSValue arr = JS_NewArray(ctx);
    int idx = 0;
    for (int i = 0; i < count; i++) {
        const char *name = font_manager_font_name(i);
        if (name) {
            JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, name));
        }
    }
    return arr;
}

/* Registration */

static const JSCFunctionListEntry js_font_funcs[] = {
    JS_CFUNC_DEF("loadFont",      2, js_font_load),
    JS_CFUNC_DEF("loadFontFile",  2, js_font_load_file),
    JS_CFUNC_DEF("hasFont",       1, js_font_has),
    JS_CFUNC_DEF("getMetrics",    2, js_font_get_metrics),
    JS_CFUNC_DEF("measureText",   3, js_font_measure_text),
    JS_CFUNC_DEF("fontCount",     0, js_font_count),
    JS_CFUNC_DEF("fontNames",     0, js_font_names),
};

void bind_font_register(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue obj = JS_NewObject(ctx);

    JS_SetPropertyFunctionList(ctx, obj, js_font_funcs,
                               sizeof(js_font_funcs) / sizeof(js_font_funcs[0]));

    JS_SetPropertyStr(ctx, global, "__native_font", obj);
    JS_FreeValue(ctx, global);
}
