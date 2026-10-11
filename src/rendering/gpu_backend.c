/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/gpu_backend.h"
#include "rendering/sprite_batch.h"
#include "rendering/glsl_translate.h"
#include "rendering/gpu_shader_runtime.h"
#include "rendering/shaders/shaders_generated.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine/error_handler.h"

#define GPU_TARGET_FORMAT SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM

/* Largest quad count a single sprite_batch flush can hand over, and so the
   size of the shared index buffer. Matches sprite_batch_create's own cap. */
#define GPU_MAX_BATCH_QUADS 16384

/* Filter passes draw this unit quad. NDC +1 is the target's first row, so V
   has to run against Y for the pass to be an identity copy. */
static const float QUAD_VERTICES[] = {
    -1.0f, -1.0f,  0.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 0.0f,
    -1.0f,  1.0f,  0.0f, 0.0f,
};

/* The screen texture holds a bottom-up image, so presenting it upright means
   sampling V with Y: the window's top row shows the texture's last row. */
static const float BLIT_VERTICES[] = {
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
};

/* Shaders a game supplied at run time. */

typedef struct {
    bool                     used;
    SDL_GPUShader           *shader;
    /* Generated to match this shader's inputs; a pipeline is only valid when
       the vertex stage writes every input the fragment stage declares. */
    SDL_GPUShader           *vertex;
    /* Indexed by whether the target is the swapchain, as for built-ins. */
    SDL_GPUGraphicsPipeline *pipelines[2];
    GlslTranslation          layout;
} RuntimeShader;

/* Textures */

typedef struct {
    SDL_GPUTexture *texture;
    int             width;
    int             height;
    bool            render_target;
    bool            linear;
    bool            used;
} GpuTextureSlot;

/* Commands */

typedef enum {
    CMD_TARGET,
    CMD_CLEAR,
    CMD_VIEWPORT,
    CMD_SCISSOR,
    CMD_SCISSOR_OFF,
    CMD_SPRITES,
    CMD_FILTER,
} GpuCmdType;

typedef struct {
    GpuCmdType type;
    union {
        struct { uint32_t id; int w, h; } target;
        struct { float color[4]; } clear;
        struct { int x, y, w, h; } rect;
        struct {
            uint32_t texture;
            int      blend;
            uint32_t first_vertex;
            int      quad_count;
            float    projection[16];
            float    premultiplied;
        } sprites;
        struct {
            int      shader;
            uint32_t textures[GPU_MAX_FILTER_TEXTURES];
            int      texture_count;
            uint32_t uniform_offset;
            uint32_t uniform_size;
        } filter;
    };
} GpuCmd;

/* Backend state */

static struct {
    SDL_Window    *window;
    SDL_GPUDevice *device;
    bool           ready;

    SDL_GPUShader *shaders[GPU_SHADER_COUNT][2];   /* [id][0]=vertex,[1]=fragment */
    /* Pipelines are keyed by shader, blend mode and whether the target is the
       swapchain (whose format differs from a render target's). */
    SDL_GPUGraphicsPipeline *pipelines[GPU_SHADER_COUNT][5][2];

    SDL_GPUSampler *sampler_nearest;
    SDL_GPUSampler *sampler_linear;

    SDL_GPUBuffer  *quad_buffer;   /* QUAD_VERTICES then BLIT_VERTICES */
    SDL_GPUBuffer  *index_buffer;  /* quad indices, shared by every draw */
    SDL_GPUBuffer  *vertex_buffer; /* recorded sprite vertices */
    uint32_t        vertex_buffer_capacity;
    SDL_GPUTransferBuffer *vertex_transfer;
    uint32_t        vertex_transfer_capacity;

    GpuTextureSlot *textures;
    uint32_t        texture_capacity;

    /* Shaders compiled from a game's own GLSL, keyed by id - GPU_SHADER_COUNT. */
    RuntimeShader  *runtime;
    uint32_t        runtime_capacity;

    /* Offscreen target the frame is composed into. */
    SDL_GPUTexture *screen;
    int             screen_w;
    int             screen_h;

    /* Recording arenas */
    GpuVertex      *vertices;
    uint32_t        vertex_count;
    uint32_t        vertex_capacity;
    uint8_t        *uniforms;
    uint32_t        uniform_size;
    uint32_t        uniform_capacity;
    GpuCmd         *cmds;
    uint32_t        cmd_count;
    uint32_t        cmd_capacity;

    /* Texture uploads are gathered onto one command buffer and submitted
       before the frame's draws, which all happen in gpu_submit(). */
    SDL_GPUCommandBuffer  *upload_cmd;
    SDL_GPUTransferBuffer **upload_transfers;
    uint32_t               upload_count;
    uint32_t               upload_capacity;

    /* Replay state */
    SDL_GPURenderPass *pass;
    SDL_GPUCommandBuffer *cmd_buffer;
    uint32_t        cur_target;
    int             cur_target_w, cur_target_h;
    SDL_GPUViewport cur_viewport;
    SDL_Rect        cur_scissor;
    bool            scissor_on;
    bool            pending_clear;
    float           pending_clear_color[4];
} G;

static void flush_uploads(void);

/* Shader loading */

static SDL_GPUShader *load_shader(const char *name, int sampler_count,
                                  int uniform_count)
{
    for (size_t i = 0; i < SDL_arraysize(RMMZ_SHADER_BLOBS); i++) {
        const RmmzShaderBlob *b = &RMMZ_SHADER_BLOBS[i];
        if (strcmp(b->name, name) != 0) continue;

        SDL_GPUShaderFormat have = SDL_GetGPUShaderFormats(G.device);
        SDL_GPUShaderCreateInfo info;
        SDL_zero(info);
        /* Vulkan, D3D12 and Metal respectively; MSL is source text rather
           than bytecode, which SDL's Metal backend accepts as-is. */
        if ((have & SDL_GPU_SHADERFORMAT_SPIRV) && b->spirv) {
            info.code = b->spirv;
            info.code_size = b->spirv_size;
            info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        } else if ((have & SDL_GPU_SHADERFORMAT_DXIL) && b->dxil) {
            info.code = b->dxil;
            info.code_size = b->dxil_size;
            info.format = SDL_GPU_SHADERFORMAT_DXIL;
        } else if ((have & SDL_GPU_SHADERFORMAT_MSL) && b->msl) {
            info.code = b->msl;
            info.code_size = b->msl_size;
            info.format = SDL_GPU_SHADERFORMAT_MSL;
            /* "main" is reserved in MSL, so SPIRV-Cross emits "main0". */
            info.entrypoint = "main0";
        } else {
            compat_report("render", "no shader format", "%s: device accepts 0x%x", name, (unsigned)have);
            return NULL;
        }
        if (!info.entrypoint) info.entrypoint = "main";
        info.stage = b->is_vertex ? SDL_GPU_SHADERSTAGE_VERTEX
                                  : SDL_GPU_SHADERSTAGE_FRAGMENT;
        info.num_samplers = (Uint32)sampler_count;
        info.num_uniform_buffers = (Uint32)uniform_count;

        SDL_GPUShader *s = SDL_CreateGPUShader(G.device, &info);
        if (!s) compat_report("render", "SDL_CreateGPUShader", "%s: %s", name, SDL_GetError());
        return s;
    }
    compat_report("render", "shader blob missing", "%s", name);
    return NULL;
}

