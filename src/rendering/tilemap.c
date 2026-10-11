/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/tilemap.h"

/* Fields per element in the packed float array. */
#define TILE_FIELDS 7

void tilemap_draw_tiles(const float *elements, int count,
                        const TilesetInfo *tilesets, int num_tilesets,
                        float offset_x, float offset_y,
                        float scale_x, float scale_y,
                        SpriteBatch *batch)
{
    if (!elements || count <= 0 || !batch) return;

    for (int i = 0; i < count; i++) {
        const float *e = elements + i * TILE_FIELDS;
        int   set_number = (int)e[0];
        float sx = e[1];
        float sy = e[2];
        float dx = e[3] * scale_x + offset_x;
        float dy = e[4] * scale_y + offset_y;
        float w  = e[5];
        float h  = e[6];
        float dw = w * scale_x;
        float dh = h * scale_y;

        if (w <= 0 || h <= 0) continue;

        if (set_number < 0) {
            /* Shadow tile: black-tinted white texture at 50% alpha. */
            sprite_batch_draw(batch, 0,
                              dx, dy, dw, dh,
                              0.0f, 0.0f, 1.0f, 1.0f,
                              0xFF000000, 0.5f);
            continue;
        }

        if (set_number >= num_tilesets) continue;

        const TilesetInfo *ts = &tilesets[set_number];
        if (ts->gl_texture == 0 || ts->width <= 0 || ts->height <= 0) continue;

        float inv_w = 1.0f / (float)ts->width;
        float inv_h = 1.0f / (float)ts->height;
        float u0 = sx * inv_w;
        float v0 = sy * inv_h;
        float u1 = (sx + w) * inv_w;
        float v1 = (sy + h) * inv_h;

        sprite_batch_draw(batch, ts->gl_texture,
                          dx, dy, dw, dh,
                          u0, v0, u1, v1,
                          0xFFFFFFFF, 1.0f);
    }
}
