/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the PS5 port contributors */
/*
 * Scaling of the finished frame to the screen on the CPU (PS5 port).
 *
 * The console's OpenGL is a software rasteriser: blitting the game's picture (say 1280x720) up to the screen
 * (1920x1080) through it costs several milliseconds, and the picture then still has to be copied twice more before
 * the video driver gets it. Doing the scaling here, straight into the window's pixel buffer, avoids both, and the
 * rows are split between helper threads.
 *
 * Pixels are RGBA, 8 bits per channel, in 32-bit words; the frame comes out of OpenGL upside down (bottom row first).
 */

#include "rendering/present_cpu.h"
#include "rendering/canvas2d.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t __attribute__((may_alias)) px32;

/* Both pixels weighted per channel: (a * (256 - w) + b * w) / 256, w in 0..256. Two channels at a time in one word. */
static inline uint32_t lerp32(uint32_t a, uint32_t b, uint32_t w)
{
    uint32_t iw = 256 - w;
    uint32_t rb = (((a & 0x00FF00FFu) * iw + (b & 0x00FF00FFu) * w) >> 8) & 0x00FF00FFu;
    uint32_t ag = ((((a >> 8) & 0x00FF00FFu) * iw + ((b >> 8) & 0x00FF00FFu) * w)) & 0xFF00FF00u;
    return rb | ag;
}

typedef struct {
    const uint8_t *src;
    int            sw, sh, flip;
    uint8_t       *dst;
    int            pitch;          /* bytes per destination row */
    int            dx, dy, dw, dh;
    int            smooth;
    int           *xi;             /* per destination column: left source column */
    uint16_t      *xw;             /* per destination column: weight (0..256) of the column to the right */
} ScaleJob;

static inline const px32 *src_row(const ScaleJob *j, int y)
{
    if (y < 0) y = 0;
    if (y > j->sh - 1) y = j->sh - 1;
    int row = j->flip ? j->sh - 1 - y : y;
    return (const px32 *)(j->src + (size_t)row * (size_t)j->sw * 4);
}

static void scale_rows(void *arg, int y0, int y1)
{
    const ScaleJob *j = arg;
    px32 *tmp = NULL;
    if (j->smooth) {
        tmp = malloc(((size_t)j->sw + 1) * 4);
        if (!tmp) return;
    }
    for (int y = y0; y < y1; y++) {
        px32 *out = (px32 *)(j->dst + (size_t)(j->dy + y) * (size_t)j->pitch) + j->dx;
        if (!j->smooth) {
            int sy = (int)((((int64_t)2 * y + 1) * j->sh) / ((int64_t)2 * j->dh));
            const px32 *row = src_row(j, sy);
            for (int x = 0; x < j->dw; x++) out[x] = row[j->xi[x]];
            continue;
        }
        double fv = ((double)y + 0.5) * j->sh / j->dh - 0.5;
        int sy0 = (int)floor(fv);
        double wv = fv - sy0;
        if (sy0 < 0) { sy0 = 0; wv = 0; }
        if (sy0 >= j->sh - 1) { sy0 = j->sh - 1; wv = 0; }
        const px32 *r0 = src_row(j, sy0), *r1 = src_row(j, sy0 + 1);
        uint32_t w8 = (uint32_t)(wv * 256.0 + 0.5);
        if (w8 == 0) {
            memcpy(tmp, r0, (size_t)j->sw * 4);
        } else {
            for (int x = 0; x < j->sw; x++) tmp[x] = lerp32(r0[x], r1[x], w8);
        }
        tmp[j->sw] = tmp[j->sw - 1];
        for (int x = 0; x < j->dw; x++) {
            int x0 = j->xi[x];
            uint32_t w = j->xw[x];
            out[x] = w ? lerp32(tmp[x0], tmp[x0 + 1], w) : tmp[x0];
        }
    }
    free(tmp);
}

typedef struct { uint8_t *dst; int pitch; int pw, ph, dx, dy, dw, dh; } BarJob;

/* Black (opaque) everywhere outside the picture. */
static void clear_bars(uint8_t *dst, int pitch, int pw, int ph, int dx, int dy, int dw, int dh)
{
    const uint32_t black = 0xFF000000u;
    for (int y = 0; y < ph; y++) {
        px32 *row = (px32 *)(dst + (size_t)y * (size_t)pitch);
        if (y < dy || y >= dy + dh) {
            for (int x = 0; x < pw; x++) row[x] = black;
        } else {
            for (int x = 0; x < dx; x++) row[x] = black;
            for (int x = dx + dw; x < pw; x++) row[x] = black;
        }
    }
}

void present_cpu_scale(const uint8_t *src, int sw, int sh, int src_bottom_up,
                       uint8_t *dst, int dst_pitch, int pw, int ph, int mode)
{
    if (!src || !dst || sw <= 0 || sh <= 0 || pw <= 0 || ph <= 0) return;

    double sx = (double)pw / sw, sy = (double)ph / sh;
    double scale = sx < sy ? sx : sy;
    int dw = (int)((double)sw * scale + 0.5), dh = (int)((double)sh * scale + 0.5);
    if (dw > pw) dw = pw;
    if (dh > ph) dh = ph;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int dx = (pw - dw) / 2, dy = (ph - dh) / 2;

    int exact = fabs(scale - round(scale)) < 0.001;
    int smooth = mode == 2 ? 1 : (mode == 1 ? 0 : !exact);
    if (dw == sw && dh == sh) smooth = 0;

    ScaleJob job;
    memset(&job, 0, sizeof job);
    job.src = src; job.sw = sw; job.sh = sh; job.flip = src_bottom_up;
    job.dst = dst; job.pitch = dst_pitch;
    job.dx = dx; job.dy = dy; job.dw = dw; job.dh = dh;
    job.smooth = smooth;
    job.xi = malloc((size_t)dw * sizeof(int));
    job.xw = malloc((size_t)dw * sizeof(uint16_t));
    if (!job.xi || !job.xw) { free(job.xi); free(job.xw); return; }
    for (int x = 0; x < dw; x++) {
        if (!smooth) {
            job.xi[x] = (int)((((int64_t)2 * x + 1) * sw) / ((int64_t)2 * dw));
            job.xw[x] = 0;
        } else {
            double fu = ((double)x + 0.5) * sw / dw - 0.5;
            int x0 = (int)floor(fu);
            double w = fu - x0;
            if (x0 < 0) { x0 = 0; w = 0; }
            if (x0 >= sw - 1) { x0 = sw - 1; w = 0; }
            job.xi[x] = x0;
            job.xw[x] = (uint16_t)(w * 256.0 + 0.5);
        }
    }

    if (dw != pw || dh != ph) clear_bars(dst, dst_pitch, pw, ph, dx, dy, dw, dh);
    canvas2d_parallel_rows(scale_rows, &job, 0, dh, (long)dw * dh * (smooth ? 3 : 1));
    free(job.xi);
    free(job.xw);
}
