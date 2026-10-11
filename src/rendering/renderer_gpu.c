/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

/* SDL3 GPU implementation of renderer.h.
 *
 * SDL3 has no framebuffer objects: a render target is just a texture. The FBO
 * id the JS layer keeps is therefore the texture id itself, which keeps
 * createRenderTexture's two-handle contract intact without a second table. */

#include "rendering/renderer.h"
#include "rendering/gpu_backend.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct Renderer {
    SpriteBatch *batch;
    int          width;
    int          height;
    int          vp_x, vp_y, vp_w, vp_h;
    uint32_t     bound_fbo;
    int          bound_w, bound_h;   /* dimensions of the bound render target */
};

Renderer *renderer_create(int width, int height)
{
    Renderer *r = calloc(1, sizeof(Renderer));
    if (!r) return NULL;

    r->width = width;
    r->height = height;
    r->vp_x = 0;
    r->vp_y = 0;
    r->vp_w = width;
    r->vp_h = height;

    r->batch = sprite_batch_create(4096);
    if (!r->batch) {
        free(r);
        return NULL;
    }

    sprite_batch_set_projection(r->batch, (float)width, (float)height);
    gpu_frame_resize(width, height);
    /* No target or viewport recorded here: renderer_begin_frame records both
       at the start of every frame. */
    return r;
}

void renderer_destroy(Renderer *r)
{
    if (!r) return;
    sprite_batch_destroy(r->batch);
    free(r);
}

/* Viewport and scissor rectangles need no vertical adjustment: OpenGL
   measures them from its framebuffer origin, which is the first row of the
   image, and SDL3 measures them from the first row too. Because targets here
   are stored bottom-up (see the projection in sprite_batch_gpu.c), those are
   the same rows. Only the direction of NDC inside the rectangle differs, and
   the projection already accounts for that. */
static void record_viewport(int x, int y, int w, int h)
{
    gpu_record_viewport(x, y, w, h);
}

void renderer_resize(Renderer *r, int width, int height)
{
    if (!r) return;
    r->width = width;
    r->height = height;
    sprite_batch_set_projection(r->batch, (float)width, (float)height);

    r->vp_x = 0;
    r->vp_y = 0;
    r->vp_w = width;
    r->vp_h = height;

    gpu_frame_resize(width, height);
    if (r->bound_fbo == 0)
        record_viewport(0, 0, width, height);
}

void renderer_get_size(const Renderer *r, int *out_w, int *out_h)
{
    if (!r) return;
    if (out_w) *out_w = r->width;
    if (out_h) *out_h = r->height;
}

void renderer_set_screen_viewport(Renderer *r, int x, int y, int w, int h)
{
    if (!r) return;
    r->vp_x = x;
    r->vp_y = y;
    r->vp_w = w;
    r->vp_h = h;

    if (r->bound_fbo == 0)
        record_viewport(x, y, w, h);
}

/* Both frame starts clear the current target and restore the screen viewport,
   exactly as the GL backend does; only the clear colour differs. */
static void begin_frame_with_clear(Renderer *r, float alpha)
{
    if (r->bound_fbo == 0) {
        gpu_record_target(0, r->width, r->height);
        record_viewport(r->vp_x, r->vp_y, r->vp_w, r->vp_h);
    }
    gpu_record_clear(0.0f, 0.0f, 0.0f, alpha);
    sprite_batch_begin(r->batch);
}

void renderer_begin_frame(Renderer *r)
{
    if (!r) return;
    begin_frame_with_clear(r, 1.0f);
}

void renderer_begin_frame_transparent(Renderer *r)
{
    if (!r) return;
    begin_frame_with_clear(r, 0.0f);
}

void renderer_end_frame(Renderer *r)
{
    if (!r) return;
    sprite_batch_end(r->batch);
}

void (*renderer_present_overlay)(void);

void renderer_present(Renderer *r)
{
    (void)r;
    if (renderer_present_overlay) renderer_present_overlay();
    gpu_frame_present();
}

void renderer_set_present_size(Renderer *r, int width, int height)
{
    /* The screen texture is the window's size already: nothing is scaled at the end */
    (void)r; (void)width; (void)height;
}

const uint8_t *renderer_read_screen(Renderer *r, int *out_w, int *out_h)
{
    static uint8_t *pixels;
    int w = 0, h = 0;
    (void)r;
    gpu_screen_size(&w, &h);
    if (w <= 0 || h <= 0) return NULL;
    free(pixels);
    pixels = gpu_texture_download(0, w, h);
    if (!pixels) return NULL;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return pixels;
}

void renderer_set_output_size(Renderer *r, int width, int height)
{
    (void)r;
    /* The backend sizes its screen texture from the window itself. */
    gpu_frame_resize(width, height);
}

