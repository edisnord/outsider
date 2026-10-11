/*
 * RPG Maker MV support: MV games are written against PIXI 4, while pixi_shim.js implements the PIXI 5 API that
 * RPG Maker MZ uses. This file (loaded before MV's rpg_core.js) adds the PIXI 4 names and call forms that MV
 * needs. MV is run in its "canvas" renderer mode: its own canvas tilemap and sprite paths are used, which is
 * what the native canvas and sprite renderer here handle.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* TypeScript's class helper. In MV it is global because pixi-tilemap.js and pixi-picture.js, which are not
 * loaded here, define it at the top level; plugins compiled from TypeScript (UltraMode7, ...) call it without
 * defining it themselves. */
if (typeof globalThis.__extends !== "function") {
    globalThis.__extends = function(d, b) {
        for (var p in b) {
            if (Object.prototype.hasOwnProperty.call(b, p)) d[p] = b[p];
        }
        function __() { this.constructor = d; }
        d.prototype = b === null ? Object.create(b) : (__.prototype = b.prototype, new __());
    };
}
(function() {
    "use strict";

    var PIXI = globalThis.PIXI;
    if (!PIXI) return;

    /* --- names that moved between PIXI 4 and 5 --- */
    PIXI.blendModes = PIXI.blendModes || PIXI.BLEND_MODES;
    PIXI.RENDERER_TYPE = { UNKNOWN: 0, WEBGL: 1, CANVAS: 2 };
    PIXI.GC_MODES = { AUTO: 0, MANUAL: 1 };
    PIXI.glCore = PIXI.glCore || { VertexArrayObject: {} };
    PIXI.dontSayHello = true;

    PIXI.extras = PIXI.extras || {};
    PIXI.extras.TilingSprite = PIXI.TilingSprite;

    /* pixi-picture's PictureTilingSprite: a tiling sprite with extra blend modes; MV subclasses it. */
    function PictureTilingSprite(texture, width, height) {
        PIXI.TilingSprite.call(this, texture, width, height);
    }
    PictureTilingSprite.prototype = Object.create(PIXI.TilingSprite.prototype);
    PictureTilingSprite.prototype.constructor = PictureTilingSprite;
    PictureTilingSprite.prototype._renderWebGL = function() {};
    PictureTilingSprite.prototype._renderCanvas = function() {};
    PIXI.extras.PictureTilingSprite = PictureTilingSprite;

    /* WebGL-only tilemap classes of pixi-tilemap.js (MV runs the canvas tilemap here) */
    PIXI.tilemap = PIXI.tilemap || {
        TileRenderer: {},
        ZLayer: function() {},
        CompositeRectTileLayer: function() {},
    };
    /* Pieces some plugins read at load time (mode-7 / perspective map plugins patch the WebGL tile layer); nothing
       here draws, MV runs its canvas tilemap. */
    ["RectTileLayer", "RectTileShader", "TilemapShader", "GraphicsLayer"].forEach(function(n) {
        if (!PIXI.tilemap[n]) PIXI.tilemap[n] = function() {};
    });
    PIXI.tilemap.Constant = PIXI.tilemap.Constant || { maxTextures: 16, bufferSize: 2048, boundSize: 1024, boundCountPerBuffer: 4,
                                                       SCALE_MODE: 0, DO_CLEAR: false, use32bitIndex: false };
    PIXI.tilemap.shaderGenerator = PIXI.tilemap.shaderGenerator || {};

    /* the WebGL paths that MV wraps (Sprite._renderWebGL_PIXI = PIXI.Sprite.prototype._renderWebGL, ...) */
    ["Sprite", "Container", "TilingSprite", "Graphics"].forEach(function(name) {
        var C = PIXI[name];
        if (!C) return;
        if (!C.prototype._renderWebGL) C.prototype._renderWebGL = function() {};
        if (!C.prototype._renderCanvas) C.prototype._renderCanvas = function() {};
        if (!C.prototype.renderWebGL) C.prototype.renderWebGL = function() {};
        if (!C.prototype.renderCanvas) C.prototype.renderCanvas = function() {};
    });

    /* PIXI 4's Graphics keeps the shapes it was told to draw in graphicsData (WindowLayer reads its mask rect) */
    if (PIXI.Graphics && PIXI.Graphics.prototype.drawRect) {
        var realDrawRect = PIXI.Graphics.prototype.drawRect;
        PIXI.Graphics.prototype.drawRect = function(x, y, w, h) {
            if (!this.graphicsData) this.graphicsData = [];
            this.graphicsData.push({ shape: new PIXI.Rectangle(x, y, w, h), type: 1 });
            return realDrawRect.apply(this, arguments);
        };
    }

    /* MV cores built for PIXI 5 (Apostle Rebellion) read the mask rectangle as graphics.geometry.graphicsData[0].shape */
    if (PIXI.Graphics && !("geometry" in PIXI.Graphics.prototype)) {
        Object.defineProperty(PIXI.Graphics.prototype, "geometry", {
            get: function() {
                var owner = this;
                if (!this._rmGeometry) {
                    this._rmGeometry = {
                        get graphicsData() { return owner.graphicsData || (owner.graphicsData = []); },
                        clear: function() { owner.graphicsData = []; },
                        invalidate: function() {},
                        dispose: function() {}
                    };
                }
                return this._rmGeometry;
            },
            configurable: true
        });
    }

    /* --- names from PIXI 3 and 4 that plugins still use --- */
    PIXI.GC_MODES.DEFAULT = PIXI.GC_MODES.AUTO;
    if (PIXI.SCALE_MODES && PIXI.SCALE_MODES.DEFAULT === undefined) PIXI.SCALE_MODES.DEFAULT = PIXI.SCALE_MODES.LINEAR;
    if (PIXI.WRAP_MODES && PIXI.WRAP_MODES.DEFAULT === undefined) PIXI.WRAP_MODES.DEFAULT = PIXI.WRAP_MODES.CLAMP;
    PIXI.DisplayObjectContainer = PIXI.Container;
    if (!PIXI.ticker) {
        PIXI.ticker = { Ticker: PIXI.Ticker };
        Object.defineProperty(PIXI.ticker, "shared", { get: function() { return PIXI.Ticker.shared; } });
    }
    PIXI.loaders = PIXI.loaders || { Loader: PIXI.Loader, Resource: PIXI.LoaderResource };
    if (PIXI.Texture && PIXI.utils) {
        PIXI.Texture.fromImage = function(url, crossorigin, scaleMode) {
            return PIXI.Texture.from(url, scaleMode === undefined ? undefined : { scaleMode: scaleMode });
        };
        PIXI.Texture.fromVideo = function(url) { return PIXI.Texture.from(url); };
        PIXI.Texture.fromFrame = function(id) {
            var t = PIXI.utils.TextureCache[id];
            if (!t) throw new Error('The frameId "' + id + '" does not exist in the texture cache');
            return t;
        };
        PIXI.Texture.removeTextureFromCache = function(id) {
            var t = PIXI.utils.TextureCache[id];
            delete PIXI.utils.TextureCache[id];
            return t;
        };
        if (PIXI.BaseTexture) PIXI.BaseTexture.fromImage = function(url, crossorigin, scaleMode) {
            return PIXI.BaseTexture.from(url, scaleMode === undefined ? undefined : { scaleMode: scaleMode });
        };
        ["TextureCache", "BaseTextureCache"].forEach(function(n) {
            if (!(n in PIXI)) Object.defineProperty(PIXI, n, { get: function() { return PIXI.utils[n]; } });
        });
    }

    /* --- filters --- */
    PIXI.filters = PIXI.filters || {};
    if (!PIXI.filters.VoidFilter) {
        /* A filter that does nothing (MV uses it to force an extra render pass); disabled so it costs nothing. */
        var VoidFilter = function VoidFilter() {
            PIXI.Filter.call(this);
            this.enabled = false;
        };
        VoidFilter.prototype = Object.create(PIXI.Filter.prototype);
        VoidFilter.prototype.constructor = VoidFilter;
        PIXI.filters.VoidFilter = VoidFilter;
    }

    /* --- small libraries MV's index.html loads --- */
    if (typeof globalThis.makeVideoPlayableInline === "undefined") {
        globalThis.makeVideoPlayableInline = function() {};   /* iphone-inline-video */
    }
    if (typeof globalThis.FPSMeter === "undefined") {
        /* fpsmeter.js draws an on-screen counter; the game only ever shows it with ?showfps, so a stand-in */
        globalThis.FPSMeter = function FPSMeter() { this.isPaused = false; };
        ["tickStart", "tick", "show", "hide", "pause", "resume", "showFps", "hideFps"].forEach(function(m) {
            globalThis.FPSMeter.prototype[m] = function() {};
        });
    }

    /* --- NW.js window API (MV asks for it at start-up and for fullscreen) --- */
    var nwWindow = {
        menu: null, x: 0, y: 0, width: 816, height: 624, title: "", isFullscreen: false,
        on: function() {}, once: function() {}, removeListener: function() {},
        focus: function() {}, blur: function() {}, show: function() {}, hide: function() {}, close: function() {},
        maximize: function() {}, restore: function() {}, minimize: function() {},
        enterFullscreen: function() {}, leaveFullscreen: function() {}, toggleFullscreen: function() {},
        resizeTo: function() {}, resizeBy: function() {}, moveTo: function() {}, setPosition: function() {},
        showDevTools: function() {}, closeDevTools: function() {}, setAlwaysOnTop: function() {},
        window: globalThis
    };
    var nwGui = {
        Window: { get: function() { return nwWindow; }, open: function() { return nwWindow; } },
        Menu: function Menu() { this.createMacBuiltin = function() {}; },
        MenuItem: function MenuItem() {},
        App: { argv: [], dataPath: "", manifest: {}, quit: function() {}, closeAllWindows: function() {} },
        Screen: { Init: function() {}, screens: [] },
        Shell: { openExternal: function() {}, openItem: function() {} }
    };
    if (typeof globalThis.require === "function") {
        var realRequire = globalThis.require;
        globalThis.require = function(name) {
            if (name === "nw.gui") return nwGui;
            return realRequire.apply(this, arguments);
        };
    }

    /* --- CSS fonts: MV loads its fonts with a <style> element and sheet.insertRule("@font-face {...}") --- */
    function registerCssRule(rule) {
        var m = /@font-face\s*\{([^}]*)\}/i.exec(rule);
        if (!m) return;
        var fam = /font-family\s*:\s*["']?([^"';]+?)["']?\s*;/i.exec(m[1]);
        var src = /url\(\s*["']?([^"')]+)["']?\s*\)/i.exec(m[1]);
        if (!fam || !src || typeof FontFace === "undefined") return;
        var face = new FontFace(fam[1], "url(" + decodeURI(src[1]) + ")");
        if (document.fonts && document.fonts.add) document.fonts.add(face);
    }
    if (typeof document !== "undefined" && typeof document.createElement === "function") {
        var realCreateElement = document.createElement;
        document.createElement = function(tag) {
            var el = realCreateElement.apply(this, arguments);
            if (el && typeof tag === "string" && tag.toLowerCase() === "style" && !el.sheet) {
                el.sheet = {
                    cssRules: [],
                    insertRule: function(rule) {
                        registerCssRule(rule);
                        this.cssRules.push({ cssText: rule });
                        el.textContent = (el.textContent || "") + rule;
                        return 0;
                    },
                    deleteRule: function() {}
                };
            }
            return el;
        };
    }

    /* --- renderers: PIXI 4 takes (width, height, options) --- */
    function makeRenderer(width, height, options) {
        var opts = {};
        var k;
        if (options) for (k in options) opts[k] = options[k];
        opts.width = width;
        opts.height = height;
        var r = new PIXI.Renderer(opts);
        r.type = PIXI.RENDERER_TYPE.CANVAS;   /* MV then uses its canvas tilemap and canvas code paths */
        if (!r.plugins) r.plugins = {};
        if (!r.textureGC) r.textureGC = { maxIdle: 1 };
        return r;
    }
    PIXI.CanvasRenderer = function CanvasRenderer(w, h, o) { return makeRenderer(w, h, o); };
    PIXI.WebGLRenderer = function WebGLRenderer(w, h, o) { return makeRenderer(w, h, o); };
    PIXI.autoDetectRenderer = function(w, h, o) { return makeRenderer(w, h, o); };

    /* RenderTexture.create(width, height, scaleMode) (PIXI 4) vs create({width, height}) (PIXI 5) */
    if (PIXI.RenderTexture && PIXI.RenderTexture.create) {
        var origCreate = PIXI.RenderTexture.create;
        PIXI.RenderTexture.create = function(a, b, c) {
            var rt = (typeof a === "number")
                ? origCreate.call(PIXI.RenderTexture, { width: a, height: b, scaleMode: c })
                : origCreate.call(PIXI.RenderTexture, a);
            /* MV's canvas mode reads a render texture back through baseTexture._canvasRenderTarget.canvas
               (Bitmap.snap); the renderer's extract plugin provides the same picture. */
            if (rt && rt.baseTexture && !("_canvasRenderTarget" in rt.baseTexture)) {
                Object.defineProperty(rt.baseTexture, "_canvasRenderTarget", {
                    configurable: true,
                    get: function() {
                        var renderer = globalThis.Graphics && globalThis.Graphics._renderer;
                        return { canvas: renderer && renderer.extract ? renderer.extract.canvas(rt) : null };
                    }
                });
            }
            return rt;
        };
    }
})();
