/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "engine/js_engine.h"
#include "engine/game_loop.h"
#include "engine/error_handler.h"
#include "audio/audio_engine.h"
#include "rendering/renderer.h"
#include "video/video_player.h"

#include <SDL3/SDL.h>
#include <quickjs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ControlLine {
    char *text;
    struct ControlLine *next;
} ControlLine;

struct GameLoop {
    Platform  *platform;
    JSEngine  *engine;
    int        target_fps;
    double     frame_time_ms;  /* 1000.0 / target_fps */
    bool       running;

    /* Timing state */
    uint64_t   perf_freq;
    uint64_t   start_ticks;
    uint64_t   last_frame_ticks;
    uint64_t   prev_frame_start_ticks;   /* start of the previous frame (video clock) */

    /* FPS tracking */
    double     fps;
    int        frame_count;
    uint64_t   fps_timer_ticks;

    /* Per-frame performance stats */
    GameLoopStats stats;

    /* Debug frame limit (0 = run until quit) and optional final screenshot */
    int        max_frames;
    int        total_frames;
    char      *screenshot_path;
    Renderer **screenshot_renderer_slot;
    char      *probe_expr;

    /* Periodic phase-time report every perf_interval frames (0 = off) */
    int           perf_interval;
    int           perf_count;
    GameLoopStats perf_acc;
    /* Worst frame of the window and how many frames took more than 25 ms: an average hides stutter. */
    GameLoopStats perf_worst;
    int           perf_slow_frames;

    /* Control mode (--control): commands read from stdin by a thread. */
    bool         control_enabled;
    bool         control_started;      /* first command received */
    bool         control_eof;
    SDL_Thread  *control_thread;
    SDL_Mutex   *control_mutex;
    ControlLine *control_head;
    ControlLine *control_tail;
    long         control_shot_id;
    char        *control_shot_path;    /* screenshot requested this frame */
    JSValue      js_controlEval;

    /* Cached JS function references to avoid repeated lookups */
    JSValue    js_global;
    JSValue    js_setWindowSize;
    JSValue    js_flushInputEvents;
    JSValue    js_updateGamepads;
    JSValue    js_flushTimers;
    JSValue    js_flushAnimationFrames;
    bool       js_funcs_cached;
};

static void control_shutdown(GameLoop *loop);

/* Cache frequently-called JS functions to avoid per-frame string lookups. */
static void cache_js_functions(GameLoop *loop)
{
    JSContext *ctx = js_engine_get_context(loop->engine);
    if (!ctx || loop->js_funcs_cached) return;

    loop->js_global = JS_GetGlobalObject(ctx);
    loop->js_setWindowSize = JS_GetPropertyStr(ctx, loop->js_global, "__dom_setWindowSize");
    loop->js_flushInputEvents = JS_GetPropertyStr(ctx, loop->js_global, "__dom_flushInputEvents");
    loop->js_updateGamepads = JS_GetPropertyStr(ctx, loop->js_global, "__dom_updateGamepads");
    loop->js_flushTimers = JS_GetPropertyStr(ctx, loop->js_global, "__dom_flushTimers");
    loop->js_flushAnimationFrames = JS_GetPropertyStr(ctx, loop->js_global, "__dom_flushAnimationFrames");
    loop->js_funcs_cached = true;
}

static void release_js_functions(GameLoop *loop)
{
    if (!loop->js_funcs_cached) return;
    JSContext *ctx = js_engine_get_context(loop->engine);
    if (!ctx) return;

    JS_FreeValue(ctx, loop->js_flushAnimationFrames);
    JS_FreeValue(ctx, loop->js_flushTimers);
    JS_FreeValue(ctx, loop->js_updateGamepads);
    JS_FreeValue(ctx, loop->js_flushInputEvents);
    JS_FreeValue(ctx, loop->js_setWindowSize);
    JS_FreeValue(ctx, loop->js_global);
    loop->js_funcs_cached = false;
}