/* Per-shader vertex/fragment blobs and their resource counts. */
static bool create_shaders(void)
{
    struct { int id; const char *vert; const char *frag; int samplers; int uniforms; } defs[] = {
        { GPU_SHADER_SPRITE,       "sprite_vert",     "sprite_frag",      1, 1 },
        { GPU_SHADER_COLOR_MATRIX, "fullscreen_vert", "colormatrix_frag", 1, 1 },
        { GPU_SHADER_BLUR,         "fullscreen_vert", "blur_frag",        1, 1 },
        { GPU_SHADER_ALPHA,        "fullscreen_vert", "alpha_frag",       1, 1 },
        { GPU_SHADER_COLOR_FILTER, "fullscreen_vert", "colorfilter_frag", 1, 1 },
        { GPU_SHADER_MASK,         "fullscreen_vert", "mask_frag",        2, 0 },
        { GPU_SHADER_BLIT,         "fullscreen_vert", "alpha_frag",       1, 1 },
    };

    for (size_t i = 0; i < SDL_arraysize(defs); i++) {
        /* The sprite vertex shader is the only one with a uniform buffer. */
        int vert_uniforms = (defs[i].id == GPU_SHADER_SPRITE) ? 1 : 0;
        G.shaders[defs[i].id][0] = load_shader(defs[i].vert, 0, vert_uniforms);
        G.shaders[defs[i].id][1] = load_shader(defs[i].frag, defs[i].samplers,
                                               defs[i].uniforms);
        if (!G.shaders[defs[i].id][0] || !G.shaders[defs[i].id][1]) return false;
    }
    return true;
}

/* Pipelines */

static void blend_state(int mode, SDL_GPUColorTargetBlendState *b)
{
    SDL_zerop(b);
    b->enable_blend = true;
    b->color_blend_op = SDL_GPU_BLENDOP_ADD;
    b->alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    /* Alpha accumulates as coverage in every mode, so a transparent target
       ends up holding premultiplied (rgb*a, a) -- as in the GL backend. */
    b->src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    b->dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    switch (mode) {
    case BLEND_MODE_ADD:
        b->src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        break;
    case BLEND_MODE_MULTIPLY:
        b->src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
        b->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BLEND_MODE_SCREEN:
        b->src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
        break;
    default:
        if (mode != BLEND_MODE_NORMAL && mode != BLEND_MODE_NORMAL_PREMULT)
            compat_report("render", "blend mode", "mode %d is not implemented, drawn as normal", mode);
        b->src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    }
}

static SDL_GPUGraphicsPipeline *build_pipeline_for(SDL_GPUShader *vertex_shader,
                                                   SDL_GPUShader *fragment_shader,
                                                   bool sprite_layout,
                                                   int shader, int blend,
                                                   bool swapchain)
{
    SDL_GPUVertexBufferDescription vbuf;
    SDL_zero(vbuf);
    vbuf.slot = 0;
    vbuf.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attrs[3];
    SDL_zeroa(attrs);
    int attr_count;

    if (sprite_layout) {
        vbuf.pitch = sizeof(GpuVertex);
        attrs[0].location = 0; attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[0].offset = offsetof(GpuVertex, x);
        attrs[1].location = 1; attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[1].offset = offsetof(GpuVertex, u);
        attrs[2].location = 2; attrs[2].buffer_slot = 0;
        attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[2].offset = offsetof(GpuVertex, r);
        attr_count = 3;
    } else {
        vbuf.pitch = 4 * sizeof(float);
        attrs[0].location = 0; attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[0].offset = 0;
        attrs[1].location = 1; attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[1].offset = 2 * sizeof(float);
        attr_count = 2;
    }

    SDL_GPUColorTargetBlendState blend_st;
    blend_state(blend, &blend_st);
    /* The final blit overwrites the swapchain outright. */
    if (shader == GPU_SHADER_BLIT) blend_st.enable_blend = false;

    SDL_GPUColorTargetDescription color;
    SDL_zero(color);
    color.format = swapchain ? SDL_GetGPUSwapchainTextureFormat(G.device, G.window)
                             : GPU_TARGET_FORMAT;
    color.blend_state = blend_st;

    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader = vertex_shader;
    info.fragment_shader = fragment_shader;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_buffer_descriptions = &vbuf;
    info.vertex_input_state.num_vertex_attributes = (Uint32)attr_count;
    info.vertex_input_state.vertex_attributes = attrs;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.num_color_targets = 1;
    info.target_info.color_target_descriptions = &color;

    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(G.device, &info);
    if (!p) compat_report("render", "SDL_CreateGPUGraphicsPipeline", "shader %d blend %d: %s",
                          shader, blend, SDL_GetError());
    return p;
}

static SDL_GPUGraphicsPipeline *build_pipeline(int shader, int blend, bool swapchain)
{
    return build_pipeline_for(G.shaders[shader][0], G.shaders[shader][1],
                              shader == GPU_SHADER_SPRITE, shader, blend, swapchain);
}

/* A plugin shader reuses the built-in fullscreen vertex stage; only the
   fragment stage came from the game. */
