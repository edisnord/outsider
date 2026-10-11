/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "engine/js_engine.h"
#include "bindings/bind_tilemap.h"
#include "rendering/tilemap.h"
#include "rendering/renderer.h"

#include <quickjs.h>
#include <stdlib.h>

/* Global renderer, defined in bind_renderer.c; drawTiles uses its sprite batch. */
extern Renderer *s_renderer;

/* drawTiles(elementsF32, count, textureIds, texWidths, texHeights, numTilesets, offsetX, offsetY [, scaleX, scaleY]) */

static JSValue js_tilemap_draw_tiles(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    RMMZ_PROF_SCOPE("tilemap.tilemap_draw_tiles");
    (void)this_val;
    if (argc < 8)
        return JS_ThrowTypeError(ctx, "drawTiles requires 8 arguments");
    if (!s_renderer)
        return JS_UNDEFINED;

    /* arg 1: count */
    int count;
    if (JS_ToInt32(ctx, &count, argv[1])) return JS_EXCEPTION;
    if (count <= 0) return JS_UNDEFINED;

    /* arg 2: textureIds — Array of uint32 */
    /* arg 3: texWidths — Array of int */
    /* arg 4: texHeights — Array of int */
    /* arg 5: numTilesets */
    int num_tilesets;
    if (JS_ToInt32(ctx, &num_tilesets, argv[5])) return JS_EXCEPTION;
    if (num_tilesets <= 0) return JS_UNDEFINED;

    /* Allocate tilesets info on the stack for small counts, heap for large. */
    TilesetInfo stack_ts[32];
    TilesetInfo *tilesets = (num_tilesets <= 32) ? stack_ts : malloc(sizeof(TilesetInfo) * num_tilesets);
    if (!tilesets) return JS_ThrowOutOfMemory(ctx);

    /* Read tileset arrays. */
    for (int i = 0; i < num_tilesets; i++) {
        JSValue tid_val = JS_GetPropertyUint32(ctx, argv[2], i);
        JSValue tw_val  = JS_GetPropertyUint32(ctx, argv[3], i);
        JSValue th_val  = JS_GetPropertyUint32(ctx, argv[4], i);

        uint32_t gl_tex = 0;
        int tw = 0, th = 0;
        int conv_err = JS_ToUint32(ctx, &gl_tex, tid_val) ||
                       JS_ToInt32(ctx, &tw, tw_val) ||
                       JS_ToInt32(ctx, &th, th_val);

        JS_FreeValue(ctx, tid_val);
        JS_FreeValue(ctx, tw_val);
        JS_FreeValue(ctx, th_val);

        if (conv_err) {
            if (tilesets != stack_ts) free(tilesets);
            return JS_EXCEPTION;
        }

        tilesets[i].gl_texture = gl_tex;
        tilesets[i].width = tw;
        tilesets[i].height = th;
    }

    /* arg 6: offsetX, arg 7: offsetY */
    double offset_x, offset_y;
    if (JS_ToFloat64(ctx, &offset_x, argv[6])) {
        if (tilesets != stack_ts) free(tilesets);
        return JS_EXCEPTION;
    }
    if (JS_ToFloat64(ctx, &offset_y, argv[7])) {
        if (tilesets != stack_ts) free(tilesets);
        return JS_EXCEPTION;
    }

    /* optional args 8 and 9: scale of the layer's world transform (a zoomed map) */
    double scale_x = 1.0, scale_y = 1.0;
    if (argc >= 10) {
        if (JS_ToFloat64(ctx, &scale_x, argv[8]) || JS_ToFloat64(ctx, &scale_y, argv[9])) {
            if (tilesets != stack_ts) free(tilesets);
            return JS_EXCEPTION;
        }
        if (!(scale_x > 0.0) || !(scale_y > 0.0)) { scale_x = 1.0; scale_y = 1.0; }
    }

    /* arg 0: elementsF32 (ArrayBuffer). Capture the backing pointer only after
       every other argument is converted: those conversions can run user JS
       (valueOf/getters/Proxy traps) that detaches or resizes this buffer. */
    size_t elem_size = 0;
    uint8_t *elem_buf = JS_GetArrayBuffer(ctx, &elem_size, argv[0]);
    if (!elem_buf) {
        if (tilesets != stack_ts) free(tilesets);
        return JS_ThrowTypeError(ctx, "drawTiles: arg 0 must be an ArrayBuffer");
    }
    size_t expected = (size_t)count * 7 * sizeof(float);
    if (elem_size < expected) {
        if (tilesets != stack_ts) free(tilesets);
        return JS_ThrowRangeError(ctx, "drawTiles: buffer too small for %d elements", count);
    }
    const float *elements = (const float *)elem_buf;

    SpriteBatch *batch = renderer_get_batch(s_renderer);
    if (batch) {
        tilemap_draw_tiles(elements, count, tilesets, num_tilesets,
                           (float)offset_x, (float)offset_y, (float)scale_x, (float)scale_y, batch);
    }

    if (tilesets != stack_ts) free(tilesets);
    return JS_UNDEFINED;
}

/* Registration */

static const JSCFunctionListEntry js_tilemap_funcs[] = {
    JS_CFUNC_DEF("drawTiles", 8, js_tilemap_draw_tiles),
};

void bind_tilemap_register(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue obj = JS_NewObject(ctx);

    JS_SetPropertyFunctionList(ctx, obj, js_tilemap_funcs,
                               sizeof(js_tilemap_funcs) / sizeof(js_tilemap_funcs[0]));

    JS_SetPropertyStr(ctx, global, "__native_tilemap", obj);
    JS_FreeValue(ctx, global);
}