GameLoop *game_loop_create(Platform *platform, JSEngine *engine, int target_fps)
{
    if (!platform || !engine) return NULL;
    if (target_fps <= 0) target_fps = 60;

    GameLoop *loop = calloc(1, sizeof(GameLoop));
    if (!loop) return NULL;

    loop->platform = platform;
    loop->engine = engine;
    loop->target_fps = target_fps;
    loop->frame_time_ms = 1000.0 / (double)target_fps;
    loop->running = true;

    loop->perf_freq = SDL_GetPerformanceFrequency();
    loop->start_ticks = SDL_GetPerformanceCounter();
    loop->last_frame_ticks = loop->start_ticks;
    loop->fps_timer_ticks = loop->start_ticks;
    loop->fps = 0.0;
    loop->frame_count = 0;

    loop->js_global = JS_UNDEFINED;
    loop->js_setWindowSize = JS_UNDEFINED;
    loop->js_flushInputEvents = JS_UNDEFINED;
    loop->js_updateGamepads = JS_UNDEFINED;
    loop->js_flushTimers = JS_UNDEFINED;
    loop->js_flushAnimationFrames = JS_UNDEFINED;
    loop->js_funcs_cached = false;
    loop->js_controlEval = JS_UNDEFINED;

    return loop;
}

void game_loop_destroy(GameLoop *loop)
{
    if (!loop) return;
    control_shutdown(loop);
    release_js_functions(loop);
    free(loop->screenshot_path);
    free(loop->probe_expr);
    free(loop);
}

void game_loop_set_perf_report(GameLoop *loop, int every_frames)
{
    if (!loop) return;
    loop->perf_interval = every_frames > 0 ? every_frames : 0;
    loop->perf_count = 0;
    memset(&loop->perf_acc, 0, sizeof(loop->perf_acc));
    memset(&loop->perf_worst, 0, sizeof(loop->perf_worst));
    loop->perf_slow_frames = 0;
}

void game_loop_set_exit_probe(GameLoop *loop, const char *js_expression)
{
    if (!loop) return;
    free(loop->probe_expr);
    loop->probe_expr = js_expression ? strdup(js_expression) : NULL;
}

void game_loop_set_frame_limit(GameLoop *loop, int max_frames,
                               const char *screenshot_path, Renderer **renderer_slot)
{
    if (!loop) return;
    loop->max_frames = max_frames > 0 ? max_frames : 0;
    free(loop->screenshot_path);
    loop->screenshot_path = screenshot_path ? strdup(screenshot_path) : NULL;
    loop->screenshot_renderer_slot = renderer_slot;
}

/* Write the current default framebuffer as a binary PPM (RGB, top-down). */
static bool write_screenshot(GameLoop *loop, const char *path)
{
    Renderer *r = loop->screenshot_renderer_slot ? *loop->screenshot_renderer_slot : NULL;
    int w = 0, h = 0;
    platform_get_window_size(loop->platform, &w, &h);
    uint8_t *pixels = r ? renderer_read_pixels(r, 0, w, h) : NULL;
    if (!pixels) {
        error_handler_log(LOG_WARN, "Screenshot failed: no renderer or read-back failed");
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        error_handler_log(LOG_WARN, "Screenshot failed: cannot open %s", path);
        free(pixels);
        return false;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++) {
        fwrite(pixels + i * 4, 1, 3, f);
    }
    fclose(f);
    free(pixels);
    error_handler_log(LOG_INFO, "Screenshot written: %s (%dx%d)", path, w, h);
    return true;
}

/* --- Control mode ---------------------------------------------------------
   A reader thread turns stdin into a queue of lines; the loop drains the
   queue at the start of every frame. Replies go to stdout as
   "@@ctl <id> <ok|err> <json>" so a driver can find them among the logs. */

static void control_send(long id, const char *status, const char *json)
{
    printf("@@ctl %ld %s %s\n", id, status, json);
    fflush(stdout);
}

