/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "engine/script_loader.h"
#include "io/file_io.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine/error_handler.h"

/* Reject plugin names containing path separators or traversal. */
/* Plugin names are paths under js/plugins/ and may use subfolders
   ("Pocket/Pocket_Mirror_BASE"); only escaping that folder is rejected. */
static bool is_valid_plugin_name(const char *name)
{
    if (!name || !*name || name[0] == '/' || strchr(name, '\\') || strchr(name, ':')) return false;
    return strstr(name, "..") == NULL;
}

/* Plugins that are only a speed-up in a browser and need something this runtime does not have: AudioStreaming
   decodes music with a WebAssembly build of stb_vorbis inside a Web Worker, so no song would ever play. Without it
   the game loads its music the normal way, which works. */
static bool is_skipped_plugin(const char *name)
{
    static const char *const skipped[] = { "AudioStreaming", NULL };
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    for (int i = 0; skipped[i]; i++)
        if (strcmp(base, skipped[i]) == 0) return true;
    return false;
}

/* Join dir + file with exactly one separator. Returns a malloc'd string. */
static char *join_path(const char *dir, const char *file)
{
    if (!dir || !file) return NULL;
    size_t dlen = strlen(dir);
    size_t flen = strlen(file);
    bool has_sep = (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\'));
    size_t total = dlen + flen + (has_sep ? 0 : 1) + 1;
    char *out = malloc(total);
    if (!out) return NULL;
    if (has_sep) {
        snprintf(out, total, "%s%s", dir, file);
    } else {
        snprintf(out, total, "%s/%s", dir, file);
    }
    return out;
}

/* Plugin parser */

static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* Copy the quoted string at *pp (which must point at the opening quote) and
   advance *pp past the closing quote. Returns a malloc'd string. */
static char *extract_quoted_string(const char **pp)
{
    const char *p = *pp;
    if (*p != '"' && *p != '\'') return NULL;
    char quote = *p;
    p++;
    const char *start = p;
    while (*p && *p != quote) {
        if (*p == '\\' && *(p + 1)) p++;
        p++;
    }
    size_t len = (size_t)(p - start);
    char *result = malloc(len + 1);
    if (result) {
        memcpy(result, start, len);
        result[len] = '\0';
    }
    if (*p == quote) p++;
    *pp = p;
    return result;
}

PluginList *script_loader_parse_plugins(const char *plugins_js_path)
{
    if (!plugins_js_path) return NULL;

    size_t file_size = 0;
    char *content = file_io_read_text(plugins_js_path, &file_size);
    if (!content) {
        fprintf(stderr, "script_loader: failed to read plugins file: %s\n", plugins_js_path);
        return NULL;
    }

    PluginList *list = calloc(1, sizeof(PluginList));
    if (!list) {
        free(content);
        return NULL;
    }

    size_t capacity = 64;
    list->entries = malloc(sizeof(PluginEntry) * capacity);
    if (!list->entries) {
        free(list);
        free(content);
        return NULL;
    }
    list->count = 0;

    /* Scan for each {name:"...", status:..., ...} object with a small
       hand-rolled scanner rather than a full JS parser. */
    const char *p = content;
    while (*p) {
        const char *brace = strchr(p, '{');
        if (!brace) break;
        p = brace + 1;

        char *name = NULL;
        bool status = false;
        bool found_name = false;
        bool found_status = false;

        while (*p && *p != '}') {
            p = skip_ws(p);
            if (*p == '}') break;

            /* Key may be quoted or bare. */
            const char *key_start = p;
            char *key = NULL;
            if (*p == '"' || *p == '\'') {
                key = extract_quoted_string(&p);
            } else {
                while (*p && *p != ':' && *p != ',' && *p != '}' && !isspace((unsigned char)*p)) p++;
                size_t klen = (size_t)(p - key_start);
                key = malloc(klen + 1);
                if (key) {
                    memcpy(key, key_start, klen);
                    key[klen] = '\0';
                }
            }

            p = skip_ws(p);
            if (*p == ':') p++;
            p = skip_ws(p);

            if (key && strcmp(key, "name") == 0) {
                name = extract_quoted_string(&p);
                found_name = (name != NULL);
            } else if (key && strcmp(key, "status") == 0) {
                if (strncmp(p, "true", 4) == 0) {
                    status = true;
                    p += 4;
                } else if (strncmp(p, "false", 5) == 0) {
                    status = false;
                    p += 5;
                } else {
                    /* Quoted "true"/"false"/"ON" */
                    char *val = extract_quoted_string(&p);
                    if (val) {
                        status = (strcmp(val, "true") == 0 || strcmp(val, "ON") == 0);
                        free(val);
                    }
                }
                found_status = true;
            } else {
                /* Skip any other value; strings inside arrays/objects are consumed
                   whole so brackets within them are not miscounted. */
                if (*p == '"' || *p == '\'') {
                    char *val = extract_quoted_string(&p);
                    free(val);
                } else if (*p == '[') {
                    int depth = 1;
                    p++;
                    while (*p && depth > 0) {
                        if (*p == '"' || *p == '\'') {
                            char *s = extract_quoted_string(&p);
                            free(s);
                        } else {
                            if (*p == '[') depth++;
                            else if (*p == ']') depth--;
                            p++;
                        }
                    }
                } else if (*p == '{') {
                    int depth = 1;
                    p++;
                    while (*p && depth > 0) {
                        if (*p == '"' || *p == '\'') {
                            char *s = extract_quoted_string(&p);
                            free(s);
                        } else {
                            if (*p == '{') depth++;
                            else if (*p == '}') depth--;
                            p++;
                        }
                    }
                } else {
                    while (*p && *p != ',' && *p != '}') p++;
                }
            }

            free(key);

            p = skip_ws(p);
            if (*p == ',') p++;
        }

        if (*p == '}') p++;

        if (found_name && name) {
            if (list->count >= capacity) {
                capacity *= 2;
                PluginEntry *tmp = realloc(list->entries, sizeof(PluginEntry) * capacity);
                if (!tmp) {
                    free(name);
                    break;
                }
                list->entries = tmp;
            }
            list->entries[list->count].name = name;
            list->entries[list->count].status = found_status ? status : true;
            list->count++;
        } else {
            free(name);
        }
    }

    free(content);
    return list;
}

void script_loader_free_plugins(PluginList *list)
{
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) {
        free(list->entries[i].name);
    }
    free(list->entries);
    free(list);
}

