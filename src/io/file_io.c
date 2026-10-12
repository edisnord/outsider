/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "io/file_io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#else
#define strcasecmp _stricmp
#endif
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define mkdir_p(p) _mkdir(p)
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#else
#include <dirent.h>
#include <unistd.h>
#define mkdir_p(p) mkdir(p, 0755)
#endif

#define MAX_PATH_LEN 4096

static char s_game_root[MAX_PATH_LEN] = "";

/* NW.js (the desktop player these games are made for) treats a path that starts with "/" as relative to the folder
   of the app (where package.json is), and "../x" from the page is allowed to reach that folder too. For a game
   exported with a "www" folder that app folder is the one above "www" (game data such as /data/x.csv can live there).
   The game root is what the runtime is given: the www folder itself. */
static char s_package_root[MAX_PATH_LEN] = "";

static bool plain_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void compute_package_root(void)
{
    snprintf(s_package_root, sizeof(s_package_root), "%s", s_game_root);
    const char *slash = strrchr(s_game_root, '/');
    if (!slash || slash == s_game_root || strcasecmp(slash + 1, "www") != 0) return;
    char parent[MAX_PATH_LEN];
    size_t n = (size_t)(slash - s_game_root);
    if (n >= sizeof(parent) - 16) return;
    memcpy(parent, s_game_root, n);
    parent[n] = '\0';
    char probe[MAX_PATH_LEN];
    snprintf(probe, sizeof(probe), "%s/package.json", parent);
    if (plain_exists(probe)) snprintf(s_package_root, sizeof(s_package_root), "%s", parent);
}

/* Path resolution */

void file_io_set_game_root(const char *root)
{
    if (!root || root[0] == '\0') {
        s_game_root[0] = '\0';
        return;
    }

    /* Resolve to an absolute path so paths derived from the root contain no
       ".." and are not rejected by the traversal check. */
#ifdef _WIN32
    char *resolved = _fullpath(NULL, root, 0);
#else
    char *resolved = realpath(root, NULL);
#endif
    if (resolved) {
        size_t len = strlen(resolved);
        if (len >= MAX_PATH_LEN) len = MAX_PATH_LEN - 1;
        memcpy(s_game_root, resolved, len);
        s_game_root[len] = '\0';
        free(resolved);
    } else {
        size_t len = strlen(root);
        if (len >= MAX_PATH_LEN) len = MAX_PATH_LEN - 1;
        memcpy(s_game_root, root, len);
        s_game_root[len] = '\0';
    }

#ifdef _WIN32
    /* Normalise to forward slashes: Win32 accepts either, and the JS path
       shims always produce '/', which keeps prefix comparisons simple. */
    for (char *c = s_game_root; *c; c++)
        if (*c == '\\') *c = '/';
#endif

    /* Strip trailing separator (but keep a bare drive root like "C:/"). */
    size_t len = strlen(s_game_root);
    while (len > 1 && (s_game_root[len - 1] == '/' || s_game_root[len - 1] == '\\')) {
        if (len == 3 && s_game_root[1] == ':') break;
        len--;
    }
    s_game_root[len] = '\0';
    compute_package_root();
}

/* Path character equality: '/' == '\\', and case-insensitive on Windows. */
static bool path_char_eq(char a, char b)
{
    if (a == '\\') a = '/';
    if (b == '\\') b = '/';
#ifdef _WIN32
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
#endif
    return a == b;
}

static bool path_has_prefix(const char *path, const char *prefix)
{
    while (*prefix) {
        if (*path == '\0' || !path_char_eq(*path, *prefix)) return false;
        path++;
        prefix++;
    }
    return true;
}

const char *file_io_get_game_root(void)
{
    return s_game_root;
}

/* True if any path component is "..". */
static bool path_has_traversal(const char *path)
{
    if (!path) return false;
    const char *p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' &&
            (p[2] == '/' || p[2] == '\\' || p[2] == '\0')) {
            if (p == path || p[-1] == '/' || p[-1] == '\\') {
                return true;
            }
        }
        p++;
    }
    return false;
}

/* Lexically resolve "." and ".." components of a relative path, writing the
   result with '/' separators and no leading or trailing separator. Returns
   false if ".." would climb above the starting directory or the result does
   not fit. Games do request paths like "data/../dataEx/x.json", which stay
   inside the game root and must work. */
