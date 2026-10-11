/*
 * The subset of the QuickJS-NG C API that Outsider uses, implemented on JavaScriptCore's C API
 * (qjs_jsc.c). Lets src/bindings and src/engine compile unchanged against JSC for the JIT.
 *
 * Semantics kept: values returned by the API are owned and freed with JS_FreeValue (they are
 * JSValueProtect'ed while owned), arguments are borrowed, JS_SetProperty* consume their value,
 * errors are reported by returning JS_EXCEPTION with the exception pending on the context.
 *
 * Not available on JSC's C API, stubbed (see qjs_jsc.c): bytecode serialisation, ES modules,
 * the interrupt handler (watchdog and sampling profiler), memory limits, rejection tracking.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RMMZ_QJS_JSC_H
#define RMMZ_QJS_JSC_H

#include <JavaScriptCore/JavaScript.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>   /* QuickJS's header brings these in and the bindings rely on it */
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;
typedef uint32_t JSClassID;

enum { JSV_UNDEFINED, JSV_NULL, JSV_FALSE, JSV_TRUE, JSV_EXCEPTION, JSV_INT, JSV_FLOAT, JSV_REF };

/* Primitives are carried by value, strings and objects by (protected) JSValueRef. */
typedef struct JSValue {
    union { JSValueRef ref; int32_t i32; double f64; } u;
    int tag;
} JSValue;
typedef JSValue JSValueConst;

#define JS_UNDEFINED ((JSValue){ { NULL }, JSV_UNDEFINED })
#define JS_NULL      ((JSValue){ { NULL }, JSV_NULL })
#define JS_FALSE     ((JSValue){ { NULL }, JSV_FALSE })
#define JS_TRUE      ((JSValue){ { NULL }, JSV_TRUE })
#define JS_EXCEPTION ((JSValue){ { NULL }, JSV_EXCEPTION })

#define JS_VALUE_GET_PTR(v) ((void *)(v).u.ref)

