/* JSC build: the module loader of quickjs-libc has no counterpart (the C API has no module support). */
#ifndef RMMZ_QJS_LIBC_JSC_H
#define RMMZ_QJS_LIBC_JSC_H
#include "quickjs.h"
#define js_module_loader NULL
#define js_module_check_attributes NULL
#endif
