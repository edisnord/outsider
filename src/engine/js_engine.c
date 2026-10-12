/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "engine/js_engine.h"
#include "engine/error_handler.h"
#ifndef RMMZ_JS_JSC
#include "engine/js_pool_alloc.h"
#endif

#include <quickjs.h>
#include <quickjs-libc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* 256 MB memory limit for game runtime. */
#define JS_MEMORY_LIMIT (256 * 1024 * 1024)
/* JS stack limit. Must stay well below the real thread stack so QuickJS
   throws a catchable "stack overflow" (some plugins rely on catching it)
   instead of the process faulting: Linux main threads default to 8 MB and
   the Windows build links with 16 MB (see CMakeLists.txt). */
#define JS_STACK_SIZE   (4 * 1024 * 1024)

struct JSEngine {
    JSRuntime  *rt;
    JSContext  *ctx;
    JSConsoleCallback console_cb;
    void              *console_userdata;

    /* Pending unhandled rejections. QuickJS reports a rejection immediately and
       retracts it if a handler is attached later, so entries are held here and
       reported only after each job drain (when a browser fires unhandledrejection). */
    JSValue *rej_promises;
    JSValue *rej_reasons;
    int      rej_count;
    int      rej_cap;

    /* Script watchdog (debug aid, 0 = off): interrupt JS that runs longer
       than watchdog_ms since the last arm. */
    unsigned watchdog_ms;
    uint64_t watchdog_armed_ms;

    /* Sampling profiler (debug aid, 0 = off): see profile_interrupt(). */
    unsigned profile_ms;
    uint64_t profile_last_us;
    uint64_t profile_report_us;
    double   profile_total_ms;
};

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
#ifdef _WIN32
    timespec_get(&ts, TIME_UTC);
#else
    /* timespec_get() only reached Android in API 29; clock_gettime() is
       everywhere and monotonic is what a watchdog wants anyway. */
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- sampling profiler ---------------------------------------------------------------------------
   QuickJS calls the interrupt handler every few thousand bytecode operations. Each time at least profile_ms
   have passed since the previous sample, the current script stack is recorded and charged with the time
   that passed. Time spent in native code (drawing on a canvas, decoding an image, ...) is charged to the
   script function that called it, because the handler only runs again once that function is executing.
   Every 10 seconds the most expensive stacks are printed and the counters are reset. */

#define PROF_MAX 1024
#define PROF_KEY 220

typedef struct {
    char     key[PROF_KEY];
    double   ms;
    unsigned samples;
} ProfEntry;

int rmmz_prof_enabled;
#ifndef RMMZ_JS_JSC
/* set by the JavaScriptCore shim (qjs_jsc.c); QuickJS builds have no such hook */
const char *volatile rmmz_native_name;
volatile uint64_t rmmz_native_since_us;
#else
extern const char *volatile rmmz_native_name;
extern volatile uint64_t rmmz_native_since_us;
#endif
const char *volatile rmmz_prof_active[8];
volatile int rmmz_prof_depth;
volatile uint64_t rmmz_frame_us;

uint64_t rmmz_prof_now_us(void);

typedef struct { const char *name; uint64_t us; uint64_t calls; } NativeProf;
static NativeProf native_prof[160];
static int native_prof_count;

void rmmz_prof_add(const char *name, uint64_t us)
{
    for (int i = 0; i < native_prof_count; i++) {
        if (native_prof[i].name == name) {
            native_prof[i].us += us;
            native_prof[i].calls++;
            return;
        }
    }
    if (native_prof_count < 160) {
        native_prof[native_prof_count].name = name;
        native_prof[native_prof_count].us = us;
        native_prof[native_prof_count].calls = 1;
        native_prof_count++;
    }
}

static int native_prof_cmp(const void *a, const void *b)
{
    int64_t d = (int64_t)((const NativeProf *)b)->us - (int64_t)((const NativeProf *)a)->us;
    return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

static double prof_overhead_ms, prof_overhead_worst_ms;
static unsigned prof_overhead_samples;
static ProfEntry prof_tables[5][PROF_MAX];
static int prof_counts[5];
#define prof_table prof_tables[0]
#define prof_count prof_counts[0]

uint64_t rmmz_prof_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* "    at name (file:line:col)" lines -> "name(file:line) < name2(file:line) < ..." for the first n frames */
static void prof_make_key(const char *stack, char *out, size_t out_size, int frames)
{
    size_t o = 0;
    const char *p = stack;
    out[0] = '\0';
    for (int f = 0; f < frames && *p; f++) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        while (len > 0 && (*p == ' ' || *p == '\t')) { p++; len--; }
        if (len > 3 && strncmp(p, "at ", 3) == 0) { p += 3; len -= 3; }
        /* Shorten long paths to the last two components. */
        const char *paren = memchr(p, '(', len);
        if (paren) {
            const char *q = paren + 1, *last = q, *prev = q;
            const char *e = p + len;
            for (const char *c = q; c < e; c++) {
                if (*c == '/') { prev = last; last = c + 1; }
            }
            char part[200];
            int n = snprintf(part, sizeof(part), "%.*s(%.*s", (int)(paren - p), p, (int)(e - prev), prev);
            if (n > 0 && o + (size_t)n + 4 < out_size) {
                if (o) { memcpy(out + o, " < ", 3); o += 3; }
                memcpy(out + o, part, (size_t)n);
                o += (size_t)n;
            }
        } else if (len > 0 && o + len + 4 < out_size) {
            if (o) { memcpy(out + o, " < ", 3); o += 3; }
            memcpy(out + o, p, len);
            o += len;
        }
        out[o] = '\0';
        p = eol ? eol + 1 : p + len;
    }
}