static bool normalize_components(const char *in, char *out, size_t out_size)
{
    size_t out_len = 0;
    const char *p = in;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/' && *p != '\\') p++;
        size_t n = (size_t)(p - start);
        if (*p) p++;

        if (n == 0 || (n == 1 && start[0] == '.')) continue;

        if (n == 2 && start[0] == '.' && start[1] == '.') {
            if (out_len == 0) return false;
            while (out_len > 0 && out[out_len - 1] != '/') out_len--;
            if (out_len > 0) out_len--;   /* drop the separator too */
            continue;
        }

        size_t need = out_len + (out_len > 0 ? 1 : 0) + n;
        if (need >= out_size) return false;
        if (out_len > 0) out[out_len++] = '/';
        memcpy(out + out_len, start, n);
        out_len += n;
    }
    out[out_len] = '\0';
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode %XX escapes in place-compatible fashion (out may not alias in).
   Sequences that are not two hex digits are copied through unchanged. */
static bool percent_decode(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c == '%' && p[1] && p[2]) {
            int hi = hex_value(p[1]), lo = hex_value(p[2]);
            if (hi >= 0 && lo >= 0) {
                c = (char)((hi << 4) | lo);
                p += 2;
            }
        }
        if (o + 1 >= out_size) return false;
        out[o++] = c;
    }
    out[o] = '\0';
    return true;
}

bool file_io_resolve_path(const char *relative_path, char *out, size_t out_size)
{
    if (!relative_path || !out || out_size == 0) return false;

    /* The engine hands out URLs: Utils.encodeURI() turns "title screen.png"
       into "title%20screen.png". A browser decodes that before touching the
       file system, so do the same here. */
    char decoded[1024];
    if (strchr(relative_path, '%')) {
        if (!percent_decode(relative_path, decoded, sizeof(decoded))) return false;
        relative_path = decoded;
    }

    /* The page the shims present is file:///game/index.html (dom_shim.js), so a script resolving "fonts/x.ttf"
       against its location gets "/game/fonts/x.ttf". "/game" is that page's root, the game root here, whatever the
       folder really is (on a console it is /data/games/...; only a host run that mounts the game at /game has
       a real /game). Without this the game's own fonts failed to load there. */
    if (s_game_root[0] != '\0' && strncmp(relative_path, "/game", 5) == 0 &&
        (relative_path[5] == '/' || relative_path[5] == '\0') && !path_has_prefix(relative_path, s_game_root)) {
        relative_path += relative_path[5] == '/' ? 6 : 5;
    }

    /* Absolute paths are accepted only inside the game root (when one is set). */
    if (relative_path[0] == '/' || relative_path[0] == '\\' ||
        (relative_path[0] != '\0' && relative_path[1] == ':')) {
        if (s_game_root[0] != '\0') {
            size_t root_len = strlen(s_game_root);
            if (!path_has_prefix(relative_path, s_game_root)) {
                /* "/data/x.csv": relative to the app folder (NW.js), else to the game root. Never outside both. */
                if (relative_path[0] != '/' && relative_path[0] != '\\') return false;
                char norm[1024], candidate[MAX_PATH_LEN];
                if (!normalize_components(relative_path, norm, sizeof(norm))) return false;
                int w1 = snprintf(candidate, sizeof(candidate), "%s/%s", s_package_root, norm);
                if (w1 > 0 && (size_t)w1 < sizeof(candidate) && plain_exists(candidate) && (size_t)w1 < out_size) {
                    memcpy(out, candidate, (size_t)w1 + 1);
                    return true;
                }
                int w2 = snprintf(out, out_size, "%s/%s", s_game_root, norm);
                return w2 > 0 && (size_t)w2 < out_size;
            }
            /* root="/game" must reject "/game2/...". */
            char next = relative_path[root_len];
            if (next != '\0' && next != '/' && next != '\\') {
                return false;
            }
            if (path_has_traversal(relative_path + root_len)) {
                /* ".." is fine as long as the path never leaves the root. */
                char norm[1024];
                if (!normalize_components(relative_path + root_len, norm, sizeof(norm))) {
                    return false;
                }
                int written = snprintf(out, out_size, "%s/%s", s_game_root, norm);
                return written > 0 && (size_t)written < out_size;
            }
        }
        size_t len = strlen(relative_path);
        if (len >= out_size) return false;
        memcpy(out, relative_path, len + 1);
        return true;
    }

    if (s_game_root[0] != '\0') {
        char norm[1024];
        if (path_has_traversal(relative_path)) {
            /* ".." is fine as long as the path never leaves the root... */
            if (!normalize_components(relative_path, norm, sizeof(norm))) {
                /* ...or, for a game in a "www" folder, the app folder above it (NW.js lets "../data/x" reach it). */
                if (strcmp(s_package_root, s_game_root) == 0) return false;
                char full[2048], clean[MAX_PATH_LEN];
                snprintf(full, sizeof(full), "%s/%s", s_game_root + (s_game_root[0] == '/' ? 1 : 0), relative_path);
                if (!normalize_components(full, clean, sizeof(clean))) return false;
                char abs_path[MAX_PATH_LEN + 2];
                int w = snprintf(abs_path, sizeof(abs_path), "%s%s", s_game_root[0] == '/' ? "/" : "", clean);
                if (w <= 0 || (size_t)w >= out_size) return false;
                size_t plen = strlen(s_package_root);
                if (strncmp(abs_path, s_package_root, plen) != 0 || (abs_path[plen] != '/' && abs_path[plen] != '\0')) return false;
                memcpy(out, abs_path, (size_t)w + 1);
                return true;
            }
            relative_path = norm;
        }
        int written = snprintf(out, out_size, "%s/%s", s_game_root, relative_path);
        return written > 0 && (size_t)written < out_size;
    }

    size_t len = strlen(relative_path);
    if (len >= out_size) return false;
    memcpy(out, relative_path, len + 1);
    return true;
}

