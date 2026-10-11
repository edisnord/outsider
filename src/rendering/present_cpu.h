/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the PS5 port contributors */
/*
 * Scaling of the finished frame to the screen on the CPU (PS5 port), see present_cpu.c.
 */
#ifndef RMMZ_PRESENT_CPU_H
#define RMMZ_PRESENT_CPU_H

#include <stdint.h>

/* Scales the picture src (sw x sh, RGBA, 4 bytes per pixel; src_bottom_up: the bottom row comes first, as OpenGL's
   read-back does) to fit the screen buffer dst (pw x ph, dst_pitch bytes per row) keeping the aspect ratio and
   centring it; the rest is filled with black. mode: 0 automatic (sharp for whole-number scales, smooth otherwise),
   1 sharp, 2 smooth. */
void present_cpu_scale(const uint8_t *src, int sw, int sh, int src_bottom_up,
                       uint8_t *dst, int dst_pitch, int pw, int ph, int mode);

#endif