/* Tell the DOM shim a game script (path relative to the game directory)
   has been evaluated, so a <script src> the game inserts for the same file
   later is not run a second time (browsers never run a script twice
   either; FOSSIL and similar loaders re-request the core scripts). */
static void note_script_loaded(JSEngine *engine, const char *rel_path)
{
    char code[1024];
    snprintf(code, sizeof(code),
             "if (typeof __dom_noteScriptLoaded === 'function') __dom_noteScriptLoaded('%s');",
             rel_path);
    js_engine_eval(engine, code, "<note-script-loaded>");
}

/* Shim files loaded before any game scripts. Order matters. */
static const char *SHIM_FILES[] = {
    "dom_shim.js",
    "canvas2d_shim.js",
    "font_shim.js",
    "navigator_shim.js",
    "xhr_shim.js",
    "storage_shim.js",
    "pixi_shim.js",
    "webaudio_shim.js",
    "effekseer_shim.js",
    "dom_overlay.js",
    "plugin_compat.js",
    "node_shim.js",
    NULL
};

bool script_loader_load_shims(JSEngine *engine, const char *shim_dir)
{
    if (!engine || !shim_dir) return false;

    for (int i = 0; SHIM_FILES[i] != NULL; i++) {
        char *path = join_path(shim_dir, SHIM_FILES[i]);
        if (!path) return false;

        bool ok = js_engine_eval_file(engine, path);
        if (!ok) {
            fprintf(stderr, "script_loader: failed to load shim: %s\n", path);
            free(path);
            return false;
        }
        js_engine_execute_pending_jobs(engine);
        free(path);
    }
    return true;
}