typedef JSValue JSCFunction(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
typedef JSValue JSCFunctionMagic(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);
typedef enum { JS_CFUNC_generic, JS_CFUNC_generic_magic } JSCFunctionEnum;

typedef struct JSCFunctionListEntry {
    const char *name;
    uint8_t length;
    JSCFunction *func;
} JSCFunctionListEntry;
#define JS_CFUNC_DEF(name, length, func1) { name, length, func1 }

typedef void JSClassFinalizer(JSRuntime *rt, JSValueConst val);
typedef struct JSClassDef {
    const char *class_name;
    JSClassFinalizer *finalizer;
    void *gc_mark;
    void *call;
    void *exotic;
} JSClassDef;

typedef struct JSMemoryUsage {
    int64_t malloc_size, malloc_count, obj_count, str_count, atom_count, shape_count;
    int64_t js_func_count, array_count, fast_array_count;
} JSMemoryUsage;

typedef int JSInterruptHandler(JSRuntime *rt, void *opaque);
typedef void JSHostPromiseRejectionTracker(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                                           bool is_handled, void *opaque);
typedef struct JSModuleDef JSModuleDef;
typedef JSModuleDef *JSModuleLoaderFunc2(JSContext *, const char *, void *, JSValueConst);
typedef int JSModuleCheckSupportedImportAttributes(JSContext *, void *, JSValueConst);

#define JS_EVAL_TYPE_GLOBAL      0
#define JS_EVAL_TYPE_MODULE      1
#define JS_EVAL_FLAG_COMPILE_ONLY (1 << 5)
#define JS_WRITE_OBJ_BYTECODE    (1 << 0)
#define JS_READ_OBJ_BYTECODE     (1 << 0)

/* runtime and context */
JSRuntime *JS_NewRuntime(void);
void JS_FreeRuntime(JSRuntime *rt);
JSContext *JS_NewContext(JSRuntime *rt);
void JS_FreeContext(JSContext *ctx);
JSRuntime *JS_GetRuntime(JSContext *ctx);
void JS_SetContextOpaque(JSContext *ctx, void *opaque);
void *JS_GetContextOpaque(JSContext *ctx);
void JS_RunGC(JSRuntime *rt);
void JS_SetMemoryLimit(JSRuntime *rt, size_t limit);
void JS_SetMaxStackSize(JSRuntime *rt, size_t stack_size);
void JS_SetGCThreshold(JSRuntime *rt, size_t gc_threshold);
void JS_SetInterruptHandler(JSRuntime *rt, JSInterruptHandler *cb, void *opaque);
void JS_SetHostPromiseRejectionTracker(JSRuntime *rt, JSHostPromiseRejectionTracker *cb, void *opaque);
void JS_SetModuleLoaderFunc2(JSRuntime *rt, void *normalize, JSModuleLoaderFunc2 *loader,
                             JSModuleCheckSupportedImportAttributes *check, void *opaque);
void JS_ComputeMemoryUsage(JSRuntime *rt, JSMemoryUsage *s);
int JS_IsJobPending(JSRuntime *rt);
int JS_ExecutePendingJob(JSRuntime *rt, JSContext **pctx);

/* evaluation */
JSValue JS_Eval(JSContext *ctx, const char *input, size_t input_len, const char *filename, int eval_flags);
JSValue JS_EvalFunction(JSContext *ctx, JSValue fun_obj);
JSValue JS_Call(JSContext *ctx, JSValueConst func_obj, JSValueConst this_obj, int argc, JSValueConst *argv);
uint8_t *JS_WriteObject(JSContext *ctx, size_t *psize, JSValueConst obj, int flags);
JSValue JS_ReadObject(JSContext *ctx, const uint8_t *buf, size_t buf_len, int flags);
void js_free(JSContext *ctx, void *ptr);

/* values */
JSValue JS_NewInt32(JSContext *ctx, int32_t v);
JSValue JS_NewUint32(JSContext *ctx, uint32_t v);
JSValue JS_NewInt64(JSContext *ctx, int64_t v);
JSValue JS_NewFloat64(JSContext *ctx, double v);
JSValue JS_NewBool(JSContext *ctx, bool v);
JSValue JS_NewString(JSContext *ctx, const char *str);
JSValue JS_NewStringLen(JSContext *ctx, const char *str, size_t len);
JSValue JS_NewObject(JSContext *ctx);
JSValue JS_NewArray(JSContext *ctx);
JSValue JS_NewError(JSContext *ctx);
JSValue JS_DupValue(JSContext *ctx, JSValueConst v);
void JS_FreeValue(JSContext *ctx, JSValue v);

bool JS_IsException(JSValueConst v);
bool JS_IsUndefined(JSValueConst v);
bool JS_IsNull(JSValueConst v);
bool JS_IsString(JSValueConst v);
bool JS_IsError(JSValueConst v);
bool JS_IsFunction(JSContext *ctx, JSValueConst v);
bool JS_IsArray(JSValueConst v);

int JS_ToInt32(JSContext *ctx, int32_t *pres, JSValueConst v);
int JS_ToUint32(JSContext *ctx, uint32_t *pres, JSValueConst v);
int JS_ToFloat64(JSContext *ctx, double *pres, JSValueConst v);
int JS_ToBool(JSContext *ctx, JSValueConst v);
const char *JS_ToCString(JSContext *ctx, JSValueConst v);
const char *JS_ToCStringLen(JSContext *ctx, size_t *plen, JSValueConst v);
void JS_FreeCString(JSContext *ctx, const char *ptr);

/* properties */
JSValue JS_GetGlobalObject(JSContext *ctx);
JSValue JS_GetPropertyStr(JSContext *ctx, JSValueConst obj, const char *prop);
JSValue JS_GetPropertyUint32(JSContext *ctx, JSValueConst obj, uint32_t idx);
int JS_SetPropertyStr(JSContext *ctx, JSValueConst obj, const char *prop, JSValue val);
int JS_SetPropertyUint32(JSContext *ctx, JSValueConst obj, uint32_t idx, JSValue val);
void JS_SetPropertyFunctionList(JSContext *ctx, JSValueConst obj, const JSCFunctionListEntry *tab, int len);

/* native functions and classes */
JSValue JS_NewCFunction(JSContext *ctx, JSCFunction *func, const char *name, int length);
JSValue JS_NewCFunctionMagic(JSContext *ctx, JSCFunctionMagic *func, const char *name, int length,
                             JSCFunctionEnum cproto, int magic);
JSClassID JS_NewClassID(JSRuntime *rt, JSClassID *pclass_id);
int JS_NewClass(JSRuntime *rt, JSClassID class_id, const JSClassDef *class_def);
JSValue JS_NewObjectClass(JSContext *ctx, int class_id);
void JS_SetOpaque(JSValue obj, void *opaque);
void *JS_GetOpaque(JSValueConst obj, JSClassID class_id);

/* buffers */
uint8_t *JS_GetArrayBuffer(JSContext *ctx, size_t *psize, JSValueConst obj);
JSValue JS_GetTypedArrayBuffer(JSContext *ctx, JSValueConst obj, size_t *pbyte_offset, size_t *pbyte_length,
                               size_t *pbytes_per_element);
JSValue JS_NewArrayBufferCopy(JSContext *ctx, const uint8_t *buf, size_t len);

/* exceptions */
JSValue JS_GetException(JSContext *ctx);
JSValue JS_Throw(JSContext *ctx, JSValue obj);
JSValue JS_ThrowTypeError(JSContext *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
JSValue JS_ThrowRangeError(JSContext *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
JSValue JS_ThrowInternalError(JSContext *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
JSValue JS_ThrowOutOfMemory(JSContext *ctx);

#ifdef __cplusplus
}
#endif
#endif
