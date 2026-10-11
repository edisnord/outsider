/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "engine/error_handler.h"
#include "engine/js_engine.h"

#include <quickjs.h>
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *s_log_file = NULL;
static LogLevel s_min_level = LOG_DEBUG;
static LogCallback s_callback = NULL;
static void *s_callback_userdata = NULL;

static const char *s_level_names[] = {
    "DEBUG", "INFO", "WARN", "ERROR"
};

void error_handler_init(const char *log_file_path, LogLevel min_level)
{
    s_min_level = min_level;

    if (log_file_path) {
        s_log_file = fopen(log_file_path, "a");
        if (!s_log_file) {
            fprintf(stderr, "[ERROR] Failed to open log file: %s\n", log_file_path);
        }
    }
}

void error_handler_shutdown(void)
{
    if (s_log_file) {
        fclose(s_log_file);
        s_log_file = NULL;
    }
    s_callback = NULL;
    s_callback_userdata = NULL;
}

const char *error_handler_level_name(LogLevel level)
{
    if (level < LOG_DEBUG || level > LOG_ERROR) return "UNKNOWN";
    return s_level_names[level];
}

void error_handler_set_level(LogLevel min_level)
{
    s_min_level = min_level;
}

LogLevel error_handler_get_level(void)
{
    return s_min_level;
}

void error_handler_set_callback(LogCallback cb, void *userdata)
{
    s_callback = cb;
    s_callback_userdata = userdata;
}

void error_handler_log(LogLevel level, const char *fmt, ...)
{
    if (level < s_min_level) return;

    char msg[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    struct timespec t0, t1, t2, t3;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    time_t now = time(NULL);
    struct tm tm_buf;
    /* localtime_r() cost 100 ms or more per call on the console (every log line stalled the game for that long;
       measured with the stall timing below). The local time offset is looked up once, and later lines are
       formatted from UTC plus that offset. */
#ifdef _WIN32
    struct tm *tm_info = (localtime_s(&tm_buf, &now) == 0) ? &tm_buf : NULL;
#else
    static int have_offset;
    static long utc_offset;
    if (!have_offset) {
        /* the local offset from UTC: UTC's fields read back as a local time are off by exactly that */
        struct tm gm;
        if (gmtime_r(&now, &gm)) {
            gm.tm_isdst = -1;
            time_t as_local = mktime(&gm);
            if (as_local != (time_t)-1) utc_offset = (long)(now - as_local);
        }
        have_offset = 1;
    }
    time_t shifted = now + (time_t)utc_offset;
    struct tm *tm_info = gmtime_r(&shifted, &tm_buf);
#endif
    char timestamp[32];
    if (tm_info) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        snprintf(timestamp, sizeof(timestamp), "<unknown time>");
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    const char *level_name = error_handler_level_name(level);

    FILE *out = (level >= LOG_WARN) ? stderr : stdout;
    fprintf(out, "[%s] [%s] %s\n", timestamp, level_name, msg);
    clock_gettime(CLOCK_MONOTONIC, &t2);

    if (s_log_file) {
        fprintf(s_log_file, "[%s] [%s] %s\n", timestamp, level_name, msg);
        fflush(s_log_file);
    }
    clock_gettime(CLOCK_MONOTONIC, &t3);

    /* Says where the time went when a message takes long (first few only). */
    {
        static int slow_reported;
        double total = (double)(t3.tv_sec - t0.tv_sec) * 1000.0 + (double)(t3.tv_nsec - t0.tv_nsec) / 1e6;
        if (total > 50.0 && slow_reported < 5) {
            slow_reported++;
            fprintf(out, "[%s] [WARN] error_handler: slow log call %.0f ms: time %.0f ms, print %.0f ms, file %.0f ms\n",
                    timestamp, total,
                    (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6,
                    (double)(t2.tv_sec - t1.tv_sec) * 1000.0 + (double)(t2.tv_nsec - t1.tv_nsec) / 1e6,
                    (double)(t3.tv_sec - t2.tv_sec) * 1000.0 + (double)(t3.tv_nsec - t2.tv_nsec) / 1e6);
        }
    }

    if (s_callback) {
        s_callback(level, msg, s_callback_userdata);
    }
}

void error_handler_show_error_dialog(const char *title, const char *message)
{
    error_handler_log(LOG_ERROR, "%s: %s", title ? title : "Error", message ? message : "");

    if (SDL_WasInit(0) != 0) {
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR,
            title ? title : "Error",
            message ? message : "An unknown error occurred.",
            NULL
        );
    }
}

void error_handler_report_js_exception(void *engine_ptr, bool fatal)
{
    JSEngine *engine = (JSEngine *)engine_ptr;
    if (!engine) return;

    JSContext *ctx = js_engine_get_context(engine);
    if (!ctx) return;

    JSValue exc = JS_GetException(ctx);

    const char *msg_str = JS_ToCString(ctx, exc);
    if (msg_str) {
        error_handler_log(LOG_ERROR, "JS Exception: %s", msg_str);
    }

    char stack_buf[4096] = "";
    if (JS_IsError(exc)) {
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (!JS_IsUndefined(stack)) {
            const char *stack_str = JS_ToCString(ctx, stack);
            if (stack_str) {
                error_handler_log(LOG_ERROR, "Stack trace:\n%s", stack_str);
                snprintf(stack_buf, sizeof(stack_buf), "\n\nStack trace:\n%s", stack_str);
                JS_FreeCString(ctx, stack_str);
            }
        }
        JS_FreeValue(ctx, stack);
    }

    /* Dispatch to window.onerror via the shim, if present. */
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue onerror_fn = JS_GetPropertyStr(ctx, global, "__error_handler_dispatch");
        if (JS_IsFunction(ctx, onerror_fn)) {
            JSValue args[2];
            args[0] = JS_NewString(ctx, msg_str ? msg_str : "Unknown error");
            args[1] = exc;
            exc = JS_DupValue(ctx, exc); /* args[1] is freed below; keep our own ref */
            JSValue ret = JS_Call(ctx, onerror_fn, global, 2, args);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, args[1]);
        }
        JS_FreeValue(ctx, onerror_fn);
        JS_FreeValue(ctx, global);
    }

    if (fatal) {
        char dialog_msg[8192];
        snprintf(dialog_msg, sizeof(dialog_msg), "%s%s",
                 msg_str ? msg_str : "Unknown JS error",
                 stack_buf);
        error_handler_show_error_dialog("RPG Maker MZ - JavaScript Error", dialog_msg);
    }

    if (msg_str) JS_FreeCString(ctx, msg_str);
    JS_FreeValue(ctx, exc);
}

