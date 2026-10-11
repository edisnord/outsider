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

#ifndef _WIN32
#include <pthread.h>
#define RMMZ_LOG_THREAD 1
#endif

static FILE *s_log_file = NULL;
static SDL_Mutex *s_compat_lock = NULL;
static LogLevel s_min_level = LOG_DEBUG;
static LogCallback s_callback = NULL;
static void *s_callback_userdata = NULL;

static const char *s_level_names[] = {
    "DEBUG", "INFO", "WARN", "ERROR"
};


/* ---- asynchronous log output ---------------------------------------------------------------------------
   On the console a line written to stdout/stderr costs 50-150 ms, and more as a run goes on (see the "slow log call"
   notes in docs/tasks/outsider-fh2-log.md), so a 100-line profile report froze the game for 8 s, then 31 s, then
   minutes. With the writer thread on, error_handler_log() formats the line and queues it; the thread does the
   stdout/stderr write, the file write and the callback. The queue never blocks the caller: when it is full the
   message is dropped and counted. On by default in the embedded (console) build; RMMZ_LOG_SYNC=1 turns it off and
   RMMZ_LOG_ASYNC=1 turns it on elsewhere. Order is kept. The callback runs on the writer thread. */
#ifdef RMMZ_LOG_THREAD
#define LOGQ_CAP 4096
typedef struct { char *text; char ts[32]; int level; } LogQItem;
static LogQItem s_logq[LOGQ_CAP];
static int s_logq_head, s_logq_count;
static unsigned long long s_logq_dropped, s_logq_reported_dropped;
static pthread_mutex_t s_logq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_logq_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t s_logq_idle = PTHREAD_COND_INITIALIZER;
static pthread_t s_logq_thread;
static volatile int s_async;        /* queueing is on */
static int s_logq_running, s_logq_busy, s_logq_stop;

static long long log_now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* One line to every sink. The file is flushed by the caller (once per batch, not per line: on the console each
   flush is a write that costs 50-300 ms, and a profile report is 100 lines). Slow sinks are named, a few times. */
static void log_emit(int level, const char *timestamp, const char *msg)
{
    static int slow_reported;
    const char *level_name = error_handler_level_name((LogLevel)level);
    FILE *out = (level >= LOG_WARN) ? stderr : stdout;
    long long t0 = log_now_us();
    fprintf(out, "[%s] [%s] %s\n", timestamp, level_name, msg);
    long long t1 = log_now_us();
    if (s_log_file) fprintf(s_log_file, "[%s] [%s] %s\n", timestamp, level_name, msg);
    long long t2 = log_now_us();
    if (s_callback) s_callback((LogLevel)level, msg, s_callback_userdata);
    long long t3 = log_now_us();
    if (t3 - t0 > 100000 && slow_reported < 5) {
        slow_reported++;
        fprintf(stderr, "error_handler: slow log write %lld ms: stdio %lld ms, file %lld ms, callback %lld ms\n",
                (t3 - t0) / 1000, (t1 - t0) / 1000, (t2 - t1) / 1000, (t3 - t2) / 1000);
    }
}

static void *logq_thread(void *arg)
{
    (void)arg;
    long long last_file_flush_us = 0;
    pthread_mutex_lock(&s_logq_lock);
    for (;;) {
        while (s_logq_count == 0 && !s_logq_stop) {
            s_logq_busy = 0;
            pthread_cond_broadcast(&s_logq_idle);
            pthread_cond_wait(&s_logq_cond, &s_logq_lock);
        }
        if (s_logq_count == 0 && s_logq_stop) break;
        LogQItem it = s_logq[s_logq_head];
        s_logq[s_logq_head].text = NULL;
        s_logq_head = (s_logq_head + 1) % LOGQ_CAP;
        s_logq_count--;
        unsigned long long dropped = s_logq_dropped - s_logq_reported_dropped;
        s_logq_reported_dropped = s_logq_dropped;
        s_logq_busy = 1;
        int behind = s_logq_count;
        pthread_mutex_unlock(&s_logq_lock);
        {
            static int backlog_reported;
            if (behind > 300 && backlog_reported < 4) {
                backlog_reported++;
                fprintf(stderr, "error_handler: log writer is %d messages behind\n", behind);
            }
        }

        if (dropped) {
            char note[96];
            snprintf(note, sizeof note, "log: %llu messages were dropped (the log queue was full)", dropped);
            log_emit(LOG_WARN, it.ts, note);
        }
        log_emit(it.level, it.ts, it.text);
        free(it.text);

        pthread_mutex_lock(&s_logq_lock);
        if (s_log_file && (s_logq_count == 0 || log_now_us() - last_file_flush_us > 500000)) {
            pthread_mutex_unlock(&s_logq_lock);
            long long f0 = log_now_us();
            fflush(s_log_file);
            last_file_flush_us = log_now_us();
            if (last_file_flush_us - f0 > 100000) {
                static int reported;
                if (reported++ < 5)
                    fprintf(stderr, "error_handler: log file flush took %lld ms\n", (last_file_flush_us - f0) / 1000);
            }
            pthread_mutex_lock(&s_logq_lock);
        }
    }
    s_logq_busy = 0;
    pthread_cond_broadcast(&s_logq_idle);
    pthread_mutex_unlock(&s_logq_lock);
    return NULL;
}