SpriteBatch *renderer_get_batch(Renderer *r)
{
    return r ? r->batch : NULL;
}

/* Texture management */

uint32_t renderer_create_texture(int width, int height)
{
    return gpu_texture_create(width, height, false);
}

void renderer_update_texture(uint32_t texture, int width, int height,
                             const uint8_t *pixels)
{
    gpu_texture_upload(texture, width, height, pixels);
}

void renderer_set_texture_filter(uint32_t texture, bool linear)
{
    gpu_texture_set_filter(texture, linear);
}

void renderer_queue_texture_delete(uint32_t texture)
{
    gpu_texture_destroy(texture);
}

void renderer_delete_texture(uint32_t texture)
{
    gpu_texture_destroy(texture);
}

/* Render targets */

uint32_t renderer_create_fbo(int width, int height, uint32_t *out_texture)
{
    uint32_t tex = gpu_texture_create(width, height, true);
    if (!tex) {
        if (out_texture) *out_texture = 0;
        return 0;
    }
    /* Start transparent, matching the GL backend's clear on creation. */
    gpu_record_target(tex, width, height);
    gpu_record_viewport(0, 0, width, height);
    gpu_record_clear(0.0f, 0.0f, 0.0f, 0.0f);
    gpu_record_target(0, width, height);

    if (out_texture) *out_texture = tex;
    return tex;
}

void renderer_delete_fbo(uint32_t fbo, uint32_t texture)
{
    /* The render target and its colour texture are the same object here. */
    gpu_texture_destroy(fbo);
    if (texture && texture != fbo) gpu_texture_destroy(texture);
}

void renderer_bind_fbo(Renderer *r, uint32_t fbo, int width, int height)
{
    if (!r) return;

    sprite_batch_flush(r->batch);
    r->bound_fbo = fbo;
    r->bound_w = width;
    r->bound_h = height;

    gpu_record_target(fbo, width, height);
    gpu_record_viewport(0, 0, width, height);
    sprite_batch_set_projection(r->batch, (float)width, (float)height);
}

void renderer_unbind_fbo(Renderer *r)
{
    if (!r) return;

    sprite_batch_flush(r->batch);
    r->bound_fbo = 0;

    gpu_record_target(0, r->width, r->height);
    record_viewport(r->vp_x, r->vp_y, r->vp_w, r->vp_h);
    sprite_batch_set_projection(r->batch, (float)r->width, (float)r->height);
}

uint8_t *renderer_read_pixels(Renderer *r, uint32_t fbo, int width, int height)
{
    if (!r || width <= 0 || height <= 0) return NULL;

    uint64_t alloc_size = (uint64_t)width * (uint64_t)height * 4;
    if (alloc_size > SIZE_MAX) return NULL;

    sprite_batch_flush(r->batch);

    /* Render targets hold their first row at the bottom, as GL textures do,
       so the rows still need reversing to come out top-down. */
    uint8_t *pixels = gpu_texture_download(fbo, width, height);
    if (!pixels) return NULL;

    size_t row_bytes = (size_t)width * 4;
    uint8_t *tmp = malloc(row_bytes);
    if (!tmp) {
        free(pixels);
        return NULL;
    }
    for (int y = 0; y < height / 2; y++) {
        uint8_t *top = pixels + y * row_bytes;
        uint8_t *bot = pixels + (height - 1 - y) * row_bytes;
        memcpy(tmp, top, row_bytes);
        memcpy(top, bot, row_bytes);
        memcpy(bot, tmp, row_bytes);
    }
    free(tmp);

    return pixels;
}

void renderer_rebind_batch(Renderer *r)
{
    if (r && r->batch) sprite_batch_rebind(r->batch);
}

void renderer_set_scissor(Renderer *r, int x, int y, int w, int h)
{
    if (!r) return;

    if (r->bound_fbo == 0 && r->width > 0 && r->height > 0) {
        /* On screen, map game-space to the letterbox viewport. Floor the
           origin and ceil the far edge so fractional scaling never clips
           boundary pixels. */
        float sx = (float)r->vp_w / (float)r->width;
        float sy = (float)r->vp_h / (float)r->height;
        int x1 = (int)floorf(x * sx + r->vp_x);
        int y1 = (int)floorf(y * sy + r->vp_y);
        int x2 = (int)ceilf((x + w) * sx + r->vp_x);
        int y2 = (int)ceilf((y + h) * sy + r->vp_y);
        x = x1;
        y = y1;
        w = x2 - x1;
        h = y2 - y1;
    }

    if (w < 0) w = 0;
    if (h < 0) h = 0;
    gpu_record_scissor(x, y, w, h);
}

void renderer_clear_scissor(Renderer *r)
{
    (void)r;
    gpu_record_scissor_off();
}
