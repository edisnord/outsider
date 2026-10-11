/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#ifndef RMMZ_PLATFORM_H
#define RMMZ_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Platform Platform;

/* Create the window and OpenGL 4.5 context. Returns NULL on failure. */
/* Application name used for the window title and dialogs. */
#define RMMZ_APP_NAME "Outsider"

Platform *platform_init(const char *title, int width, int height);

/* Shut down and free all platform resources. */
void platform_shutdown(Platform *p);

/* Poll input events. Returns false when the user requests quit. */
bool platform_poll_events(Platform *p);

/* Swap the back buffer to the screen. */
void platform_swap_buffers(Platform *p);

/* Query the current window dimensions. */
void platform_get_window_size(Platform *p, int *w, int *h);

/* The size the frame is finally presented at. Usually the window size; on the PS5 the game is drawn at its own
   (smaller) size, see platform_set_virtual_size(), and scaled up to the screen. */
void platform_get_present_size(Platform *p, int *w, int *h);

/* PS5: show a finished frame (RGBA, bottom row first) by scaling it on the CPU into the window's pixel buffer and
   handing that to the video driver, instead of blitting through the software OpenGL. Returns false when this is
   not available (other platforms, RMMZ_GL_PRESENT=1): the caller then uses renderer_present() + platform_swap_buffers(). */
bool platform_present_pixels(Platform *p, const uint8_t *rgba, int w, int h);
/* True when platform_present_pixels() can be used (checked first so that the frame is only read back when it will be). */
bool platform_cpu_present_available(Platform *p);

/* Debug aid: the next frame shown by platform_present_pixels() is also written to `path` as a PPM file. */
void platform_request_dump(Platform *p, const char *path);

/* Tell the platform the size the game is drawn at (its resolution). Only the PS5 platform acts on it: the
   window size that the runtime and the game see becomes this size, and the picture is scaled to the screen. */
void platform_set_virtual_size(Platform *p, int w, int h);

/* Set the window title. */
void platform_set_window_title(Platform *p, const char *title);

/* Enter or leave (borderless desktop) fullscreen. */
void platform_set_fullscreen(Platform *p, bool fullscreen);

/* Query fullscreen state. */
bool platform_is_fullscreen(Platform *p);

/* Get the resolution of the display the window is on. */
void platform_get_display_size(Platform *p, int *w, int *h);

/* Get the display DPI scale factor (1.0 = normal, 2.0 = HiDPI). */
float platform_get_display_scale(Platform *p);

/* The desktop scaling of the window's monitor (1.25 for Windows "125%"),
   as far as it is not already folded into platform_get_display_scale().
   A DPI-aware window opens in raw pixels, so this is what a game window
   must be multiplied by to appear as large as a browser would draw it. */
float platform_get_desktop_scale(Platform *p);

/* Resize the window (client area, in pixels) and keep it centred. */
void platform_set_window_size(Platform *p, int w, int h);

/* Returns true (and the new size) once per resize since the last call. */
bool platform_check_resize(Platform *p, int *w, int *h);

#endif /* RMMZ_PLATFORM_H */
