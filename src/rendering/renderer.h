/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#ifndef RMMZ_RENDERER_H
#define RMMZ_RENDERER_H

#include "rendering/sprite_batch.h"

#include <stdbool.h>
#include <stdint.h>

/* High-level renderer that wraps the sprite batch and manages
   framebuffer objects for render-to-texture. */
typedef struct Renderer Renderer;

/* Create the renderer with the given viewport dimensions. */
Renderer *renderer_create(int width, int height);

/* Destroy the renderer and all associated resources. */
void renderer_destroy(Renderer *r);

/* Resize the renderer viewport. */
void renderer_resize(Renderer *r, int width, int height);

/* Get current viewport dimensions. */
void renderer_get_size(const Renderer *r, int *out_w, int *out_h);

/* Set the screen viewport for letterbox scaling. Applied whenever the default
   framebuffer is bound; FBO rendering uses its own viewport and restores this. */
void renderer_set_screen_viewport(Renderer *r, int x, int y, int w, int h);

/* Begin a new render frame. Clears the screen to opaque black. */
void renderer_begin_frame(Renderer *r);

/* Begin a frame clearing to transparent black (for FBO rendering). */
void renderer_begin_frame_transparent(Renderer *r);

/* End the current frame. Flushes the sprite batch. */
void renderer_end_frame(Renderer *r);

/* Composite the finished frame onto the window and make it visible.
 *
 * Frames are drawn into an offscreen surface rather than straight to the
 * window: no graphics API guarantees that the image being presented can be
 * read back, and screenshots and the visual-regression harness both need to
 * read it. Call once per frame, after renderer_end_frame. */
void renderer_present(Renderer *r);
/* GPU backend: drawn over the finished frame just before it is shown (the PS5's touchpad pointer); NULL for none */
extern void (*renderer_present_overlay)(void);

/* Size of the window's drawable, which the offscreen surface matches. Call on
 * startup and whenever the window resizes; the game's own resolution, set by
 * renderer_resize, is a separate thing. */
void renderer_set_output_size(Renderer *r, int width, int height);

/* Size of the window the frame is finally shown in. When it differs from the output size the frame is scaled up
   (keeping its aspect ratio, black bars around it), so the game can be drawn at its own resolution and stretched
   once at the end. 0 x 0 means "same as the output size". */
void renderer_set_present_size(Renderer *r, int width, int height);

/* Deletes a texture at the start of the next frame (draws queued for this frame may still use it). For textures whose
   owner the script engine has just garbage collected. */
void renderer_queue_texture_delete(uint32_t texture);

/* The finished frame (the offscreen surface renderer_present() would blit) as RGBA, bottom row first, in a buffer
   that belongs to the renderer and stays valid until the next call. NULL when there is no frame. */
const uint8_t *renderer_read_screen(Renderer *r, int *out_w, int *out_h);

/* Get the sprite batch for direct draw calls. */
SpriteBatch *renderer_get_batch(Renderer *r);

/* Texture management */

/* Create a blank RGBA texture. Returns GL texture ID, 0 on failure. */
uint32_t renderer_create_texture(int width, int height);

/* Upload pixel data to an existing texture. */
void renderer_update_texture(uint32_t texture, int width, int height,
                             const uint8_t *pixels);

/* Delete a texture. */
void renderer_delete_texture(uint32_t texture);

/* Set a texture's sampling filter (linear or nearest). Mirrors PIXI's
   BaseTexture.scaleMode; RPG Maker bitmaps default to smooth. */
void renderer_set_texture_filter(uint32_t texture, bool linear);

/* Framebuffer objects (for RenderTexture) */

/* Create a framebuffer with an attached color texture. Returns the FBO ID
   (0 on failure) and writes the color texture ID to *out_texture. */
uint32_t renderer_create_fbo(int width, int height, uint32_t *out_texture);

/* Delete a framebuffer and its attached texture. */
void renderer_delete_fbo(uint32_t fbo, uint32_t texture);

/* Bind an FBO as the current render target. Pass 0 to unbind. */
void renderer_bind_fbo(Renderer *r, uint32_t fbo, int width, int height);

/* Unbind any FBO, restoring the default framebuffer. */
void renderer_unbind_fbo(Renderer *r);

/* Read RGBA pixels from an FBO (0 = default framebuffer) into a malloc'd
   width*height*4 buffer that the caller frees. Returns NULL on failure. */
uint8_t *renderer_read_pixels(Renderer *r, uint32_t fbo, int width, int height);

/* Re-bind the sprite batch state after external GL state changes
   (e.g., after filter rendering uses a different shader/VAO). */
void renderer_rebind_batch(Renderer *r);

/* Set GL scissor, transforming game-space coordinates to window coordinates
   when rendering to the default framebuffer with a letterbox viewport. */
void renderer_set_scissor(Renderer *r, int x, int y, int w, int h);

/* Disable the GL scissor test. */
void renderer_clear_scissor(Renderer *r);

#endif /* RMMZ_RENDERER_H */
