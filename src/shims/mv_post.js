/*
 * RPG Maker MV support, run after the MV core scripts and the game's plugins.
 *
 * A browser reads index.html before any script runs; the runtime does not, so what the page contains
 * besides script tags is picked up here: style sheets with @font-face rules (MV keeps its GameFont in
 * fonts/gamefont.css).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
(function() {
    "use strict";

    function readText(path) {
        try {
            if (typeof __native_io === "undefined" || !__native_io.existsSync(path)) return null;
            return __native_io.readFileSync(path);
        } catch (e) {
            return null;
        }
    }

    /* "fonts/" + "../img/x.ttf" -> "img/x.ttf" */
    function joinPath(dir, rel) {
        if (/^[a-z]+:/i.test(rel) || rel.charAt(0) === "/") return rel;
        var parts = (dir + rel).split("/");
        var out = [];
        for (var i = 0; i < parts.length; i++) {
            if (parts[i] === "..") out.pop();
            else if (parts[i] !== "." && parts[i] !== "") out.push(parts[i]);
        }
        return out.join("/");
    }

    function processCss(css, dir) {
        if (!css || typeof FontFace === "undefined" || !document.fonts) return;
        var re = /@font-face\s*\{([^}]*)\}/gi;
        var m;
        while ((m = re.exec(css)) !== null) {
            var fam = /font-family\s*:\s*["']?([^"';]+?)["']?\s*;/i.exec(m[1] + ";");
            var src = /url\(\s*["']?([^"')]+)["']?\s*\)/i.exec(m[1]);
            if (!fam || !src) continue;
            var path = joinPath(dir, decodeURI(src[1]));
            document.fonts.add(new FontFace(fam[1], "url(" + path + ")"));
        }
    }

    /* QuickJS' error.stack does not start with the message, so MV's own error report (console.error(e.stack))
       would leave it out; log it first. */
    if (typeof SceneManager !== "undefined" && SceneManager.catchException) {
        var realCatch = SceneManager.catchException;
        SceneManager.catchException = function(e) {
            try { console.error(String(e && e.name) + ": " + String(e && e.message)); } catch (ignore) { /* ignore */ }
            return realCatch.apply(this, arguments);
        };
    }

    /* MV cores built for PIXI 5 (Apostle Rebellion) make their renderer themselves with `new PIXI.Renderer(...)`, which says
       "WebGL" and so selects ShaderTilemap (pixi-tilemap). MV is run in its canvas mode here (CanvasTilemap, canvas
       sprite paths), as for PIXI 4 cores, see mv_compat.js. */
    if (typeof Graphics !== "undefined" && typeof Graphics._createRenderer === "function" && PIXI.RENDERER_TYPE) {
        var realCreateRenderer = Graphics._createRenderer;
        Graphics._createRenderer = function() {
            var result = realCreateRenderer.apply(this, arguments);
            if (this._renderer && this._renderer.type !== PIXI.RENDERER_TYPE.CANVAS) {
                this._renderer.type = PIXI.RENDERER_TYPE.CANVAS;
            }
            return result;
        };
    }

    /* The canvas mode above makes Graphics.isWebGL() false, and MV then tints the screen (Tint Screen, command 223: every
       fade to black inside an event) with a ToneSprite that draws through the canvas renderer's 2D context, which does not
       exist here: the tint never appeared. The tone changer is made with the WebGL variant instead, a ToneFilter on the
       spriteset's base sprite, which Container.render applies with the colour-matrix filter. Only these two methods see
       isWebGL() as true, so a plugin's own override of them still runs. */
    if (typeof Spriteset_Base !== "undefined" && typeof ToneFilter !== "undefined" &&
        typeof Graphics !== "undefined" && typeof Graphics.isWebGL === "function") {
        var asWebGL = function(fn) {
            return function() {
                var saved = Graphics.isWebGL;
                Graphics.isWebGL = function() { return true; };
                try { return fn.apply(this, arguments); } finally { Graphics.isWebGL = saved; }
            };
        };
        var sp = Spriteset_Base.prototype;
        if (typeof sp.createToneChanger === "function") sp.createToneChanger = asWebGL(sp.createToneChanger);
        if (typeof sp.updateToneChanger === "function") sp.updateToneChanger = asWebGL(sp.updateToneChanger);
    }

    var html = readText("index.html");
    if (!html) return;

    var linkRe = /<link\b[^>]*>/gi;
    var m;
    while ((m = linkRe.exec(html)) !== null) {
        var tag = m[0];
        if (!/rel\s*=\s*["']?stylesheet/i.test(tag)) continue;
        var href = /href\s*=\s*["']([^"']+)["']/i.exec(tag);
        if (!href) continue;
        processCss(readText(href[1]), href[1].replace(/[^\/]*$/, ""));
    }
    var styleRe = /<style\b[^>]*>([\s\S]*?)<\/style>/gi;
    while ((m = styleRe.exec(html)) !== null) processCss(m[1], "");
})();

/* Bitmap.adjustTone (RPG Maker MV core): the window background's colour tone is applied by reading the whole bitmap
 * with getImageData, adding to every channel in a JavaScript loop and writing it back. In this interpreter that loop
 * costs a few hundred milliseconds for a message-window-sized bitmap, every time a window refreshes its back sprite.
 * The canvas keeps straight RGBA bytes, so the same addition with clamping is done natively. Only the core's own
 * function is replaced (fingerprint of its text, as perf_post.js does for the MZ core): a plugin that wrapped or
 * rewrote it keeps its version. */
(function() {
    "use strict";
    try {
        if (typeof Bitmap === "undefined" || typeof __native_canvas2d === "undefined" ||
            typeof __native_canvas2d.adjustTone !== "function") return;
        var text = Function.prototype.toString.call(Bitmap.prototype.adjustTone).replace(/\s+/g, "");
        var h = 0x811c9dc5;
        for (var i = 0; i < text.length; i++) {
            h ^= text.charCodeAt(i);
            h = Math.imul(h, 0x01000193) >>> 0;
        }
        if (h !== 0x4f79c9e6) return;
        var original = Bitmap.prototype.adjustTone;
        Bitmap.prototype.adjustTone = function(r, g, b) {
            var context = this._context;
            if ((r || g || b) && this.width > 0 && this.height > 0 && context && context._handle) {
                __native_canvas2d.adjustTone(context._handle, r, g, b);
                this._setDirty();
                return;
            }
            return original.call(this, r, g, b);
        };
        globalThis.__mvPostApplied = (globalThis.__mvPostApplied || []).concat("Bitmap.adjustTone");
    } catch (e) {}
})();
