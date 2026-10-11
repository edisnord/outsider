/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/gl_loader.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include "engine/error_handler.h"

#define RMMZ_GL_DEFINE(type, name) type rmmz_##name = NULL;
RMMZ_GL_FUNCS(RMMZ_GL_DEFINE)
#undef RMMZ_GL_DEFINE

static int s_ready = 0;

int gl_loader_init(void)
{
    int missing = 0;

#define RMMZ_GL_LOAD(type, name) \
    rmmz_##name = (type)SDL_GL_GetProcAddress(#name); \
    if (!rmmz_##name) { \
        compat_report("render", "OpenGL entry point " #name, "missing from the driver"); \
        missing++; \
    }
    RMMZ_GL_FUNCS(RMMZ_GL_LOAD)
#undef RMMZ_GL_LOAD

    s_ready = (missing == 0);
    return missing;
}

int gl_loader_is_ready(void)
{
    return s_ready;
}
