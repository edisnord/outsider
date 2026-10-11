/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#ifndef RMMZ_SPRITE_BATCH_H
#define RMMZ_SPRITE_BATCH_H

#include <stdbool.h>
#include <stdint.h>

/* Blend mode constants matching PIXI.BLEND_MODES. */
enum {
    BLEND_MODE_NORMAL   = 0,
    BLEND_MODE_ADD      = 1,
    BLEND_MODE_MULTIPLY = 2,
    BLEND_MODE_SCREEN   = 3,
    /* Internal: source already has premultiplied alpha (an FBO produced by
       the NORMAL mode above). Used to composite filter output back. */
    BLEND_MODE_NORMAL_PREMULT = 4,
};

/* Batched quad renderer that groups quads by texture and flushes them in
   minimal indexed draw calls, culling quads that fall outside the viewport. */
typedef struct SpriteBatch SpriteBatch;

/* Create a sprite batch with the given maximum number of quads per batch.
   Typically 4096. Returns NULL on failure. */
SpriteBatch *sprite_batch_create(int max_quads);

/* Destroy the sprite batch and free all resources. */
void sprite_batch_destroy(SpriteBatch *sb);

/* Set the orthographic projection matrix (top-left origin).
   Also sets the viewport bounds for frustum culling. */
void sprite_batch_set_projection(SpriteBatch *sb, float width, float height);

/* Begin a new batch frame. Must be called before draw calls. */
void sprite_batch_begin(SpriteBatch *sb);

/* Queue an axis-aligned textured quad (texture 0 = untextured white).
   UVs are normalized; tint is ARGB multiplied with the texture. */
/* Tells the batch that the render target has just been cleared to opaque black (1) or is not in that state (0); see
   sprite_batch.c. */
void sprite_batch_set_pristine(SpriteBatch *sb, int pristine);

void sprite_batch_draw(SpriteBatch *sb, uint32_t texture,
                       float x, float y, float w, float h,
                       float u0, float v0, float u1, float v1,
                       uint32_t tint, float alpha);

/* Queue a quad from four transformed corners in TL, TR, BR, BL order, which
   preserves rotation, skew and mirroring. UVs map TL=(u0,v0) .. BR=(u1,v1). */
void sprite_batch_draw_verts(SpriteBatch *sb, uint32_t texture,
                             float x0, float y0, float x1, float y1,
                             float x2, float y2, float x3, float y3,
                             float u0, float v0, float u1, float v1,
                             uint32_t tint, float alpha);

/* Like sprite_batch_draw_verts, but with a UV per corner (same TL, TR, BR,
   BL order), for textured Graphics fills whose texture space is not
   axis-aligned with the shape. */
void sprite_batch_draw_verts_uv(SpriteBatch *sb, uint32_t texture,
                                const float xy[8], const float uv[8],
                                uint32_t tint, float alpha);

/* Flush any queued quads to the GPU. Called automatically on texture
   change, batch full, or end of frame. */
void sprite_batch_flush(SpriteBatch *sb);

/* End the batch frame. Flushes remaining quads. */
void sprite_batch_end(SpriteBatch *sb);

/* Change blend mode. Flushes current batch if mode differs. */
void sprite_batch_set_blend_mode(SpriteBatch *sb, int mode);

/* Get the number of draw calls in the current/last frame. */
int sprite_batch_get_draw_calls(const SpriteBatch *sb);

/* Get the number of quads drawn in the current/last frame. */
int sprite_batch_get_quad_count(const SpriteBatch *sb);

/* Get the number of quads culled (off-screen) in the current/last frame. */
int sprite_batch_get_culled_count(const SpriteBatch *sb);

/* Re-bind shader, VAO, and projection after external GL state changes
   (e.g., after filter rendering uses a different shader). */
void sprite_batch_rebind(SpriteBatch *sb);

#endif /* RMMZ_SPRITE_BATCH_H */