static void logq_start(void)
{
    if (s_logq_running) return;
    if (getenv("RMMZ_LOG_SYNC")) return;
#ifndef RMMZ_EMBEDDED
    if (!getenv("RMMZ_LOG_ASYNC")) return;
#endif
    s_logq_stop = 0;
    if (pthread_create(&s_logq_thread, NULL, logq_thread, NULL) == 0) {
        s_logq_running = 1;
        s_async = 1;
    }
}

/* Waits (up to timeout_ms) until everything queued so far has been written. */
void error_handler_flush(int timeout_ms)
{
    if (!s_logq_running) return;
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += timeout_ms / 1000;
    dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&s_logq_lock);
    while ((s_logq_count > 0 || s_logq_busy) &&
           pthread_cond_timedwait(&s_logq_idle, &s_logq_lock, &dl) == 0) {
    }
    pthread_mutex_unlock(&s_logq_lock);
}

static void logq_stop(void)
{
    if (!s_logq_running) return;
    error_handler_flush(10000);
    s_async = 0;                       /* later messages are written by the caller */
    pthread_mutex_lock(&s_logq_lock);
    s_logq_stop = 1;
    pthread_cond_broadcast(&s_logq_cond);
    int pending = s_logq_count;
    pthread_mutex_unlock(&s_logq_lock);
    if (pending == 0) pthread_join(s_logq_thread, NULL);
    else pthread_detach(s_logq_thread);   /* the sinks are stuck: do not hold the exit */
    s_logq_running = 0;
}

/* true when the message was queued */
static int logq_push(int level, const char *timestamp, const char *msg)
{
    char *copy = strdup(msg);
    if (!copy) return 0;
    pthread_mutex_lock(&s_logq_lock);
    if (s_logq_count >= LOGQ_CAP) {
        s_logq_dropped++;
        pthread_mutex_unlock(&s_logq_lock);
        free(copy);
        return 1;
    }
    LogQItem *it = &s_logq[(s_logq_head + s_logq_count) % LOGQ_CAP];
    it->text = copy;
    it->level = level;
    snprintf(it->ts, sizeof it->ts, "%s", timestamp);
    s_logq_count++;
    pthread_cond_signal(&s_logq_cond);
    pthread_mutex_unlock(&s_logq_lock);
    return 1;
}
#else
void error_handler_flush(int timeout_ms) { (void)timeout_ms; }
#endif

void error_handler_init(const char *log_file_path, LogLevel min_level)
{
    s_min_level = min_level;
    if (!s_compat_lock) s_compat_lock = SDL_CreateMutex();

    if (log_file_path) {
        s_log_file = fopen(log_file_path, "a");
        if (!s_log_file) {
            fprintf(stderr, "[ERROR] Failed to open log file: %s\n", log_file_path);
        }
    }
#ifdef RMMZ_LOG_THREAD
    logq_start();
#endif
}

void error_handler_shutdown(void)
{
    compat_summary();
#ifdef RMMZ_LOG_THREAD
    logq_stop();
#endif
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

#ifdef RMMZ_LOG_THREAD
    if (s_async && logq_push((int)level, timestamp, msg)) return;
#endif
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

/* ---- compatibility log ---- */

#define COMPAT_MAX 512
#define COMPAT_CAT 20
#define COMPAT_KEY 168
#define COMPAT_DETAIL 480

typedef struct {
    char cat[COMPAT_CAT];
    char key[COMPAT_KEY];
    char detail[COMPAT_DETAIL];
    unsigned long long count;
    unsigned long long next_log;
    unsigned long long first_frame;
    double first_s;
    int used;
} CompatEntry;

static CompatEntry s_compat[COMPAT_MAX];
static int s_compat_used;
static unsigned long long s_compat_total;
static unsigned long long s_compat_overflow;
static unsigned long long s_compat_frame;
static unsigned long long s_compat_printed_total;
static double s_compat_last_summary_s;

static double compat_now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    static double base = -1.0;
    double now = (double)t.tv_sec + (double)t.tv_nsec / 1e9;
    if (base < 0.0) base = now;
    return now - base;
}