static int prof_cmp(const void *a, const void *b)
{
    double d = ((const ProfEntry *)b)->ms - ((const ProfEntry *)a)->ms;
    return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

static void prof_report(JSEngine *engine)
{
    static const char *titles[5] = {
        "top script stacks", "time per file (innermost frame)", "time per function in game code (not shims)",
        "time inside native functions (innermost frame is native)", "time per execution tier (innermost frame)"
    };
    static const int limits[5] = { 15, 15, 20, 18, 8 };
    double total = engine->profile_total_ms > 0 ? engine->profile_total_ms : 1;
    JSMemoryUsage mu;
    JS_ComputeMemoryUsage(engine->rt, &mu);
    if (mu.malloc_size > 0) {
        error_handler_log(LOG_INFO, "[profile] heap: %.1f MB in %lld blocks; %lld objects, %lld strings, %lld atoms, %lld shapes, "
                "%lld js functions, %lld arrays (%lld fast)",
                mu.malloc_size / 1048576.0, (long long)mu.malloc_count, (long long)mu.obj_count,
                (long long)mu.str_count, (long long)mu.atom_count, (long long)mu.shape_count,
                (long long)mu.js_func_count, (long long)mu.array_count, (long long)mu.fast_array_count);
    }
    for (int t = 0; t < 5; t++) {
        if (prof_counts[t] == 0) continue;
        qsort(prof_tables[t], (size_t)prof_counts[t], sizeof(ProfEntry), prof_cmp);
        error_handler_log(LOG_INFO, "[profile] ---- %s over the last %.1f s (charged time, ms) ----",
                titles[t], engine->profile_total_ms / 1000.0);
        for (int i = 0; i < prof_counts[t] && i < limits[t]; i++) {
            error_handler_log(LOG_INFO, "[profile] %5.1f%% %8.1f ms %5u  %s",
                    100.0 * prof_tables[t][i].ms / total,
                    prof_tables[t][i].ms, prof_tables[t][i].samples, prof_tables[t][i].key);
        }
        prof_counts[t] = 0;
    }
    if (native_prof_count > 0) {
        qsort(native_prof, (size_t)native_prof_count, sizeof(NativeProf), native_prof_cmp);
        error_handler_log(LOG_INFO, "[profile] ---- native calls (time inside, ms; includes nested calls) ----");
        for (int i = 0; i < native_prof_count && i < 22; i++) {
            error_handler_log(LOG_INFO, "[profile] %8.1f ms %8llu calls %8.1f us/call  %s", native_prof[i].us / 1000.0,
                    (unsigned long long)native_prof[i].calls,
                    native_prof[i].calls ? (double)native_prof[i].us / (double)native_prof[i].calls : 0.0,
                    native_prof[i].name);
        }
        native_prof_count = 0;
    }
    engine->profile_total_ms = 0;
}

static void prof_add(int t, const char *key, double dt_ms)
{
    ProfEntry *tab = prof_tables[t];
    for (int i = 0; i < prof_counts[t]; i++) {
        if (strcmp(tab[i].key, key) == 0) {
            tab[i].ms += dt_ms;
            tab[i].samples++;
            return;
        }
    }
    if (prof_counts[t] < PROF_MAX) {
        snprintf(tab[prof_counts[t]].key, PROF_KEY, "%s", key);
        tab[prof_counts[t]].ms = dt_ms;
        tab[prof_counts[t]].samples = 1;
        prof_counts[t]++;
    }
}

/* Innermost frame: "name (file:line:col)" -> file (basename) and, for game code, "name(file:line)". */
static void prof_frames(const char *stack, char *file_key, size_t fk, char *game_key, size_t gk)
{
    file_key[0] = '\0';
    game_key[0] = '\0';
    const char *p = stack;
    int first = 1;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[300];
        size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        p = eol ? eol + 1 : p + len;
        char *paren = strchr(line, '(');
        char *close = strrchr(line, ')');
        if (!paren || !close || close < paren) continue;   /* "(native)" style frames have no file */
        *close = '\0';
        const char *path = paren + 1;
        if (strcmp(path, "native") == 0 || strcmp(path, "missing") == 0) continue;
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        char fbuf[200];
        snprintf(fbuf, sizeof(fbuf), "%s", base);
        char *colon = strchr(fbuf, ':');
        if (colon) *colon = '\0';
        char name[100];
        const char *at = line;
        while (*at == ' ') at++;
        if (strncmp(at, "at ", 3) == 0) at += 3;
        size_t nl = (size_t)(paren - at);
        while (nl > 0 && at[nl - 1] == ' ') nl--;
        snprintf(name, sizeof(name), "%.*s", (int)nl, at);
        if (first) {
            snprintf(file_key, fk, "%s", fbuf);
            first = 0;
        }
        if (!strstr(path, "shims/")) {
            snprintf(game_key, gk, "%s (%s)", name, base);
            return;
        }
    }
}

static void profile_sample(JSEngine *engine, double dt_ms)
{
    JSContext *ctx = engine->ctx;
    JSValue err = JS_NewError(ctx);
    JSValue st = JS_GetPropertyStr(ctx, err, "stack");
    const char *stack = JS_IsString(st) ? JS_ToCString(ctx, st) : NULL;
    char key[PROF_KEY], file_key[PROF_KEY], game_key[PROF_KEY];
    file_key[0] = game_key[0] = '\0';
    if (stack) {
        prof_make_key(stack, key, sizeof(key), 3);
        prof_frames(stack, file_key, sizeof(file_key), game_key, sizeof(game_key));
    } else {
        snprintf(key, sizeof(key), "(no stack)");
    }
    if (stack) JS_FreeCString(ctx, stack);
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, err);

    engine->profile_total_ms += dt_ms;
    prof_add(0, key, dt_ms);
    prof_add(1, file_key[0] ? file_key : "(native / no file)", dt_ms);
    prof_add(2, game_key[0] ? game_key : "(shims / native only)", dt_ms);
}

/* Called periodically by QuickJS while JS runs. Returning 1 raises an
   uncatchable InternalError, so the stack trace shows where the script was. */
