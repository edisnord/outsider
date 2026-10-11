/* Exercises Outsider's QuickJS-on-JSC shim (src/engine/jsc/qjs_jsc.c of the outsider fork) on the console.
   Sent to elfldr; logs to /data/jsc-shim.log, klog and UDP 18194. */
#include "quickjs.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int logfd = -1, udp = -1, failures, checks;
static struct sockaddr_in bc;

static void say(const char *fmt, ...)
{
    char b[512];
    va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap);
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    if (logfd >= 0) write(logfd, b, n);
    if (udp >= 0) sendto(udp, b, n, 0, (struct sockaddr *)&bc, sizeof bc);
}
#define CHECK(c) do { checks++; if (!(c)) { failures++; say("FAIL line %d: %s", __LINE__, #c); } } while (0)

static int finalized;
static JSClassID cls;
static void fin(JSRuntime *rt, JSValueConst v) { (void)rt; (void)v; finalized++; }

static JSValue add(JSContext *c, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    double a = 0, b = 0;
    if (argc < 2) return JS_ThrowTypeError(c, "need two arguments, got %d", argc);
    JS_ToFloat64(c, &a, argv[0]); JS_ToFloat64(c, &b, argv[1]);
    return JS_NewFloat64(c, a + b);
}
static JSValue magic(JSContext *c, JSValueConst t, int argc, JSValueConst *argv, int m)
{
    (void)t; (void)argc; (void)argv;
    return JS_NewInt32(c, m * 10);
}
static JSValue make_token(JSContext *c, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    JSValue o = JS_NewObjectClass(c, (int)cls);
    JS_SetOpaque(o, (void *)(uintptr_t)1234);
    return o;
}
static JSValue token_value(JSContext *c, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc;
    return JS_NewUint32(c, (uint32_t)(uintptr_t)JS_GetOpaque(argv[0], cls));
}
static JSValue buf_sum(JSContext *c, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc;
    size_t n;
    uint8_t *p = JS_GetArrayBuffer(c, &n, argv[0]);
    if (!p) return JS_EXCEPTION;
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return JS_NewUint32(c, s);
}
static JSValue view_info(JSContext *c, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc;
    size_t off, len, el;
    JSValue ab = JS_GetTypedArrayBuffer(c, argv[0], &off, &len, &el);
    if (JS_IsException(ab)) return JS_EXCEPTION;
    size_t total;
    uint8_t *p = JS_GetArrayBuffer(c, &total, ab);
    JS_FreeValue(c, ab);
    return JS_NewInt32(c, p ? (int)(off * 1000000 + len * 1000 + el) : -1);
}
static JSValue make_buf(JSContext *c, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    uint8_t d[5] = { 1, 2, 3, 4, 250 };
    return JS_NewArrayBufferCopy(c, d, sizeof d);
}

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("add", 2, add),
    JS_CFUNC_DEF("makeToken", 0, make_token),
    JS_CFUNC_DEF("tokenValue", 1, token_value),
    JS_CFUNC_DEF("bufSum", 1, buf_sum),
    JS_CFUNC_DEF("viewInfo", 1, view_info),
    JS_CFUNC_DEF("makeBuf", 0, make_buf),
};

static JSValue eval(JSContext *c, const char *s)
{
    return JS_Eval(c, s, strlen(s), "<test>", JS_EVAL_TYPE_GLOBAL);
}
static double num(JSContext *c, const char *s)
{
    JSValue v = eval(c, s);
    double d = -12345;
    if (JS_IsException(v)) { JSValue e = JS_GetException(c); const char *m = JS_ToCString(c, e); say("exception in %s: %s", s, m ? m : "?"); JS_FreeCString(c, m); JS_FreeValue(c, e); }
    else JS_ToFloat64(c, &d, v);
    JS_FreeValue(c, v);
    return d;
}
static char *str(JSContext *c, const char *s, char *out, size_t n)
{
    JSValue v = eval(c, s);
    const char *p = JS_ToCString(c, v);
    snprintf(out, n, "%s", p ? p : "(null)");
    JS_FreeCString(c, p);
    JS_FreeValue(c, v);
    return out;
}

