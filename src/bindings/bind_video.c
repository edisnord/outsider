/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "bindings/bind_video.h"
#include "video/video_player.h"

#include <quickjs.h>

static bool handle_arg(JSContext *ctx, JSValueConst arg, VideoHandle *out)
{
    int32_t h = 0;
    if (JS_ToInt32(ctx, &h, arg)) return false;
    *out = (VideoHandle)h;
    return true;
}

/* updateTexture(handle, texture) -> true when the texture received a new picture */
static JSValue js_video_update_texture(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val;
    VideoHandle h;
    uint32_t tex = 0;
    if (argc < 2 || !handle_arg(ctx, argv[0], &h) || JS_ToUint32(ctx, &tex, argv[1])) return JS_EXCEPTION;
    return JS_NewBool(ctx, video_update_texture(h, tex));
}

/* open(path) -> handle (0 on failure) */
static JSValue js_video_open(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "open requires a path");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    VideoHandle h = video_open(path);
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, h);
}

static JSValue js_video_close(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    VideoHandle h;
    if (argc < 1 || !handle_arg(ctx, argv[0], &h)) return JS_EXCEPTION;
    video_close(h);
    return JS_UNDEFINED;
}

static JSValue js_video_play(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    VideoHandle h;
    if (argc < 1 || !handle_arg(ctx, argv[0], &h)) return JS_EXCEPTION;
    video_play(h);
    return JS_UNDEFINED;
}

static JSValue js_video_pause(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    VideoHandle h;
    if (argc < 1 || !handle_arg(ctx, argv[0], &h)) return JS_EXCEPTION;
    video_pause(h);
    return JS_UNDEFINED;
}

/* setVolume(handle, 0..1) */
static JSValue js_video_set_volume(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    VideoHandle h;
    double vol = 1.0;
    if (argc < 2 || !handle_arg(ctx, argv[0], &h) || JS_ToFloat64(ctx, &vol, argv[1])) {
        return JS_EXCEPTION;
    }
    video_set_volume(h, (float)vol);
    return JS_UNDEFINED;
}

static JSValue js_video_set_loop(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    VideoHandle h;
    if (argc < 2 || !handle_arg(ctx, argv[0], &h)) return JS_EXCEPTION;
    video_set_loop(h, JS_ToBool(ctx, argv[1]) > 0);
    return JS_UNDEFINED;
}

static JSValue js_video_set_visible(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    VideoHandle h;
    if (argc < 2 || !handle_arg(ctx, argv[0], &h)) return JS_EXCEPTION;
    video_set_visible(h, JS_ToBool(ctx, argv[1]) > 0);
    return JS_UNDEFINED;
}

/* getState(handle) -> {playing, paused, ended, time, duration, width, height} | null */
static JSValue js_video_get_state(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    VideoHandle h;
    if (argc < 1 || !handle_arg(ctx, argv[0], &h)) return JS_EXCEPTION;
    VideoState st;
    if (!video_get_state(h, &st)) return JS_NULL;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "playing",  JS_NewBool(ctx, st.playing));
    JS_SetPropertyStr(ctx, obj, "paused",   JS_NewBool(ctx, st.paused));
    JS_SetPropertyStr(ctx, obj, "ended",    JS_NewBool(ctx, st.ended));
    JS_SetPropertyStr(ctx, obj, "time",     JS_NewFloat64(ctx, st.time));
    JS_SetPropertyStr(ctx, obj, "duration", JS_NewFloat64(ctx, st.duration));
    JS_SetPropertyStr(ctx, obj, "width",    JS_NewInt32(ctx, st.width));
    JS_SetPropertyStr(ctx, obj, "height",   JS_NewInt32(ctx, st.height));
    return obj;
}

static const JSCFunctionListEntry js_video_funcs[] = {
    JS_CFUNC_DEF("open",       1, js_video_open),
    JS_CFUNC_DEF("close",      1, js_video_close),
    JS_CFUNC_DEF("play",       1, js_video_play),
    JS_CFUNC_DEF("updateTexture", 2, js_video_update_texture),
    JS_CFUNC_DEF("pause",      1, js_video_pause),
    JS_CFUNC_DEF("setVolume",  2, js_video_set_volume),
    JS_CFUNC_DEF("setLoop",    2, js_video_set_loop),
    JS_CFUNC_DEF("setVisible", 2, js_video_set_visible),
    JS_CFUNC_DEF("getState",   1, js_video_get_state),
};

void bind_video_register(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, obj, js_video_funcs,
                               sizeof(js_video_funcs) / sizeof(js_video_funcs[0]));
    JS_SetPropertyStr(ctx, global, "__native_video", obj);
    JS_FreeValue(ctx, global);
}