static int watchdog_interrupt(JSRuntime *rt, void *opaque)
{
    (void)rt;
    JSEngine *engine = opaque;

    if (engine->profile_ms) {
        uint64_t now_us = monotonic_us();
        if (engine->profile_last_us == 0) {
            engine->profile_last_us = now_us;
            engine->profile_report_us = now_us;
        } else if (now_us - engine->profile_last_us >= (uint64_t)engine->profile_ms * 1000u) {
            profile_sample(engine, (double)(now_us - engine->profile_last_us) / 1000.0);
            uint64_t after = monotonic_us();
            double sample_ms = (double)(after - now_us) / 1000.0;
            prof_overhead_ms += sample_ms;
            prof_overhead_samples++;
            if (sample_ms > prof_overhead_worst_ms) prof_overhead_worst_ms = sample_ms;
            engine->profile_last_us = after;
            if (after - engine->profile_report_us >= 10000000u) {
                uint64_t r0 = monotonic_us();
                prof_report(engine);
                uint64_t r1 = monotonic_us();
                error_handler_log(LOG_INFO, "[profile] overhead of profiling in this window: %u samples took %.0f ms "
                        "(%.2f ms each, worst %.1f ms); printing the report took %.0f ms",
                        prof_overhead_samples, prof_overhead_ms,
                        prof_overhead_samples ? prof_overhead_ms / prof_overhead_samples : 0.0,
                        prof_overhead_worst_ms, (double)(r1 - r0) / 1000.0);
                prof_overhead_ms = prof_overhead_worst_ms = 0;
                prof_overhead_samples = 0;
                engine->profile_report_us = monotonic_us();
            }
        }
    }

    if (!engine->watchdog_ms) return 0;
    uint64_t now = monotonic_ms();
    if (now - engine->watchdog_armed_ms > engine->watchdog_ms) {
        fprintf(stderr, "js_engine: watchdog: script ran longer than %u ms, interrupting\n",
                engine->watchdog_ms);
        engine->watchdog_armed_ms = now;   /* one interruption per overrun */
        return 1;
    }
    return 0;
}

void js_engine_set_watchdog(JSEngine *engine, unsigned limit_ms)
{
    if (!engine) return;
    engine->watchdog_ms = limit_ms;
    engine->watchdog_armed_ms = monotonic_ms();
    JS_SetInterruptHandler(engine->rt, (limit_ms || engine->profile_ms) ? watchdog_interrupt : NULL, engine);
}


#ifdef RMMZ_JS_JSC
/* ---- JavaScriptCore: the profile comes from the engine's own sampler ---------------------------------
   JSC has no interrupt handler to hang a sampler on, but it has a sampling profiler thread that suspends the script
   thread on a timer and records its stack, native frames included. js_engine_profile_tick() collects its JSON every
   10 s and feeds the same tables as the QuickJS sampler, plus two it can fill that QuickJS cannot: time whose
   innermost frame is a native function (a stall inside drawImage is charged to drawImage, not to the next script
   line) and time per execution tier (LLInt / Baseline / DFG / FTL). Every sample is charged one interval. */

typedef struct { double id; char url[200]; } JscSource;

static const char *jp_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++; return p; }

/* a JSON string at p (on the opening quote) -> out (truncated), returns the position after the closing quote */
static const char *jp_string(const char *p, char *out, size_t cap)
{
    size_t o = 0;
    if (*p != '"') { if (out && cap) out[0] = '\0'; return p; }
    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            if (e == 'n') c = ' ';
            else if (e == 'u') { for (int i = 0; i < 4 && *p; i++) p++; c = '?'; }
            else c = e;
        }
        if (out && o + 1 < cap) out[o++] = c;
    }
    if (out && cap) out[o] = '\0';
    return *p == '"' ? p + 1 : p;
}