/* File read */

/* ---- encrypted assets -------------------------------------------------------------------------------
   RPG Maker MV/MZ can encrypt images and sounds when a game is deployed: "img/x.png" becomes "img/x.png_"
   (MZ) or "img/x.rpgmvp" (MV), "audio/bgm/y.ogg" becomes "y.ogg_" / "y.rpgmvo", and so on. The format is a
   16 byte header ("RPGMV" ...) followed by the file whose first 16 bytes are XORed with the key stored in
   data/System.json ("encryptionKey", 32 hex digits). The browser player decrypts in JavaScript; here a read
   of a file that does not exist looks for the encrypted twin and decrypts it on the fly, so encrypted games
   run as they are. data/System.json is reported with the encryption flags cleared, so that the game's own
   scripts do not try to decrypt again. */

static bool s_key_tried;
static bool s_key_valid;
static uint8_t s_key[16];

static void load_encryption_key(void)
{
    if (s_key_tried) return;
    s_key_tried = true;

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s%sdata/System.json", s_game_root, s_game_root[0] ? "/" : "");
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 16 * 1024 * 1024) { fclose(f); return; }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';

    const char *k = strstr(buf, "\"encryptionKey\"");
    if (k) {
        k = strchr(k + 15, ':');
        if (k) k = strchr(k, '"');
        if (k) {
            k++;
            int ok = 1;
            for (int i = 0; i < 16 && ok; i++) {
                int hi = hex_value(k[2 * i]), lo = hex_value(k[2 * i + 1]);
                if (hi < 0 || lo < 0) ok = 0;
                else s_key[i] = (uint8_t)(hi * 16 + lo);
            }
            s_key_valid = ok != 0;
        }
    }
    free(buf);
}

static const char *const s_enc_exts[][3] = {
    { ".png", ".png_", ".rpgmvp" },
    { ".ogg", ".ogg_", ".rpgmvo" },
    { ".m4a", ".m4a_", ".rpgmvm" },
};

/* Path of the idx-th (0 or 1) encrypted twin of `path`, or false if `path` is not an image or sound name. */
static bool encrypted_twin(const char *path, int idx, char *out, size_t out_size)
{
    size_t len = strlen(path);
    for (size_t e = 0; e < sizeof(s_enc_exts) / sizeof(s_enc_exts[0]); e++) {
        size_t elen = strlen(s_enc_exts[e][0]);
        if (len > elen && strcasecmp(path + len - elen, s_enc_exts[e][0]) == 0) {
            const char *repl = s_enc_exts[e][1 + idx];
            if (len - elen + strlen(repl) + 1 > out_size) return false;
            memcpy(out, path, len - elen);
            strcpy(out + len - elen, repl);
            return true;
        }
    }
    return false;
}