/* RPG Maker MZ core scripts in load order. */
static const char *RMMZ_CORE_SCRIPTS[] = {
    "js/rmmz_core.js",
    "js/rmmz_managers.js",
    "js/rmmz_objects.js",
    "js/rmmz_scenes.js",
    "js/rmmz_sprites.js",
    "js/rmmz_windows.js",
    NULL
};

/* RPG Maker MV core scripts in load order (MV games ship js/rpg_*.js instead). */
static const char *RPGMV_CORE_SCRIPTS[] = {
    "js/rpg_core.js",
    "js/rpg_managers.js",
    "js/rpg_objects.js",
    "js/rpg_scenes.js",
    "js/rpg_sprites.js",
    "js/rpg_windows.js",
    NULL
};

/* Library scripts of an MV game (PIXI itself is replaced by pixi_shim.js). */
static const char *MV_LIB_SCRIPTS[] = {
    "js/libs/lz-string.js",
    NULL
};

/* True when the game directory holds an RPG Maker MV project rather than an MZ one. */
static bool game_is_mv(const char *game_dir)
{
    char *mv = join_path(game_dir, "js/rpg_core.js");
    char *mz = join_path(game_dir, "js/rmmz_core.js");
    bool result = mv && file_io_exists(mv) && !(mz && file_io_exists(mz));
    free(mv);
    free(mz);
    return result;
}

/* Shims loaded after the core scripts and plugins, to patch RPG Maker
   classes that depend on PIXI internals the runtime does not implement. */
static const char *POST_CORE_SHIMS[] = {
    "tilemap_shim.js",
    "perf_post.js",
    "picture_slots.js",
    NULL
};

const char *const *script_loader_shim_files(void)
{
    static const char *all[32];
    static bool built = false;
    if (!built) {
        int n = 0;
        for (int i = 0; SHIM_FILES[i] && n < 31; i++) all[n++] = SHIM_FILES[i];
        for (int i = 0; POST_CORE_SHIMS[i] && n < 31; i++) all[n++] = POST_CORE_SHIMS[i];
        all[n] = NULL;
        built = true;
    }
    return all;
}

/* Library scripts loaded before core. */
static const char *LIB_SCRIPTS[] = {
    "js/libs/pako.min.js",
    NULL
};

/* RPG Maker MZ's js/main.js declares `class Main` and then starts the game in its last lines ("const main = new Main();
   main.run();"). In a browser the plugins load after that class exists and some of them patch Main.prototype, so the
   class has to be defined before the plugins run, while the starting lines still have to wait until everything is loaded.
   Evaluates the part before the starting lines now and returns the rest (malloc'ed) for later; NULL when main.js has
   another shape (MV's, or a changed one), in which case it is simply evaluated as a whole later. */
static char *preload_main_class(JSEngine *engine, const char *game_dir)
{
    char *main_path = join_path(game_dir, "js/main.js");
    if (!main_path || !file_io_exists(main_path)) {
        free(main_path);
        return NULL;
    }
    size_t size = 0;
    char *text = file_io_read_text(main_path, &size);
    char *tail = NULL;
    if (text && strstr(text, "class Main")) {
        char *start = NULL;
        for (char *q = strstr(text, "new Main("); q; q = strstr(q + 1, "new Main(")) start = q;
        if (start) {
            while (start > text && start[-1] != '\n') start--;
            size_t tail_len = strlen(start);
            /* only a few starting lines may follow the class */
            if (tail_len < 400 && !strstr(start, "class ") && !strstr(start, "function ")) {
                tail = strdup(start);
                *start = '\0';
                js_engine_eval(engine, "__dom_setCurrentScript('js/main.js');", "<set-main-script>");
                if (!js_engine_eval(engine, text, main_path))
                    fprintf(stderr, "script_loader: warning: errors during main.js (class part)\n");
                js_engine_execute_pending_jobs(engine);
                js_engine_eval(engine, "__dom_setCurrentScript(null);", "<clear-main-script>");
            }
        }
    }
    free(text);
    free(main_path);
    return tail;
}

