/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/glsl_translate.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A growable output buffer. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    bool   failed;
} Buf;

static void buf_add(Buf *b, const char *text, size_t n)
{
    if (b->failed) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char *grown = realloc(b->data, cap);
        if (!grown) { b->failed = true; return; }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, text, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_puts(Buf *b, const char *text)
{
    buf_add(b, text, strlen(text));
}

static void buf_printf(Buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) buf_add(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

/* std140 layout for the types a plugin filter can actually set from JS. */
typedef struct {
    const char *name;
    uint32_t    size;
    uint32_t    align;
    uint32_t    components;
    bool        integer;
} GlslType;

static const GlslType TYPES[] = {
    { "float", 4,  4,  1,  false },
    { "int",   4,  4,  1,  true  },
    { "bool",  4,  4,  1,  true  },
    { "vec2",  8,  8,  2,  false },
    { "vec3",  12, 16, 3,  false },
    { "vec4",  16, 16, 4,  false },
    { "mat2",  32, 16, 4,  false },
    { "mat3",  48, 16, 9,  false },
    { "mat4",  64, 16, 16, false },
};

static const GlslType *find_type(const char *name)
{
    for (size_t i = 0; i < sizeof(TYPES) / sizeof(TYPES[0]); i++) {
        if (strcmp(TYPES[i].name, name) == 0) return &TYPES[i];
    }
    return NULL;
}

static bool is_sampler_type(const char *name)
{
    return strncmp(name, "sampler", 7) == 0;
}

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/* Strip // and /* *\/ comments so declarations can be matched by keyword
   without a comment smuggling one in. Newlines are preserved so the compiler's
   error line numbers still mean something. */
static char *strip_comments(const char *src)
{
    size_t n = strlen(src);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        if (src[i] == '/' && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') i++;
        } else if (src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i < n && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n') out[o++] = '\n';
                i++;
            }
            i += 2;
        } else {
            out[o++] = src[i++];
        }
    }
    out[o] = '\0';
    return out;
}

/* Read the next whitespace-delimited word, returning how far it advanced. */
static const char *next_word(const char *p, char *out, size_t out_size)
{
    while (*p && (unsigned char)*p <= ' ') p++;
    size_t i = 0;
    while (*p && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_')) {
        if (i + 1 < out_size) out[i++] = *p;
        p++;
    }
    out[i] = '\0';
    return p;
}

/* Reserved in GLSL 450 but ordinary identifiers in the ES 1.00 these shaders
   were written for. pixi-filters' ZoomBlurFilter declares `vec4 sample`. */
static const char *RESERVED_IN_450[] = {
    "sample", "filter", "buffer", "shared", "patch", "subroutine",
    "precise", "resource", "active", "input", "output", "partition",
};

