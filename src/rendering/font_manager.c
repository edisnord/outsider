/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/font_manager.h"
#include "rendering/woff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"

#define MAX_FONTS 512

typedef struct {
    char            *family;     /* Font family name (owned). */
    uint8_t         *data;       /* TTF file data (owned). */
    size_t           data_size;
    stbtt_fontinfo   info;
    bool             valid;
} FontEntry;

static FontEntry s_fonts[MAX_FONTS];
static int       s_font_count = 0;

static FontEntry *find_font(const char *family)
{
    if (!family) return NULL;
    for (int i = 0; i < s_font_count; i++) {
        if (s_fonts[i].valid && strcmp(s_fonts[i].family, family) == 0) {
            return &s_fonts[i];
        }
    }
    return NULL;
}

/* Decode one UTF-8 codepoint and advance the pointer. */
static int decode_utf8(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    int cp;
    if ((*p & 0x80) == 0) {
        cp = *p++;
    } else if ((*p & 0xE0) == 0xC0 && p[1]) {
        cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
        p += 2;
    } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
        cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        p += 3;
    } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
             ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        p += 4;
    } else {
        cp = '?';
        p++;
    }
    *pp = (const char *)p;
    return cp;
}

void font_manager_init(void)
{
    memset(s_fonts, 0, sizeof(s_fonts));
    s_font_count = 0;
}

void font_manager_shutdown(void)
{
    for (int i = 0; i < s_font_count; i++) {
        free(s_fonts[i].family);
        free(s_fonts[i].data);
        s_fonts[i].valid = false;
    }
    s_font_count = 0;
}

static bool load_sfnt(const char *family, const uint8_t *data, size_t size);

bool font_manager_load(const char *family, const uint8_t *data, size_t size)
{
    if (woff_is_woff(data, size)) {
        size_t sfnt_size = 0;
        uint8_t *sfnt = woff_to_sfnt(data, size, &sfnt_size);
        if (!sfnt) return false;
        bool ok = load_sfnt(family, sfnt, sfnt_size);
        free(sfnt);
        return ok;
    }
    return load_sfnt(family, data, size);
}

static bool load_sfnt(const char *family, const uint8_t *data, size_t size)
{
    /* stb_truetype needs at least the TrueType offset table (12 bytes). */
    if (!family || !data || size < 12) return false;

    /* Replace existing font with the same family name. */
    FontEntry *existing = find_font(family);
    if (existing) {
        uint8_t *new_data = malloc(size);
        if (!new_data) return false;   /* keep the old font intact on OOM */
        memcpy(new_data, data, size);
        free(existing->data);
        existing->data = new_data;
        existing->data_size = size;
        existing->valid = stbtt_InitFont(&existing->info, existing->data, 0);
        if (!existing->valid) {
            /* Unparseable font: release and compact rather than keep a dead entry. */
            free(existing->data);
            free(existing->family);
            int idx = (int)(existing - s_fonts);
            s_font_count--;
            if (idx != s_font_count) s_fonts[idx] = s_fonts[s_font_count];
            memset(&s_fonts[s_font_count], 0, sizeof(FontEntry));
            return false;
        }
        return true;
    }

    if (s_font_count >= MAX_FONTS) return false;

    FontEntry *fe = &s_fonts[s_font_count];
    fe->family = strdup(family);
    fe->data = malloc(size);
    if (!fe->data) { free(fe->family); return false; }
    memcpy(fe->data, data, size);
    fe->data_size = size;
    fe->valid = stbtt_InitFont(&fe->info, fe->data, 0);
    if (!fe->valid) {
        free(fe->family);
        free(fe->data);
        return false;
    }
    s_font_count++;
    return true;
}

bool font_manager_load_file(const char *family, const char *path)
{
    if (!family || !path) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc((size_t)sz);
    if (!data) { fclose(f); return false; }

    size_t read = fread(data, 1, (size_t)sz, f);
    fclose(f);

    if (read != (size_t)sz) { free(data); return false; }

    bool ok = font_manager_load(family, data, (size_t)sz);
    free(data);
    return ok;
}

bool font_manager_has_font(const char *family)
{
    return find_font(family) != NULL;
}

bool font_manager_get_metrics(const char *family, float pixel_size, FontMetrics *out)
{
    if (!out) return false;
    FontEntry *fe = find_font(family);
    if (!fe) return false;

    /* A CSS pixel size is the em size (browsers map font-size to the em
       square), not the ascent-to-descent span stbtt_ScaleForPixelHeight uses. */
    float scale = stbtt_ScaleForMappingEmToPixels(&fe->info, pixel_size);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&fe->info, &ascent, &descent, &lineGap);

    out->ascent    = ascent * scale;
    out->descent   = descent * scale;
    out->line_gap  = lineGap * scale;
    out->line_height = out->ascent - out->descent + out->line_gap;
    return true;
}

float font_manager_measure_text(const char *family, float pixel_size, const char *text)
{
    if (!text || !*text) return 0.0f;

    FontEntry *fe = find_font(family);
    if (!fe) {
        /* No font: rough estimate of 0.6 * size per character. */
        int len = 0;
        const char *p = text;
        while (*p) { len++; p++; }
        return len * pixel_size * 0.6f;
    }

    float scale = stbtt_ScaleForMappingEmToPixels(&fe->info, pixel_size);
    float width = 0.0f;

    const char *p = text;
    int prev_cp = 0;
    while (*p) {
        int cp = decode_utf8(&p);

        if (prev_cp) {
            width += stbtt_GetCodepointKernAdvance(&fe->info, prev_cp, cp) * scale;
        }

        int advance, lsb;
        stbtt_GetCodepointHMetrics(&fe->info, cp, &advance, &lsb);
        width += advance * scale;
        prev_cp = cp;
    }
    return width;
}

int font_manager_font_count(void)
{
    return s_font_count;
}

const char *font_manager_font_name(int index)
{
    if (index < 0 || index >= s_font_count) return NULL;
    if (!s_fonts[index].valid) return NULL;
    return s_fonts[index].family;
}