bool script_loader_load_all(JSEngine *engine, const char *shim_dir, const char *game_dir)
{
    if (!engine || !shim_dir || !game_dir) return false;

    /* Step 1: Load shims */
    if (!script_loader_load_shims(engine, shim_dir)) {
        return false;
    }
    printf("script_loader: shims loaded\n");

    const bool mv = game_is_mv(game_dir);
    const char *const *lib_scripts = mv ? MV_LIB_SCRIPTS : LIB_SCRIPTS;
    const char *const *core_scripts = mv ? RPGMV_CORE_SCRIPTS : RMMZ_CORE_SCRIPTS;
    if (mv) {
        printf("script_loader: RPG Maker MV game\n");
        /* PIXI 4 API differences (MV uses PIXI 4, the shim implements PIXI 5) */
        char *compat = join_path(shim_dir, "mv_compat.js");
        if (compat) {
            if (!js_engine_eval_file(engine, compat))
                fprintf(stderr, "script_loader: warning: failed to load mv_compat.js\n");
            js_engine_execute_pending_jobs(engine);
            free(compat);
        }
    }

    /* Step 2: Load library scripts (pako, etc.) */
    for (int i = 0; lib_scripts[i] != NULL; i++) {
        char *path = join_path(game_dir, lib_scripts[i]);
        if (!path) return false;

        if (file_io_exists(path)) {
            bool ok = js_engine_eval_file(engine, path);
            if (!ok) {
                fprintf(stderr, "script_loader: warning: failed to load lib: %s\n", path);
            }
            js_engine_execute_pending_jobs(engine);
            note_script_loaded(engine, lib_scripts[i]);
        } else {
            fprintf(stderr, "script_loader: warning: lib not found: %s\n", path);
        }
        free(path);
    }
    printf("script_loader: libraries loaded\n");

    /* Step 3: Load RPG Maker MZ core scripts */
    for (int i = 0; core_scripts[i] != NULL; i++) {
        char *path = join_path(game_dir, core_scripts[i]);
        if (!path) return false;

        if (file_io_exists(path)) {
            bool ok = js_engine_eval_file(engine, path);
            if (!ok) {
                /* Non-fatal: core scripts may reference PIXI features the shim lacks. */
                fprintf(stderr, "script_loader: note: errors during %s (may be expected for PIXI deps)\n",
                        core_scripts[i]);
            }
            js_engine_execute_pending_jobs(engine);
            note_script_loaded(engine, core_scripts[i]);
        } else {
            fprintf(stderr, "script_loader: warning: core script not found: %s\n", path);
        }
        free(path);
    }
    printf("script_loader: core scripts loaded\n");

    char *main_tail = mv ? NULL : preload_main_class(engine, game_dir);

    /* MV: main.js sets window.onload before the plugin scripts are inserted in a browser; plugins wrap it. */
    if (mv) {
        js_engine_eval(engine,
            "window.__rm_default_onload = function() { SceneManager.run(Scene_Boot); };"
            "if (typeof window.onload !== 'function') window.onload = window.__rm_default_onload;",
            "<mv-onload-default>");
    }

    /* Step 4: Load enabled plugins */
    char *plugins_path = join_path(game_dir, "js/plugins.js");
    if (plugins_path && file_io_exists(plugins_path)) {
        /* Evaluating plugins.js defines $plugins. */
        js_engine_eval_file(engine, plugins_path);
        js_engine_execute_pending_jobs(engine);
        note_script_loaded(engine, "js/plugins.js");

        /* Register every plugin's parameters and name before evaluating any
           plugin script. In a browser PluginManager.setup() does both for all
           plugins before any injected script tag runs, and plugins rely on it:
           they call PluginManager.parameters() at eval time and probe
           PluginManager._scripts to detect sibling plugins. Mirrors setup()
           minus loadScript(), so the later setup() fallback is skipped. */
        js_engine_eval(engine,
            "if (typeof PluginManager !== 'undefined' && typeof $plugins !== 'undefined') {"
            "    for (var _i = 0; _i < $plugins.length; _i++) {"
            "        var _p = $plugins[_i];"
            "        var _n = (typeof Utils !== 'undefined' && Utils.extractFileName)"
            "                 ? Utils.extractFileName(_p.name) : _p.name;"
            "        if (_p.status && !PluginManager._scripts.includes(_n)) {"
            "            PluginManager.setParameters(_n, _p.parameters);"
            "            PluginManager._scripts.push(_n);"
            "        }"
            "    }"
            "}",
            "<plugin-params-setup>");
        js_engine_execute_pending_jobs(engine);

        PluginList *plugins = script_loader_parse_plugins(plugins_path);
        if (plugins) {
            for (size_t i = 0; i < plugins->count; i++) {
                if (!plugins->entries[i].status) continue;

                const char *pname = plugins->entries[i].name;
                /* PluginManager.setup() loads a plugin name once, even when plugins.js lists it twice: running a
                   plugin twice makes plugins that keep their alias in a shared property call themselves forever. */
                bool listed_before = false;
                for (size_t j = 0; j < i; j++) {
                    if (plugins->entries[j].status && strcmp(plugins->entries[j].name, pname) == 0) { listed_before = true; break; }
                }
                if (listed_before) {
                    printf("script_loader: plugin %s is listed again, not run twice\n", pname);
                    continue;
                }
                if (!is_valid_plugin_name(pname)) {
                    fprintf(stderr, "Skipping plugin with invalid name: %s\n", pname);
                    continue;
                }
                if (is_skipped_plugin(pname)) {
                    printf("script_loader: plugin %s not run (needs WebAssembly workers; normal audio is used)\n", pname);
                    continue;
                }

                char plugin_file[512];
                snprintf(plugin_file, sizeof(plugin_file),
                         "js/plugins/%s.js", pname);

                char *path = join_path(game_dir, plugin_file);
                if (!path) continue;

                if (file_io_exists(path)) {
                    /* Plugins read document.currentScript.src to find their own
                       name, so set it before eval. The name is escaped because it
                       is spliced into a JS string literal. */
                    char safe_name[512];
                    size_t si = 0;
                    const char *pn = plugins->entries[i].name;
                    for (size_t j = 0; pn[j] && si < sizeof(safe_name) - 2; j++) {
                        char ch = pn[j];
                        if (ch == '\'' || ch == '\\') {
                            safe_name[si++] = '\\';
                            safe_name[si++] = ch;
                        } else if (ch == '\n') {
                            safe_name[si++] = '\\';
                            safe_name[si++] = 'n';
                        } else if (ch == '\r') {
                            safe_name[si++] = '\\';
                            safe_name[si++] = 'r';
                        } else if ((unsigned char)ch < 0x20) {
                            /* drop other control characters */
                        } else {
                            safe_name[si++] = ch;
                        }
                    }
                    safe_name[si] = '\0';

                    char set_script[1024];
                    snprintf(set_script, sizeof(set_script),
                             "__dom_setCurrentScript('js/plugins/%s.js');",
                             safe_name);
                    js_engine_eval(engine, set_script, "<set-current-script>");

                    /* Logged before eval so a native crash inside a plugin
                       leaves its name as the last line of the log. */
                    printf("script_loader: plugin %s\n", plugins->entries[i].name);
                    bool ok = deobf_eval_plugin_file(engine, path, shim_dir);
                    if (!ok) {
                        compat_report("plugin", plugins->entries[i].name, "failed to load: its script threw "
                                      "(the exception is in the lines above)");
                        fprintf(stderr, "script_loader: warning: plugin error: %s\n",
                                plugins->entries[i].name);
                    }
                    js_engine_execute_pending_jobs(engine);

                    js_engine_eval(engine, "__dom_setCurrentScript(null);",
                                   "<clear-current-script>");
                } else {
                    compat_report("plugin", path, "listed in plugins.js but the file does not exist");
                    fprintf(stderr, "script_loader: warning: plugin not found: %s\n", path);
                }
                free(path);
            }
            printf("script_loader: %zu plugins processed\n", plugins->count);
            script_loader_free_plugins(plugins);
        }
    }
    free(plugins_path);

    /* Step 4b: Post-core shims go after plugins so they win over plugin
       patches to the same engine classes (e.g. Tilemap.Layer.prototype.render). */
    for (int i = 0; POST_CORE_SHIMS[i] != NULL; i++) {
        if (mv) break;   /* tilemap_shim.js replaces the MZ tilemap; MV uses its own canvas tilemap */
        char *path = join_path(shim_dir, POST_CORE_SHIMS[i]);
        if (!path) return false;

        bool ok = js_engine_eval_file(engine, path);
        if (!ok) {
            fprintf(stderr, "script_loader: warning: failed to load post-core shim: %s\n", path);
        }
        js_engine_execute_pending_jobs(engine);
        free(path);
    }
    if (mv) {
        char *post = join_path(shim_dir, "mv_post.js");
        if (post) {
            if (file_io_exists(post)) js_engine_eval_file(engine, post);
            js_engine_execute_pending_jobs(engine);
            free(post);
        }
        /* picture slots on demand applies to MV as well (the MZ loop above handles MZ) */
        char *slots = join_path(shim_dir, "picture_slots.js");
        if (slots) {
            if (file_io_exists(slots)) js_engine_eval_file(engine, slots);
            js_engine_execute_pending_jobs(engine);
            free(slots);
        }
    }
    printf("script_loader: post-core shims loaded\n");

    /* Step 5: Load main.js (for MZ the class part ran before the plugins, only the starting lines are left) */
    char *main_path = join_path(game_dir, "js/main.js");
    if (mv) js_engine_eval(engine, "window.__rm_onload_chain = window.onload;", "<mv-onload-chain>");
    if (main_path && file_io_exists(main_path)) {
        js_engine_eval(engine, "__dom_setCurrentScript('js/main.js');",
                       "<set-main-script>");
        bool ok = main_tail ? js_engine_eval(engine, main_tail, main_path) : js_engine_eval_file(engine, main_path);
        if (!ok) {
            fprintf(stderr, "script_loader: warning: errors during main.js\n");
        }
        js_engine_execute_pending_jobs(engine);
        js_engine_eval(engine, "__dom_setCurrentScript(null);",
                       "<clear-main-script>");
        printf("script_loader: main.js loaded\n");
    }
    free(main_path);
    free(main_tail);
    if (mv) {
        js_engine_eval(engine,
            "(function() {"
            "  var chain = window.__rm_onload_chain, cur = window.onload;"
            "  if (typeof chain === 'function' && chain !== cur && typeof cur === 'function' &&"
            "      /^function\\(\\)\\{SceneManager\\.run\\(Scene_Boot\\);?\\}$/.test(String(cur).replace(/\\s+/g, ''))) {"
            "    window.onload = chain;"
            "  }"
            "})();",
            "<mv-onload-restore>");
    }

    /* Step 6: Start the game. main.js waits for DOM script-load callbacks that
       never fire here (scripts are pre-loaded), so run PluginManager.setup and
       SceneManager.run ourselves unless main.js already did. */
    js_engine_eval(engine,
        "if (typeof PluginManager !== 'undefined' && typeof $plugins !== 'undefined'"
        "    && !PluginManager._scripts.length) {"
        "    PluginManager.setup($plugins);"
        "}",
        "<plugin-setup>");
    js_engine_execute_pending_jobs(engine);

    if (mv) {
        /* the browser's load event: runs the plugins' wrappers, then the game's own start */
        js_engine_eval(engine,
            "if (typeof SceneManager !== 'undefined' && !SceneManager._scene && !SceneManager._nextScene"
            "    && typeof window.onload === 'function') {"
            "    try { window.onload(); } catch (e) { console.error('window.onload: ' + (e && e.stack || e)); }"
            "}",
            "<mv-window-onload>");
        js_engine_execute_pending_jobs(engine);
    }

    js_engine_eval(engine,
        "if (typeof SceneManager !== 'undefined'"
        "    && !SceneManager._scene && !SceneManager._nextScene) {"
        "    if (typeof effekseer !== 'undefined') {"
        "        effekseer.initRuntime('', function() {"
        "            SceneManager.run(Scene_Boot);"
        "        });"
        "    } else {"
        "        SceneManager.run(Scene_Boot);"
        "    }"
        "}",
        "<game-start>");
    js_engine_execute_pending_jobs(engine);

    return true;
}