/* JS: __native_control_send(text) prints one reply line. */
static JSValue js_control_send(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *text = JS_ToCString(ctx, argv[0]);
    if (text) {
        printf("%s\n", text);
        fflush(stdout);
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

/* Read one line of any length from stdin (without the newline). */
static char *read_stdin_line(void)
{
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (c == '\n') break;
        if (len + 1 >= cap) {
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (!grown) { free(buf); return NULL; }
            buf = grown;
        }
        buf[len++] = (char)c;
    }
    if (c == EOF && len == 0) { free(buf); return NULL; }
    while (len > 0 && buf[len - 1] == '\r') len--;
    buf[len] = '\0';
    return buf;
}

static int control_reader_thread(void *arg)
{
    GameLoop *loop = arg;
    for (;;) {
        char *text = read_stdin_line();
        SDL_LockMutex(loop->control_mutex);
        if (!text) {
            loop->control_eof = true;
            SDL_UnlockMutex(loop->control_mutex);
            return 0;
        }
        ControlLine *line = calloc(1, sizeof(*line));
        if (line) {
            line->text = text;
            if (loop->control_tail) loop->control_tail->next = line;
            else loop->control_head = line;
            loop->control_tail = line;
        } else {
            free(text);
        }
        SDL_UnlockMutex(loop->control_mutex);
    }
}

static ControlLine *control_pop(GameLoop *loop, bool *eof)
{
    SDL_LockMutex(loop->control_mutex);
    ControlLine *line = loop->control_head;
    if (line) {
        loop->control_head = line->next;
        if (!loop->control_head) loop->control_tail = NULL;
    }
    *eof = loop->control_eof && !loop->control_head;
    SDL_UnlockMutex(loop->control_mutex);
    return line;
}

bool game_loop_enable_control(GameLoop *loop, struct Renderer **renderer_slot)
{
    if (!loop || loop->control_enabled) return false;
    JSContext *ctx = js_engine_get_context(loop->engine);
    if (!ctx) return false;

    loop->control_mutex = SDL_CreateMutex();
    if (!loop->control_mutex) return false;

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__native_control_send",
                      JS_NewCFunction(ctx, js_control_send, "__native_control_send", 1));
    loop->js_controlEval = JS_GetPropertyStr(ctx, global, "__control_eval");
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, loop->js_controlEval)) {
        error_handler_log(LOG_ERROR, "Control mode: __control_eval missing (control_shim.js not loaded)");
        JS_FreeValue(ctx, loop->js_controlEval);
        loop->js_controlEval = JS_UNDEFINED;
        return false;
    }

    loop->screenshot_renderer_slot = renderer_slot;
    loop->control_enabled = true;
    loop->control_thread = SDL_CreateThread(control_reader_thread, "control-stdin", loop);
    if (!loop->control_thread) {
        error_handler_log(LOG_ERROR, "Control mode: cannot start stdin reader");
        loop->control_enabled = false;
        return false;
    }
    error_handler_log(LOG_INFO, "Control mode enabled: reading commands from stdin");
    return true;
}

/* Handle one "<id> <command> [payload]" line. */
static void control_handle_line(GameLoop *loop, JSContext *ctx, const char *text)
{
    char *end = NULL;
    long id = strtol(text, &end, 10);
    if (end == text) return;                      /* not a command line */
    while (*end == ' ') end++;
    const char *cmd = end;
    while (*end && *end != ' ') end++;
    size_t cmd_len = (size_t)(end - cmd);
    while (*end == ' ') end++;
    const char *payload = end;

    if (cmd_len == 4 && strncmp(cmd, "eval", 4) == 0) {
        JSValue args[2];
        args[0] = JS_NewInt64(ctx, id);
        args[1] = JS_NewString(ctx, payload);
        JSValue ret = JS_Call(ctx, loop->js_controlEval, JS_UNDEFINED, 2, args);
        if (JS_IsException(ret)) {
            error_handler_report_js_exception(loop->engine, false);
            control_send(id, "err", "\"__control_eval threw\"");
        }
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
    } else if (cmd_len == 4 && strncmp(cmd, "shot", 4) == 0) {
        if (loop->control_shot_path) {
            control_send(id, "err", "\"a screenshot is already pending this frame\"");
        } else if (!*payload) {
            control_send(id, "err", "\"shot needs a file path\"");
        } else {
            loop->control_shot_id = id;
            loop->control_shot_path = strdup(payload);
        }
    } else if (cmd_len == 4 && strncmp(cmd, "quit", 4) == 0) {
        control_send(id, "ok", "null");
        loop->running = false;
    } else {
        control_send(id, "err", "\"unknown command\"");
    }
}