/* skips one JSON value of any kind */
static const char *jp_skip(const char *p)
{
    p = jp_ws(p);
    if (*p == '"') return jp_string(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        char open = *p, close = open == '{' ? '}' : ']';
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') { p = jp_string(p, NULL, 0) - 1; continue; }
            if (*p == open) depth++;
            else if (*p == close && --depth == 0) return p + 1;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

static void jp_basename(const char *url, char *out, size_t cap)
{
    const char *b = strrchr(url, '/');
    snprintf(out, cap, "%s", b ? b + 1 : url);
}

typedef struct {
    char   name[96];
    char   url[200];
    char   category[16];
    double line;
    int    native;
} JscFrame;

/* parse one frame object at p ('{'); returns the position after it */
static const char *jp_frame(const char *p, JscFrame *f, const JscSource *src, int nsrc)
{
    memset(f, 0, sizeof *f);
    double id = 0;
    p = jp_ws(p) + 1;
    while (*p && *p != '}') {
        p = jp_ws(p);
        if (*p == ',') { p++; continue; }
        char key[24];
        p = jp_string(p, key, sizeof key);
        p = jp_ws(p);
        if (*p == ':') p++;
        p = jp_ws(p);
        if (strcmp(key, "name") == 0) p = jp_string(p, f->name, sizeof f->name);
        else if (strcmp(key, "category") == 0) p = jp_string(p, f->category, sizeof f->category);
        else if (strcmp(key, "sourceID") == 0) { id = atof(p); p = jp_skip(p); }
        else if (strcmp(key, "line") == 0) { f->line = atof(p); p = jp_skip(p); }
        else p = jp_skip(p);
    }
    for (int i = 0; i < nsrc; i++)
        if (src[i].id == id) { snprintf(f->url, sizeof f->url, "%s", src[i].url); break; }
    f->native = f->url[0] == '\0';
    if (!f->name[0]) snprintf(f->name, sizeof f->name, "(anonymous)");
    return *p == '}' ? p + 1 : p;
}

static void jsc_profile_consume(JSEngine *engine, const char *json, double wall_s)
{
    static JscSource sources[512];
    int nsrc = 0;
    /* "sources" follows "traces" in the output, and traces need it: read it first (search from the end) */
    const char *sp = NULL;
    for (const char *q = json; (q = strstr(q, "\"sources\":")) != NULL; q++) sp = q;
    if (sp) {
        const char *p = jp_ws(sp + 10);
        if (*p == '[') {
            p++;
            while (*p && *p != ']' && nsrc < 512) {
                p = jp_ws(p);
                if (*p == ',') { p++; continue; }
                if (*p != '{') break;
                p++;
                JscSource *s = &sources[nsrc];
                s->id = -1;
                s->url[0] = '\0';
                while (*p && *p != '}') {
                    p = jp_ws(p);
                    if (*p == ',') { p++; continue; }
                    char key[24];
                    p = jp_string(p, key, sizeof key);
                    p = jp_ws(p);
                    if (*p == ':') p++;
                    p = jp_ws(p);
                    if (strcmp(key, "sourceID") == 0) { s->id = atof(p); p = jp_skip(p); }
                    else if (strcmp(key, "url") == 0) p = jp_string(p, s->url, sizeof s->url);
                    else p = jp_skip(p);
                }
                if (*p == '}') p++;
                if (s->id >= 0) nsrc++;
            }
        }
    }

    double interval_ms = 1.0;
    const char *ip = strstr(json, "\"interval\":");
    if (ip) interval_ms = atof(ip + 11) * 1000.0;
    if (interval_ms <= 0) interval_ms = 1.0;

    const char *p = strstr(json, "\"traces\":");
    if (!p) return;
    p = jp_ws(p + 9);
    if (*p != '[') return;
    p++;
    unsigned traces = 0;
    while (*p && *p != ']') {
        p = jp_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        p++;                                   /* into the trace object */
        JscFrame fr[3];
        int nfr = 0;
        JscFrame first_game, innermost_file;
        int have_game = 0, have_file = 0;
        while (*p && *p != '}') {
            p = jp_ws(p);
            if (*p == ',') { p++; continue; }
            char key[24];
            p = jp_string(p, key, sizeof key);
            p = jp_ws(p);
            if (*p == ':') p++;
            p = jp_ws(p);
            if (strcmp(key, "frames") == 0 && *p == '[') {
                p++;
                while (*p && *p != ']') {
                    p = jp_ws(p);
                    if (*p == ',') { p++; continue; }
                    if (*p != '{') break;
                    JscFrame f;
                    p = jp_frame(p, &f, sources, nsrc);
                    if (nfr < 3) fr[nfr++] = f;
                    if (!f.native && !have_file) { innermost_file = f; have_file = 1; }
                    if (!f.native && !have_game && !strstr(f.url, "shims/")) { first_game = f; have_game = 1; }
                }
                if (*p == ']') p++;
            } else {
                p = jp_skip(p);
            }
        }
        if (*p == '}') p++;
        traces++;
        if (nfr == 0) continue;

        char key[PROF_KEY], part[260];
        key[0] = '\0';
        for (int i = 0; i < nfr; i++) {
            char base[100];
            if (fr[i].native) snprintf(part, sizeof part, "%s%s(native)", i ? " < " : "", fr[i].name);
            else {
                jp_basename(fr[i].url, base, sizeof base);
                snprintf(part, sizeof part, "%s%s(%s:%d)", i ? " < " : "", fr[i].name, base, (int)fr[i].line);
            }
            if (strlen(key) + strlen(part) + 1 < sizeof key) strcat(key, part);
        }
        prof_add(0, key, interval_ms);
        if (have_file) {
            char base[100];
            jp_basename(innermost_file.url, base, sizeof base);
            prof_add(1, base, interval_ms);
        } else {
            prof_add(1, "(native / no file)", interval_ms);
        }
        if (have_game) {
            char base[100], gk[PROF_KEY];
            jp_basename(first_game.url, base, sizeof base);
            snprintf(gk, sizeof gk, "%s (%s)", first_game.name, base);
            prof_add(2, gk, interval_ms);
        } else {
            prof_add(2, "(shims / native only)", interval_ms);
        }
        if (fr[0].native) prof_add(3, fr[0].name, interval_ms);
        prof_add(4, fr[0].native ? "native" : (fr[0].category[0] ? fr[0].category : "?"), interval_ms);
        engine->profile_total_ms += interval_ms;
    }
    (void)wall_s;
    (void)traces;
}

static void jsc_profile_report(JSEngine *engine, int final)
{
    uint64_t t0 = monotonic_us();
    size_t len = 0;
    char *json = rmmz_jsc_profiler_take(engine->rt, &len);
    uint64_t t1 = monotonic_us();
    double wall_s = engine->profile_report_us ? (double)(t1 - engine->profile_report_us) / 1e6 : 0;
    if (json) {
        jsc_profile_consume(engine, json, wall_s);
        free(json);
    }
    uint64_t t2 = monotonic_us();
    if (engine->profile_total_ms > 0) {
        error_handler_log(LOG_INFO, "[profile] %s%.1f s of wall time, %.1f s sampled (JavaScriptCore sampler, %u ms)",
                          final ? "final window: " : "window: ", wall_s, engine->profile_total_ms / 1000.0,
                          engine->profile_ms);
        prof_report(engine);
    } else if (final) {
        prof_report(engine);       /* natives timed, no samples */
    }
    uint64_t t3 = monotonic_us();
    error_handler_log(LOG_INFO, "[profile] overhead of profiling in this window: taking the samples %.0f ms (%zu bytes), "
                      "reading them %.0f ms, printing the report %.0f ms",
                      (double)(t1 - t0) / 1000.0, len, (double)(t2 - t1) / 1000.0, (double)(t3 - t2) / 1000.0);
    engine->profile_report_us = monotonic_us();
}

void js_engine_profile_tick(JSEngine *engine)
{
    if (!engine || !engine->profile_ms) return;
    uint64_t now = monotonic_us();
    if (engine->profile_report_us == 0) {
        engine->profile_report_us = now;
        return;
    }
    if (now - engine->profile_report_us < 10000000u) return;
    jsc_profile_report(engine, 0);
}

void js_engine_profile_flush(JSEngine *engine)
{
    if (!engine || !engine->profile_ms || engine->profile_report_us == 0) return;
    jsc_profile_report(engine, 1);
}
#else
void js_engine_profile_tick(JSEngine *engine) { (void)engine; }
void js_engine_profile_flush(JSEngine *engine) { (void)engine; }
#endif

/* Stall watchdog (with --profile): the sampler above only runs between bytecode instructions, so a long stretch inside
   native code shows up as a single sample charged to whatever ran next. This thread notices that the script thread has
   not reached the sampler for a while and says which native scopes (RMMZ_PROF_SCOPE) it is inside. Scopes that are
   not instrumented (JSON.parse, String.replace, the collector, ...) show as "none". */
#include <pthread.h>
#include <unistd.h>

static void *stall_watchdog(void *arg)
{
    JSEngine *engine = arg;
    int stalled = 0;
    uint64_t episode_start = 0, last_report = 0;
    for (;;) {
        usleep(500 * 1000);
        uint64_t last = engine->profile_last_us;
        uint64_t frame = rmmz_frame_us;
        if (frame > last) last = frame;     /* either the frame loop or the sampler advancing counts as progress */
        if (last == 0) continue;            /* nothing has run yet (loading) */
        uint64_t now = monotonic_us();
        uint64_t gap = now > last ? now - last : 0;
        if (gap >= 2000000u) {
            if (!stalled) { stalled = 1; episode_start = last; last_report = 0; }
            if (now - last_report >= 5000000u) {
                char where[256];
                where[0] = '\0';
                int depth = rmmz_prof_depth;
                for (int i = 0; i < depth && i < 8; i++) {
                    const char *n = rmmz_prof_active[i];
                    if (!n) continue;
                    size_t used = strlen(where);
                    snprintf(where + used, sizeof where - used, "%s%s", used ? " > " : "", n);
                }
                const char *nn = rmmz_native_name;
                uint64_t since = rmmz_native_since_us;
                char call[96];
                call[0] = '\0';
                if (nn && since && now > since)
                    snprintf(call, sizeof call, "; in script-called native %s for %.1f s", nn, (double)(now - since) / 1e6);
                printf("[stall] no frame or script progress for %.1f s; inside native: %s (depth %d)%s\n",
                       (double)gap / 1e6, where[0] ? where : "none", depth, call);
                fflush(stdout);
                last_report = now;
            }
        } else if (stalled) {
            printf("[stall] ended after about %.1f s\n", (double)(last - episode_start) / 1e6);
            fflush(stdout);
            stalled = 0;
        }
    }
    return NULL;
}

void js_engine_set_profile(JSEngine *engine, unsigned interval_ms)
{
    if (!engine) return;
    engine->profile_ms = interval_ms;
    rmmz_prof_enabled = interval_ms > 0;
    engine->profile_last_us = 0;
#ifdef RMMZ_JS_JSC
    if (interval_ms) {
        if (rmmz_jsc_profiler_start(engine->rt))
            error_handler_log(LOG_INFO, "JavaScriptCore sampling profiler started");
        else
            error_handler_log(LOG_WARN, "JavaScriptCore sampling profiler is not available in this build");
    }
#endif
    JS_SetInterruptHandler(engine->rt, (interval_ms || engine->watchdog_ms) ? watchdog_interrupt : NULL, engine);
}

void js_engine_watchdog_arm(JSEngine *engine)
{
    if (engine && engine->watchdog_ms) engine->watchdog_armed_ms = monotonic_ms();
}

/* console.log / console.warn / console.error; `magic` selects the level (0/1/2). */
static JSValue js_console_output(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic)
{
    (void)this_val;

    static const char *levels[] = { "log", "warn", "error" };
    const char *level = levels[magic < 0 || magic > 2 ? 0 : magic];

    JSEngine *engine = JS_GetContextOpaque(ctx);

    size_t total_len = 0;
    const char **parts = NULL;
    size_t *part_lens = NULL;

    if (argc > 0) {
        parts = malloc(sizeof(char *) * (size_t)argc);
        part_lens = malloc(sizeof(size_t) * (size_t)argc);
        if (!parts || !part_lens) {
            free(parts);
            free(part_lens);
            return JS_UNDEFINED;
        }
    }

    for (int i = 0; i < argc; i++) {
        size_t len = 0;
        parts[i] = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!parts[i]) {
            /* toString threw; clear the pending exception so console.* never throws. */
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        part_lens[i] = len;
        total_len += len;
        if (i > 0) total_len++;
    }

    char *msg = malloc(total_len + 1);
    if (msg) {
        size_t pos = 0;
        for (int i = 0; i < argc; i++) {
            if (i > 0) msg[pos++] = ' ';
            if (parts[i]) {
                memcpy(msg + pos, parts[i], part_lens[i]);
                pos += part_lens[i];
            }
        }
        msg[pos] = '\0';
    }

    if (msg && magic >= 1) {
        /* warnings and errors from the game, its plugins and the shims end up in the compatibility table; digit
           runs are folded so "tile 12" and "tile 13" count as one problem */
        char key[160];
        size_t k = 0;
        for (const char *p = msg; *p && k + 1 < sizeof(key); p++) {
            if (*p >= '0' && *p <= '9') {
                if (k == 0 || key[k - 1] != '#') key[k++] = '#';
            } else {
                key[k++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
            }
        }
        key[k] = '\0';
        int has_letter = 0;
        for (size_t i = 0; i < k; i++)
            if ((key[i] >= 'a' && key[i] <= 'z') || (key[i] >= 'A' && key[i] <= 'Z')) { has_letter = 1; break; }
        if (has_letter) compat_report(magic == 2 ? "console-error" : "console-warn", key, "%s", msg);
    }

    if (engine && engine->console_cb) {
        engine->console_cb(level, msg ? msg : "", engine->console_userdata);
    } else {
        FILE *out = (magic == 2) ? stderr : stdout;
        if (magic == 1)
            fprintf(out, "[WARN] %s\n", msg ? msg : "");
        else if (magic == 2)
            fprintf(out, "[ERROR] %s\n", msg ? msg : "");
        else
            fprintf(out, "%s\n", msg ? msg : "");
    }

    free(msg);
    for (int i = 0; i < argc; i++) {
        JS_FreeCString(ctx, parts[i]);
    }
    free(parts);
    free(part_lens);

    return JS_UNDEFINED;
}

/* __compat_report(category, key[, detail]): the shims' way into the compatibility log (see error_handler.h). */
static JSValue js_compat_report(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    const char *cat = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    const char *key = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    const char *detail = argc > 2 ? JS_ToCString(ctx, argv[2]) : NULL;
    compat_report(cat ? cat : "misc", key ? key : "", "%s", detail ? detail : "");
    if (cat) JS_FreeCString(ctx, cat);
    if (key) JS_FreeCString(ctx, key);
    if (detail) JS_FreeCString(ctx, detail);
    return JS_UNDEFINED;
}

static void js_register_console(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, console, "log",
        JS_NewCFunctionMagic(ctx, js_console_output, "log", 0,
                             JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, console, "warn",
        JS_NewCFunctionMagic(ctx, js_console_output, "warn", 0,
                             JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, console, "error",
        JS_NewCFunctionMagic(ctx, js_console_output, "error", 0,
                             JS_CFUNC_generic_magic, 2));

    JS_SetPropertyStr(ctx, global, "console", console);
    JS_SetPropertyStr(ctx, global, "__compat_report",
        JS_NewCFunction(ctx, js_compat_report, "__compat_report", 3));
    JS_FreeValue(ctx, global);
}

/* One compatibility-log entry per kind of error and throw site: the message with digit runs folded, plus the first
   stack frame that names a source position (engine formats differ: "at f (file:12)" / "f@file:12:3"). */
static void compat_report_exception(const char *category, const char *msg, const char *stack)
{
    char key[240];
    size_t k = 0;
    for (const char *p = msg ? msg : ""; *p && k < 100; p++) {
        if (*p >= '0' && *p <= '9') {
            if (k == 0 || key[k - 1] != '#') key[k++] = '#';
        } else {
            key[k++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
        }
    }
    const char *frame = NULL;
    size_t frame_len = 0;
    for (const char *line = stack; line && *line;) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        while (len && (*line == ' ' || *line == '\t')) { line++; len--; }
        int has_pos = 0;
        for (size_t i = 0; i + 1 < len; i++)
            if (line[i] == ':' && line[i + 1] >= '0' && line[i + 1] <= '9') { has_pos = 1; break; }
        int native = 0;
        for (size_t i = 0; i + 13 <= len; i++)
            if (strncmp(line + i, "[native code]", 13) == 0) { native = 1; break; }
        if (has_pos && !native) { frame = line; frame_len = len; break; }
        if (!eol) break;
        line = eol + 1;
    }
    if (frame_len > 110) frame_len = 110;
    k += (size_t)snprintf(key + k, sizeof(key) - k, " @ %.*s", frame ? (int)frame_len : 1, frame ? frame : "?");
    compat_report(category, key, "%s", msg ? msg : "");
}

/* Print the pending exception and stack to stderr, or to the console callback. */
static void js_dump_exception(JSEngine *engine, JSContext *ctx)
{
    JSValue exc = JS_GetException(ctx);

    const char *str = JS_ToCString(ctx, exc);
    char msg_copy[256];
    snprintf(msg_copy, sizeof(msg_copy), "%s", str ? str : "");
    int reported = 0;
    if (str) {
        if (engine && engine->console_cb) {
            engine->console_cb("error", str, engine->console_userdata);
        } else {
            fprintf(stderr, "JS Exception: %s\n", str);
        }
        JS_FreeCString(ctx, str);
    }

    if (JS_IsError(exc)) {
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (!JS_IsUndefined(stack)) {
            const char *stack_str = JS_ToCString(ctx, stack);
            if (stack_str) {
                compat_report_exception("exception", msg_copy, stack_str);
                reported = 1;
                if (engine && engine->console_cb) {
                    engine->console_cb("error", stack_str, engine->console_userdata);
                } else {
                    fprintf(stderr, "Stack:\n%s\n", stack_str);
                }
                JS_FreeCString(ctx, stack_str);
            }
        }
        JS_FreeValue(ctx, stack);
    }
    if (!reported) compat_report_exception("exception", msg_copy, NULL);

    JS_FreeValue(ctx, exc);
}

/* Unhandled promise rejection tracking */

static void promise_rejection_tracker(JSContext *ctx, JSValueConst promise,
                                      JSValueConst reason, bool is_handled,
                                      void *opaque)
{
    JSEngine *engine = opaque;
    if (!engine) return;

    if (is_handled) {
        /* A handler was attached after the fact — retract the pending report. */
        for (int i = 0; i < engine->rej_count; i++) {
            if (JS_VALUE_GET_PTR(engine->rej_promises[i]) == JS_VALUE_GET_PTR(promise)) {
                JS_FreeValue(ctx, engine->rej_promises[i]);
                JS_FreeValue(ctx, engine->rej_reasons[i]);
                engine->rej_count--;
                engine->rej_promises[i] = engine->rej_promises[engine->rej_count];
                engine->rej_reasons[i]  = engine->rej_reasons[engine->rej_count];
                return;
            }
        }
        return;
    }

    if (engine->rej_count >= engine->rej_cap) {
        int ncap = engine->rej_cap ? engine->rej_cap * 2 : 8;
        JSValue *np = realloc(engine->rej_promises, sizeof(JSValue) * (size_t)ncap);
        if (!np) return;
        engine->rej_promises = np;
        JSValue *nr = realloc(engine->rej_reasons, sizeof(JSValue) * (size_t)ncap);
        if (!nr) return;                 /* rej_cap unchanged, so the larger array is harmless */
        engine->rej_reasons = nr;
        engine->rej_cap = ncap;
    }
    engine->rej_promises[engine->rej_count] = JS_DupValue(ctx, promise);
    engine->rej_reasons[engine->rej_count]  = JS_DupValue(ctx, reason);
    engine->rej_count++;
}

/* Report every rejection still unhandled via the shim's
   __error_handler_dispatchRejection, so the game's `unhandledrejection` listener fires. */
static void flush_unhandled_rejections(JSEngine *engine)
{
    if (!engine || engine->rej_count == 0) return;
    JSContext *ctx = engine->ctx;

    /* Detach the list first: dispatching may reject further promises. */
    int n = engine->rej_count;
    JSValue *promises = engine->rej_promises;
    JSValue *reasons  = engine->rej_reasons;
    engine->rej_promises = NULL;
    engine->rej_reasons  = NULL;
    engine->rej_count = 0;
    engine->rej_cap   = 0;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "__error_handler_dispatchRejection");
    bool have_fn = JS_IsFunction(ctx, fn);

    for (int i = 0; i < n; i++) {
        if (have_fn) {
            JSValue ret = JS_Call(ctx, fn, global, 1, &reasons[i]);
            if (JS_IsException(ret)) js_dump_exception(engine, ctx);
            JS_FreeValue(ctx, ret);
        } else {
            const char *s = JS_ToCString(ctx, reasons[i]);
            if (!s) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                s = "<unprintable reason>";
            }
            if (engine->console_cb) {
                char msg[512];
                snprintf(msg, sizeof(msg), "Unhandled promise rejection: %s", s);
                engine->console_cb("error", msg, engine->console_userdata);
            } else {
                fprintf(stderr, "[ERROR] Unhandled promise rejection: %s\n", s);
            }
            if (strcmp(s, "<unprintable reason>") != 0) JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, promises[i]);
        JS_FreeValue(ctx, reasons[i]);
    }

    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
    free(promises);
    free(reasons);
}

JSEngine *js_engine_init(void)
{
#if defined(RMMZ_PS5) && !defined(RMMZ_JS_JSC)
    /* The console's malloc is several times slower than a PC's for the small, short-lived blocks QuickJS
       allocates all the time (see js_pool_alloc.h). RMMZ_SYSTEM_MALLOC=1 switches back, for comparison. */
    const char *sys_malloc = getenv("RMMZ_SYSTEM_MALLOC");
    JSRuntime *rt = (sys_malloc && *sys_malloc == '1') ? JS_NewRuntime()
                                                        : JS_NewRuntime2(&js_pool_malloc_functions, NULL);
#else
    JSRuntime *rt = JS_NewRuntime();
#endif
    if (!rt) {
        fprintf(stderr, "Failed to create JS runtime\n");
        return NULL;
    }

    JS_SetMemoryLimit(rt, JS_MEMORY_LIMIT);
    JS_SetMaxStackSize(rt, JS_STACK_SIZE);
    /* dynamic import() and static import in a plugin (Plug2in loads libPlug2in.js this way): modules come from files
       next to the importing script, names resolved the usual way */
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    {
        /* Experiment: RMMZ_GC_THRESHOLD_MB raises the size at which the cycle collector first runs. */
        const char *gc = getenv("RMMZ_GC_THRESHOLD_MB");
        if (gc && atoi(gc) > 0) JS_SetGCThreshold(rt, (size_t)atoi(gc) * 1024 * 1024);
    }

    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        fprintf(stderr, "Failed to create JS context\n");
        JS_FreeRuntime(rt);
        return NULL;
    }

    JSEngine *engine = calloc(1, sizeof(JSEngine));
    if (!engine) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return NULL;
    }
    engine->rt = rt;
    engine->ctx = ctx;

    JS_SetContextOpaque(ctx, engine);

    /* Without a tracker QuickJS drops unhandled rejections silently. */
    JS_SetHostPromiseRejectionTracker(rt, promise_rejection_tracker, engine);

    js_register_console(ctx);

    {
        pthread_t wd;
        if (pthread_create(&wd, NULL, stall_watchdog, engine) == 0) pthread_detach(wd);
    }
    return engine;
}