static SDL_GPUGraphicsPipeline *runtime_pipeline_for(uint32_t id, bool swapchain)
{
    uint32_t index = id - GPU_SHADER_COUNT;
    if (index >= G.runtime_capacity || !G.runtime[index].used) return NULL;
    RuntimeShader *rs = &G.runtime[index];
    int slot = swapchain ? 1 : 0;
    if (!rs->pipelines[slot]) {
        if (!rs->vertex) return NULL;
        rs->pipelines[slot] = build_pipeline_for(
            rs->vertex, rs->shader, false, (int)id, BLEND_MODE_NORMAL, swapchain);
    }
    return rs->pipelines[slot];
}

static SDL_GPUGraphicsPipeline *pipeline_for(int shader, int blend, bool swapchain)
{
    if (blend < 0 || blend > BLEND_MODE_NORMAL_PREMULT) blend = BLEND_MODE_NORMAL;
    /* NORMAL and NORMAL_PREMULT share blend state; they differ only in the
       fragment uniform. */
    int slot = (blend == BLEND_MODE_NORMAL_PREMULT) ? BLEND_MODE_NORMAL : blend;
    int sc = swapchain ? 1 : 0;
    if (!G.pipelines[shader][slot][sc])
        G.pipelines[shader][slot][sc] = build_pipeline(shader, slot, swapchain);
    return G.pipelines[shader][slot][sc];
}

/* Device lifecycle */

bool gpu_backend_init(SDL_Window *window)
{
    if (G.ready) return true;
    SDL_zero(G);
    G.window = window;

    G.device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL |
        SDL_GPU_SHADERFORMAT_MSL, false, NULL);
    if (!G.device) {
        compat_report("render", "SDL_CreateGPUDevice", "%s", SDL_GetError());
        return false;
    }
    if (window) {
        if (!SDL_ClaimWindowForGPUDevice(G.device, window)) {
            compat_report("render", "SDL_ClaimWindowForGPUDevice", "%s", SDL_GetError());
            SDL_DestroyGPUDevice(G.device);
            G.device = NULL;
            return false;
        }
        SDL_SetGPUSwapchainParameters(G.device, window,
                                      SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                      SDL_GPU_PRESENTMODE_VSYNC);
    }

    if (!create_shaders()) {
        gpu_backend_shutdown();
        return false;
    }

    SDL_GPUSamplerCreateInfo si;
    SDL_zero(si);
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.min_filter = SDL_GPU_FILTER_NEAREST;
    si.mag_filter = SDL_GPU_FILTER_NEAREST;
    G.sampler_nearest = SDL_CreateGPUSampler(G.device, &si);
    si.min_filter = SDL_GPU_FILTER_LINEAR;
    si.mag_filter = SDL_GPU_FILTER_LINEAR;
    G.sampler_linear = SDL_CreateGPUSampler(G.device, &si);
    if (!G.sampler_nearest || !G.sampler_linear) {
        gpu_backend_shutdown();
        return false;
    }

    /* Static quads: the filter quad followed by the flipped blit quad. */
    SDL_GPUBufferCreateInfo bi;
    SDL_zero(bi);
    bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bi.size = sizeof(QUAD_VERTICES) + sizeof(BLIT_VERTICES);
    G.quad_buffer = SDL_CreateGPUBuffer(G.device, &bi);

    SDL_GPUTransferBufferCreateInfo ti;
    SDL_zero(ti);
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = bi.size;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(G.device, &ti);
    if (!G.quad_buffer || !tb) {
        gpu_backend_shutdown();
        return false;
    }
    uint8_t *dst = SDL_MapGPUTransferBuffer(G.device, tb, false);
    memcpy(dst, QUAD_VERTICES, sizeof(QUAD_VERTICES));
    memcpy(dst + sizeof(QUAD_VERTICES), BLIT_VERTICES, sizeof(BLIT_VERTICES));
    SDL_UnmapGPUTransferBuffer(G.device, tb);

    /* Indices for the largest batch a sprite flush can produce. Each quad is
       two triangles over four vertices, in the same order as the GL backend. */
    Uint16 *indices = malloc(GPU_MAX_BATCH_QUADS * 6 * sizeof(Uint16));
    if (!indices) {
        gpu_backend_shutdown();
        return false;
    }
    for (int q = 0; q < GPU_MAX_BATCH_QUADS; q++) {
        Uint16 v = (Uint16)(q * 4);
        Uint16 *ix = &indices[q * 6];
        ix[0] = v; ix[1] = v + 1; ix[2] = v + 2;
        ix[3] = v + 1; ix[4] = v + 3; ix[5] = v + 2;
    }
    SDL_GPUBufferCreateInfo ii;
    SDL_zero(ii);
    ii.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ii.size = GPU_MAX_BATCH_QUADS * 6 * (Uint32)sizeof(Uint16);
    G.index_buffer = SDL_CreateGPUBuffer(G.device, &ii);

    SDL_GPUTransferBufferCreateInfo iti;
    SDL_zero(iti);
    iti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    iti.size = ii.size;
    SDL_GPUTransferBuffer *itb = SDL_CreateGPUTransferBuffer(G.device, &iti);
    if (!G.index_buffer || !itb) {
        free(indices);
        gpu_backend_shutdown();
        return false;
    }
    void *idst = SDL_MapGPUTransferBuffer(G.device, itb, false);
    memcpy(idst, indices, ii.size);
    SDL_UnmapGPUTransferBuffer(G.device, itb);
    free(indices);

    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(G.device);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTransferBufferLocation src = { tb, 0 };
    SDL_GPUBufferRegion region = { G.quad_buffer, 0, bi.size };
    SDL_UploadToGPUBuffer(cp, &src, &region, false);
    SDL_GPUTransferBufferLocation isrc = { itb, 0 };
    SDL_GPUBufferRegion iregion = { G.index_buffer, 0, ii.size };
    SDL_UploadToGPUBuffer(cp, &isrc, &iregion, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cb);
    SDL_ReleaseGPUTransferBuffer(G.device, tb);
    SDL_ReleaseGPUTransferBuffer(G.device, itb);

    G.ready = true;

    const char *driver = SDL_GetGPUDeviceDriver(G.device);
    fprintf(stderr, "[GPU] Driver: %s\n", driver ? driver : "?");
    return true;
}

