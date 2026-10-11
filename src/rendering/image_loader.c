/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/image_loader.h"
#ifdef RMMZ_HAVE_WEBP
#include <webp/decode.h>
#endif
#include "io/file_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>   /* INT_MAX */

/* stb_image implementation lives in this translation unit. */
#define STB_IMAGE_IMPLEMENTATION
/* Browsers decode by content, not extension: games ship JPEG data under
   .png names now and then (Pocket Mirror's perlinNoise.png). */
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO   /* We handle file I/O ourselves. */
#include "stb_image.h"

#include "rendering/renderer.h"

typedef struct CacheEntry {
    char        *key;       /* File path or blob key (owned). */
    uint8_t     *pixels;    /* RGBA pixel data (stbi_image_free). */
    int          width;
    int          height;
    uint32_t     gl_texture; /* Renderer texture id, 0 if none. */
    ImageHandle  handle;
    int          refcount;  /* Loads sharing this entry; freed when it hits 0. */
    struct CacheEntry *next;
} CacheEntry;

#define CACHE_BUCKETS 256
#define MAX_HANDLES   4096

static CacheEntry *s_cache[CACHE_BUCKETS];
static CacheEntry *s_handle_index[MAX_HANDLES]; /* Direct lookup by handle. */
static bool         s_gl_enabled  = false;
static size_t       s_cache_count = 0;

/* FNV-1a hash of a cache key, reduced to a bucket index. */
static unsigned int hash_key(const char *key)
{
    unsigned int h = 2166136261u;
    for (const char *p = key; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    return h % CACHE_BUCKETS;
}

static CacheEntry *cache_find_by_key(const char *key)
{
    unsigned int bucket = hash_key(key);
    for (CacheEntry *e = s_cache[bucket]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) return e;
    }
    return NULL;
}

static CacheEntry *cache_find_by_handle(ImageHandle handle)
{
    if (handle == 0 || handle >= MAX_HANDLES) return NULL;
    return s_handle_index[handle];
}

/* Texture creation goes through the renderer so the image cache works with
   whichever rendering backend was built. */
static uint32_t upload_gl_texture(const uint8_t *pixels, int w, int h)
{
    uint32_t tex = renderer_create_texture(w, h);
    if (!tex) return 0;
    renderer_update_texture(tex, w, h, pixels);
    return tex;
}

uint32_t image_upload_rgba_texture(const uint8_t *rgba, int width, int height)
{
    if (!s_gl_enabled || !rgba || width <= 0 || height <= 0) return 0;
    return upload_gl_texture(rgba, width, height);
}

static void delete_gl_texture(uint32_t tex)
{
    if (tex) renderer_queue_texture_delete(tex);
}

static CacheEntry *cache_insert(const char *key, uint8_t *pixels,
                                int w, int h)
{
    CacheEntry *entry = calloc(1, sizeof(CacheEntry));
    if (!entry) return NULL;

    ImageHandle new_handle = 0;
    for (ImageHandle i = 1; i < MAX_HANDLES; i++) {
        if (s_handle_index[i] == NULL) {
            new_handle = i;
            break;
        }
    }
    if (new_handle == 0) {
        fprintf(stderr, "image_loader: handle limit exceeded\n");
        free(entry);
        return NULL;
    }

    entry->key    = strdup(key);
    if (!entry->key) {
        fprintf(stderr, "image_loader: out of memory duplicating cache key\n");
        free(entry);
        return NULL;
    }
    entry->pixels = pixels;
    entry->width  = w;
    entry->height = h;
    entry->handle = new_handle;
    entry->refcount = 1;

    if (s_gl_enabled) {
        entry->gl_texture = upload_gl_texture(pixels, w, h);
    }

    unsigned int bucket = hash_key(key);
    entry->next = s_cache[bucket];
    s_cache[bucket] = entry;
    s_handle_index[entry->handle] = entry;
    s_cache_count++;

    return entry;
}

static void cache_remove(CacheEntry *entry)
{
    unsigned int bucket = hash_key(entry->key);
    CacheEntry **pp = &s_cache[bucket];
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->next;
            break;
        }
        pp = &(*pp)->next;
    }

    if (entry->handle < MAX_HANDLES) {
        s_handle_index[entry->handle] = NULL;
    }
    delete_gl_texture(entry->gl_texture);
    stbi_image_free(entry->pixels);
    free(entry->key);
    free(entry);
    s_cache_count--;
}

void image_loader_init(bool enable_gl)
{
    s_gl_enabled = enable_gl;
    s_cache_count = 0;
    memset(s_cache, 0, sizeof(s_cache));
    memset(s_handle_index, 0, sizeof(s_handle_index));
}

