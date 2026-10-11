/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/gpu_shader_runtime.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef RMMZ_RUNTIME_SHADERS

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <SDL3_shadercross/SDL_shadercross.h>
#include "engine/error_handler.h"

static bool s_ready = false;
static SDL_GPUShader *s_fullscreen_vertex = NULL;

bool gpu_shader_runtime_init(void)
{
    if (s_ready) return true;
    glslang_initialize_process();
    if (!SDL_ShaderCross_Init()) {
        fprintf(stderr, "gpu: SDL_ShaderCross_Init failed: %s\n", SDL_GetError());
        glslang_finalize_process();
        return false;
    }
    s_ready = true;
    return true;
}

void gpu_shader_runtime_shutdown(void)
{
    if (!s_ready) return;
    /* The device owns the shader object; it is released with the device. */
    s_fullscreen_vertex = NULL;
    SDL_ShaderCross_Quit();
    glslang_finalize_process();
    s_ready = false;
}

bool gpu_shader_runtime_available(void)
{
    return s_ready;
}

/* GLSL -> SPIR-V. Returns a malloc'd word array the caller frees, or NULL. */
static unsigned int *compile_spirv(const char *source, bool vertex_stage,
                                   size_t *out_bytes)
{
    glslang_input_t input;
    SDL_zero(input);
    input.language = GLSLANG_SOURCE_GLSL;
    const glslang_stage_t stage = vertex_stage ? GLSLANG_STAGE_VERTEX
                                              : GLSLANG_STAGE_FRAGMENT;
    input.stage = stage;
    input.client = GLSLANG_CLIENT_VULKAN;
    input.client_version = GLSLANG_TARGET_VULKAN_1_0;
    input.target_language = GLSLANG_TARGET_SPV;
    input.target_language_version = GLSLANG_TARGET_SPV_1_0;
    input.code = source;
    input.default_version = 450;
    input.default_profile = GLSLANG_NO_PROFILE;
    input.force_default_version_and_profile = 0;
    input.forward_compatible = 0;
    input.messages = GLSLANG_MSG_DEFAULT_BIT | GLSLANG_MSG_SPV_RULES_BIT |
                     GLSLANG_MSG_VULKAN_RULES_BIT;
    input.resource = glslang_default_resource();

    glslang_shader_t *shader = glslang_shader_create(&input);
    if (!shader) return NULL;

    if (!glslang_shader_preprocess(shader, &input)) {
        compat_report("shader", "glslang preprocess failed", "%s", glslang_shader_get_info_log(shader));
        glslang_shader_delete(shader);
        return NULL;
    }
    if (!glslang_shader_parse(shader, &input)) {
        compat_report("shader", "glslang parse failed", "%s", glslang_shader_get_info_log(shader));
        /* The source the compiler saw is the rewrite, not what the plugin
           wrote, so print it with line numbers or the diagnostic above is
           impossible to place. */
        fprintf(stderr, "--- translated source ---\n");
        int line = 1;
        const char *at = source;
        while (*at && line <= 200) {
            const char *eol = strchr(at, '\n');
            int len = eol ? (int)(eol - at) : (int)strlen(at);
            fprintf(stderr, "%4d | %.*s\n", line++, len, at);
            if (!eol) break;
            at = eol + 1;
        }
        fprintf(stderr, "--- end ---\n");
        glslang_shader_delete(shader);
        return NULL;
    }

    glslang_program_t *program = glslang_program_create();
    glslang_program_add_shader(program, shader);
    if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT |
                                       GLSLANG_MSG_VULKAN_RULES_BIT)) {
        compat_report("shader", "glslang link failed", "%s", glslang_program_get_info_log(program));
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return NULL;
    }

    glslang_program_SPIRV_generate(program, stage);
    size_t words = glslang_program_SPIRV_get_size(program);
    unsigned int *spirv = NULL;
    if (words) {
        spirv = malloc(words * sizeof(unsigned int));
        if (spirv) {
            glslang_program_SPIRV_get(program, spirv);
            *out_bytes = words * sizeof(unsigned int);
        }
    }

    glslang_program_delete(program);
    glslang_shader_delete(shader);
    return spirv;
}

SDL_GPUShader *gpu_shader_runtime_compile(SDL_GPUDevice *device,
                                          const char *glsl450,
                                          bool vertex_stage,
                                          int sampler_count,
                                          int uniform_buffer_count)
{
    if (!s_ready || !device || !glsl450) return NULL;

    size_t bytes = 0;
    unsigned int *spirv = compile_spirv(glsl450, vertex_stage, &bytes);
    if (!spirv || !bytes) {
        free(spirv);
        return NULL;
    }

    SDL_ShaderCross_SPIRV_Info info;
    SDL_zero(info);
    info.bytecode = (const Uint8 *)spirv;
    info.bytecode_size = bytes;
    info.entrypoint = "main";
    info.shader_stage = vertex_stage ? SDL_SHADERCROSS_SHADERSTAGE_VERTEX
                                     : SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;

    /* Describe the resources ourselves rather than reflecting: the translation
       already knows what it declared, and reflection would drop a sampler the
       shader happens not to reference. */
    SDL_ShaderCross_GraphicsShaderResourceInfo resources;
    SDL_zero(resources);
    resources.num_samplers = (Uint32)sampler_count;
    resources.num_uniform_buffers = (Uint32)uniform_buffer_count;

    SDL_GPUShader *shader =
        SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(device, &info, &resources, 0);
    if (!shader) {
        compat_report("shader", "shadercross build failed", "%s", SDL_GetError());
    }
    free(spirv);
    return shader;
}

/* Mirrors src/rendering/shaders/fullscreen.vert.hlsl. A runtime filter needs
   its vertex stage in the same bytecode format as its fragment stage, so it
   is built through the same compiler rather than taken from the shipped
   bytecode. */
static const char *FULLSCREEN_VERT_GLSL =
    "#version 450\n"
    "layout(location = 0) in vec2 a_position;\n"
    "layout(location = 1) in vec2 a_texcoord;\n"
    "layout(location = 0) out vec2 vTextureCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    vTextureCoord = a_texcoord;\n"
    "}\n";

SDL_GPUShader *gpu_shader_runtime_fullscreen_vertex(SDL_GPUDevice *device)
{
    if (!s_ready || !device) return NULL;
    if (!s_fullscreen_vertex) {
        s_fullscreen_vertex =
            gpu_shader_runtime_compile(device, FULLSCREEN_VERT_GLSL, true, 0, 0);
    }
    return s_fullscreen_vertex;
}

#else /* !RMMZ_RUNTIME_SHADERS */

bool gpu_shader_runtime_init(void) { return false; }
void gpu_shader_runtime_shutdown(void) { }
bool gpu_shader_runtime_available(void) { return false; }

SDL_GPUShader *gpu_shader_runtime_compile(SDL_GPUDevice *device,
                                          const char *glsl450,
                                          bool vertex_stage,
                                          int sampler_count,
                                          int uniform_buffer_count)
{
    (void)device; (void)glsl450; (void)vertex_stage;
    (void)sampler_count; (void)uniform_buffer_count;
    return NULL;
}

SDL_GPUShader *gpu_shader_runtime_fullscreen_vertex(SDL_GPUDevice *device)
{
    (void)device;
    return NULL;
}

#endif /* RMMZ_RUNTIME_SHADERS */