void gpu_backend_shutdown(void)
{
    if (!G.device) return;
    flush_uploads();
    SDL_WaitForGPUIdle(G.device);

    for (uint32_t i = 0; i < G.texture_capacity; i++) {
        if (G.textures[i].used && G.textures[i].texture)
            SDL_ReleaseGPUTexture(G.device, G.textures[i].texture);
    }
    free(G.textures);

    for (int s = 0; s < GPU_SHADER_COUNT; s++) {
        for (int b = 0; b < 5; b++)
            for (int f = 0; f < 2; f++)
                if (G.pipelines[s][b][f])
                    SDL_ReleaseGPUGraphicsPipeline(G.device, G.pipelines[s][b][f]);
        for (int k = 0; k < 2; k++)
            if (G.shaders[s][k]) SDL_ReleaseGPUShader(G.device, G.shaders[s][k]);
    }
    for (uint32_t i = 0; i < G.runtime_capacity; i++) {
        if (!G.runtime[i].used) continue;
        for (int k = 0; k < 2; k++) {
            if (G.runtime[i].pipelines[k])
                SDL_ReleaseGPUGraphicsPipeline(G.device, G.runtime[i].pipelines[k]);
        }
        if (G.runtime[i].shader) SDL_ReleaseGPUShader(G.device, G.runtime[i].shader);
        if (G.runtime[i].vertex) SDL_ReleaseGPUShader(G.device, G.runtime[i].vertex);
        glsl_translation_free(&G.runtime[i].layout);
    }
    free(G.runtime);
    gpu_shader_runtime_shutdown();

    if (G.screen) SDL_ReleaseGPUTexture(G.device, G.screen);
    if (G.quad_buffer) SDL_ReleaseGPUBuffer(G.device, G.quad_buffer);
    if (G.index_buffer) SDL_ReleaseGPUBuffer(G.device, G.index_buffer);
    if (G.vertex_buffer) SDL_ReleaseGPUBuffer(G.device, G.vertex_buffer);
    if (G.vertex_transfer) SDL_ReleaseGPUTransferBuffer(G.device, G.vertex_transfer);
    if (G.sampler_nearest) SDL_ReleaseGPUSampler(G.device, G.sampler_nearest);
    if (G.sampler_linear) SDL_ReleaseGPUSampler(G.device, G.sampler_linear);

    if (G.window) SDL_ReleaseWindowFromGPUDevice(G.device, G.window);
    SDL_DestroyGPUDevice(G.device);

    free(G.vertices);
    free(G.uniforms);
    free(G.cmds);
    free(G.upload_transfers);
    SDL_zero(G);
}

bool gpu_backend_ready(void) { return G.ready; }
SDL_GPUDevice *gpu_backend_device(void) { return G.device; }

/* Texture registry */

static uint32_t texture_alloc_slot(void)
{
    for (uint32_t i = 0; i < G.texture_capacity; i++) {
        if (!G.textures[i].used) return i + 1;
    }
    uint32_t grown = G.texture_capacity ? G.texture_capacity * 2 : 256;
    GpuTextureSlot *t = realloc(G.textures, grown * sizeof(*t));
    if (!t) return 0;
    memset(t + G.texture_capacity, 0,
           (grown - G.texture_capacity) * sizeof(*t));
    G.textures = t;
    uint32_t id = G.texture_capacity + 1;
    G.texture_capacity = grown;
    return id;
}

uint32_t gpu_texture_create(int width, int height, bool render_target)
{
    if (!G.ready || width <= 0 || height <= 0) return 0;

    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = GPU_TARGET_FORMAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (render_target) info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = (Uint32)width;
    info.height = (Uint32)height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;

    SDL_GPUTexture *tex = SDL_CreateGPUTexture(G.device, &info);
    if (!tex) {
        compat_report("render", "SDL_CreateGPUTexture", "%dx%d: %s", width, height, SDL_GetError());
        return 0;
    }

    uint32_t id = texture_alloc_slot();
    if (!id) { SDL_ReleaseGPUTexture(G.device, tex); return 0; }

    GpuTextureSlot *slot = &G.textures[id - 1];
    slot->texture = tex;
    slot->width = width;
    slot->height = height;
    slot->render_target = render_target;
    slot->linear = false;
    slot->used = true;
    return id;
}

SDL_GPUTexture *gpu_texture_handle(uint32_t id, int *w, int *h)
{
    if (id == 0 || id > G.texture_capacity) return NULL;
    GpuTextureSlot *slot = &G.textures[id - 1];
    if (!slot->used) return NULL;
    if (w) *w = slot->width;
    if (h) *h = slot->height;
    return slot->texture;
}

void gpu_texture_destroy(uint32_t id)
{
    if (id == 0 || id > G.texture_capacity) return;
    GpuTextureSlot *slot = &G.textures[id - 1];
    if (!slot->used) return;
    /* Recorded commands may still reference it, so flush before releasing. */
    gpu_submit();
    SDL_ReleaseGPUTexture(G.device, slot->texture);
    memset(slot, 0, sizeof(*slot));
}

void gpu_texture_set_filter(uint32_t id, bool linear)
{
    if (id == 0 || id > G.texture_capacity) return;
    if (G.textures[id - 1].used) G.textures[id - 1].linear = linear;
}

static SDL_GPUSampler *sampler_for(uint32_t id)
{
    if (id && id <= G.texture_capacity && G.textures[id - 1].used &&
        G.textures[id - 1].linear)
        return G.sampler_linear;
    return G.sampler_nearest;
}