static uint8_t *raw_read(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 0) {
        fclose(f);
        return NULL;
    }

    /* +1: NUL terminator for text, and an empty file still yields a non-NULL buffer. */
    uint8_t *buf = malloc((size_t)fsize + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[nread] = '\0';
    *out_size = nread;
    return buf;
}

/* Reads and decrypts the encrypted twin of a missing file. */
static uint8_t *read_encrypted_twin(const char *path, size_t *out_size)
{
    char twin[MAX_PATH_LEN];
    for (int idx = 0; idx < 2; idx++) {
        if (!encrypted_twin(path, idx, twin, sizeof(twin))) return NULL;
        uint8_t *buf = raw_read(twin, out_size);
        if (!buf) continue;
        load_encryption_key();
        if (!s_key_valid || *out_size < 32 || memcmp(buf, "RPGMV", 5) != 0) {
            free(buf);
            return NULL;
        }
        for (int i = 0; i < 16; i++) buf[16 + i] ^= s_key[i];
        memmove(buf, buf + 16, *out_size - 16);
        *out_size -= 16;
        buf[*out_size] = '\0';
        return buf;
    }
    return NULL;
}

static bool is_system_json(const char *path)
{
    size_t len = strlen(path);
    const char *name = "data/System.json";
    size_t nlen = strlen(name);
    return len >= nlen && strcasecmp(path + len - nlen, name) == 0;
}

/* Sets "hasEncryptedImages"/"hasEncryptedAudio" to false in the JSON text (the buffer is replaced). */
static uint8_t *patch_system_json(uint8_t *buf, size_t *size)
{
    static const char *const keys[2] = { "\"hasEncryptedImages\"", "\"hasEncryptedAudio\"" };
    load_encryption_key();
    if (!s_key_valid) return buf;   /* cannot decrypt: leave the game to fail as it would */
    for (int k = 0; k < 2; k++) {
        char *at = strstr((char *)buf, keys[k]);
        if (!at) continue;
        char *v = at + strlen(keys[k]);
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
        if (*v != ':') continue;
        v++;
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
        if (strncmp(v, "true", 4) != 0) continue;
        size_t off = (size_t)(v - (char *)buf);
        uint8_t *nb = malloc(*size + 2);
        if (!nb) return buf;
        memcpy(nb, buf, off);
        memcpy(nb + off, "false", 5);
        memcpy(nb + off + 5, buf + off + 4, *size - off - 4 + 1);   /* including the NUL */
        free(buf);
        buf = nb;
        *size += 1;
    }
    return buf;
}

/* File read */

char *file_io_read_text(const char *path, size_t *out_size)
{
    if (!path) return NULL;
    size_t size = 0;
    uint8_t *buf = raw_read(path, &size);
    if (!buf) buf = read_encrypted_twin(path, &size);
    if (buf && is_system_json(path)) buf = patch_system_json(buf, &size);
    if (buf && out_size) *out_size = size;
    return (char *)buf;
}

uint8_t *file_io_read_binary(const char *path, size_t *out_size)
{
    if (!path || !out_size) return NULL;
    uint8_t *buf = raw_read(path, out_size);
    if (!buf) buf = read_encrypted_twin(path, out_size);
    if (buf && is_system_json(path)) buf = patch_system_json(buf, out_size);
    return buf;
}

/* File write */

bool file_io_write_text(const char *path, const char *data)
{
    if (!path || !data) return false;

    FILE *f = fopen(path, "wb");
    if (!f) return false;

    size_t len = strlen(data);
    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    return written == len;
}

bool file_io_write_binary(const char *path, const uint8_t *data, size_t size)
{
    if (!path || (!data && size > 0)) return false;

    FILE *f = fopen(path, "wb");
    if (!f) return false;

    if (size > 0) {
        size_t written = fwrite(data, 1, size, f);
        fclose(f);
        return written == size;
    }

    fclose(f);
    return true;
}

/* File/directory queries */

/* On Windows, stat("dir/") with a trailing separator fails (except for a
   drive root), so the separator is stripped first. */
static int portable_stat(const char *path, struct stat *st)
{
#ifdef _WIN32
    size_t len = strlen(path);
    if (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\') &&
        !(len == 3 && path[1] == ':') && len < MAX_PATH_LEN) {
        char tmp[MAX_PATH_LEN];
        memcpy(tmp, path, len - 1);
        tmp[len - 1] = '\0';
        return stat(tmp, st);
    }
#endif
    return stat(path, st);
}