/* Whole-word search, so `sampler2D` does not look like `sample`. */
static bool contains_word(const char *haystack, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = strstr(haystack, word); p; p = strstr(p + 1, word)) {
        char before = (p == haystack) ? ' ' : p[-1];
        char after = p[n];
        bool bok = !((before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') ||
                     (before >= '0' && before <= '9') || before == '_');
        bool aok = !((after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') ||
                     (after >= '0' && after <= '9') || after == '_');
        if (bok && aok) return true;
    }
    return false;
}

static bool starts_with_word(const char *p, const char *word)
{
    size_t n = strlen(word);
    if (strncmp(p, word, n) != 0) return false;
    char after = p[n];
    return !((after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') ||
             (after >= '0' && after <= '9') || after == '_');
}

static char s_error[160];

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    va_end(ap);
}

const char *glsl_translate_error(void)
{
    return s_error[0] ? s_error : "unknown";
}

bool glsl_translate_fragment(const char *source, GlslTranslation *out)
{
    s_error[0] = '\0';
    if (!source || !out) { set_error("no source"); return false; }

    char *src = strip_comments(source);
    if (!src) return false;

    GlslTranslation t;
    memset(&t, 0, sizeof(t));

    Buf body = { 0 };      /* everything that is not a declaration we rewrote */
    uint32_t offset = 0;
    int varying_location = 0;
    bool uses_frag_color = false;

    int depth = 0;
    const char *p = src;
    const char *line_start = p;

    while (*p) {
        /* Work a line at a time; GLSL declarations are line-oriented in
           practice, and function bodies are copied through untouched. */
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        const char *line = p;

        const char *scan = line;
        while (*scan == ' ' || *scan == '\t') scan++;

        bool consumed = false;

        if (depth == 0) {
            if (starts_with_word(scan, "precision")) {
                /* Precision qualifiers are meaningless in the desktop
                   profile; dropping the statement avoids a parse error. */
                consumed = true;
            } else if (starts_with_word(scan, "varying") ||
                       starts_with_word(scan, "attribute")) {
                /* `varying vec2 vTextureCoord;` -> an explicit input. The
                   vertex stage writes these in declaration order. */
                char type[GLSL_MAX_NAME], name[GLSL_MAX_NAME];
                const char *q = next_word(scan, type, sizeof(type));   /* varying */
                q = next_word(q, type, sizeof(type));
                q = next_word(q, name, sizeof(name));
                if (type[0] && name[0]) {
                    if (varying_location >= GLSL_MAX_VARYINGS) { set_error("more than %d varyings", GLSL_MAX_VARYINGS); goto fail; }
                    snprintf(t.varyings[varying_location].name, GLSL_MAX_NAME, "%s", name);
                    snprintf(t.varyings[varying_location].type,
                             sizeof(t.varyings[0].type), "%s", type);
                    buf_printf(&body, "layout(location = %d) in %s %s;\n",
                               varying_location++, type, name);
                    consumed = true;
                }
            } else if (starts_with_word(scan, "uniform")) {
                char type[GLSL_MAX_NAME], name[GLSL_MAX_NAME];
                const char *q = next_word(scan, type, sizeof(type));   /* uniform */
                q = next_word(q, type, sizeof(type));
                q = next_word(q, name, sizeof(name));

                /* Arrays would need std140's 16-byte element stride and are
                   not settable from the JS side anyway; leave them alone
                   rather than mislay them in the block. */
                bool is_array = (*q == '[');

                if (is_sampler_type(type)) {
                    if (t.sampler_count >= GLSL_MAX_SAMPLERS) { set_error("more than %d samplers", GLSL_MAX_SAMPLERS); goto fail; }
                    snprintf(t.samplers[t.sampler_count], GLSL_MAX_NAME, "%s", name);
                    buf_printf(&body,
                               "layout(set = 2, binding = %d) uniform %s %s;\n",
                               t.sampler_count, type, name);
                    t.sampler_count++;
                    consumed = true;
                } else if (!is_array) {
                    const GlslType *gt = find_type(type);
                    if (!gt) { set_error("uniform '%s' has unsupported type '%s'", name, type); goto fail; }
                    if (t.uniform_count >= GLSL_MAX_UNIFORMS) { set_error("more than %d uniforms", GLSL_MAX_UNIFORMS); goto fail; }
                    offset = align_up(offset, gt->align);
                    GlslUniform *u = &t.uniforms[t.uniform_count++];
                    snprintf(u->name, GLSL_MAX_NAME, "%s", name);
                    snprintf(u->type, sizeof(u->type), "%s", gt->name);
                    u->offset = offset;
                    u->size = gt->size;
                    u->components = gt->components;
                    u->integer = gt->integer;
                    offset += gt->size;
                    /* The declaration moves into the block emitted below; the
                       body keeps referring to the bare name. */
                    consumed = true;
                }
            }
        }

        if (!consumed) {
            if (strstr(line, "gl_FragColor")) uses_frag_color = true;
            buf_add(&body, line, line_len);
            buf_add(&body, "\n", 1);
        }

        /* Track brace depth so declarations inside functions are left alone. */
        for (size_t i = 0; i < line_len; i++) {
            if (line[i] == '{') depth++;
            else if (line[i] == '}') depth--;
        }

        if (!eol) break;
        p = eol + 1;
    }
    (void)line_start;

    if (body.failed) { set_error("out of memory"); goto fail; }

    t.varying_count = varying_location;

    t.uniform_size = t.uniform_count ? align_up(offset, 16) : 0;

    /* Assemble: version, the uniform block, the aliases that redirect the
       shader's own references, then its untouched body. */
    Buf outbuf = { 0 };
    buf_puts(&outbuf, "#version 450\n");

    if (t.uniform_count) {
        /* Deliberately anonymous: a block with no instance name puts its
           members in global scope, so the shader's own references still read
           `size` rather than `_rmmz.size`. Aliasing them with #define instead
           would rewrite every occurrence of the name, including a function
           parameter that shadows it -- which is exactly what pixi-filters'
           PixelateFilter does with `vec2 pixelate(vec2 coord, vec2 size)`. */
        buf_puts(&outbuf, "layout(set = 3, binding = 0, std140) uniform _RmmzUniforms {\n");
        for (int i = 0; i < t.uniform_count; i++) {
            buf_printf(&outbuf, "    %s %s;\n",
                       t.uniforms[i].type, t.uniforms[i].name);
        }
        buf_puts(&outbuf, "};\n");
    }

    if (uses_frag_color) {
        buf_puts(&outbuf, "layout(location = 0) out vec4 _rmmzFragColor;\n");
        buf_puts(&outbuf, "#define gl_FragColor _rmmzFragColor\n");
    }
    /* Renaming through #define is safe here: these are never declared by the
       block above, so every occurrence really is the shader's own identifier. */
    for (size_t i = 0; i < sizeof(RESERVED_IN_450) / sizeof(RESERVED_IN_450[0]); i++) {
        if (contains_word(src, RESERVED_IN_450[i])) {
            buf_printf(&outbuf, "#define %s _rmmz_%s\n",
                       RESERVED_IN_450[i], RESERVED_IN_450[i]);
        }
    }

    buf_puts(&outbuf, "#define texture2D texture\n");
    buf_puts(&outbuf, "#define textureCube texture\n");
    buf_puts(&outbuf, "#define texture2DProj textureProj\n");

    buf_puts(&outbuf, body.data ? body.data : "");

    if (outbuf.failed) {
        free(outbuf.data);
        set_error("out of memory");
        goto fail;
    }

    free(body.data);
    free(src);
    src = NULL;

    t.source = outbuf.data;
    *out = t;
    return true;

fail:
    free(body.data);
    free(src);
    return false;
}

void glsl_translation_free(GlslTranslation *t)
{
    if (!t) return;
    free(t->source);
    t->source = NULL;
}

const GlslUniform *glsl_translation_find(const GlslTranslation *t, const char *name)
{
    if (!t || !name) return NULL;
    for (int i = 0; i < t->uniform_count; i++) {
        if (strcmp(t->uniforms[i].name, name) == 0) return &t->uniforms[i];
    }
    return NULL;
}

char *glsl_generate_vertex(const GlslTranslation *t)
{
    if (!t) return NULL;

    Buf b = { 0 };
    buf_puts(&b, "#version 450\n");
    buf_puts(&b, "layout(location = 0) in vec2 a_position;\n");
    buf_puts(&b, "layout(location = 1) in vec2 a_texcoord;\n");

    for (int i = 0; i < t->varying_count; i++) {
        buf_printf(&b, "layout(location = %d) out %s %s;\n",
                   i, t->varyings[i].type, t->varyings[i].name);
    }

    buf_puts(&b, "void main() {\n");
    buf_puts(&b, "    gl_Position = vec4(a_position, 0.0, 1.0);\n");
    for (int i = 0; i < t->varying_count; i++) {
        if (i == 0 && strcmp(t->varyings[i].type, "vec2") == 0) {
            buf_printf(&b, "    %s = a_texcoord;\n", t->varyings[i].name);
        } else {
            /* Nothing sensible to put here, but the pipeline needs the output
               written for its signature to match. */
            buf_printf(&b, "    %s = %s(0.0);\n",
                       t->varyings[i].name, t->varyings[i].type);
        }
    }
    buf_puts(&b, "}\n");

    if (b.failed) {
        free(b.data);
        return NULL;
    }
    return b.data;
}

int glsl_translation_sampler_index(const GlslTranslation *t, const char *name)
{
    if (!t || !name) return -1;
    for (int i = 0; i < t->sampler_count; i++) {
        if (strcmp(t->samplers[i], name) == 0) return i;
    }
    return -1;
}
