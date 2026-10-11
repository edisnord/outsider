/*
 * A fast allocator for the JS engine.
 *
 * QuickJS allocates and frees enormous numbers of small blocks (objects, strings, closures, ...). On the
 * PlayStation 5 the system malloc is several times slower than on a PC for that pattern, so game scripts
 * that create a lot of garbage run many times slower than they should. This is a simple size-class
 * allocator: blocks come from big chunks, freed blocks go on a free list per size class and are reused,
 * and memory is never given back to the system (a game keeps needing about the same amount).
 *
 * Not thread safe: use it for exactly one JS runtime that is only used from one thread.
 * Header only, so that the engine and the small benchmark program can share it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RMMZ_JS_POOL_ALLOC_H
#define RMMZ_JS_POOL_ALLOC_H

#include <quickjs.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define POOL_HDR        16                 /* per block: capacity and kind, keeps 16-byte alignment */
#define POOL_SMALL_MAX  1024               /* 16-byte size classes up to here */
#define POOL_MED_MAX    65536              /* four classes per power of two up to here */
#define POOL_SMALL_CLS  (POOL_SMALL_MAX / 16)
#define POOL_NCLASS     (POOL_SMALL_CLS + 24)
#define POOL_CHUNK      (1024 * 1024)

typedef struct PoolHeader {
    size_t cap;     /* usable bytes after the header */
    size_t system;  /* 1: too big for the pool, allocated with malloc */
} PoolHeader;

typedef struct PoolFree { struct PoolFree *next; } PoolFree;

static PoolFree *pool_free_list[POOL_NCLASS];
static char *pool_bump[POOL_NCLASS];
static char *pool_bump_end[POOL_NCLASS];

/* Size class of a request of n bytes (1 <= n <= POOL_MED_MAX); *cap gets the block capacity. */
static inline int pool_class_of(size_t n, size_t *cap)
{
    if (n <= POOL_SMALL_MAX) {
        int cls = (int)((n + 15) / 16) - 1;
        *cap = (size_t)(cls + 1) * 16;
        return cls;
    }
    int lg = 63 - __builtin_clzll((unsigned long long)(n - 1));   /* 2^lg < n <= 2^(lg+1) */
    size_t step = ((size_t)1 << lg) / 4;
    size_t k = (n - ((size_t)1 << lg) + step - 1) / step;         /* 1..4 */
    *cap = ((size_t)1 << lg) + k * step;
    return POOL_SMALL_CLS + (lg - 10) * 4 + (int)k - 1;
}

static inline void *pool_alloc(size_t n)
{
    if (n == 0) n = 1;
    if (n > POOL_MED_MAX) {
        PoolHeader *h = (PoolHeader *)malloc(n + POOL_HDR);
        if (!h) return NULL;
        h->cap = n;
        h->system = 1;
        return (char *)h + POOL_HDR;
    }
    size_t cap;
    int cls = pool_class_of(n, &cap);
    PoolFree *f = pool_free_list[cls];
    if (f) {
        pool_free_list[cls] = f->next;
        return f;
    }
    size_t stride = cap + POOL_HDR;
    if (pool_bump[cls] + stride > pool_bump_end[cls] || !pool_bump[cls]) {
        char *chunk = (char *)malloc(POOL_CHUNK);
        if (!chunk) return NULL;
        pool_bump[cls] = chunk;
        pool_bump_end[cls] = chunk + POOL_CHUNK;
    }
    PoolHeader *h = (PoolHeader *)pool_bump[cls];
    pool_bump[cls] += stride;
    h->cap = cap;
    h->system = 0;
    return (char *)h + POOL_HDR;
}

static inline void pool_release(void *ptr)
{
    if (!ptr) return;
    PoolHeader *h = (PoolHeader *)((char *)ptr - POOL_HDR);
    if (h->system) {
        free(h);
        return;
    }
    size_t cap;
    int cls = pool_class_of(h->cap, &cap);
    PoolFree *f = (PoolFree *)ptr;
    f->next = pool_free_list[cls];
    pool_free_list[cls] = f;
}

static inline void *pool_resize(void *ptr, size_t n)
{
    if (!ptr) return pool_alloc(n);
    if (n == 0) {
        pool_release(ptr);
        return NULL;
    }
    PoolHeader *h = (PoolHeader *)((char *)ptr - POOL_HDR);
    if (h->system) {
        if (n > POOL_MED_MAX) {
            PoolHeader *nh = (PoolHeader *)realloc(h, n + POOL_HDR);
            if (!nh) return NULL;
            nh->cap = n;
            return (char *)nh + POOL_HDR;
        }
    } else if (n <= h->cap) {
        return ptr;   /* still fits in its block */
    }
    void *np = pool_alloc(n);
    if (!np) return NULL;
    memcpy(np, ptr, h->cap < n ? h->cap : n);
    pool_release(ptr);
    return np;
}

static void *js_pool_calloc(void *opaque, size_t count, size_t size)
{
    (void)opaque;
    if (size != 0 && count > (size_t)-1 / size) return NULL;
    size_t total = count * size;
    void *p = pool_alloc(total);
    if (p) memset(p, 0, total ? total : 1);
    return p;
}

static void *js_pool_malloc(void *opaque, size_t size) { (void)opaque; return pool_alloc(size); }
static void js_pool_free(void *opaque, void *ptr) { (void)opaque; pool_release(ptr); }
static void *js_pool_realloc(void *opaque, void *ptr, size_t size) { (void)opaque; return pool_resize(ptr, size); }
static size_t js_pool_usable_size(const void *ptr)
{
    return ptr ? ((const PoolHeader *)((const char *)ptr - POOL_HDR))->cap : 0;
}

static const JSMallocFunctions js_pool_malloc_functions = {
    js_pool_calloc, js_pool_malloc, js_pool_free, js_pool_realloc, js_pool_usable_size,
};

#endif /* RMMZ_JS_POOL_ALLOC_H */
