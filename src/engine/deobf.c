/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the PS5 port contributors */
/*
 * Plugin de-obfuscation (PS5 port). See shims/deobf.js for what it does and why: plugins obfuscated with the
 * obfuscator.io "string array" scheme call a decoder function for every property name, which makes them several
 * times slower than they need to be. Before a plugin is evaluated, it is run once in a scratch context to get at
 * its decoder, and the decoder calls with literal arguments are replaced by the strings they return.
 */

#include "engine/js_engine.h"
#include "engine/error_handler.h"

#include <quickjs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_whole_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    if (len) *len = n;
    return buf;
}

/* Obfuscated code is full of "_0x" identifiers; a plain plugin has none or a few. */
static int looks_obfuscated(const char *src, size_t len)
{
    int hits = 0;
    for (const char *p = src, *end = src + len; (p = memchr(p, '_', (size_t)(end - p))) != NULL; p++) {
        if (end - p > 3 && p[1] == '0' && p[2] == 'x' && ++hits >= 300) return 1;
    }
    return 0;
}

static uint64_t now_ms(void)
{
    return rmmz_prof_now_us() / 1000;
}

/* Returns a malloc'ed replacement for src, or NULL when nothing could be replaced. */
static char *deobfuscate(JSEngine *engine, const char *src, size_t len, const char *shim_dir, const char *name)
{
    char script_path[1024];
    snprintf(script_path, sizeof script_path, "%s/deobf.js", shim_dir);
    size_t script_len = 0;
    char *script = read_whole_file(script_path, &script_len);
    if (!script) return NULL;

    JSContext *main_ctx = js_engine_get_context(engine);
    JSRuntime *rt = JS_GetRuntime(main_ctx);
    JSContext *sc = JS_NewContext(rt);
    if (!sc) { free(script); return NULL; }

    uint64_t t0 = now_ms();
    char *result = NULL;

    /* the analysis code first, before the plugin can change any built-ins */
    JSValue v = JS_Eval(sc, script, script_len, "<deobf>", JS_EVAL_TYPE_GLOBAL);
    free(script);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(sc);
        JS_FreeValue(sc, exc);
        JS_FreeValue(sc, v);
        JS_FreeContext(sc);
        return NULL;
    }
    JS_FreeValue(sc, v);

    /* the plugin itself: its decoder is defined afterwards; the body usually stops soon at a global the bare
       context does not have, which is fine */
    v = JS_Eval(sc, src, len, name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(sc);
        JS_FreeValue(sc, exc);
    }
    JS_FreeValue(sc, v);

    JSValue global = JS_GetGlobalObject(sc);
    JSValue fn = JS_GetPropertyStr(sc, global, "__deobf");
    if (JS_IsFunction(sc, fn)) {
        JSValue arg = JS_NewStringLen(sc, src, len);
        JSValue out = JS_Call(sc, fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(sc, arg);
        if (JS_IsString(out)) {
            size_t out_len = 0;
            const char *s = JS_ToCStringLen(sc, &out_len, out);
            if (s) {
                result = malloc(out_len + 1);
                if (result) {
                    memcpy(result, s, out_len);
                    result[out_len] = '\0';
                }
                JS_FreeCString(sc, s);
            }
        } else if (JS_IsException(out)) {
            JSValue exc = JS_GetException(sc);
            JS_FreeValue(sc, exc);
        }
        JS_FreeValue(sc, out);
    }
    JSValue count = JS_GetPropertyStr(sc, global, "__deobf_count");
    int32_t replaced = 0;
    JS_ToInt32(sc, &replaced, count);
    JS_FreeValue(sc, count);
    JS_FreeValue(sc, fn);
    JS_FreeValue(sc, global);
    JS_FreeContext(sc);
    JS_RunGC(rt);

    error_handler_log(LOG_INFO, "de-obfuscated %s: %d string lookups replaced (%llu ms)", name, (int)replaced,
                      (unsigned long long)(now_ms() - t0));
    return result;
}

bool deobf_eval_plugin_file(JSEngine *engine, const char *path, const char *shim_dir)
{
    if (!engine || !path) return false;
    size_t len = 0;
    char *src = read_whole_file(path, &len);
    if (!src) return js_engine_eval_file(engine, path);   /* prints the error */

    char *replaced = NULL;
    if (shim_dir && !getenv("RMMZ_NO_DEOBF") && looks_obfuscated(src, len))
        replaced = deobfuscate(engine, src, len, shim_dir, path);

    bool ok = js_engine_eval(engine, replaced ? replaced : src, path);
    free(replaced);
    free(src);
    return ok;
}
