/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#ifndef RMMZ_CANVAS2D_H
#define RMMZ_CANVAS2D_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opaque handle for a Canvas2D rendering context. */
typedef uint32_t Canvas2DHandle;
#define CANVAS2D_HANDLE_INVALID 0

/* Text alignment constants. */
enum {
    CANVAS2D_ALIGN_LEFT   = 0,
    CANVAS2D_ALIGN_CENTER = 1,
    CANVAS2D_ALIGN_RIGHT  = 2,
    CANVAS2D_ALIGN_START  = 3,
    CANVAS2D_ALIGN_END    = 4,
};

/* Text baseline constants. */
enum {
    CANVAS2D_BASELINE_TOP         = 0,
    CANVAS2D_BASELINE_HANGING     = 1,
    CANVAS2D_BASELINE_MIDDLE      = 2,
    CANVAS2D_BASELINE_ALPHABETIC  = 3,
    CANVAS2D_BASELINE_IDEOGRAPHIC = 4,
    CANVAS2D_BASELINE_BOTTOM      = 5,
};

/* Composite operation constants. */
enum {
    CANVAS2D_COMP_SOURCE_OVER = 0,
    CANVAS2D_COMP_SOURCE_ATOP = 1,
    CANVAS2D_COMP_SOURCE_IN   = 2,
    CANVAS2D_COMP_SOURCE_OUT  = 3,
    CANVAS2D_COMP_DEST_OVER   = 4,
    CANVAS2D_COMP_DEST_ATOP   = 5,
    CANVAS2D_COMP_DEST_IN     = 6,
    CANVAS2D_COMP_DEST_OUT    = 7,
    CANVAS2D_COMP_LIGHTER     = 8,
    CANVAS2D_COMP_COPY        = 9,
    CANVAS2D_COMP_XOR         = 10,
    CANVAS2D_COMP_MULTIPLY    = 11,
    CANVAS2D_COMP_SCREEN      = 12,
    CANVAS2D_COMP_DIFFERENCE  = 13,
    CANVAS2D_COMP_SATURATION  = 14,
};

/* Initialize the Canvas2D subsystem. Call once at startup. */
void canvas2d_init(void);

/* Shut down and free all Canvas2D contexts. */
void canvas2d_shutdown(void);

/* Create a new Canvas2D context with the given dimensions.
   The backing pixel buffer is initialized to transparent black. */
Canvas2DHandle canvas2d_create(int width, int height);

/* Destroy a Canvas2D context and free its resources. */
void canvas2d_destroy(Canvas2DHandle handle);

/* Resize the canvas. Existing pixel data is discarded (cleared to transparent). */
void canvas2d_resize(Canvas2DHandle handle, int width, int height);

/* Get the canvas dimensions. Returns false if handle is invalid. */
bool canvas2d_get_size(Canvas2DHandle handle, int *out_w, int *out_h);

/* Get a pointer to the raw RGBA pixel data. Returns NULL if handle is invalid.
   The buffer has width * height * 4 bytes. Caller must NOT free the pointer. */
uint8_t *canvas2d_get_pixels(Canvas2DHandle handle);

/* Drawing state */