/* Drain queued commands. Before the first frame this blocks until the
   driver has sent something, so a prelude can run before any game frame. */
static void control_dispatch(GameLoop *loop, JSContext *ctx)
{
    bool eof = false;
    for (;;) {
        ControlLine *line = control_pop(loop, &eof);
        if (!line) {
            if (eof) {
                error_handler_log(LOG_INFO, "Control mode: stdin closed, stopping");
                loop->running = false;
                return;
            }
            if (loop->control_started) return;
            SDL_Delay(1);
            continue;
        }
        loop->control_started = true;
        control_handle_line(loop, ctx, line->text);
        free(line->text);
        free(line);
        if (!loop->running) return;
    }
}

static void control_finish_frame(GameLoop *loop)
{
    if (!loop->control_shot_path) return;
    bool ok = write_screenshot(loop, loop->control_shot_path);
    control_send(loop->control_shot_id, ok ? "ok" : "err",
                 ok ? "null" : "\"screenshot failed\"");
    free(loop->control_shot_path);
    loop->control_shot_path = NULL;
}

static void control_shutdown(GameLoop *loop)
{
    if (!loop->control_enabled) return;
    /* The reader thread blocks in fgetc; detach it so shutdown never waits
       on a driver that keeps stdin open. */
    if (loop->control_thread) SDL_DetachThread(loop->control_thread);
    JSContext *ctx = js_engine_get_context(loop->engine);
    if (ctx) JS_FreeValue(ctx, loop->js_controlEval);
    free(loop->control_shot_path);
    /* Lines left in the queue are leaked deliberately: the reader thread
       may still be appending, and the process is about to exit. */
    loop->control_enabled = false;
}

/* Convert performance counter ticks to milliseconds. */
static double ticks_to_ms(const GameLoop *loop, uint64_t ticks)
{
    return (double)ticks * 1000.0 / (double)loop->perf_freq;
}

double game_loop_elapsed_ms(const GameLoop *loop)
{
    if (!loop) return 0.0;
    uint64_t now = SDL_GetPerformanceCounter();
    return ticks_to_ms(loop, now - loop->start_ticks);
}

double game_loop_get_fps(const GameLoop *loop)
{
    if (!loop) return 0.0;
    return loop->fps;
}

GameLoopStats game_loop_get_stats(const GameLoop *loop)
{
    GameLoopStats empty = {0};
    if (!loop) return empty;
    return loop->stats;
}

void game_loop_request_stop(GameLoop *loop)
{
    if (loop) loop->running = false;
}



/* Benchmark/debug aid: RMMZ_AUTOSCRIPT="<frames>:<javascript>|<frames>:@dump=<file.ppm>|..." runs script code, or has the
   presented picture written to a file, that many frames after RMMZ_AUTOSTART began (used to reproduce things on the
   console, where nobody holds the controller). */
static int script_base = -1;

static void autoscript_step(GameLoop *loop, JSContext *ctx)
{
    static const char *spec;
    static int state = -1;
    if (state == 0 || script_base < 0) return;
    if (state < 0) {
        spec = getenv("RMMZ_AUTOSCRIPT");
        state = (spec && *spec) ? 1 : 0;
        if (state == 0) return;
    }
    int now = loop->total_frames - script_base;
    const char *p = spec;
    static int done_upto = -1;      /* items are handled in order; each once */
    int index = 0;
    while (*p) {
        const char *end = strchr(p, '|');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (index > done_upto) {
            int frames = atoi(p);
            const char *colon = memchr(p, ':', len);
            if (colon && frames <= now) {
                char item[900];
                size_t n = len - (size_t)(colon + 1 - p);
                if (n >= sizeof item) n = sizeof item - 1;
                memcpy(item, colon + 1, n);
                item[n] = '\0';
                if (strncmp(item, "@dump=", 6) == 0) {
                    platform_request_dump(loop->platform, item + 6);
                } else {
                    JSValue v = JS_Eval(ctx, item, n, "<autoscript>", JS_EVAL_TYPE_GLOBAL);
                    if (JS_IsException(v)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); }
                    JS_FreeValue(ctx, v);
                }
                error_handler_log(LOG_INFO, "autoscript: %d frames: %.60s", frames, item);
                done_upto = index;
            } else {
                break;      /* the next item is not due yet */
            }
        }
        index++;
        if (!end) break;
        p = end + 1;
    }
}