void gpu_texture_upload(uint32_t id, int width, int height, const uint8_t *pixels)
{
    int tw = 0, th = 0;
    SDL_GPUTexture *tex = gpu_texture_handle(id, &tw, &th);
    if (!tex || !pixels || width <= 0 || height <= 0) return;

    Uint32 bytes = (Uint32)width * (Uint32)height * 4;

    SDL_GPUTransferBufferCreateInfo ti;
    SDL_zero(ti);
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = bytes;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(G.device, &ti);
    if (!tb) return;

    void *dst = SDL_MapGPUTransferBuffer(G.device, tb, false);
    memcpy(dst, pixels, bytes);
    SDL_UnmapGPUTransferBuffer(G.device, tb);

    /* Uploads ride a command buffer of their own: a copy pass cannot be
       opened while a render pass is recording. One buffer collects every
       upload and is submitted ahead of the frame's draws, so a texture is
       always current by the time anything samples it. */
    if (!G.upload_cmd) G.upload_cmd = SDL_AcquireGPUCommandBuffer(G.device);
    if (!G.upload_cmd) {
        SDL_ReleaseGPUTransferBuffer(G.device, tb);
        return;
    }
    if (G.upload_count == G.upload_capacity) {
        uint32_t cap = G.upload_capacity ? G.upload_capacity * 2 : 64;
        SDL_GPUTransferBuffer **t = realloc(G.upload_transfers, cap * sizeof(*t));
        if (!t) {
            SDL_ReleaseGPUTransferBuffer(G.device, tb);
            return;
        }
        G.upload_transfers = t;
        G.upload_capacity = cap;
    }
    G.upload_transfers[G.upload_count++] = tb;

    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(G.upload_cmd);
    SDL_GPUTextureTransferInfo src;
    SDL_zero(src);
    src.transfer_buffer = tb;
    src.pixels_per_row = (Uint32)width;
    src.rows_per_layer = (Uint32)height;
    SDL_GPUTextureRegion region;
    SDL_zero(region);
    region.texture = tex;
    region.w = (Uint32)width;
    region.h = (Uint32)height;
    region.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &region, false);
    SDL_EndGPUCopyPass(cp);
}

/* Submit any gathered texture uploads. Their transfer buffers are released
   straight away: SDL keeps them alive until the copy has run. */
static void flush_uploads(void)
{
    if (!G.upload_cmd) return;
    SDL_SubmitGPUCommandBuffer(G.upload_cmd);
    G.upload_cmd = NULL;
    for (uint32_t i = 0; i < G.upload_count; i++)
        SDL_ReleaseGPUTransferBuffer(G.device, G.upload_transfers[i]);
    G.upload_count = 0;
}

/* Screen texture */

static void ensure_screen_texture(void)
{
    int w = G.screen_w, h = G.screen_h;
    if (G.window) SDL_GetWindowSizeInPixels(G.window, &w, &h);
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (G.screen && G.screen_w == w && G.screen_h == h) return;

    if (G.screen) {
        SDL_WaitForGPUIdle(G.device);
        SDL_ReleaseGPUTexture(G.device, G.screen);
    }
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = GPU_TARGET_FORMAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = (Uint32)w;
    info.height = (Uint32)h;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    G.screen = SDL_CreateGPUTexture(G.device, &info);
    G.screen_w = w;
    G.screen_h = h;
}

void gpu_frame_resize(int width, int height)
{
    if (!G.ready) return;
    if (!G.window && width > 0 && height > 0) {
        G.screen_w = width;
        G.screen_h = height;
    }
    ensure_screen_texture();
}

void gpu_screen_size(int *w, int *h)
{
    if (G.ready) ensure_screen_texture();
    if (w) *w = G.screen_w > 0 ? G.screen_w : 1;
    if (h) *h = G.screen_h > 0 ? G.screen_h : 1;
}

/* Recording */

static bool reserve_vertices(uint32_t extra)
{
    if (G.vertex_count + extra <= G.vertex_capacity) return true;
    uint32_t cap = G.vertex_capacity ? G.vertex_capacity : 16384;
    while (cap < G.vertex_count + extra) cap *= 2;
    GpuVertex *v = realloc(G.vertices, cap * sizeof(GpuVertex));
    if (!v) return false;
    G.vertices = v;
    G.vertex_capacity = cap;
    return true;
}