bool error_handler_attempt_auto_save(void *engine_ptr)
{
    JSEngine *engine = (JSEngine *)engine_ptr;
    if (!engine) return false;

    JSContext *ctx = js_engine_get_context(engine);
    if (!ctx) return false;

    error_handler_log(LOG_INFO, "Attempting crash recovery auto-save...");

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue save_fn = JS_GetPropertyStr(ctx, global, "__error_handler_autoSave");

    bool saved = false;
    if (JS_IsFunction(ctx, save_fn)) {
        JSValue ret = JS_Call(ctx, save_fn, global, 0, NULL);
        if (!JS_IsException(ret)) {
            int result = 0;
            JS_ToInt32(ctx, &result, ret);
            saved = (result != 0);
        } else {
            /* Auto-save itself threw; log and consume so nothing leaks into later JS. */
            JSValue inner_exc = JS_GetException(ctx);
            const char *inner_msg = JS_ToCString(ctx, inner_exc);
            if (inner_msg) {
                error_handler_log(LOG_ERROR, "Auto-save failed: %s", inner_msg);
                JS_FreeCString(ctx, inner_msg);
            }
            JS_FreeValue(ctx, inner_exc);
        }
        JS_FreeValue(ctx, ret);
    } else {
        error_handler_log(LOG_WARN, "Auto-save function not available");
    }

    JS_FreeValue(ctx, save_fn);
    JS_FreeValue(ctx, global);

    if (saved) {
        error_handler_log(LOG_INFO, "Auto-save succeeded");
    }

    return saved;
}