void js_engine_shutdown(JSEngine *engine)
{
    if (!engine) return;
    if (engine->ctx) {
        for (int i = 0; i < engine->rej_count; i++) {
            JS_FreeValue(engine->ctx, engine->rej_promises[i]);
            JS_FreeValue(engine->ctx, engine->rej_reasons[i]);
        }
    }
    free(engine->rej_promises);
    free(engine->rej_reasons);
    if (engine->ctx) JS_FreeContext(engine->ctx);
    if (engine->rt)  JS_FreeRuntime(engine->rt);
    free(engine);
}

bool js_engine_eval(JSEngine *engine, const char *script, const char *filename)
{
    if (!engine || !script) return false;
    const char *fname = filename ? filename : "<eval>";

    js_engine_watchdog_arm(engine);
    JSValue result = JS_Eval(engine->ctx, script, strlen(script),
                             fname, JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(result)) {
        js_dump_exception(engine, engine->ctx);
        return false;
    }

    JS_FreeValue(engine->ctx, result);
    return true;
}

char *js_engine_eval_string(JSEngine *engine, const char *script, const char *filename)
{
    if (!engine || !script) return NULL;
    const char *fname = filename ? filename : "<eval>";

    JSValue result = JS_Eval(engine->ctx, script, strlen(script),
                             fname, JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(result)) {
        js_dump_exception(engine, engine->ctx);
        return NULL;
    }

    const char *cstr = JS_ToCString(engine->ctx, result);
    JS_FreeValue(engine->ctx, result);

    if (!cstr) return NULL;

    char *copy = strdup(cstr);
    JS_FreeCString(engine->ctx, cstr);
    return copy;
}

