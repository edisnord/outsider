/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "bindings/bind_io.h"
#include "io/file_io.h"
#include "stb_image.h" /* its zlib decoder: Node's zlib.inflate* for plugins */

#include <quickjs.h>
#include <stdlib.h>
#include <string.h>

#define PATH_BUF_SIZE 4096

/* Resolve a JS string argument to an absolute path in `out`. Returns the JS
   C-string (caller must JS_FreeCString) or NULL with a pending exception. */
static const char *resolve_path_arg(JSContext *ctx, JSValueConst arg,
                                    char *out, size_t out_size)
{
    const char *raw = JS_ToCString(ctx, arg);
    if (!raw) return NULL;

    if (!file_io_resolve_path(raw, out, out_size)) {
        JS_ThrowTypeError(ctx, "path resolution failed: %s", raw);
        JS_FreeCString(ctx, raw);
        return NULL;
    }
    return raw;
}

/* readFileSync(path) -> string */
static JSValue js_read_file_sync(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "readFileSync requires a path argument");

    char resolved[PATH_BUF_SIZE];
    const char *raw = resolve_path_arg(ctx, argv[0], resolved, sizeof(resolved));
    if (!raw) return JS_EXCEPTION;
    JS_FreeCString(ctx, raw);

    size_t size = 0;
    char *data = file_io_read_text(resolved, &size);
    if (!data) {
        return JS_ThrowInternalError(ctx, "failed to read file: %s", resolved);
    }

    JSValue result = JS_NewStringLen(ctx, data, size);
    free(data);
    return result;
}

/* readFileBinary(path) -> ArrayBuffer */
static JSValue js_read_file_binary(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "readFileBinary requires a path argument");

    char resolved[PATH_BUF_SIZE];
    const char *raw = resolve_path_arg(ctx, argv[0], resolved, sizeof(resolved));
    if (!raw) return JS_EXCEPTION;
    JS_FreeCString(ctx, raw);

    size_t size = 0;
    uint8_t *data = file_io_read_binary(resolved, &size);
    if (!data) {
        return JS_ThrowInternalError(ctx, "failed to read binary file: %s", resolved);
    }

    JSValue result = JS_NewArrayBufferCopy(ctx, data, size);
    free(data);
    return result;
}

/* writeFileSync(path, data) */
static JSValue js_write_file_sync(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "writeFileSync requires path and data arguments");

    char resolved[PATH_BUF_SIZE];
    const char *raw = resolve_path_arg(ctx, argv[0], resolved, sizeof(resolved));
    if (!raw) return JS_EXCEPTION;
    JS_FreeCString(ctx, raw);

    size_t data_len = 0;
    const char *data = JS_ToCStringLen(ctx, &data_len, argv[1]);
    if (!data) return JS_EXCEPTION;

    /* Write with explicit length: strings may contain null bytes (e.g. deflated save data). */
    bool ok = file_io_write_binary(resolved, (const uint8_t *)data, data_len);
    JS_FreeCString(ctx, data);

    if (!ok) {
        return JS_ThrowInternalError(ctx, "failed to write file: %s", resolved);
    }
    return JS_UNDEFINED;
}

/* writeFileBinary(path, arrayBuffer) */
static JSValue js_write_file_binary(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "writeFileBinary requires path and data arguments");

    char resolved[PATH_BUF_SIZE];
    const char *raw = resolve_path_arg(ctx, argv[0], resolved, sizeof(resolved));
    if (!raw) return JS_EXCEPTION;
    JS_FreeCString(ctx, raw);

    size_t size = 0;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &size, argv[1]);
    if (!buf) {
        return JS_ThrowTypeError(ctx, "writeFileBinary: second argument must be an ArrayBuffer");
    }

    bool ok = file_io_write_binary(resolved, buf, size);
    if (!ok) {
        return JS_ThrowInternalError(ctx, "failed to write binary file: %s", resolved);
    }
    return JS_UNDEFINED;
}

/* mkdirSync(path) -> bool */
static JSValue js_mkdir_sync(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "mkdirSync requires a path argument");

    char resolved[PATH_BUF_SIZE];
    const char *raw = resolve_path_arg(ctx, argv[0], resolved, sizeof(resolved));
    if (!raw) return JS_EXCEPTION;
    JS_FreeCString(ctx, raw);

    return JS_NewBool(ctx, file_io_mkdir(resolved));
}

/* existsSync(path) -> bool */
static JSValue js_exists_sync(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "existsSync requires a path argument");

    char resolved[PATH_BUF_SIZE];
    const char *raw = resolve_path_arg(ctx, argv[0], resolved, sizeof(resolved));
    if (!raw) return JS_EXCEPTION;
    JS_FreeCString(ctx, raw);

    return JS_NewBool(ctx, file_io_exists(resolved));
}

/* isDirSync(path) -> bool */
static JSValue js_is_dir_sync(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "isDirSync requires a path argument");

    char resolved[PATH_BUF_SIZE];
    const char *raw = resolve_path_arg(ctx, argv[0], resolved, sizeof(resolved));
    if (!raw) return JS_EXCEPTION;
    JS_FreeCString(ctx, raw);

    return JS_NewBool(ctx, file_io_is_directory(resolved));
}

