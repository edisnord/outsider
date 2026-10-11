/*
 * OpenGL 1.x entry points for the PlayStation 5 build.
 *
 * On desktop these come from libGL. The console has no libGL: OpenGL is Mesa's OSMesa, which SDL loads
 * with dlopen, so every function has to be looked up at run time. gl_loader.c already does that for
 * everything newer than OpenGL 1.1; this file defines the few 1.x functions the renderer calls
 * directly as thin wrappers that resolve themselves on first use.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#define GL_TRAMPOLINE_V(name, params, args)                                        \
    void APIENTRY name params                                                      \
    {                                                                              \
        static void (APIENTRY *fn) params;                                         \
        if (!fn) fn = (void (APIENTRY *) params)SDL_GL_GetProcAddress(#name);      \
        fn args;                                                                   \
    }

#define GL_TRAMPOLINE_R(ret, name, params, args)                                   \
    ret APIENTRY name params                                                       \
    {                                                                              \
        static ret (APIENTRY *fn) params;                                          \
        if (!fn) fn = (ret (APIENTRY *) params)SDL_GL_GetProcAddress(#name);       \
        return fn args;                                                            \
    }

GL_TRAMPOLINE_V(glBindTexture, (GLenum target, GLuint texture), (target, texture))
GL_TRAMPOLINE_V(glBlendFunc, (GLenum sfactor, GLenum dfactor), (sfactor, dfactor))
GL_TRAMPOLINE_V(glClear, (GLbitfield mask), (mask))
GL_TRAMPOLINE_V(glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a), (r, g, b, a))
GL_TRAMPOLINE_V(glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a), (r, g, b, a))
GL_TRAMPOLINE_V(glDeleteTextures, (GLsizei n, const GLuint *textures), (n, textures))
GL_TRAMPOLINE_V(glDepthMask, (GLboolean flag), (flag))
GL_TRAMPOLINE_V(glDisable, (GLenum cap), (cap))
GL_TRAMPOLINE_V(glDrawArrays, (GLenum mode, GLint first, GLsizei count), (mode, first, count))
GL_TRAMPOLINE_V(glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void *indices),
                (mode, count, type, indices))
GL_TRAMPOLINE_V(glEnable, (GLenum cap), (cap))
GL_TRAMPOLINE_V(glGetIntegerv, (GLenum pname, GLint *data), (pname, data))
GL_TRAMPOLINE_R(const GLubyte *, glGetString, (GLenum name), (name))
GL_TRAMPOLINE_R(GLboolean, glIsEnabled, (GLenum cap), (cap))
GL_TRAMPOLINE_V(glReadPixels,
                (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels),
                (x, y, width, height, format, type, pixels))
GL_TRAMPOLINE_V(glScissor, (GLint x, GLint y, GLsizei width, GLsizei height), (x, y, width, height))
GL_TRAMPOLINE_V(glViewport, (GLint x, GLint y, GLsizei width, GLsizei height), (x, y, width, height))