void canvas2d_set_fill_color(Canvas2DHandle h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void canvas2d_set_stroke_color(Canvas2DHandle h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void canvas2d_set_global_alpha(Canvas2DHandle h, float alpha);
void canvas2d_set_composite_op(Canvas2DHandle h, int op);
void canvas2d_set_line_width(Canvas2DHandle h, float width);
void canvas2d_set_text_align(Canvas2DHandle h, int align);
void canvas2d_set_text_baseline(Canvas2DHandle h, int baseline);

/* State stack (save/restore). */
void canvas2d_save(Canvas2DHandle h);
void canvas2d_restore(Canvas2DHandle h);

/* Drawing operations */

/* Fill a rectangle with the current fill color. */
void canvas2d_fill_rect(Canvas2DHandle h, int x, int y, int w, int h2);

/* Fill a rectangle with a gradient. Linear: (p0,p1) -> (p2,p3) is the gradient line. Radial: centre
   (p0,p1), radii p2 (t = 0) to p3 (t = 1). offsets[i] and rgba[4*i..4*i+3] are the n colour stops,
   sorted by offset. Pixels are blended like canvas2d_fill_rect (global alpha, composite operation). */
void canvas2d_fill_gradient_rect(Canvas2DHandle h, int x, int y, int w, int h2, bool radial,
                                 double p0, double p1, double p2, double p3,
                                 const double *offsets, const uint8_t *rgba, int n);

/* Fill polygons (nonzero winding, no anti-aliasing). pts holds x,y pairs of all subpaths one after the
   other, counts[i] is the number of points of subpath i. Clipped to the canvas and, if has_clip, to the
   given rectangle. gradient_kind: -1 = current fill colour, 0 = linear, 1 = radial (parameters and stops
   as for canvas2d_fill_gradient_rect). */
void canvas2d_fill_path(Canvas2DHandle h, const double *pts, const int *counts, int nsub,
                        bool has_clip, int clip_x, int clip_y, int clip_w, int clip_h,
                        int gradient_kind, double p0, double p1, double p2, double p3,
                        const double *offsets, const uint8_t *rgba, int n);

/* Clear a rectangle to transparent black. */
void canvas2d_clear_rect(Canvas2DHandle h, int x, int y, int w, int h2);

/* Stroke the outline of a rectangle with the current stroke color. */
void canvas2d_stroke_rect(Canvas2DHandle h, int x, int y, int w, int h2);

/* Blit source rect (sx,sy,sw,sh) of an RGBA buffer to dest rect (dx,dy,dw,dh). */
void canvas2d_draw_image(Canvas2DHandle h,
                          const uint8_t *src_pixels, int src_w, int src_h,
                          int sx, int sy, int sw, int sh,
                          int dx, int dy, int dw, int dh);

/* Pixel manipulation */

/* Copy a region of the canvas pixel data into `out`.
   `out` must have space for w * h * 4 bytes. */
/* Adds r, g, b to every pixel's colour channels with clamping (RPG Maker MV Bitmap.adjustTone). */
void canvas2d_adjust_tone(Canvas2DHandle h, double r, double g, double b);

void canvas2d_get_image_data(Canvas2DHandle h, int x, int y, int w, int h2,
                              uint8_t *out);

/* Write pixel data into the canvas at (x, y).
   `data` must have w * h * 4 bytes (RGBA). */
void canvas2d_put_image_data(Canvas2DHandle h, const uint8_t *data,
                              int x, int y, int w, int h2);

/* Font and text */

/* Load a TrueType font from a file. Returns true on success. */
bool canvas2d_load_font(const char *name, const uint8_t *data, size_t size);

/* Set the current font for a context by name and pixel size. */
void canvas2d_set_font(Canvas2DHandle h, const char *name, float pixel_size);

/* Render filled text at (x, y). */
void canvas2d_fill_text(Canvas2DHandle h, const char *text, float x, float y);

/* Render stroked text at (x, y) (outline only). */
void canvas2d_stroke_text(Canvas2DHandle h, const char *text, float x, float y);

/* Measure the width of `text` in pixels using the current font. */
float canvas2d_measure_text(Canvas2DHandle h, const char *text);
bool canvas2d_measure_text_bounds(Canvas2DHandle h, const char *text, float out[4]);

/* Return the number of active Canvas2D contexts. */
size_t canvas2d_context_count(void);

/* Pixel memory (bytes) held by all canvases. */
size_t canvas2d_bytes_alive(void);
/* Sizes of the live canvases (diagnostics): fills w/h up to max entries, returns the count. */
size_t canvas2d_list_sizes(int *w, int *h, size_t max);

/* Runs fn(arg, y0', y1') over the rows [y0, y1), split between the calling thread and helper threads when the job is
   big (work_px: roughly the number of pixels touched). One caller at a time. */
void canvas2d_parallel_rows(void (*fn)(void *arg, int y0, int y1), void *arg, int y0, int y1, long work_px);

#endif /* RMMZ_CANVAS2D_H */
