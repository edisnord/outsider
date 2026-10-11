# JavaScriptCore instead of QuickJS (PlayStation 5, Vulkan build)

Outsider can run on JavaScriptCore with its JIT instead of QuickJS-NG: about 10x faster on script-heavy code in a
micro-benchmark, and frame times of a large MV game on the console went from 30-250 ms to about 16 ms.

## How

- `src/engine/jsc/quickjs.h` + `qjs_jsc.c` implement the QuickJS API Outsider calls (about 80 functions) on
  JavaScriptCore's C API, so `src/bindings` and `src/engine` compile **unchanged**. Values are a tagged struct
  (primitives by value, strings and objects as `JSValueProtect`'ed refs); `JS_FreeValue`/`JS_DupValue` are
  unprotect/protect; exceptions are pending on the context and `JS_EXCEPTION` is returned, as in QuickJS. Native
  functions are callable objects of one API class; classes keep `{class id, opaque}` as private data and run
  QuickJS finalizers. It also defines (weakly) `memmem`, `mkostemp` and `timegm`, which the PS5's libc lacks.
- Build: `-DRMMZ_JS_ENGINE=jsc -DRMMZ_JSC_HEADERS=<JavaScriptCore/Headers of a JSCOnly build>` with
  `RMMZ_PS5_VULKAN` (`cmake/ps5-vulkan.cmake`). The final link adds the JSC libraries, the objects of the
  `JavaScriptCoreJIT` subtarget and ICU.
- `tests/ps5_jsc_shim`: 25 checks of the shim on the console (calls, exceptions both ways, classes and finalizers,
  ArrayBuffer/TypedArray access, microtasks, fib(30) through the API); all pass.

## What the PS5 needs from the JSC build and the process

- JSC reserves a 512 MB code pool by default. A title is refused that (`mmap` ENOMEM; 128 MB works) and JSC then
  switches **every JIT tier off without a log line**. `qjs_jsc.c` sets `JSC_jitMemoryReservationSize` to 64 MB.
- Anonymous memory that is writable and executable at once maps, but running code in it faults (protection
  violation). The same pages take PROT_EXEC from an `mprotect()` afterwards, so WTF's `OSAllocatorPOSIX.cpp` maps
  without PROT_EXEC and adds it (FreeBSD branch).
- The PS5 has 16 KB pages: WTF's `CeilingOnPageSize` must say so for FreeBSD or VM creation aborts.
- JSC puts `g_config` and the opcode config in Mach-O style sections (`__DATA,__wtf_config`, `__DATA,__jsc_opcodes`);
  the PS5 converter's layout check rejects the image, so they are plain page-aligned arrays on FreeBSD.
- Concurrent GC and concurrent JIT crashed once on the console and were slower in a benchmark: `qjs_jsc.c` sets
  `JSC_useConcurrentGC=0`, `JSC_useConcurrentJIT=0`, `JSC_numberOfGCMarkers=1` unless already set. Not understood.
- `RMMZ_JSC_DUMP_OPTIONS=1` makes JSC print its final options at start (`useJIT`...). The first lines of the log also
  carry `jsc: self-test fib(27) runs: A / B / C ms`: a few ms means the JIT is running, about 30 ms means it is not.

## Not available through JSC's C API (stubbed in the shim)

| QuickJS feature | now |
|---|---|
| bytecode cache (`.jsc` files) | `js_engine_eval_file_cached` evaluates the source |
| ES modules (`import` in plugins) | `JS_Eval(MODULE)` throws; needs WebKit internals |
| interrupt handler (`--profile` sampler, watchdog) | no-op; `--profile` uses JSC's sampling profiler thread instead |
| unhandled-rejection tracker | stored, never called; needs a hook in WebKit's `JSGlobalObject` |
| memory limit, `JS_ComputeMemoryUsage`, GC threshold | no-ops |
| pooled allocator (`js_pool_alloc.h`) | not used; JSC uses libc malloc |
