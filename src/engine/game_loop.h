/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#ifndef RMMZ_GAME_LOOP_H
#define RMMZ_GAME_LOOP_H

#include "engine/js_engine.h"
#include "platform/platform.h"
#include <stdbool.h>

typedef struct GameLoop GameLoop;

/* Per-frame performance statistics (all times in milliseconds). */
typedef struct {
    double events_ms;    /* SDL event polling + resize + input */
    double timers_ms;    /* JS timer queue flush */
    double jobs_ms;      /* Promise/microtask execution */
    double raf_ms;       /* requestAnimationFrame callbacks (includes rendering) */
    double swap_ms;      /* Buffer swap */
    double present_ms;   /* Scaling the finished frame to the screen (part of swap) */
    double total_ms;     /* Total frame time */
    int    draw_calls;   /* GPU draw calls this frame */
    int    quad_count;   /* Total quads rendered this frame */
} GameLoopStats;

/* Create a game loop bound to a platform and JS engine.
   The loop drives: SDL events → JS timers → microtasks → rAF → render → swap.
   target_fps controls frame pacing (typically 60). */
GameLoop *game_loop_create(Platform *platform, JSEngine *engine, int target_fps);

/* Destroy the game loop. Does NOT destroy the platform or engine. */
void game_loop_destroy(GameLoop *loop);

/* Run a single frame of the game loop.
   Returns false when the loop should exit (user quit, etc.). */
bool game_loop_step(GameLoop *loop);

/* Run the game loop until exit. Blocking call. */
void game_loop_run(GameLoop *loop);

/* Request the game loop to stop (from JS or another thread). */
void game_loop_request_stop(GameLoop *loop);

/* Debug aid: stop automatically after max_frames frames (0 = never). When
   screenshot_path is non-NULL, the final frame is read back through
   *renderer_slot (dereferenced at capture time, since the renderer is created
   lazily by the game) and written there as a binary PPM (P6). */
struct Renderer;
void game_loop_set_frame_limit(GameLoop *loop, int max_frames,
                               const char *screenshot_path, struct Renderer **renderer_slot);

/* Control mode (--control): read "<id> eval <json source>", "<id> shot <path>"
   and "<id> quit" lines from stdin, one per line, and reply on stdout with
   "@@ctl <id> ok|err <json>". Requires control_shim.js to be loaded. The
   first frame waits for the first command so a prelude can run before the
   game starts. Screenshots are PPM files read back through *renderer_slot. */
bool game_loop_enable_control(GameLoop *loop, struct Renderer **renderer_slot);

/* Debug aid: a JS expression evaluated and logged when the frame limit stops
   the loop, instead of the default active-scene name. */
void game_loop_set_exit_probe(GameLoop *loop, const char *js_expression);

/* Debug aid: log average per-phase frame times every N frames (0 = off). */
void game_loop_set_perf_report(GameLoop *loop, int every_frames);

/* Get the elapsed time in milliseconds since the loop was created. */
double game_loop_elapsed_ms(const GameLoop *loop);

/* Get the current FPS (averaged over recent frames). */
double game_loop_get_fps(const GameLoop *loop);

/* Get per-frame performance statistics for the last completed frame. */
GameLoopStats game_loop_get_stats(const GameLoop *loop);

#endif /* RMMZ_GAME_LOOP_H */