static GpuCmd *push_cmd(GpuCmdType type)
{
    if (G.cmd_count == G.cmd_capacity) {
        uint32_t cap = G.cmd_capacity ? G.cmd_capacity * 2 : 1024;
        GpuCmd *c = realloc(G.cmds, cap * sizeof(GpuCmd));
        if (!c) return NULL;
        G.cmds = c;
        G.cmd_capacity = cap;
    }
    GpuCmd *cmd = &G.cmds[G.cmd_count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = type;
    return cmd;
}

void gpu_record_target(uint32_t id, int width, int height)
{
    GpuCmd *c = push_cmd(CMD_TARGET);
    if (!c) return;
    c->target.id = id;
    c->target.w = width;
    c->target.h = height;
}

void gpu_record_clear(float r, float g, float b, float a)
{
    GpuCmd *c = push_cmd(CMD_CLEAR);
    if (!c) return;
    c->clear.color[0] = r; c->clear.color[1] = g;
    c->clear.color[2] = b; c->clear.color[3] = a;
}

void gpu_record_viewport(int x, int y, int w, int h)
{
    GpuCmd *c = push_cmd(CMD_VIEWPORT);
    if (!c) return;
    c->rect.x = x; c->rect.y = y; c->rect.w = w; c->rect.h = h;
}

void gpu_record_scissor(int x, int y, int w, int h)
{
    GpuCmd *c = push_cmd(CMD_SCISSOR);
    if (!c) return;
    c->rect.x = x; c->rect.y = y; c->rect.w = w; c->rect.h = h;
}

void gpu_record_scissor_off(void)
{
    push_cmd(CMD_SCISSOR_OFF);
}

void gpu_record_sprites(const GpuVertex *verts, int quad_count, uint32_t texture,
                        int blend_mode, const float projection[16],
                        float premultiplied)
{
    if (quad_count <= 0 || !verts) return;
    if (quad_count > GPU_MAX_BATCH_QUADS) quad_count = GPU_MAX_BATCH_QUADS;
    /* Four vertices per quad; the shared index buffer assembles the two
       triangles, so the arena copy stays a single memcpy. */
    uint32_t needed = (uint32_t)quad_count * 4;
    if (!reserve_vertices(needed)) return;

    memcpy(&G.vertices[G.vertex_count], verts, needed * sizeof(GpuVertex));

    GpuCmd *c = push_cmd(CMD_SPRITES);
    if (!c) return;
    c->sprites.texture = texture;
    c->sprites.blend = blend_mode;
    c->sprites.first_vertex = G.vertex_count;
    c->sprites.quad_count = quad_count;
    c->sprites.premultiplied = premultiplied;
    memcpy(c->sprites.projection, projection, 16 * sizeof(float));

    G.vertex_count += needed;
}

void gpu_record_filter(int shader, const uint32_t *textures, int texture_count,
                       const void *uniforms, uint32_t uniform_size)
{
    if (shader <= 0 || !textures || texture_count <= 0) return;
    if (texture_count > GPU_MAX_FILTER_TEXTURES)
        texture_count = GPU_MAX_FILTER_TEXTURES;

    uint32_t offset = G.uniform_size;
    if (uniforms && uniform_size) {
        if (G.uniform_size + uniform_size > G.uniform_capacity) {
            uint32_t cap = G.uniform_capacity ? G.uniform_capacity : 4096;
            while (cap < G.uniform_size + uniform_size) cap *= 2;
            uint8_t *u = realloc(G.uniforms, cap);
            if (!u) return;
            G.uniforms = u;
            G.uniform_capacity = cap;
        }
        memcpy(G.uniforms + offset, uniforms, uniform_size);
        G.uniform_size += uniform_size;
    }

    GpuCmd *c = push_cmd(CMD_FILTER);
    if (!c) return;
    c->filter.shader = shader;
    for (int i = 0; i < texture_count; i++) c->filter.textures[i] = textures[i];
    c->filter.texture_count = texture_count;
    c->filter.uniform_offset = offset;
    c->filter.uniform_size = uniform_size;
}

/* Runtime shaders */

/* A plugin shader that did not make it: counted per shader (the hash of its source) so a filter applied every
   frame is one line, and the plugin's source is printed once so the failure can be reproduced offline. */
static void runtime_shader_failed(const char *stage, const char *reason, const char *source, bool dump)
{
    unsigned h = 2166136261u;
    for (const char *p = source; p && *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    char key[96];
    snprintf(key, sizeof(key), "plugin shader %08x: %s", h, stage);
    compat_report("shader", key, "%s; drawing unfiltered", reason ? reason : "failed");

    static unsigned dumped[32];
    static int ndumped;
    if (!dump || !source) return;
    for (int i = 0; i < ndumped; i++) if (dumped[i] == h) return;
    if (ndumped < 32) dumped[ndumped++] = h;
    error_handler_log(LOG_WARN, "[compat] source of plugin shader %08x (%zu bytes):", h, strlen(source));
    const char *at = source;
    for (int line = 1; *at && line <= 120; line++) {
        const char *eol = strchr(at, '\n');
        int len = eol ? (int)(eol - at) : (int)strlen(at);
        error_handler_log(LOG_WARN, "[compat] %4d | %.*s", line, len, at);
        if (!eol) break;
        at = eol + 1;
    }
}

uint32_t gpu_runtime_shader_create(const char *source)
{
    if (!G.ready || !source) return 0;
    if (!gpu_shader_runtime_available() && !gpu_shader_runtime_init()) return 0;

    GlslTranslation xlat;
    if (!glsl_translate_fragment(source, &xlat)) {
        runtime_shader_failed("translate", glsl_translate_error(), source, true);
        return 0;
    }

    SDL_GPUShader *shader = gpu_shader_runtime_compile(
        G.device, xlat.source, false, xlat.sampler_count,
        xlat.uniform_size ? 1 : 0);
    if (!shader) {
        runtime_shader_failed("fragment compile", "the fragment shader did not compile (see gpu: lines above)",
                              source, true);
        glsl_translation_free(&xlat);
        return 0;
    }

    /* The vertex stage is built here rather than shared, both because it must
       mirror this shader's varyings and because on D3D12 the runtime compiler
       emits DXBC, which cannot share a pipeline with the shipped DXIL. */
    char *vertex_src = glsl_generate_vertex(&xlat);
    SDL_GPUShader *vertex = vertex_src
        ? gpu_shader_runtime_compile(G.device, vertex_src, true, 0, 0) : NULL;
    free(vertex_src);
    if (!vertex) {
        runtime_shader_failed("vertex compile", "the generated vertex stage did not compile", source, false);
        SDL_ReleaseGPUShader(G.device, shader);
        glsl_translation_free(&xlat);
        return 0;
    }

    uint32_t index = G.runtime_capacity;
    for (uint32_t i = 0; i < G.runtime_capacity; i++) {
        if (!G.runtime[i].used) { index = i; break; }
    }
    if (index == G.runtime_capacity) {
        uint32_t grown = G.runtime_capacity ? G.runtime_capacity * 2 : 8;
        RuntimeShader *r = realloc(G.runtime, grown * sizeof(*r));
        if (!r) {
            SDL_ReleaseGPUShader(G.device, shader);
            SDL_ReleaseGPUShader(G.device, vertex);
            glsl_translation_free(&xlat);
            return 0;
        }
        memset(r + G.runtime_capacity, 0,
               (grown - G.runtime_capacity) * sizeof(*r));
        G.runtime = r;
        G.runtime_capacity = grown;
    }

    G.runtime[index].used = true;
    G.runtime[index].shader = shader;
    G.runtime[index].vertex = vertex;
    G.runtime[index].layout = xlat;
    fprintf(stderr, "gpu: compiled a plugin shader (%d uniform%s, %d sampler%s)\n",
            xlat.uniform_count, xlat.uniform_count == 1 ? "" : "s",
            xlat.sampler_count, xlat.sampler_count == 1 ? "" : "s");
    return GPU_SHADER_COUNT + index;
}

void gpu_runtime_shader_destroy(uint32_t id)
{
    if (id < GPU_SHADER_COUNT) return;
    uint32_t index = id - GPU_SHADER_COUNT;
    if (index >= G.runtime_capacity || !G.runtime[index].used) return;

    gpu_submit();
    RuntimeShader *rs = &G.runtime[index];
    for (int i = 0; i < 2; i++) {
        if (rs->pipelines[i]) SDL_ReleaseGPUGraphicsPipeline(G.device, rs->pipelines[i]);
    }
    if (rs->shader) SDL_ReleaseGPUShader(G.device, rs->shader);
    if (rs->vertex) SDL_ReleaseGPUShader(G.device, rs->vertex);
    glsl_translation_free(&rs->layout);
    memset(rs, 0, sizeof(*rs));
}

const struct GlslTranslation *gpu_runtime_shader_layout(uint32_t id)
{
    if (id < GPU_SHADER_COUNT) return NULL;
    uint32_t index = id - GPU_SHADER_COUNT;
    if (index >= G.runtime_capacity || !G.runtime[index].used) return NULL;
    return &G.runtime[index].layout;
}

/* Replay */

static void end_pass(void)
{
    if (G.pass) {
        SDL_EndGPURenderPass(G.pass);
        G.pass = NULL;
    }
}

static SDL_GPUTexture *target_texture(uint32_t id)
{
    if (id == 0) {
        ensure_screen_texture();
        return G.screen;
    }
    return gpu_texture_handle(id, NULL, NULL);
}

static void begin_pass(bool clear)
{
    SDL_GPUTexture *tex = target_texture(G.cur_target);
    if (!tex) return;

    SDL_GPUColorTargetInfo info;
    SDL_zero(info);
    info.texture = tex;
    info.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    info.store_op = SDL_GPU_STOREOP_STORE;
    if (clear) {
        info.clear_color.r = G.pending_clear_color[0];
        info.clear_color.g = G.pending_clear_color[1];
        info.clear_color.b = G.pending_clear_color[2];
        info.clear_color.a = G.pending_clear_color[3];
    }
    G.pass = SDL_BeginGPURenderPass(G.cmd_buffer, &info, 1, NULL);
    if (!G.pass) return;

    SDL_SetGPUViewport(G.pass, &G.cur_viewport);
    if (G.scissor_on) SDL_SetGPUScissor(G.pass, &G.cur_scissor);
}

static void ensure_pass(void)
{
    if (!G.pass) begin_pass(false);
}

static void replay_sprites(const GpuCmd *c)
{
    ensure_pass();
    if (!G.pass) return;

    SDL_GPUGraphicsPipeline *pipe =
        pipeline_for(GPU_SHADER_SPRITE, c->sprites.blend, false);
    if (!pipe) return;
    SDL_BindGPUGraphicsPipeline(G.pass, pipe);

    SDL_GPUBufferBinding vb = { G.vertex_buffer,
                                c->sprites.first_vertex * (Uint32)sizeof(GpuVertex) };
    SDL_BindGPUVertexBuffers(G.pass, 0, &vb, 1);

    SDL_GPUTextureSamplerBinding tsb;
    SDL_zero(tsb);
    tsb.texture = gpu_texture_handle(c->sprites.texture, NULL, NULL);
    tsb.sampler = sampler_for(c->sprites.texture);
    if (!tsb.texture) return;
    SDL_BindGPUFragmentSamplers(G.pass, 0, &tsb, 1);

    SDL_GPUBufferBinding ib = { G.index_buffer, 0 };
    SDL_BindGPUIndexBuffer(G.pass, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);

    SDL_PushGPUVertexUniformData(G.cmd_buffer, 0, c->sprites.projection,
                                 16 * sizeof(float));
    float frag[4] = { c->sprites.premultiplied, 0, 0, 0 };
    SDL_PushGPUFragmentUniformData(G.cmd_buffer, 0, frag, sizeof(frag));

    SDL_DrawGPUIndexedPrimitives(G.pass, (Uint32)c->sprites.quad_count * 6,
                                 1, 0, 0, 0);
}

static void replay_filter(const GpuCmd *c)
{
    ensure_pass();
    if (!G.pass) return;

    SDL_GPUGraphicsPipeline *pipe =
        ((uint32_t)c->filter.shader >= GPU_SHADER_COUNT)
            ? runtime_pipeline_for((uint32_t)c->filter.shader, false)
            : pipeline_for(c->filter.shader, BLEND_MODE_NORMAL, false);
    if (!pipe) return;
    SDL_BindGPUGraphicsPipeline(G.pass, pipe);

    SDL_GPUBufferBinding vb = { G.quad_buffer, 0 };
    SDL_BindGPUVertexBuffers(G.pass, 0, &vb, 1);

    SDL_GPUTextureSamplerBinding tsb[GPU_MAX_FILTER_TEXTURES];
    SDL_zeroa(tsb);
    for (int i = 0; i < c->filter.texture_count; i++) {
        tsb[i].texture = gpu_texture_handle(c->filter.textures[i], NULL, NULL);
        tsb[i].sampler = sampler_for(c->filter.textures[i]);
        if (!tsb[i].texture) return;
    }
    SDL_BindGPUFragmentSamplers(G.pass, 0, tsb, (Uint32)c->filter.texture_count);

    if (c->filter.uniform_size) {
        SDL_PushGPUFragmentUniformData(G.cmd_buffer, 0,
                                       G.uniforms + c->filter.uniform_offset,
                                       c->filter.uniform_size);
    }
    SDL_DrawGPUPrimitives(G.pass, 6, 1, 0, 0);
}

/* Grow and fill the GPU vertex buffer with everything recorded this batch. */
static bool upload_vertices(void)
{
    if (G.vertex_count == 0) return true;
    Uint32 bytes = G.vertex_count * (Uint32)sizeof(GpuVertex);

    if (!G.vertex_buffer || G.vertex_buffer_capacity < bytes) {
        if (G.vertex_buffer) {
            SDL_WaitForGPUIdle(G.device);
            SDL_ReleaseGPUBuffer(G.device, G.vertex_buffer);
        }
        SDL_GPUBufferCreateInfo bi;
        SDL_zero(bi);
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = bytes * 2;
        G.vertex_buffer = SDL_CreateGPUBuffer(G.device, &bi);
        if (!G.vertex_buffer) return false;
        G.vertex_buffer_capacity = bi.size;
    }
    if (!G.vertex_transfer || G.vertex_transfer_capacity < bytes) {
        if (G.vertex_transfer)
            SDL_ReleaseGPUTransferBuffer(G.device, G.vertex_transfer);
        SDL_GPUTransferBufferCreateInfo ti;
        SDL_zero(ti);
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = bytes * 2;
        G.vertex_transfer = SDL_CreateGPUTransferBuffer(G.device, &ti);
        if (!G.vertex_transfer) return false;
        G.vertex_transfer_capacity = ti.size;
    }

    void *dst = SDL_MapGPUTransferBuffer(G.device, G.vertex_transfer, true);
    if (!dst) return false;
    memcpy(dst, G.vertices, bytes);
    SDL_UnmapGPUTransferBuffer(G.device, G.vertex_transfer);

    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(G.device);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTransferBufferLocation src = { G.vertex_transfer, 0 };
    SDL_GPUBufferRegion region = { G.vertex_buffer, 0, bytes };
    SDL_UploadToGPUBuffer(cp, &src, &region, true);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cb);
    return true;
}

void gpu_submit(void)
{
    flush_uploads();
    if (!G.ready || G.cmd_count == 0) {
        G.cmd_count = 0;
        G.vertex_count = 0;
        G.uniform_size = 0;
        return;
    }
    if (!upload_vertices()) {
        G.cmd_count = 0;
        G.vertex_count = 0;
        G.uniform_size = 0;
        return;
    }

    G.cmd_buffer = SDL_AcquireGPUCommandBuffer(G.device);
    if (!G.cmd_buffer) {
        G.cmd_count = 0;
        G.vertex_count = 0;
        G.uniform_size = 0;
        return;
    }

    ensure_screen_texture();
    G.cur_target = 0;
    G.cur_target_w = G.screen_w;
    G.cur_target_h = G.screen_h;
    G.cur_viewport = (SDL_GPUViewport){ 0, 0, (float)G.screen_w, (float)G.screen_h, 0.0f, 1.0f };
    G.scissor_on = false;
    G.pass = NULL;

    for (uint32_t i = 0; i < G.cmd_count; i++) {
        const GpuCmd *c = &G.cmds[i];
        switch (c->type) {
        case CMD_TARGET:
            end_pass();
            G.cur_target = c->target.id;
            G.cur_target_w = c->target.w;
            G.cur_target_h = c->target.h;
            break;

        case CMD_CLEAR:
            /* Clearing happens through a pass's load op, so start a fresh
               pass rather than drawing a quad over the target. */
            end_pass();
            memcpy(G.pending_clear_color, c->clear.color, sizeof(G.pending_clear_color));
            begin_pass(true);
            break;

        case CMD_VIEWPORT:
            G.cur_viewport = (SDL_GPUViewport){ (float)c->rect.x, (float)c->rect.y,
                                               (float)c->rect.w, (float)c->rect.h,
                                               0.0f, 1.0f };
            if (G.pass) SDL_SetGPUViewport(G.pass, &G.cur_viewport);
            break;

        case CMD_SCISSOR:
            G.cur_scissor = (SDL_Rect){ c->rect.x, c->rect.y, c->rect.w, c->rect.h };
            G.scissor_on = true;
            if (G.pass) SDL_SetGPUScissor(G.pass, &G.cur_scissor);
            break;

        case CMD_SCISSOR_OFF:
            G.scissor_on = false;
            if (G.pass) {
                SDL_Rect full = { 0, 0, G.cur_target_w, G.cur_target_h };
                SDL_SetGPUScissor(G.pass, &full);
            }
            break;

        case CMD_SPRITES:
            replay_sprites(c);
            break;

        case CMD_FILTER:
            replay_filter(c);
            break;
        }
    }

    end_pass();
    SDL_SubmitGPUCommandBuffer(G.cmd_buffer);
    G.cmd_buffer = NULL;

    G.cmd_count = 0;
    G.vertex_count = 0;
    G.uniform_size = 0;
}

void gpu_frame_present(void)
{
    if (!G.ready) return;
    gpu_submit();
    /* Windowless: the frame lives in the offscreen surface, which is where
       readbacks look anyway. */
    if (!G.window) return;

    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(G.device);
    if (!cb) return;

    SDL_GPUTexture *swap = NULL;
    Uint32 sw = 0, sh = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cb, G.window, &swap, &sw, &sh) || !swap) {
        /* Minimised or occluded: nothing to present. */
        SDL_SubmitGPUCommandBuffer(cb);
        return;
    }

    ensure_screen_texture();

    SDL_GPUColorTargetInfo info;
    SDL_zero(info);
    info.texture = swap;
    info.load_op = SDL_GPU_LOADOP_DONT_CARE;
    info.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cb, &info, 1, NULL);
    if (pass) {
        SDL_GPUGraphicsPipeline *pipe = pipeline_for(GPU_SHADER_BLIT, BLEND_MODE_NORMAL, true);
        if (pipe) {
            SDL_BindGPUGraphicsPipeline(pass, pipe);
            /* The blit quad follows the filter quad in the shared buffer. */
            SDL_GPUBufferBinding vb = { G.quad_buffer, (Uint32)sizeof(QUAD_VERTICES) };
            SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
            SDL_GPUTextureSamplerBinding tsb;
            SDL_zero(tsb);
            tsb.texture = G.screen;
            tsb.sampler = G.sampler_nearest;
            SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
            float one[4] = { 1.0f, 0, 0, 0 };
            SDL_PushGPUFragmentUniformData(cb, 0, one, sizeof(one));
            SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
        }
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(cb);
}