/* Benchmark aid: RMMZ_AUTOSTART=load[:slot] loads a save; RMMZ_AUTOSTART=<mapId>,<x>,<y> starts a new game as soon as the title screen is up and moves the
   player there (RMMZ_AUTOSTART=new just starts). Lets the console run a heavy map without anyone at the controller. */
static void autostart_step(GameLoop *loop, JSContext *ctx)
{
    static int state = -1;          /* -1: not read yet, 0: off, 1: waiting for the title, 2: waiting on the map */
    static const char *spec;
    static int map_frame;
    if (state == 0) return;
    if (state < 0) {
        spec = getenv("RMMZ_AUTOSTART");
        state = (spec && *spec) ? 1 : 0;
        if (state == 0) return;
    }
    if (loop->total_frames % 15 != 0) return;
    if (state == 1) {
        const char *probe = "typeof SceneManager !== 'undefined' && SceneManager._scene ? SceneManager._scene.constructor.name : ''";
        JSValue v = JS_Eval(ctx, probe, strlen(probe), "<autostart>", JS_EVAL_TYPE_GLOBAL);
        const char *name = JS_IsException(v) ? NULL : JS_ToCString(ctx, v);
        int on_title = name && strcmp(name, "Scene_Title") == 0;
        if (name) JS_FreeCString(ctx, name);
        if (JS_IsException(v)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); }
        JS_FreeValue(ctx, v);
        int title_mode = strcmp(spec, "title") == 0;
        /* title mode: games whose "title" is a map (no Scene_Title) start the clock after about 15 s instead */
        if (!on_title && !(title_mode && loop->total_frames > 900)) return;
        if (title_mode) {      /* RMMZ_AUTOSTART=title: only start the clock of RMMZ_AUTOSCRIPT */
            error_handler_log(LOG_INFO, "autostart: title screen reached");
            script_base = loop->total_frames;
            state = 0;
            return;
        }
        char go[512];
        int slot = strncmp(spec, "load", 4) == 0 ? (spec[4] == ':' ? atoi(spec + 5) : 0) : -1;
        if (slot >= 0)
            snprintf(go, sizeof go,
                     "(function(){ var go = function(){ $gameSystem.onAfterLoad(); SceneManager.goto(Scene_Map); };"
                     " var r = DataManager.loadGame(%d); if (r && typeof r.then === 'function') r.then(go); else if (r) go(); })(); 1", slot);
        else
            snprintf(go, sizeof go,
                     "(function(){ var s = SceneManager._scene;"
                     " if (s && typeof s.commandNewGame === 'function') s.commandNewGame();"
                     " else { DataManager.setupNewGame(); SceneManager.goto(Scene_Map); } })(); 1");
        JS_FreeValue(ctx, JS_Eval(ctx, go, strlen(go), "<autostart>", JS_EVAL_TYPE_GLOBAL));
        error_handler_log(LOG_INFO, slot >= 0 ? "autostart: loading save %d" : "autostart: new game started", slot);
        state = slot >= 0 ? 0 : 2;
        map_frame = loop->total_frames + 150;
        script_base = loop->total_frames;
    } else if (state == 2 && loop->total_frames >= map_frame) {
        int m = 0, x = 0, y = 0;
        if (sscanf(spec, "%d,%d,%d", &m, &x, &y) == 3) {
            char code[512];
            snprintf(code, sizeof code,
                     "$gameMessage.clear(); $gameScreen.clear(); $gameMap._interpreter.terminate();"
                     "$gamePlayer.reserveTransfer(%d, %d, %d, 2, 0); $gamePlayer.setTransparent(false); 1", m, x, y);
            JSValue v = JS_Eval(ctx, code, strlen(code), "<autostart>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(v)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); }
            JS_FreeValue(ctx, v);
            error_handler_log(LOG_INFO, "autostart: moved to map %d (%d,%d)", m, x, y);
        }
        state = 0;
    }
}