void image_loader_shutdown(void)
{
    image_cache_clear();
}

/* Decodes PNG/JPEG/GIF/BMP (stb_image) and, when libwebp is built in, WebP. Some games ship WebP pictures under .png
   names (the browser engine recognises the content, not the name). The result is RGBA, released with free(). */
static uint8_t *decode_rgba(const uint8_t *data, size_t size, int *w, int *h)
{
#ifdef RMMZ_HAVE_WEBP
    if (size > 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) {
        uint8_t *px = WebPDecodeRGBA(data, size, w, h);
        if (px) return px;
    }
#endif
    int channels;
    return stbi_load_from_memory(data, (int)size, w, h, &channels, 4);
}

ImageHandle image_load(const char *path)
{
    if (!path) return IMAGE_HANDLE_INVALID;

    CacheEntry *existing = cache_find_by_key(path);
    if (existing) {
        /* Each sharing loader must balance this with image_free. */
        existing->refcount++;
        return existing->handle;
    }

    size_t file_size = 0;
    uint8_t *file_data = file_io_read_binary(path, &file_size);
    if (!file_data) {
        fprintf(stderr, "image_load: failed to read file: %s\n", path);
        return IMAGE_HANDLE_INVALID;
    }

    if (file_size > INT_MAX) {
        fprintf(stderr, "image_load: file too large: %s\n", path);
        free(file_data);
        return IMAGE_HANDLE_INVALID;
    }
    int w, h;
    uint8_t *pixels = decode_rgba(file_data, file_size, &w, &h);
    free(file_data);

    if (!pixels) {
        fprintf(stderr, "image_load: failed to decode image: %s (%s)\n",
                path, stbi_failure_reason());
        return IMAGE_HANDLE_INVALID;
    }

    CacheEntry *entry = cache_insert(path, pixels, w, h);
    if (!entry) {
        stbi_image_free(pixels);
        return IMAGE_HANDLE_INVALID;
    }

    return entry->handle;
}

ImageHandle image_load_from_memory(const char *key,
                                   const uint8_t *data, size_t size)
{
    if (!key) return IMAGE_HANDLE_INVALID;

    /* A cache hit is valid even when data is NULL. */
    CacheEntry *existing = cache_find_by_key(key);
    if (existing) {
        existing->refcount++;
        return existing->handle;
    }

    if (!data || size == 0) return IMAGE_HANDLE_INVALID;
    if (size > INT_MAX) return IMAGE_HANDLE_INVALID;

    int w, h;
    uint8_t *pixels = decode_rgba(data, size, &w, &h);
    if (!pixels) {
        fprintf(stderr, "image_load_from_memory: failed to decode (%s)\n",
                stbi_failure_reason());
        return IMAGE_HANDLE_INVALID;
    }

    CacheEntry *entry = cache_insert(key, pixels, w, h);
    if (!entry) {
        stbi_image_free(pixels);
        return IMAGE_HANDLE_INVALID;
    }

    return entry->handle;
}

bool image_get_info(ImageHandle handle, ImageInfo *out)
{
    if (handle == IMAGE_HANDLE_INVALID || !out) return false;

    CacheEntry *e = cache_find_by_handle(handle);
    if (!e) return false;

    out->width      = e->width;
    out->height     = e->height;
    out->gl_texture = e->gl_texture;
    return true;
}

const uint8_t *image_get_pixels(ImageHandle handle)
{
    if (handle == IMAGE_HANDLE_INVALID) return NULL;
    CacheEntry *e = cache_find_by_handle(handle);
    return e ? e->pixels : NULL;
}

void image_free(ImageHandle handle)
{
    if (handle == IMAGE_HANDLE_INVALID) return;
    CacheEntry *e = cache_find_by_handle(handle);
    if (!e) return;
    /* Entries may be shared by several loads; pixels and the GL texture are
       released only when the last holder is gone. */
    if (--e->refcount <= 0) cache_remove(e);
}

size_t image_cache_count(void)
{
    return s_cache_count;
}

void image_cache_clear(void)
{
    for (int i = 0; i < CACHE_BUCKETS; i++) {
        CacheEntry *e = s_cache[i];
        while (e) {
            CacheEntry *next = e->next;
            delete_gl_texture(e->gl_texture);
            stbi_image_free(e->pixels);
            free(e->key);
            free(e);
            e = next;
        }
        s_cache[i] = NULL;
    }
    s_cache_count = 0;
    memset(s_handle_index, 0, sizeof(s_handle_index));
}
