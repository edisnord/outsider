/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "bindings/bind_platform.h"
#include "platform/platform.h"

#include <quickjs.h>
#include <SDL3/SDL.h>
#include <stdio.h>

static Platform *s_platform = NULL;

/* Fullscreen */

/* setFullscreen(bool) */
static JSValue js_platform_set_fullscreen(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!s_platform) return JS_UNDEFINED;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setFullscreen requires a boolean");

    int fs = JS_ToBool(ctx, argv[0]);
    if (fs < 0) return JS_EXCEPTION;
    platform_set_fullscreen(s_platform, fs != 0);
    return JS_UNDEFINED;
}

/* isFullscreen() -> bool */
static JSValue js_platform_is_fullscreen(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (!s_platform) return JS_FALSE;
    return JS_NewBool(ctx, platform_is_fullscreen(s_platform));
}

/* Display information */

/* getDisplaySize() -> {width, height} */
static JSValue js_platform_get_display_size(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    int w = 1920, h = 1080;
    if (s_platform) platform_get_display_size(s_platform, &w, &h);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, h));
    return obj;
}

/* getDisplayScale() -> number */
static JSValue js_platform_get_display_scale(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    float scale = 1.0f;
    if (s_platform) scale = platform_get_display_scale(s_platform);
    return JS_NewFloat64(ctx, (double)scale);
}

/* getWindowSize() -> {width, height} */
static JSValue js_platform_get_window_size(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    int w = 0, h = 0;
    if (s_platform) platform_get_window_size(s_platform, &w, &h);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, h));
    return obj;
}

/* setVirtualSize(width, height): the resolution the game is drawn at (PS5: scaled to the screen) */
static JSValue js_platform_set_virtual_size(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!s_platform || argc < 2) return JS_UNDEFINED;
    int w, h;
    if (JS_ToInt32(ctx, &w, argv[0]) || JS_ToInt32(ctx, &h, argv[1])) return JS_EXCEPTION;
    platform_set_virtual_size(s_platform, w, h);
    return JS_UNDEFINED;
}

/* setWindowTitle(title) - window title becomes "<title> [Outsider]", or just
   "Outsider" when the title is empty. Called from the document.title setter. */
static JSValue js_platform_set_window_title(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!s_platform || argc < 1) return JS_UNDEFINED;
    const char *title = JS_ToCString(ctx, argv[0]);
    if (!title) return JS_EXCEPTION;
    char full[512];
    if (title[0] != '\0') {
        snprintf(full, sizeof(full), "%s [%s]", title, RMMZ_APP_NAME);
    } else {
        snprintf(full, sizeof(full), "%s", RMMZ_APP_NAME);
    }
    JS_FreeCString(ctx, title);
    platform_set_window_title(s_platform, full);
    return JS_UNDEFINED;
}

/* quit() - push an SDL_EVENT_QUIT event to terminate the game loop. */
static JSValue js_platform_quit(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    SDL_Event event;
    SDL_memset(&event, 0, sizeof(event));
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
    return JS_UNDEFINED;
}

/* Registration */

static const JSCFunctionListEntry js_platform_funcs[] = {
    JS_CFUNC_DEF("setFullscreen",    1, js_platform_set_fullscreen),
    JS_CFUNC_DEF("isFullscreen",     0, js_platform_is_fullscreen),
    JS_CFUNC_DEF("getDisplaySize",   0, js_platform_get_display_size),
    JS_CFUNC_DEF("getDisplayScale",  0, js_platform_get_display_scale),
    JS_CFUNC_DEF("getWindowSize",    0, js_platform_get_window_size),
    JS_CFUNC_DEF("setVirtualSize",   2, js_platform_set_virtual_size),
    JS_CFUNC_DEF("setWindowTitle",   1, js_platform_set_window_title),
    JS_CFUNC_DEF("quit",             0, js_platform_quit),
};

void bind_platform_register(JSContext *ctx, Platform *platform)
{
    s_platform = platform;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue obj = JS_NewObject(ctx);

    JS_SetPropertyFunctionList(ctx, obj, js_platform_funcs,
                               sizeof(js_platform_funcs) / sizeof(js_platform_funcs[0]));

    JS_SetPropertyStr(ctx, global, "__native_platform", obj);
    JS_FreeValue(ctx, global);
}