bool game_loop_step(GameLoop *loop)
{
    if (!loop || !loop->running) return false;

    uint64_t frame_start = SDL_GetPerformanceCounter();
    uint64_t phase_start, phase_end;

    JSContext *ctx = js_engine_get_context(loop->engine);

    if (!loop->js_funcs_cached && ctx) {
        cache_js_functions(loop);
    }

    if (loop->control_enabled && ctx) {
        control_dispatch(loop, ctx);
        if (!loop->running) return false;
    }

    js_engine_watchdog_arm(loop->engine);

    /* --- Phase 1: Events (SDL polling + resize + input) --- */
    phase_start = frame_start;

    if (!platform_poll_events(loop->platform)) {
        loop->running = false;
        return false;
    }

    if (ctx && loop->js_funcs_cached) {
        int rw, rh;
        if (platform_check_resize(loop->platform, &rw, &rh)) {
            if (JS_IsFunction(ctx, loop->js_setWindowSize)) {
                JSValue args[2];
                args[0] = JS_NewInt32(ctx, rw);
                args[1] = JS_NewInt32(ctx, rh);
                JSValue ret = JS_Call(ctx, loop->js_setWindowSize, loop->js_global, 2, args);
                if (JS_IsException(ret)) {
                    error_handler_report_js_exception(loop->engine, false);
                }
                JS_FreeValue(ctx, ret);
                JS_FreeValue(ctx, args[0]);
                JS_FreeValue(ctx, args[1]);
            }
        }

        if (JS_IsFunction(ctx, loop->js_flushInputEvents)) {
            JSValue ret = JS_Call(ctx, loop->js_flushInputEvents, loop->js_global, 0, NULL);
            if (JS_IsException(ret)) {
                error_handler_report_js_exception(loop->engine, false);
            }
            JS_FreeValue(ctx, ret);
        }

        if (JS_IsFunction(ctx, loop->js_updateGamepads)) {
            JSValue ret = JS_Call(ctx, loop->js_updateGamepads, loop->js_global, 0, NULL);
            if (JS_IsException(ret)) {
                error_handler_report_js_exception(loop->engine, false);
            }
            JS_FreeValue(ctx, ret);
        }
    }

    phase_end = SDL_GetPerformanceCounter();
    loop->stats.events_ms = ticks_to_ms(loop, phase_end - phase_start);

    /* --- Phase 2: Timers --- */
    phase_start = phase_end;

    if (ctx && loop->js_funcs_cached) {
        if (JS_IsFunction(ctx, loop->js_flushTimers)) {
            JSValue ret = JS_Call(ctx, loop->js_flushTimers, loop->js_global, 0, NULL);
            if (JS_IsException(ret)) {
                error_handler_report_js_exception(loop->engine, false);
            }
            JS_FreeValue(ctx, ret);
        }
    }

    phase_end = SDL_GetPerformanceCounter();
    loop->stats.timers_ms = ticks_to_ms(loop, phase_end - phase_start);

    /* --- Phase 3: Microtasks/Jobs --- */
    phase_start = phase_end;

    js_engine_execute_pending_jobs(loop->engine);

    phase_end = SDL_GetPerformanceCounter();
    loop->stats.jobs_ms = ticks_to_ms(loop, phase_end - phase_start);

    /* --- Phase 4: requestAnimationFrame callbacks --- */
    phase_start = phase_end;

    if (ctx && loop->js_funcs_cached) {
        if (JS_IsFunction(ctx, loop->js_flushAnimationFrames)) {
            double timestamp = game_loop_elapsed_ms(loop);
            JSValue args[1];
            args[0] = JS_NewFloat64(ctx, timestamp);
            JSValue ret = JS_Call(ctx, loop->js_flushAnimationFrames, loop->js_global, 1, args);
            if (JS_IsException(ret)) {
                error_handler_report_js_exception(loop->engine, false);
            }
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, args[0]);
        }
    }

    js_engine_execute_pending_jobs(loop->engine);

    /* --- Video: advance playback and draw it over the finished frame --- */
    {
        uint64_t prev = loop->prev_frame_start_ticks ? loop->prev_frame_start_ticks : frame_start;
        double dt = ticks_to_ms(loop, frame_start - prev) / 1000.0;
        loop->prev_frame_start_ticks = frame_start;
        video_player_update(dt);
        video_player_draw();
    }

    phase_end = SDL_GetPerformanceCounter();
    loop->stats.raf_ms = ticks_to_ms(loop, phase_end - phase_start);

    /* --- Audio update (fades, voice cleanup) --- */
    audio_update();

    /* --- Control mode: a requested screenshot is read back before the swap --- */
    if (loop->control_enabled) control_finish_frame(loop);

    /* --- Debug frame limit: capture the final frame before it is swapped --- */
    loop->total_frames++;
    compat_set_frame((unsigned long long)loop->total_frames);
    if ((loop->total_frames & 127) == 0) compat_tick();
    if ((loop->total_frames & 15) == 0) js_engine_profile_tick(loop->engine);
    if (ctx) autostart_step(loop, ctx);
    if (ctx) autoscript_step(loop, ctx);
    if (loop->max_frames > 0 && loop->total_frames >= loop->max_frames) {
        if (loop->screenshot_path) {
            write_screenshot(loop, loop->screenshot_path);
        }
        /* Report engine state so a log alone says where the game got to:
           the active scene by default, or the caller's probe expression. */
        char probe_result[1024] = "unknown";
        if (ctx) {
            const char *probe = loop->probe_expr ? loop->probe_expr :
                "typeof SceneManager !== 'undefined' && SceneManager._scene"
                "    ? SceneManager._scene.constructor.name : 'none'";
            JSValue v = JS_Eval(ctx, probe, strlen(probe), "<exit-probe>", JS_EVAL_TYPE_GLOBAL);
            if (!JS_IsException(v)) {
                const char *s = JS_ToCString(ctx, v);
                if (s) {
                    snprintf(probe_result, sizeof(probe_result), "%s", s);
                    JS_FreeCString(ctx, s);
                }
            } else {
                JSValue exc = JS_GetException(ctx);
                const char *s = JS_ToCString(ctx, exc);
                snprintf(probe_result, sizeof(probe_result), "probe threw: %s", s ? s : "?");
                if (s) JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, exc);
            }
            JS_FreeValue(ctx, v);
        }
        error_handler_log(LOG_INFO, "Frame limit reached (%d frames), stopping; %s: %s",
                          loop->total_frames, loop->probe_expr ? "probe" : "scene",
                          probe_result);
        loop->running = false;
    }

    /* --- Phase 5: Present --- */
    phase_start = phase_end;

    bool presented = false;
    {
        /* The frame was drawn into an offscreen surface; putting it on screen
           is the renderer's job, and the window swap follows it. */
        Renderer *r = loop->screenshot_renderer_slot ? *loop->screenshot_renderer_slot : NULL;
        if (r) {
            int ww = 0, wh = 0;
            platform_get_window_size(loop->platform, &ww, &wh);
            renderer_set_output_size(r, ww, wh);
            int pw = 0, ph = 0;
            platform_get_present_size(loop->platform, &pw, &ph);
            renderer_set_present_size(r, pw, ph);
            uint64_t present_start = SDL_GetPerformanceCounter();
            /* PS5: scale and hand over the frame on the CPU; elsewhere (or if that is not possible) blit it through
               OpenGL and swap the window. */
            int sw = 0, sh = 0;
            const uint8_t *frame = platform_cpu_present_available(loop->platform) ? renderer_read_screen(r, &sw, &sh) : NULL;
            if (frame && platform_present_pixels(loop->platform, frame, sw, sh)) presented = true;
            else renderer_present(r);
            loop->stats.present_ms = ticks_to_ms(loop, SDL_GetPerformanceCounter() - present_start);
        }
    }

    if (!presented) platform_swap_buffers(loop->platform);

    phase_end = SDL_GetPerformanceCounter();
    loop->stats.swap_ms = ticks_to_ms(loop, phase_end - phase_start);

    loop->stats.total_ms = ticks_to_ms(loop, phase_end - frame_start);
    rmmz_frame_us = rmmz_prof_now_us();   /* heartbeat for the stall watchdog thread */

    /* --- Optional periodic phase report (debug aid) --- */
    if (loop->perf_interval > 0) {
        loop->perf_acc.events_ms += loop->stats.events_ms;
        loop->perf_acc.timers_ms += loop->stats.timers_ms;
        loop->perf_acc.jobs_ms   += loop->stats.jobs_ms;
        loop->perf_acc.raf_ms    += loop->stats.raf_ms;
        loop->perf_acc.swap_ms   += loop->stats.swap_ms;
        loop->perf_acc.present_ms += loop->stats.present_ms;
        loop->perf_acc.total_ms  += loop->stats.total_ms;
        if (loop->stats.total_ms > loop->perf_worst.total_ms) loop->perf_worst = loop->stats;
        if (loop->stats.total_ms > 25.0) loop->perf_slow_frames++;
        if (++loop->perf_count >= loop->perf_interval) {
            double n = (double)loop->perf_count;
            error_handler_log(LOG_INFO,
                "perf: %d frames avg %.1f ms (events %.1f, timers %.1f, jobs %.1f, "
                "rAF %.1f, swap %.1f of which scaling %.1f); "
                "worst frame %.1f ms (rAF %.1f, swap %.1f), %d frames over 25 ms",
                loop->perf_count, loop->perf_acc.total_ms / n,
                loop->perf_acc.events_ms / n, loop->perf_acc.timers_ms / n,
                loop->perf_acc.jobs_ms / n, loop->perf_acc.raf_ms / n,
                loop->perf_acc.swap_ms / n, loop->perf_acc.present_ms / n,
                loop->perf_worst.total_ms, loop->perf_worst.raf_ms, loop->perf_worst.swap_ms,
                loop->perf_slow_frames);
            AudioBackendStats as;
            if (audio_backend_take_stats(&as)) {
                error_handler_log(LOG_INFO,
                    "audio: %u callbacks, %u found the queue empty, %u late (>2 blocks of %u frames); "
                    "longest callback %.1f ms, longest gap %.1f ms, largest request %u frames",
                    as.callbacks, as.empty, as.late, as.block_frames,
                    as.max_mix_us / 1000.0, as.max_gap_us / 1000.0, as.max_request_frames);
                if (as.late)
                    compat_report("audio", "mixer starved", "device ran dry: %u empty, %u late, longest callback %.1f ms",
                                  as.empty, as.late, as.max_mix_us / 1000.0);
            }
            memset(&loop->perf_acc, 0, sizeof(loop->perf_acc));
            memset(&loop->perf_worst, 0, sizeof(loop->perf_worst));
            loop->perf_slow_frames = 0;
            loop->perf_count = 0;
        }
    }

    /* --- FPS tracking --- */
    uint64_t now = phase_end;

    loop->frame_count++;
    uint64_t fps_elapsed = now - loop->fps_timer_ticks;
    double fps_elapsed_ms = ticks_to_ms(loop, fps_elapsed);
    if (fps_elapsed_ms >= 1000.0) {
        loop->fps = (double)loop->frame_count * 1000.0 / fps_elapsed_ms;
        loop->frame_count = 0;
        loop->fps_timer_ticks = now;
    }

    /* If VSync is off or not working, manually pace to target FPS. */
    uint64_t frame_elapsed = now - loop->last_frame_ticks;
    double frame_elapsed_ms = ticks_to_ms(loop, frame_elapsed);
    if (frame_elapsed_ms < loop->frame_time_ms) {
        double sleep_ms = loop->frame_time_ms - frame_elapsed_ms;
        if (sleep_ms > 1.0) {
            SDL_Delay((uint32_t)(sleep_ms - 1.0));
        }
    }
    loop->last_frame_ticks = SDL_GetPerformanceCounter();

    return true;
}

void game_loop_run(GameLoop *loop)
{
    if (!loop) return;
    while (game_loop_step(loop)) {
    }
}