/* Load order query (for testing) */

static bool append_path(char ***list, size_t *count, size_t *cap, const char *path)
{
    if (*count >= *cap) {
        *cap = (*cap == 0) ? 64 : *cap * 2;
        char **tmp = realloc(*list, sizeof(char *) * (*cap));
        if (!tmp) return false;
        *list = tmp;
    }
    (*list)[*count] = strdup(path);
    if (!(*list)[*count]) return false;
    (*count)++;
    return true;
}

char **script_loader_get_load_order(const char *shim_dir, const char *game_dir, size_t *out_count)
{
    if (!shim_dir || !game_dir) return NULL;

    char **list = NULL;
    size_t count = 0;
    size_t cap = 0;

    /* 1. Shims */
    for (int i = 0; SHIM_FILES[i] != NULL; i++) {
        char *path = join_path(shim_dir, SHIM_FILES[i]);
        if (path) {
            append_path(&list, &count, &cap, path);
            free(path);
        }
    }

    /* 2. Libs */
    for (int i = 0; LIB_SCRIPTS[i] != NULL; i++) {
        char *path = join_path(game_dir, LIB_SCRIPTS[i]);
        if (path) {
            append_path(&list, &count, &cap, path);
            free(path);
        }
    }

    /* 3. Core scripts */
    for (int i = 0; RMMZ_CORE_SCRIPTS[i] != NULL; i++) {
        char *path = join_path(game_dir, RMMZ_CORE_SCRIPTS[i]);
        if (path) {
            append_path(&list, &count, &cap, path);
            free(path);
        }
    }

    /* 3b. Post-core shims */
    for (int i = 0; POST_CORE_SHIMS[i] != NULL; i++) {
        char *path = join_path(shim_dir, POST_CORE_SHIMS[i]);
        if (path) {
            append_path(&list, &count, &cap, path);
            free(path);
        }
    }

    /* 4. Plugins (from plugins.js if it exists) */
    char *plugins_path = join_path(game_dir, "js/plugins.js");
    if (plugins_path && file_io_exists(plugins_path)) {
        PluginList *plugins = script_loader_parse_plugins(plugins_path);
        if (plugins) {
            for (size_t i = 0; i < plugins->count; i++) {
                if (!plugins->entries[i].status) continue;
                if (!is_valid_plugin_name(plugins->entries[i].name)) continue;
                char plugin_file[512];
                snprintf(plugin_file, sizeof(plugin_file),
                         "js/plugins/%s.js", plugins->entries[i].name);
                char *path = join_path(game_dir, plugin_file);
                if (path) {
                    append_path(&list, &count, &cap, path);
                    free(path);
                }
            }
            script_loader_free_plugins(plugins);
        }
    }
    free(plugins_path);

    /* 5. main.js */
    {
        char *path = join_path(game_dir, "js/main.js");
        if (path) {
            append_path(&list, &count, &cap, path);
            free(path);
        }
    }

    if (out_count) *out_count = count;
    return list;
}

void script_loader_free_path_list(char **list, size_t count)
{
    if (!list) return;
    for (size_t i = 0; i < count; i++) {
        free(list[i]);
    }
    free(list);
}
