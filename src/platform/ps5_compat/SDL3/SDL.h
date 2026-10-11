/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the PS5 port contributors */
/* PS5 build: the runtime is written against SDL3 but the console SDK ships SDL2. Files that only
 * need a few SDL calls (timers, mutexes, message boxes, events) are compiled against this header,
 * which is found before the real <SDL3/...> ones. The window and input code is in
 * platform_sdl2.c instead. */
#ifndef RMMZ_PS5_COMPAT_SDL_H
#define RMMZ_PS5_COMPAT_SDL_H

#include <SDL2/SDL.h>

typedef SDL_mutex SDL_Mutex;
#define SDL_EVENT_QUIT SDL_QUIT

#endif