bool file_io_exists(const char *path)
{
    if (!path) return false;
    struct stat st;
    if (portable_stat(path, &st) == 0) return true;
    /* an encrypted image or sound counts as existing (it is decrypted when read) */
    char twin[MAX_PATH_LEN];
    for (int idx = 0; idx < 2; idx++) {
        if (!encrypted_twin(path, idx, twin, sizeof(twin))) break;
        if (portable_stat(twin, &st) == 0) return true;
    }
    return false;
}

bool file_io_is_directory(const char *path)
{
    if (!path) return false;
    struct stat st;
    return portable_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Directory operations */

bool file_io_mkdir(const char *path)
{
    if (!path) return false;

    if (file_io_is_directory(path)) return true;

    char tmp[MAX_PATH_LEN];
    size_t len = strlen(path);
    if (len >= MAX_PATH_LEN) return false;
    memcpy(tmp, path, len + 1);

    /* Create each intermediate component in turn. */
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = '\0';
#ifdef _WIN32
            /* "C:" is a drive designator rather than a directory. Windows
               reads it as that drive's *current* directory, which does not
               exist when the process is running from another drive: stat()
               then fails with ENOENT and _mkdir() with EACCES, and a game on
               a different drive from the process could not create its save
               folder. The drive itself always exists, so skip it. */
            if (i == 2 && tmp[1] == ':') {
                tmp[i] = '/';
                continue;
            }
#endif
            if (!file_io_is_directory(tmp)) {
                if (mkdir_p(tmp) != 0 && errno != EEXIST) return false;
            }
            tmp[i] = '/';
        }
    }

    if (mkdir_p(tmp) != 0 && errno != EEXIST) return false;
    return file_io_is_directory(path);
}

#ifdef _WIN32
char **file_io_readdir(const char *path, size_t *out_count)
{
    if (!path) return NULL;

    /* FindFirstFile wants a "<path>\*" pattern. */
    size_t plen = strlen(path);
    if (plen == 0 || plen + 3 >= MAX_PATH_LEN) return NULL;
    char pattern[MAX_PATH_LEN];
    memcpy(pattern, path, plen);
    if (pattern[plen - 1] != '/' && pattern[plen - 1] != '\\')
        pattern[plen++] = '\\';
    pattern[plen++] = '*';
    pattern[plen] = '\0';

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    size_t count = 0, cap = 32;
    char **list = calloc(cap + 1, sizeof(char *));
    if (!list) {
        FindClose(h);
        return NULL;
    }

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        if (count == cap) {
            size_t new_cap = cap * 2;
            char **grown = realloc(list, (new_cap + 1) * sizeof(char *));
            if (!grown) goto fail;
            list = grown;
            cap = new_cap;
        }
        list[count] = strdup(fd.cFileName);
        if (!list[count]) goto fail;
        count++;
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    list[count] = NULL;
    if (out_count) *out_count = count;
    return list;

fail:
    for (size_t j = 0; j < count; j++) free(list[j]);
    free(list);
    FindClose(h);
    return NULL;
}
#else
char **file_io_readdir(const char *path, size_t *out_count)
{
    if (!path) return NULL;

    DIR *d = opendir(path);
    if (!d) return NULL;

    /* First pass counts entries so the array can be sized exactly. */
    size_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        count++;
    }

    rewinddir(d);

    char **list = calloc(count + 1, sizeof(char *));
    if (!list) {
        closedir(d);
        return NULL;
    }

    size_t idx = 0;
    while ((ent = readdir(d)) != NULL && idx < count) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        list[idx] = strdup(ent->d_name);
        if (!list[idx]) {
            for (size_t j = 0; j < idx; j++) free(list[j]);
            free(list);
            closedir(d);
            return NULL;
        }
        idx++;
    }
    list[idx] = NULL;
    closedir(d);

    if (out_count) *out_count = idx;
    return list;
}
#endif /* _WIN32 */

void file_io_free_dirlist(char **list)
{
    if (!list) return;
    for (size_t i = 0; list[i] != NULL; i++) {
        free(list[i]);
    }
    free(list);
}

bool file_io_unlink(const char *path)
{
    if (!path) return false;
#ifdef _WIN32
    return _unlink(path) == 0;
#else
    return unlink(path) == 0;
#endif
}

bool file_io_rename(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) return false;
    return rename(old_path, new_path) == 0;
}
