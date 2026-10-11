/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#ifndef RMMZ_JS_ENGINE_H
#define RMMZ_JS_ENGINE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct JSEngine JSEngine;

/* Callback signature for console output capture (used by tests). */
typedef void (*JSConsoleCallback)(const char *level, const char *message, void *userdata);

/* Create a QuickJS runtime and context. Returns NULL on failure. */
JSEngine *js_engine_init(void);

/* Shut down and free all engine resources. Safe to call with NULL. */
void js_engine_shutdown(JSEngine *engine);

/* Evaluate a JS string. Returns false on error (the exception is printed). */
bool js_engine_eval(JSEngine *engine, const char *script, const char *filename);

/* Evaluate a JS string and return its result as a string, or NULL on error.
   Free the result with js_engine_free_string(). */
char *js_engine_eval_string(JSEngine *engine, const char *script, const char *filename);

/* Free a string returned by js_engine_eval_string(). */
void js_engine_free_string(char *str);

/* Load and evaluate a JS file from disk. Returns true on success. */
bool js_engine_eval_file(JSEngine *engine, const char *filepath);

/* Evaluates a plugin file after replacing the string lookups of obfuscator.io-style plugins with the strings
   themselves (see src/engine/deobf.c). shim_dir holds deobf.js. */
bool deobf_eval_plugin_file(JSEngine *engine, const char *filepath, const char *shim_dir);

/* Like js_engine_eval_file(), but caches compiled bytecode in a .jsc file next
   to the source (invalidated by source mtime). Falls back to plain eval. */
bool js_engine_eval_file_cached(JSEngine *engine, const char *filepath);

/* Drain the job queue (promise callbacks) and report unhandled rejections.
   Returns the number of jobs executed, or -1 on error. */
int js_engine_execute_pending_jobs(JSEngine *engine);

/* Route console.log/warn/error to a callback instead of stdout/stderr. */
void js_engine_set_console_callback(JSEngine *engine, JSConsoleCallback cb, void *userdata);

/* Get the underlying QuickJS context (NULL if engine is NULL). */
struct JSContext *js_engine_get_context(JSEngine *engine);

/* Debug aid: interrupt JS that runs longer than limit_ms since the last
   js_engine_watchdog_arm() call, raising an uncatchable InternalError whose
   stack trace shows where the script was stuck. 0 disables (the default). */
void js_engine_set_watchdog(JSEngine *engine, unsigned limit_ms);

/* Debug aid: sample the running script stack about every interval_ms milliseconds and print the most
   expensive stacks every 10 seconds (native time is charged to the script function that called it).
   0 disables (the default). */
void js_engine_set_profile(JSEngine *engine, unsigned interval_ms);

/* Restart the watchdog clock. Eval entry points arm it themselves; the game
   loop arms it once per frame. */
void js_engine_watchdog_arm(JSEngine *engine);


/* ---- native call timing (debug aid, see --profile) ----
   RMMZ_PROF_SCOPE("name") at the top of a function adds the time it takes to a table that the profiler
   prints together with the script stacks. Costs one flag check when profiling is off. */
#include <stdint.h>
extern int rmmz_prof_enabled;
uint64_t rmmz_prof_now_us(void);
void rmmz_prof_add(const char *name, uint64_t us);
typedef struct { const char *name; uint64_t t0; } RmmzProfScope;
/* The native scopes the script thread is inside right now, outermost first. Tracked always (a few stores per
   call), timed only with --profile: the stall watchdog thread reads it to say where the game was blocked, see
   stall_watchdog() in js_engine.c. rmmz_frame_us is the end of the last frame, set by the game loop. */
extern const char *volatile rmmz_prof_active[8];
extern volatile int rmmz_prof_depth;
extern volatile uint64_t rmmz_frame_us;
static inline RmmzProfScope rmmz_prof_scope_begin(const char *n)
{
    RmmzProfScope s = { n, rmmz_prof_enabled ? rmmz_prof_now_us() : 0 };
    int d = rmmz_prof_depth;
    if (d >= 0 && d < 8) rmmz_prof_active[d] = n;
    rmmz_prof_depth = d + 1;
    return s;
}
static inline void rmmz_prof_scope_end(RmmzProfScope *s)
{
    if (s->t0) rmmz_prof_add(s->name, rmmz_prof_now_us() - s->t0);
    rmmz_prof_depth--;
}
#define RMMZ_PROF_SCOPE(n) \
    RmmzProfScope rmmz_ps_ __attribute__((cleanup(rmmz_prof_scope_end))) = rmmz_prof_scope_begin(n)

#endif /* RMMZ_JS_ENGINE_H */