/* readdirSync(path) -> string[] */
static JSValue js_readdir_sync(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "readdirSync requires a path argument");

    char resolved[PATH_BUF_SIZE];
    const char *raw = resolve_path_arg(ctx, argv[0], resolved, sizeof(resolved));
    if (!raw) return JS_EXCEPTION;
    JS_FreeCString(ctx, raw);

    size_t count = 0;
    char **entries = file_io_readdir(resolved, &count);
    if (!entries) {
        return JS_ThrowInternalError(ctx, "failed to read directory: %s", resolved);
    }

    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < count; i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                             JS_NewString(ctx, entries[i]));
    }
    file_io_free_dirlist(entries);
    return arr;
}

/* unlinkSync(path) -> bool */
static JSValue js_unlink_sync(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "unlinkSync requires a path argument");

    char resolved[PATH_BUF_SIZE];
    const char *raw = resolve_path_arg(ctx, argv[0], resolved, sizeof(resolved));
    if (!raw) return JS_EXCEPTION;
    JS_FreeCString(ctx, raw);

    return JS_NewBool(ctx, file_io_unlink(resolved));
}

/* renameSync(oldPath, newPath) -> bool */
static JSValue js_rename_sync(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "renameSync requires oldPath and newPath arguments");

    char resolved_old[PATH_BUF_SIZE];
    const char *raw_old = resolve_path_arg(ctx, argv[0], resolved_old, sizeof(resolved_old));
    if (!raw_old) return JS_EXCEPTION;
    JS_FreeCString(ctx, raw_old);

    char resolved_new[PATH_BUF_SIZE];
    const char *raw_new = resolve_path_arg(ctx, argv[1], resolved_new, sizeof(resolved_new));
    if (!raw_new) return JS_EXCEPTION;
    JS_FreeCString(ctx, raw_new);

    return JS_NewBool(ctx, file_io_rename(resolved_old, resolved_new));
}

/* getGameRoot() -> string */
static JSValue js_get_game_root(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewString(ctx, file_io_get_game_root());
}

/* resolvePath(path) -> string */
static JSValue js_resolve_path(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "resolvePath requires a path argument");

    char resolved[PATH_BUF_SIZE];
    const char *raw = resolve_path_arg(ctx, argv[0], resolved, sizeof(resolved));
    if (!raw) return JS_EXCEPTION;
    JS_FreeCString(ctx, raw);

    return JS_NewString(ctx, resolved);
}

/* inflate(data, mode) -> ArrayBuffer. mode: 0 zlib, 1 raw deflate, 2 gzip, 3 zlib or gzip (by its header).
   Used by the Node.js zlib shim (zlib.inflateSync, gunzipSync, ...). */
static JSValue js_inflate(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "inflate requires data");

    size_t size = 0;
    uint8_t *in = JS_GetArrayBuffer(ctx, &size, argv[0]);
    if (!in) return JS_EXCEPTION;

    int mode = 3;
    if (argc > 1) JS_ToInt32(ctx, &mode, argv[1]);

    const uint8_t *p = in;
    size_t n = size;
    int header = mode == 1 ? 0 : 1;

    if (mode == 2 || (mode == 3 && n >= 2 && p[0] == 0x1f && p[1] == 0x8b)) {
        if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) {
            return JS_ThrowInternalError(ctx, "incorrect header check");
        }
        size_t pos = 10;
        int flg = p[3];
        if (flg & 4) {
            if (pos + 2 > n) return JS_ThrowInternalError(ctx, "unexpected end of file");
            pos += 2 + (size_t)(p[pos] | (p[pos + 1] << 8));
        }
        if (flg & 8) { while (pos < n && p[pos]) pos++; pos++; }
        if (flg & 16) { while (pos < n && p[pos]) pos++; pos++; }
        if (flg & 2) pos += 2;
        if (pos >= n) return JS_ThrowInternalError(ctx, "unexpected end of file");
        p += pos;
        n -= pos;
        header = 0;
    }

    int outlen = 0;
    char *dec = stbi_zlib_decode_malloc_guesssize_headerflag(
        (const char *)p, (int)n, (int)(n * 4 + 64), &outlen, header);
    if (!dec) return JS_ThrowInternalError(ctx, "incorrect header check");

    JSValue result = JS_NewArrayBufferCopy(ctx, (const uint8_t *)dec, (size_t)outlen);
    free(dec);
    return result;
}

/* Registration */

static const JSCFunctionListEntry js_io_funcs[] = {
    JS_CFUNC_DEF("readFileSync",    1, js_read_file_sync),
    JS_CFUNC_DEF("readFileBinary",  1, js_read_file_binary),
    JS_CFUNC_DEF("writeFileSync",   2, js_write_file_sync),
    JS_CFUNC_DEF("writeFileBinary", 2, js_write_file_binary),
    JS_CFUNC_DEF("mkdirSync",       1, js_mkdir_sync),
    JS_CFUNC_DEF("existsSync",      1, js_exists_sync),
    JS_CFUNC_DEF("isDirSync",       1, js_is_dir_sync),
    JS_CFUNC_DEF("readdirSync",     1, js_readdir_sync),
    JS_CFUNC_DEF("unlinkSync",      1, js_unlink_sync),
    JS_CFUNC_DEF("renameSync",      2, js_rename_sync),
    JS_CFUNC_DEF("getGameRoot",     0, js_get_game_root),
    JS_CFUNC_DEF("resolvePath",     1, js_resolve_path),
    JS_CFUNC_DEF("inflate",         2, js_inflate),
};

void bind_io_register(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue io_obj = JS_NewObject(ctx);

    JS_SetPropertyFunctionList(ctx, io_obj, js_io_funcs,
                               sizeof(js_io_funcs) / sizeof(js_io_funcs[0]));

    JS_SetPropertyStr(ctx, global, "__native_io", io_obj);
    JS_FreeValue(ctx, global);
}