static unsigned compat_hash(const char *cat, const char *key)
{
    unsigned h = 2166136261u;
    for (const char *p = cat; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    h = (h ^ 0xFFu) * 16777619u;
    for (const char *p = key; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return h;
}

void compat_set_frame(unsigned long long frame)
{
    s_compat_frame = frame;
}

void compat_report(const char *category, const char *key, const char *fmt, ...)
{
    char detail[COMPAT_DETAIL];
    detail[0] = '\0';
    if (fmt) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(detail, sizeof(detail), fmt, args);
        va_end(args);
    }
    if (!category) category = "misc";
    if (!key || !*key) key = detail;

    char cat[COMPAT_CAT], k[COMPAT_KEY];
    snprintf(cat, sizeof(cat), "%s", category);
    snprintf(k, sizeof(k), "%s", key);
    /* a key is one line */
    for (char *p = k; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    for (char *p = detail; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';

    int log_first = 0;
    int log_repeat = 0;
    unsigned long long count = 0;
    double now = compat_now_s();

    if (s_compat_lock) SDL_LockMutex(s_compat_lock);
    s_compat_total++;
    unsigned h = compat_hash(cat, k);
    CompatEntry *e = NULL;
    for (int i = 0; i < COMPAT_MAX; i++) {
        CompatEntry *c = &s_compat[(h + (unsigned)i) % COMPAT_MAX];
        if (!c->used) {
            if (s_compat_used >= COMPAT_MAX - 1) break;      /* keep one slot free; the table is full */
            c->used = 1;
            snprintf(c->cat, sizeof(c->cat), "%s", cat);
            snprintf(c->key, sizeof(c->key), "%s", k);
            snprintf(c->detail, sizeof(c->detail), "%s", detail);
            c->first_frame = s_compat_frame;
            c->first_s = now;
            c->next_log = 10;
            s_compat_used++;
            e = c;
            log_first = 1;
            break;
        }
        if (strcmp(c->cat, cat) == 0 && strcmp(c->key, k) == 0) { e = c; break; }
    }
    if (e) {
        e->count++;
        count = e->count;
        if (!log_first && e->count >= e->next_log) {
            e->next_log *= 10;
            log_repeat = 1;
        }
    } else {
        log_first = (++s_compat_overflow == 1);
    }
    if (s_compat_lock) SDL_UnlockMutex(s_compat_lock);

    if (log_first && e) {
        error_handler_log(LOG_WARN, "[compat] %s: %s%s%s (t=%.1fs frame %llu)", cat, k,
                          detail[0] && strcmp(detail, k) != 0 ? " -- " : "",
                          detail[0] && strcmp(detail, k) != 0 ? detail : "", now, s_compat_frame);
    } else if (log_first) {
        error_handler_log(LOG_WARN, "[compat] table full (%d entries); further new problems are only counted", COMPAT_MAX);
    } else if (log_repeat) {
        error_handler_log(LOG_WARN, "[compat] %s: %s x%llu", cat, k, count);
    }
}

static int compat_cmp(const void *a, const void *b)
{
    const CompatEntry *x = *(const CompatEntry *const *)a;
    const CompatEntry *y = *(const CompatEntry *const *)b;
    int c = strcmp(x->cat, y->cat);
    if (c) return c;
    if (x->count != y->count) return x->count < y->count ? 1 : -1;
    return strcmp(x->key, y->key);
}

static void compat_print(int limit)
{
    if (s_compat_lock) SDL_LockMutex(s_compat_lock);
    int n = 0;
    static const CompatEntry *list[COMPAT_MAX];
    unsigned long long total = s_compat_total;
    unsigned long long overflow = s_compat_overflow;
    for (int i = 0; i < COMPAT_MAX; i++)
        if (s_compat[i].used) list[n++] = &s_compat[i];
    qsort(list, (size_t)n, sizeof(list[0]), compat_cmp);
    s_compat_printed_total = total;
    s_compat_last_summary_s = compat_now_s();
    double now = s_compat_last_summary_s;

    error_handler_log(LOG_WARN, "[compat] ---- summary at %.0f s, frame %llu: %d distinct, %llu total%s ----", now,
                      s_compat_frame, n, total, overflow ? " (table was full)" : "");
    for (int i = 0; i < n && (limit <= 0 || i < limit); i++) {
        const CompatEntry *e = list[i];
        error_handler_log(LOG_WARN, "[compat] %8llux %-13s %s%s%s (first t=%.1fs frame %llu)", e->count, e->cat, e->key,
                          e->detail[0] && strcmp(e->detail, e->key) != 0 ? " -- " : "",
                          e->detail[0] && strcmp(e->detail, e->key) != 0 ? e->detail : "", e->first_s, e->first_frame);
    }
    if (limit > 0 && n > limit)
        error_handler_log(LOG_WARN, "[compat] ... and %d more entries (the full table is printed at shutdown)", n - limit);
    if (s_compat_lock) SDL_UnlockMutex(s_compat_lock);
}

void compat_summary(void)
{
    compat_print(0);
}

void compat_tick(void)
{
    if (s_compat_total == s_compat_printed_total) return;
    if (compat_now_s() - s_compat_last_summary_s < 60.0) return;
    compat_summary();
}