void js_engine_free_string(char *str)
{
    free(str);
}

bool js_engine_eval_file(JSEngine *engine, const char *filepath)
{
    if (!engine || !filepath) return false;

    js_engine_watchdog_arm(engine);
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open file: %s\n", filepath);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        fprintf(stderr, "Failed to determine file size: %s\n", filepath);
        return false;
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        fprintf(stderr, "Out of memory loading file: %s\n", filepath);
        return false;
    }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';

    bool ok = js_engine_eval(engine, buf, filepath);
    free(buf);
    return ok;
}

/* Bytecode caching: <file>.js -> <file>.jsc, prefixed with the source mtime. */

/* Append 'c' to the JS file path (.js -> .jsc). */
static char *make_cache_path(const char *filepath)
{
    size_t len = strlen(filepath);
    char *cache_path = malloc(len + 2);
    if (!cache_path) return NULL;
    memcpy(cache_path, filepath, len);
    cache_path[len] = 'c';
    cache_path[len + 1] = '\0';
    return cache_path;
}

/* Returns 0 and the file's mtime on success, -1 if the file is missing. */
static int get_file_mtime(const char *path, int64_t *mtime)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *mtime = (int64_t)st.st_mtime;
    return 0;
}

/* Read a whole file into a NUL-terminated buffer. Returns NULL on failure. */
static uint8_t *read_file_binary(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }

    uint8_t *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';
    *out_size = read;
    return buf;
}

