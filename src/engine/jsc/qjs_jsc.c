/*
 * QuickJS API on JavaScriptCore's C API; see quickjs.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "quickjs.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* WTF calls memmem(), which the PS5's libc does not have. */
__attribute__((weak)) void *memmem(const void *h, size_t hl, const void *n, size_t nl)
{
    if (nl == 0) return (void *)h;
    if (hl < nl) return NULL;
    const unsigned char *p = h, *q = n;
    for (size_t i = 0; i + nl <= hl; i++)
        if (p[i] == q[0] && memcmp(p + i, q, nl) == 0) return (void *)(p + i);
    return NULL;
}

/* ... and mkostemp() and timegm(), which the title's libc lacks as well */
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
__attribute__((weak)) int mkostemp(char *tmpl, int flags)
{
    int fd = mkstemp(tmpl);
    if (fd >= 0 && (flags & O_CLOEXEC)) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

__attribute__((weak)) time_t timegm(struct tm *tm)
{
    /* days from civil (H. Hinnant), normalising the month first */
    long long y = tm->tm_year + 1900LL, m = tm->tm_mon;
    y += m / 12;
    m %= 12;
    if (m < 0) { m += 12; y--; }
    long long mm = m + 1;
    y -= mm <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (mm + (mm > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec);
}

/* JSContextRefPrivate.h */
extern void JSSynchronousGarbageCollectForDebugging(JSContextRef ctx);

#define MAX_CLASSES 64
#define MAX_CONTEXTS 16

struct JSRuntime {
    JSContextGroupRef group;
    JSClassRef classes[MAX_CLASSES];
    JSClassDef defs[MAX_CLASSES];
    JSClassID next_class;
    JSClassRef func_class;
    JSHostPromiseRejectionTracker *rejection_cb;
    void *rejection_opaque;
};

struct JSContext {
    JSRuntime *rt;
    JSGlobalContextRef g;
    JSValueRef pending;      /* protected, NULL when nothing is pending */
    JSObjectRef func_proto;  /* Function.prototype, protected */
    void *opaque;
};

typedef struct {
    JSCFunction *fn;
    JSCFunctionMagic *mfn;
    int magic;
} FnData;

typedef struct {
    JSClassID id;
    void *opaque;
} ObjData;

static JSRuntime *g_rt;
static JSContext *g_ctxs[MAX_CONTEXTS];

static JSContext *ctx_of(JSContextRef c)
{
    JSGlobalContextRef g = JSContextGetGlobalContext(c);
    for (int i = 0; i < MAX_CONTEXTS; i++)
        if (g_ctxs[i] && g_ctxs[i]->g == g) return g_ctxs[i];
    return NULL;
}

/* A context for operations that only inspect a value (JS_IsString has no context argument). */
static JSContextRef any_ctx(void)
{
    for (int i = 0; i < MAX_CONTEXTS; i++)
        if (g_ctxs[i]) return g_ctxs[i]->g;
    return NULL;
}

/* ---- value conversion ---- */

static JSValue mk(int tag) { JSValue v; memset(&v, 0, sizeof v); v.tag = tag; return v; }
static JSValue mk_int(int32_t i) { JSValue v = mk(JSV_INT); v.u.i32 = i; return v; }
static JSValue mk_float(double d) { JSValue v = mk(JSV_FLOAT); v.u.f64 = d; return v; }

/* A ref coming out of JSC becomes an owned JSValue: primitives by value, cells protected. */
static JSValue from_ref(JSContext *ctx, JSValueRef r)
{
    if (!r) return JS_UNDEFINED;
    switch (JSValueGetType(ctx->g, r)) {
    case kJSTypeUndefined: return JS_UNDEFINED;
    case kJSTypeNull: return JS_NULL;
    case kJSTypeBoolean: return JSValueToBoolean(ctx->g, r) ? JS_TRUE : JS_FALSE;
    case kJSTypeNumber: return mk_float(JSValueToNumber(ctx->g, r, NULL));
    default: {
        JSValue v = mk(JSV_REF);
        v.u.ref = r;
        JSValueProtect(ctx->g, r);
        return v;
    }
    }
}

/* the same for a ref JSC hands to a native function: borrowed, not protected */
static JSValue borrow_ref(JSContext *ctx, JSValueRef r)
{
    JSValue v = from_ref(ctx, r);
    if (v.tag == JSV_REF) JSValueUnprotect(ctx->g, r);
    return v;
}

static JSValueRef to_ref(JSContext *ctx, JSValueConst v)
{
    switch (v.tag) {
    case JSV_NULL: return JSValueMakeNull(ctx->g);
    case JSV_FALSE: return JSValueMakeBoolean(ctx->g, false);
    case JSV_TRUE: return JSValueMakeBoolean(ctx->g, true);
    case JSV_INT: return JSValueMakeNumber(ctx->g, v.u.i32);
    case JSV_FLOAT: return JSValueMakeNumber(ctx->g, v.u.f64);
    case JSV_REF: return v.u.ref;
    default: return JSValueMakeUndefined(ctx->g);
    }
}

static void set_pending(JSContext *ctx, JSValueRef exc)
{
    if (ctx->pending) JSValueUnprotect(ctx->g, ctx->pending);
    ctx->pending = exc;
    if (exc) JSValueProtect(ctx->g, exc);
}

/* record exc as the pending exception and give back JS_EXCEPTION */
static JSValue fail(JSContext *ctx, JSValueRef exc)
{
    set_pending(ctx, exc);
    return JS_EXCEPTION;
}

static JSObjectRef to_object(JSContext *ctx, JSValueConst v)
{
    JSValueRef exc = NULL;
    JSObjectRef o = JSValueToObject(ctx->g, to_ref(ctx, v), &exc);
    if (!o && exc) set_pending(ctx, exc);
    return o;
}

static JSStringRef str_len(const char *s, size_t len)
{
    char *tmp = malloc(len + 1);
    if (!tmp) return NULL;
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    JSStringRef r = JSStringCreateWithUTF8CString(tmp);
    free(tmp);
    return r;
}

/* ---- runtime and context ---- */

JSRuntime *JS_NewRuntime(void)
{
    JSRuntime *rt = calloc(1, sizeof *rt);
    if (!rt) return NULL;
    rt->group = JSContextGroupCreate();
    rt->next_class = 1;
    g_rt = rt;
    return rt;
}

void JS_FreeRuntime(JSRuntime *rt)
{
    if (!rt) return;
    JSContextGroupRelease(rt->group);
    if (g_rt == rt) g_rt = NULL;
    free(rt);
}

static void fn_finalize(JSObjectRef o) { free(JSObjectGetPrivate(o)); }

static JSValueRef fn_call(JSContextRef cref, JSObjectRef function, JSObjectRef thisObject, size_t argc,
                          const JSValueRef argv[], JSValueRef *exception)
{
    JSContext *ctx = ctx_of(cref);
    FnData *d = JSObjectGetPrivate(function);
    if (!ctx || !d) return JSValueMakeUndefined(cref);

    JSValue stack_args[8];
    JSValue *args = argc <= 8 ? stack_args : malloc(argc * sizeof *args);
    for (size_t i = 0; i < argc; i++) args[i] = borrow_ref(ctx, argv[i]);
    JSValue this_v = thisObject ? borrow_ref(ctx, thisObject) : JS_UNDEFINED;

    JSValue r = d->mfn ? d->mfn(ctx, this_v, (int)argc, args, d->magic) : d->fn(ctx, this_v, (int)argc, args);
    if (args != stack_args) free(args);

    if (r.tag == JSV_EXCEPTION) {
        JSValueRef e = ctx->pending;
        if (!e) e = JSValueMakeString(cref, JSStringCreateWithUTF8CString("native function failed"));
        else JSValueUnprotect(ctx->g, e);   /* hand the pending exception over to JSC */
        ctx->pending = NULL;
        *exception = e;
        return JSValueMakeUndefined(cref);
    }
    JSValueRef out = to_ref(ctx, r);
    /* the callee's value was owned: keep it alive until JSC has taken it */
    if (r.tag == JSV_REF) JSValueUnprotect(ctx->g, r.u.ref);
    return out;
}

static void ensure_func_class(JSRuntime *rt)
{
    if (rt->func_class) return;
    JSClassDefinition d = kJSClassDefinitionEmpty;
    d.className = "NativeFunction";
    d.callAsFunction = fn_call;
    d.finalize = fn_finalize;
    rt->func_class = JSClassCreate(&d);
}

JSContext *JS_NewContext(JSRuntime *rt)
{
    JSContext *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return NULL;
    ctx->rt = rt;
    ctx->g = JSGlobalContextCreateInGroup(rt->group, NULL);
    if (!ctx->g) { free(ctx); return NULL; }
    int slot = -1;
    for (int i = 0; i < MAX_CONTEXTS; i++)
        if (!g_ctxs[i]) { slot = i; break; }
    if (slot < 0) { JSGlobalContextRelease(ctx->g); free(ctx); return NULL; }
    g_ctxs[slot] = ctx;
    ensure_func_class(rt);

    JSObjectRef global = JSContextGetGlobalObject(ctx->g);
    JSStringRef fname = JSStringCreateWithUTF8CString("Function");
    JSValueRef fn = JSObjectGetProperty(ctx->g, global, fname, NULL);
    JSStringRelease(fname);
    if (fn && JSValueIsObject(ctx->g, fn)) {
        JSStringRef pname = JSStringCreateWithUTF8CString("prototype");
        JSValueRef proto = JSObjectGetProperty(ctx->g, (JSObjectRef)fn, pname, NULL);
        JSStringRelease(pname);
        if (proto && JSValueIsObject(ctx->g, proto)) {
            ctx->func_proto = (JSObjectRef)proto;
            JSValueProtect(ctx->g, proto);
        }
    }
    return ctx;
}

void JS_FreeContext(JSContext *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < MAX_CONTEXTS; i++)
        if (g_ctxs[i] == ctx) g_ctxs[i] = NULL;
    if (ctx->pending) JSValueUnprotect(ctx->g, ctx->pending);
    if (ctx->func_proto) JSValueUnprotect(ctx->g, ctx->func_proto);
    JSGlobalContextRelease(ctx->g);
    free(ctx);
}

JSRuntime *JS_GetRuntime(JSContext *ctx) { return ctx->rt; }
void JS_SetContextOpaque(JSContext *ctx, void *opaque) { ctx->opaque = opaque; }
void *JS_GetContextOpaque(JSContext *ctx) { return ctx->opaque; }

void JS_RunGC(JSRuntime *rt)
{
    JSContextRef c = any_ctx();
    if (c && rt) JSSynchronousGarbageCollectForDebugging(c);
}

/* JSC reads its limits from the environment (JSC_*) before the first VM; nothing to do per runtime. */
void JS_SetMemoryLimit(JSRuntime *rt, size_t limit) { (void)rt; (void)limit; }
void JS_SetMaxStackSize(JSRuntime *rt, size_t stack_size) { (void)rt; (void)stack_size; }
void JS_SetGCThreshold(JSRuntime *rt, size_t gc_threshold) { (void)rt; (void)gc_threshold; }
void JS_SetInterruptHandler(JSRuntime *rt, JSInterruptHandler *cb, void *opaque) { (void)rt; (void)cb; (void)opaque; }
void JS_SetModuleLoaderFunc2(JSRuntime *rt, void *n, JSModuleLoaderFunc2 *l, JSModuleCheckSupportedImportAttributes *c,
                             void *o)
{
    (void)rt; (void)n; (void)l; (void)c; (void)o;
}

void JS_SetHostPromiseRejectionTracker(JSRuntime *rt, JSHostPromiseRejectionTracker *cb, void *opaque)
{
    rt->rejection_cb = cb;      /* JSC's C API has no hook for it: stored, never called (see jsc-port.md) */
    rt->rejection_opaque = opaque;
}

void JS_ComputeMemoryUsage(JSRuntime *rt, JSMemoryUsage *s)
{
    (void)rt;
    memset(s, 0, sizeof *s);
}

/* JSC drains the microtask queue when the outermost API call returns, so nothing is ever left pending. */
int JS_IsJobPending(JSRuntime *rt) { (void)rt; return 0; }
int JS_ExecutePendingJob(JSRuntime *rt, JSContext **pctx) { (void)rt; (void)pctx; return 0; }

/* ---- evaluation ---- */

JSValue JS_Eval(JSContext *ctx, const char *input, size_t input_len, const char *filename, int flags)
{
    if (flags & JS_EVAL_TYPE_MODULE)
        return JS_ThrowInternalError(ctx, "ES modules are not supported on JavaScriptCore");
    JSStringRef src = str_len(input, input_len);
    JSStringRef url = JSStringCreateWithUTF8CString(filename ? filename : "<eval>");
    JSValueRef exc = NULL;
    JSValueRef r = NULL;
    if (flags & JS_EVAL_FLAG_COMPILE_ONLY) {
        if (!JSCheckScriptSyntax(ctx->g, src, url, 1, &exc)) {
            if (!exc) exc = JSValueMakeString(ctx->g, JSStringCreateWithUTF8CString("SyntaxError"));
        }
    } else {
        r = JSEvaluateScript(ctx->g, src, NULL, url, 1, &exc);
    }
    JSStringRelease(src);
    JSStringRelease(url);
    if (exc) return fail(ctx, exc);
    return from_ref(ctx, r);
}

JSValue JS_EvalFunction(JSContext *ctx, JSValue fun_obj)
{
    (void)ctx; (void)fun_obj;
    return JS_ThrowInternalError(ctx, "bytecode is not supported on JavaScriptCore");
}

uint8_t *JS_WriteObject(JSContext *ctx, size_t *psize, JSValueConst obj, int flags)
{
    (void)ctx; (void)obj; (void)flags;
    *psize = 0;
    return NULL;
}

JSValue JS_ReadObject(JSContext *ctx, const uint8_t *buf, size_t buf_len, int flags)
{
    (void)buf; (void)buf_len; (void)flags;
    return JS_ThrowInternalError(ctx, "bytecode is not supported on JavaScriptCore");
}

void js_free(JSContext *ctx, void *ptr) { (void)ctx; free(ptr); }

JSValue JS_Call(JSContext *ctx, JSValueConst func_obj, JSValueConst this_obj, int argc, JSValueConst *argv)
{
    JSObjectRef fn = to_object(ctx, func_obj);
    if (!fn) return JS_EXCEPTION;
    JSObjectRef self = NULL;
    if (this_obj.tag != JSV_UNDEFINED && this_obj.tag != JSV_EXCEPTION) {
        self = to_object(ctx, this_obj);
        if (!self) return JS_EXCEPTION;
    }
    JSValueRef stack_args[8];
    JSValueRef *args = argc <= 8 ? stack_args : malloc((size_t)argc * sizeof *args);
    for (int i = 0; i < argc; i++) args[i] = to_ref(ctx, argv[i]);
    JSValueRef exc = NULL;
    JSValueRef r = JSObjectCallAsFunction(ctx->g, fn, self, (size_t)argc, args, &exc);
    if (args != stack_args) free(args);
    if (exc) return fail(ctx, exc);
    return from_ref(ctx, r);
}

/* ---- values ---- */

JSValue JS_NewInt32(JSContext *ctx, int32_t v) { (void)ctx; return mk_int(v); }
JSValue JS_NewUint32(JSContext *ctx, uint32_t v)
{
    (void)ctx;
    return v <= INT32_MAX ? mk_int((int32_t)v) : mk_float((double)v);
}
JSValue JS_NewInt64(JSContext *ctx, int64_t v) { (void)ctx; return mk_float((double)v); }
JSValue JS_NewFloat64(JSContext *ctx, double v) { (void)ctx; return mk_float(v); }
JSValue JS_NewBool(JSContext *ctx, bool v) { (void)ctx; return v ? JS_TRUE : JS_FALSE; }

JSValue JS_NewString(JSContext *ctx, const char *str)
{
    JSStringRef s = JSStringCreateWithUTF8CString(str);
    JSValue v = from_ref(ctx, JSValueMakeString(ctx->g, s));
    JSStringRelease(s);
    return v;
}

JSValue JS_NewStringLen(JSContext *ctx, const char *str, size_t len)
{
    JSStringRef s = str_len(str, len);
    JSValue v = from_ref(ctx, JSValueMakeString(ctx->g, s));
    JSStringRelease(s);
    return v;
}

JSValue JS_NewObject(JSContext *ctx) { return from_ref(ctx, JSObjectMake(ctx->g, NULL, NULL)); }

JSValue JS_NewArray(JSContext *ctx)
{
    JSValueRef exc = NULL;
    JSObjectRef a = JSObjectMakeArray(ctx->g, 0, NULL, &exc);
    return exc ? fail(ctx, exc) : from_ref(ctx, a);
}

JSValue JS_NewError(JSContext *ctx)
{
    JSValueRef exc = NULL;
    JSObjectRef e = JSObjectMakeError(ctx->g, 0, NULL, &exc);
    return exc ? fail(ctx, exc) : from_ref(ctx, e);
}

JSValue JS_DupValue(JSContext *ctx, JSValueConst v)
{
    if (v.tag == JSV_REF) JSValueProtect(ctx->g, v.u.ref);
    return v;
}

void JS_FreeValue(JSContext *ctx, JSValue v)
{
    if (v.tag == JSV_REF) JSValueUnprotect(ctx->g, v.u.ref);
}

bool JS_IsException(JSValueConst v) { return v.tag == JSV_EXCEPTION; }
bool JS_IsUndefined(JSValueConst v) { return v.tag == JSV_UNDEFINED; }
bool JS_IsNull(JSValueConst v) { return v.tag == JSV_NULL; }

bool JS_IsString(JSValueConst v)
{
    JSContextRef c = any_ctx();
    return v.tag == JSV_REF && c && JSValueIsString(c, v.u.ref);
}

bool JS_IsError(JSValueConst v)
{
    JSContextRef c = any_ctx();
    if (v.tag != JSV_REF || !c || !JSValueIsObject(c, v.u.ref)) return false;
    JSObjectRef global = JSContextGetGlobalObject(c);
    JSStringRef n = JSStringCreateWithUTF8CString("Error");
    JSValueRef ctor = JSObjectGetProperty(c, global, n, NULL);
    JSStringRelease(n);
    if (!ctor || !JSValueIsObject(c, ctor)) return false;
    return JSValueIsInstanceOfConstructor(c, v.u.ref, (JSObjectRef)ctor, NULL);
}

bool JS_IsFunction(JSContext *ctx, JSValueConst v)
{
    return v.tag == JSV_REF && JSValueIsObject(ctx->g, v.u.ref) && JSObjectIsFunction(ctx->g, (JSObjectRef)v.u.ref);
}

bool JS_IsArray(JSValueConst v)
{
    JSContextRef c = any_ctx();
    return v.tag == JSV_REF && c && JSValueIsArray(c, v.u.ref);
}

static int to_number(JSContext *ctx, JSValueConst v, double *out)
{
    switch (v.tag) {
    case JSV_INT: *out = v.u.i32; return 0;
    case JSV_FLOAT: *out = v.u.f64; return 0;
    case JSV_TRUE: *out = 1; return 0;
    case JSV_FALSE: case JSV_NULL: *out = 0; return 0;
    case JSV_UNDEFINED: *out = NAN; return 0;
    case JSV_REF: {
        JSValueRef exc = NULL;
        double d = JSValueToNumber(ctx->g, v.u.ref, &exc);
        if (exc) { set_pending(ctx, exc); return -1; }
        *out = d;
        return 0;
    }
    default: return -1;
    }
}

int JS_ToFloat64(JSContext *ctx, double *pres, JSValueConst v) { return to_number(ctx, v, pres); }

int JS_ToInt32(JSContext *ctx, int32_t *pres, JSValueConst v)
{
    if (v.tag == JSV_INT) { *pres = v.u.i32; return 0; }
    double d;
    if (to_number(ctx, v, &d)) { *pres = 0; return -1; }
    if (!__builtin_isfinite(d)) { *pres = 0; return 0; }
    if (d >= -2147483648.0 && d <= 2147483647.0) { *pres = (int32_t)d; return 0; }
    double m = fmod(trunc(d), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    *pres = (int32_t)(uint32_t)m;
    return 0;
}

int JS_ToUint32(JSContext *ctx, uint32_t *pres, JSValueConst v)
{
    int32_t i;
    int r = JS_ToInt32(ctx, &i, v);
    *pres = (uint32_t)i;
    return r;
}

int JS_ToBool(JSContext *ctx, JSValueConst v)
{
    switch (v.tag) {
    case JSV_TRUE: return 1;
    case JSV_INT: return v.u.i32 != 0;
    case JSV_FLOAT: return v.u.f64 != 0 && !__builtin_isnan(v.u.f64);
    case JSV_REF: return JSValueToBoolean(ctx->g, v.u.ref);
    default: return 0;
    }
}

const char *JS_ToCStringLen(JSContext *ctx, size_t *plen, JSValueConst v)
{
    JSValueRef exc = NULL;
    JSStringRef s = JSValueToStringCopy(ctx->g, to_ref(ctx, v), &exc);
    if (exc || !s) {
        if (exc) set_pending(ctx, exc);
        if (plen) *plen = 0;
        return NULL;
    }
    size_t cap = JSStringGetMaximumUTF8CStringSize(s);
    char *buf = malloc(cap);
    if (!buf) { JSStringRelease(s); return NULL; }
    size_t n = JSStringGetUTF8CString(s, buf, cap);
    JSStringRelease(s);
    if (plen) *plen = n ? n - 1 : 0;
    return buf;
}

const char *JS_ToCString(JSContext *ctx, JSValueConst v) { return JS_ToCStringLen(ctx, NULL, v); }
void JS_FreeCString(JSContext *ctx, const char *ptr) { (void)ctx; free((void *)ptr); }

/* ---- properties ---- */

JSValue JS_GetGlobalObject(JSContext *ctx) { return from_ref(ctx, JSContextGetGlobalObject(ctx->g)); }

JSValue JS_GetPropertyStr(JSContext *ctx, JSValueConst obj, const char *prop)
{
    JSObjectRef o = to_object(ctx, obj);
    if (!o) return JS_EXCEPTION;
    JSStringRef n = JSStringCreateWithUTF8CString(prop);
    JSValueRef exc = NULL;
    JSValueRef r = JSObjectGetProperty(ctx->g, o, n, &exc);
    JSStringRelease(n);
    return exc ? fail(ctx, exc) : from_ref(ctx, r);
}

JSValue JS_GetPropertyUint32(JSContext *ctx, JSValueConst obj, uint32_t idx)
{
    JSObjectRef o = to_object(ctx, obj);
    if (!o) return JS_EXCEPTION;
    JSValueRef exc = NULL;
    JSValueRef r = JSObjectGetPropertyAtIndex(ctx->g, o, idx, &exc);
    return exc ? fail(ctx, exc) : from_ref(ctx, r);
}

int JS_SetPropertyStr(JSContext *ctx, JSValueConst obj, const char *prop, JSValue val)
{
    JSObjectRef o = to_object(ctx, obj);
    if (!o) { JS_FreeValue(ctx, val); return -1; }
    JSStringRef n = JSStringCreateWithUTF8CString(prop);
    JSValueRef exc = NULL;
    JSObjectSetProperty(ctx->g, o, n, to_ref(ctx, val), kJSPropertyAttributeNone, &exc);
    JSStringRelease(n);
    JS_FreeValue(ctx, val);
    if (exc) { set_pending(ctx, exc); return -1; }
    return 1;
}

int JS_SetPropertyUint32(JSContext *ctx, JSValueConst obj, uint32_t idx, JSValue val)
{
    JSObjectRef o = to_object(ctx, obj);
    if (!o) { JS_FreeValue(ctx, val); return -1; }
    JSValueRef exc = NULL;
    JSObjectSetPropertyAtIndex(ctx->g, o, idx, to_ref(ctx, val), &exc);
    JS_FreeValue(ctx, val);
    if (exc) { set_pending(ctx, exc); return -1; }
    return 1;
}

void JS_SetPropertyFunctionList(JSContext *ctx, JSValueConst obj, const JSCFunctionListEntry *tab, int len)
{
    for (int i = 0; i < len; i++)
        JS_SetPropertyStr(ctx, obj, tab[i].name, JS_NewCFunction(ctx, tab[i].func, tab[i].name, tab[i].length));
}

/* ---- native functions and classes ---- */

static JSValue new_function(JSContext *ctx, JSCFunction *fn, JSCFunctionMagic *mfn, int magic, const char *name,
                            int length)
{
    FnData *d = calloc(1, sizeof *d);
    if (!d) return JS_ThrowOutOfMemory(ctx);
    d->fn = fn;
    d->mfn = mfn;
    d->magic = magic;
    JSObjectRef f = JSObjectMake(ctx->g, ctx->rt->func_class, d);
    JSValue v = from_ref(ctx, f);
    if (name) {
        JSStringRef n = JSStringCreateWithUTF8CString("name");
        JSStringRef s = JSStringCreateWithUTF8CString(name);
        JSObjectSetProperty(ctx->g, f, n, JSValueMakeString(ctx->g, s),
                            kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum, NULL);
        JSStringRelease(s);
        JSStringRelease(n);
    }
    JSStringRef ln = JSStringCreateWithUTF8CString("length");
    JSObjectSetProperty(ctx->g, f, ln, JSValueMakeNumber(ctx->g, length),
                        kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum, NULL);
    JSStringRelease(ln);
    /* last: Function.prototype's read-only name and length would make the sets above fail silently */
    if (ctx->func_proto) JSObjectSetPrototype(ctx->g, f, ctx->func_proto);
    return v;
}

JSValue JS_NewCFunction(JSContext *ctx, JSCFunction *func, const char *name, int length)
{
    return new_function(ctx, func, NULL, 0, name, length);
}

JSValue JS_NewCFunctionMagic(JSContext *ctx, JSCFunctionMagic *func, const char *name, int length,
                             JSCFunctionEnum cproto, int magic)
{
    (void)cproto;
    return new_function(ctx, NULL, func, magic, name, length);
}

JSClassID JS_NewClassID(JSRuntime *rt, JSClassID *pclass_id)
{
    if (*pclass_id == 0) *pclass_id = rt->next_class++;
    return *pclass_id;
}

static void obj_finalize(JSObjectRef o)
{
    ObjData *d = JSObjectGetPrivate(o);
    if (!d) return;
    JSRuntime *rt = g_rt;
    if (rt && d->id < MAX_CLASSES && rt->defs[d->id].finalizer) {
        JSValue v = mk(JSV_REF);
        v.u.ref = o;
        rt->defs[d->id].finalizer(rt, v);
    }
    free(d);
}

int JS_NewClass(JSRuntime *rt, JSClassID class_id, const JSClassDef *class_def)
{
    if (class_id >= MAX_CLASSES) return -1;
    rt->defs[class_id] = *class_def;
    JSClassDefinition d = kJSClassDefinitionEmpty;
    d.className = class_def->class_name;
    d.finalize = obj_finalize;
    rt->classes[class_id] = JSClassCreate(&d);
    return 0;
}

JSValue JS_NewObjectClass(JSContext *ctx, int class_id)
{
    JSRuntime *rt = ctx->rt;
    if (class_id <= 0 || class_id >= MAX_CLASSES || !rt->classes[class_id])
        return JS_ThrowInternalError(ctx, "unknown class id %d", class_id);
    ObjData *d = calloc(1, sizeof *d);
    if (!d) return JS_ThrowOutOfMemory(ctx);
    d->id = (JSClassID)class_id;
    return from_ref(ctx, JSObjectMake(ctx->g, rt->classes[class_id], d));
}

void JS_SetOpaque(JSValue obj, void *opaque)
{
    if (obj.tag != JSV_REF) return;
    ObjData *d = JSObjectGetPrivate((JSObjectRef)obj.u.ref);
    if (d) d->opaque = opaque;
}

void *JS_GetOpaque(JSValueConst obj, JSClassID class_id)
{
    if (obj.tag != JSV_REF) return NULL;
    JSContextRef c = any_ctx();
    if (!c || !JSValueIsObject(c, obj.u.ref)) return NULL;
    JSRuntime *rt = g_rt;
    if (!rt || class_id >= MAX_CLASSES || !rt->classes[class_id]) return NULL;
    if (!JSValueIsObjectOfClass(c, obj.u.ref, rt->classes[class_id])) return NULL;
    ObjData *d = JSObjectGetPrivate((JSObjectRef)obj.u.ref);
    return d && d->id == class_id ? d->opaque : NULL;
}

/* ---- buffers ---- */

uint8_t *JS_GetArrayBuffer(JSContext *ctx, size_t *psize, JSValueConst obj)
{
    if (obj.tag == JSV_REF && JSValueIsObject(ctx->g, obj.u.ref)) {
        JSValueRef exc = NULL;
        if (JSValueGetTypedArrayType(ctx->g, obj.u.ref, &exc) == kJSTypedArrayTypeArrayBuffer) {
            void *p = JSObjectGetArrayBufferBytesPtr(ctx->g, (JSObjectRef)obj.u.ref, &exc);
            size_t n = JSObjectGetArrayBufferByteLength(ctx->g, (JSObjectRef)obj.u.ref, &exc);
            if (!exc) {
                *psize = n;
                return p;
            }
            set_pending(ctx, exc);
            return NULL;
        }
    }
    *psize = 0;
    JS_Throw(ctx, JS_ThrowTypeError(ctx, "not an ArrayBuffer"));
    return NULL;
}

static size_t element_size(JSTypedArrayType t)
{
    switch (t) {
    case kJSTypedArrayTypeInt8Array: case kJSTypedArrayTypeUint8Array: case kJSTypedArrayTypeUint8ClampedArray: return 1;
    case kJSTypedArrayTypeInt16Array: case kJSTypedArrayTypeUint16Array: return 2;
    case kJSTypedArrayTypeInt32Array: case kJSTypedArrayTypeUint32Array: case kJSTypedArrayTypeFloat32Array: return 4;
    case kJSTypedArrayTypeFloat64Array: case kJSTypedArrayTypeBigInt64Array: case kJSTypedArrayTypeBigUint64Array: return 8;
    default: return 1;
    }
}

JSValue JS_GetTypedArrayBuffer(JSContext *ctx, JSValueConst obj, size_t *pbyte_offset, size_t *pbyte_length,
                               size_t *pbytes_per_element)
{
    JSValueRef exc = NULL;
    if (obj.tag == JSV_REF && JSValueIsObject(ctx->g, obj.u.ref)) {
        JSTypedArrayType t = JSValueGetTypedArrayType(ctx->g, obj.u.ref, &exc);
        if (!exc && t != kJSTypedArrayTypeNone && t != kJSTypedArrayTypeArrayBuffer) {
            JSObjectRef o = (JSObjectRef)obj.u.ref;
            JSObjectRef ab = JSObjectGetTypedArrayBuffer(ctx->g, o, &exc);
            if (!exc && ab) {
                if (pbyte_offset) *pbyte_offset = JSObjectGetTypedArrayByteOffset(ctx->g, o, &exc);
                if (pbyte_length) *pbyte_length = JSObjectGetTypedArrayByteLength(ctx->g, o, &exc);
                if (pbytes_per_element) *pbytes_per_element = element_size(t);
                if (!exc) return from_ref(ctx, ab);
            }
        }
    }
    if (exc) return fail(ctx, exc);
    return JS_ThrowTypeError(ctx, "not a TypedArray");
}

static void free_bytes(void *bytes, void *context) { (void)context; free(bytes); }

JSValue JS_NewArrayBufferCopy(JSContext *ctx, const uint8_t *buf, size_t len)
{
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) return JS_ThrowOutOfMemory(ctx);
    if (len) memcpy(copy, buf, len);
    JSValueRef exc = NULL;
    JSObjectRef ab = JSObjectMakeArrayBufferWithBytesNoCopy(ctx->g, copy, len, free_bytes, NULL, &exc);
    if (exc || !ab) {
        free(copy);
        return exc ? fail(ctx, exc) : JS_ThrowOutOfMemory(ctx);
    }
    return from_ref(ctx, ab);
}

/* ---- exceptions ---- */

JSValue JS_GetException(JSContext *ctx)
{
    JSValueRef p = ctx->pending;
    if (!p) return JS_NULL;
    ctx->pending = NULL;
    JSValue v = from_ref(ctx, p);     /* protects again for the caller */
    JSValueUnprotect(ctx->g, p);      /* ... and drop the pending slot's protection */
    return v;
}

JSValue JS_Throw(JSContext *ctx, JSValue obj)
{
    if (obj.tag == JSV_EXCEPTION) return JS_EXCEPTION;   /* already pending */
    set_pending(ctx, to_ref(ctx, obj));
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue throw_typed(JSContext *ctx, const char *type, const char *fmt, va_list ap)
{
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    JSObjectRef global = JSContextGetGlobalObject(ctx->g);
    JSStringRef n = JSStringCreateWithUTF8CString(type);
    JSValueRef ctor = JSObjectGetProperty(ctx->g, global, n, NULL);
    JSStringRelease(n);
    JSStringRef m = JSStringCreateWithUTF8CString(msg);
    JSValueRef arg = JSValueMakeString(ctx->g, m);
    JSStringRelease(m);
    JSObjectRef err = NULL;
    if (ctor && JSValueIsObject(ctx->g, ctor)) err = JSObjectCallAsConstructor(ctx->g, (JSObjectRef)ctor, 1, &arg, NULL);
    if (!err) err = JSObjectMakeError(ctx->g, 1, &arg, NULL);
    set_pending(ctx, err);
    return JS_EXCEPTION;
}

JSValue JS_ThrowTypeError(JSContext *ctx, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    JSValue r = throw_typed(ctx, "TypeError", fmt, ap);
    va_end(ap);
    return r;
}

JSValue JS_ThrowRangeError(JSContext *ctx, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    JSValue r = throw_typed(ctx, "RangeError", fmt, ap);
    va_end(ap);
    return r;
}

JSValue JS_ThrowInternalError(JSContext *ctx, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    JSValue r = throw_typed(ctx, "Error", fmt, ap);
    va_end(ap);
    return r;
}

JSValue JS_ThrowOutOfMemory(JSContext *ctx) { return JS_ThrowRangeError(ctx, "out of memory"); }