/* Readback */

uint8_t *gpu_texture_download(uint32_t id, int width, int height)
{
    if (!G.ready || width <= 0 || height <= 0) return NULL;

    /* Everything recorded so far must have executed before the copy. */
    gpu_submit();

    SDL_GPUTexture *tex = target_texture(id);
    if (!tex) return NULL;

    Uint32 bytes = (Uint32)width * (Uint32)height * 4;
    uint8_t *pixels = malloc(bytes);
    if (!pixels) return NULL;

    SDL_GPUTransferBufferCreateInfo ti;
    SDL_zero(ti);
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = bytes;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(G.device, &ti);
    if (!tb) { free(pixels); return NULL; }

    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(G.device);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTextureRegion region;
    SDL_zero(region);
    region.texture = tex;
    region.w = (Uint32)width;
    region.h = (Uint32)height;
    region.d = 1;
    SDL_GPUTextureTransferInfo dst;
    SDL_zero(dst);
    dst.transfer_buffer = tb;
    dst.pixels_per_row = (Uint32)width;
    dst.rows_per_layer = (Uint32)height;
    SDL_DownloadFromGPUTexture(cp, &region, &dst);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    if (fence) {
        SDL_WaitForGPUFences(G.device, true, &fence, 1);
        SDL_ReleaseGPUFence(G.device, fence);
    }

    void *src = SDL_MapGPUTransferBuffer(G.device, tb, false);
    if (src) {
        memcpy(pixels, src, bytes);
        SDL_UnmapGPUTransferBuffer(G.device, tb);
    } else {
        memset(pixels, 0, bytes);
    }
    SDL_ReleaseGPUTransferBuffer(G.device, tb);
    return pixels;
}