static bool write_file_binary(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    return written == size;
}

bool js_engine_eval_file_cached(JSEngine *engine, const char *filepath)
{
    if (!engine || !filepath) return false;
#ifdef RMMZ_JS_JSC
    /* JavaScriptCore's C API cannot serialise bytecode. */
    return js_engine_eval_file(engine, filepath);
#endif

    char *cache_path = make_cache_path(filepath);
    if (!cache_path) return js_engine_eval_file(engine, filepath);

    /* Use the cache only if its stored source mtime matches the current one. */
    int64_t src_mtime = 0, cache_mtime = 0;
    if (get_file_mtime(filepath, &src_mtime) == 0 &&
        get_file_mtime(cache_path, &cache_mtime) == 0) {
        size_t cache_size = 0;
        uint8_t *cache_data = read_file_binary(cache_path, &cache_size);
        if (cache_data && cache_size > sizeof(int64_t)) {
            int64_t stored_mtime;
            memcpy(&stored_mtime, cache_data, sizeof(int64_t));
            if (stored_mtime == src_mtime) {
                JSValue obj = JS_ReadObject(engine->ctx,
                    cache_data + sizeof(int64_t),
                    cache_size - sizeof(int64_t),
                    JS_READ_OBJ_BYTECODE);
                free(cache_data);

                if (!JS_IsException(obj)) {
                    JSValue result = JS_EvalFunction(engine->ctx, obj);
                    if (JS_IsException(result)) {
                        js_dump_exception(engine, engine->ctx);
                        free(cache_path);
                        return false;
                    }
                    JS_FreeValue(engine->ctx, result);
                    free(cache_path);
                    return true;
                }
                /* Unreadable cache: consume the exception and recompile from source. */
                JS_FreeValue(engine->ctx, obj);
                {
                    JSValue exc = JS_GetException(engine->ctx);
                    JS_FreeValue(engine->ctx, exc);
                }
            } else {
                free(cache_data);
            }
        } else {
            free(cache_data);
        }
    }

    size_t src_size = 0;
    uint8_t *src_data = read_file_binary(filepath, &src_size);
    if (!src_data) {
        fprintf(stderr, "Failed to open file: %s\n", filepath);
        free(cache_path);
        return false;
    }

    JSValue obj = JS_Eval(engine->ctx, (const char *)src_data, src_size,
                          filepath, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    free(src_data);

    if (JS_IsException(obj)) {
        js_dump_exception(engine, engine->ctx);
        free(cache_path);
        return false;
    }

    /* Write the cache: source mtime header followed by the bytecode. */
    size_t bc_size = 0;
    uint8_t *bc_data = JS_WriteObject(engine->ctx, &bc_size, obj, JS_WRITE_OBJ_BYTECODE);
    if (bc_data && bc_size > 0) {
        size_t total = sizeof(int64_t) + bc_size;
        uint8_t *full_cache = malloc(total);
        if (full_cache) {
            memcpy(full_cache, &src_mtime, sizeof(int64_t));
            memcpy(full_cache + sizeof(int64_t), bc_data, bc_size);
            write_file_binary(cache_path, full_cache, total);
            free(full_cache);
        }
        js_free(engine->ctx, bc_data);
    }

    JSValue result = JS_EvalFunction(engine->ctx, obj);
    free(cache_path);

    if (JS_IsException(result)) {
        js_dump_exception(engine, engine->ctx);
        return false;
    }

    JS_FreeValue(engine->ctx, result);
    return true;
}

int js_engine_execute_pending_jobs(JSEngine *engine)
{
    if (!engine) return -1;

    int count = 0;
    JSContext *pctx = NULL;
    while (JS_IsJobPending(engine->rt)) {
        int ret = JS_ExecutePendingJob(engine->rt, &pctx);
        if (ret < 0) {
            js_dump_exception(engine, pctx ? pctx : engine->ctx);
            flush_unhandled_rejections(engine);
            return -1;
        }
        count += ret;
    }
    /* Microtask checkpoint: anything still unhandled is reported now. */
    flush_unhandled_rejections(engine);
    return count;
}

void js_engine_set_console_callback(JSEngine *engine, JSConsoleCallback cb, void *userdata)
{
    if (!engine) return;
    engine->console_cb = cb;
    engine->console_userdata = userdata;
}

JSContext *js_engine_get_context(JSEngine *engine)
{
    if (!engine) return NULL;
    return engine->ctx;
}