int main(void)
{
    logfd = open("/data/jsc-shim.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    udp = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    bc.sin_family = AF_INET; bc.sin_port = htons(18194); bc.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    const char *opts = "useConcurrentGC=0 useConcurrentJIT=0 numberOfGCMarkers=1";
    (void)opts;
    setenv("JSC_useConcurrentGC", "0", 1);
    setenv("JSC_useConcurrentJIT", "0", 1);
    setenv("JSC_numberOfGCMarkers", "1", 1);

    JSRuntime *rt = JS_NewRuntime();
    JSContext *c = JS_NewContext(rt);
    say("shim test: runtime %p context %p", (void *)rt, (void *)c);

    JSValue g = JS_GetGlobalObject(c);
    JS_SetPropertyFunctionList(c, g, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, g, "m3", JS_NewCFunctionMagic(c, magic, "m3", 0, JS_CFUNC_generic_magic, 3));
    JS_FreeValue(c, g);
    JS_NewClassID(rt, &cls);
    JSClassDef def = { "Token", fin, NULL, NULL, NULL };
    JS_NewClass(rt, cls, &def);

    char s[256];
    CHECK(num(c, "1+2") == 3);
    CHECK(num(c, "add(1.5, 2)") == 3.5);
    CHECK(num(c, "m3()") == 30);
    CHECK(strcmp(str(c, "typeof add", s, sizeof s), "function") == 0);
    CHECK(strcmp(str(c, "add.name", s, sizeof s), "add") == 0);
    CHECK(num(c, "add.call(null, 4, 5)") == 9);
    CHECK(strcmp(str(c, "try { add(1) } catch (e) { e.name + ': ' + e.message }", s, sizeof s),
                 "TypeError: need two arguments, got 1") == 0);
    CHECK(strcmp(str(c, "[1,2,3].map(x => x * 2).join('-')", s, sizeof s), "2-4-6") == 0);
    CHECK(strcmp(str(c, "'h\\u00e9llo \\u3042'", s, sizeof s), "h\xc3\xa9llo \xe3\x81\x82") == 0);

    /* an exception from native code reaches script, an uncaught one reaches the host */
    {
        JSValue v = eval(c, "add()");
        CHECK(JS_IsException(v));
        JSValue e = JS_GetException(c);
        CHECK(JS_IsError(e));
        JSValue st = JS_GetPropertyStr(c, e, "message");
        const char *m = JS_ToCString(c, st);
        CHECK(m && strstr(m, "need two"));
        JS_FreeCString(c, m); JS_FreeValue(c, st); JS_FreeValue(c, e);
        v = eval(c, "syntax error here (");
        CHECK(JS_IsException(v));
        JS_FreeValue(c, JS_GetException(c));
    }

    /* classes, opaque pointers, finalizers */
    CHECK(num(c, "tokenValue(makeToken())") == 1234);
    CHECK(num(c, "tokenValue({})") == 0);
    num(c, "for (var i = 0; i < 2000; i++) makeToken(); 0");
    JS_RunGC(rt);
    say("finalized %d of 2001 tokens after a forced GC", finalized);
    CHECK(finalized > 1000);

    /* buffers */
    CHECK(num(c, "bufSum(makeBuf())") == 260);
    CHECK(num(c, "bufSum(new Uint8Array([1,2,3]).buffer)") == 6);
    CHECK(num(c, "viewInfo(new Float32Array(new ArrayBuffer(64), 16, 4))") == 16 * 1000000 + 16 * 1000 + 4);
    CHECK(strcmp(str(c, "try { bufSum({}) } catch (e) { e.name }", s, sizeof s), "TypeError") == 0);

    /* promises: are microtasks run when the call returns? */
    num(c, "var done = 0; Promise.resolve().then(function () { done = 1; }); 0");
    double after_eval = num(c, "done");
    say("microtask ran by the next eval: %s", after_eval == 1 ? "yes" : "no");
    CHECK(after_eval == 1);

    /* ToInt32 wraparound, property access on primitives, arrays */
    {
        JSValue v = eval(c, "4294967297");
        int32_t i = 0; JS_ToInt32(c, &i, v); CHECK(i == 1);
        JS_FreeValue(c, v);
        v = JS_NewString(c, "abc");
        JSValue len = JS_GetPropertyStr(c, v, "length");
        JS_ToInt32(c, &i, len); CHECK(i == 3);
        JS_FreeValue(c, len); JS_FreeValue(c, v);
        JSValue a = JS_NewArray(c);
        JS_SetPropertyUint32(c, a, 0, JS_NewInt32(c, 7));
        JSValue e0 = JS_GetPropertyUint32(c, a, 0);
        JS_ToInt32(c, &i, e0); CHECK(i == 7 && JS_IsArray(a));
        JS_FreeValue(c, e0); JS_FreeValue(c, a);
    }

    /* the JIT warms up through this API too */
    {
        uint64_t t0 = 0;
        struct timespec { long s, ns; } ts;
        (void)ts; (void)t0;
        double r = num(c, "function fib(n){return n<2?n:fib(n-1)+fib(n-2)} fib(30)");
        CHECK(r == 832040);
    }

    JS_FreeContext(c);
    JS_FreeRuntime(rt);
    say("shim test done: %d checks, %d failures", checks, failures);
    return failures != 0;
}
