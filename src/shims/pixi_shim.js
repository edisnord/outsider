/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

(function() {
    "use strict";

    /* PIXI shim: a minimal PIXI.js v5 API (scene graph, textures, filters,
       loader, ticker) for RPG Maker MZ, backed by the native renderer. */

    /* Version of the PIXI release RPG Maker MZ bundles. Libraries embedded in
       plugins (pixi-particles, pixi-filters) parse it to pick API variants. */
    var PIXI = { VERSION: "5.3.12" };

    /* PIXI.SCALE_MODES */

    PIXI.SCALE_MODES = {
        NEAREST: 0,
        LINEAR:  1,
    };

    /* PIXI.BLEND_MODES */

    PIXI.BLEND_MODES = {
        NORMAL:       0,
        ADD:          1,
        MULTIPLY:     2,
        SCREEN:       3,
        OVERLAY:      4,
        DARKEN:       5,
        LIGHTEN:      6,
        COLOR_DODGE:  7,
        COLOR_BURN:   8,
        HARD_LIGHT:   9,
        SOFT_LIGHT:   10,
        DIFFERENCE:   11,
        EXCLUSION:    12,
        HUE:          13,
        SATURATION:   14,
        COLOR:        15,
        LUMINOSITY:   16,
        NORMAL_NPM:   17,
        ADD_NPM:      18,
        SCREEN_NPM:   19,
        NONE:         20,
        SRC_OVER:     0,
        SRC_IN:       21,
        SRC_OUT:      22,
        SRC_ATOP:     23,
        DST_OVER:     24,
        DST_IN:       25,
        DST_OUT:      26,
        DST_ATOP:     27,
        ERASE:        26,
        SUBTRACT:     28,
        XOR:          29,
    };

    /* PIXI.WRAP_MODES */

    PIXI.WRAP_MODES = {
        CLAMP:           33071,
        REPEAT:          10497,
        MIRRORED_REPEAT: 33648,
    };

    /* PIXI.FORMATS */

    PIXI.FORMATS = {
        RGBA:    6408,
        RGB:     6407,
        ALPHA:   6406,
        LUMINANCE: 6409,
        LUMINANCE_ALPHA: 6410,
    };

    /* PIXI.TYPES */

    PIXI.TYPES = {
        UNSIGNED_BYTE:  5121,
        UNSIGNED_SHORT: 5123,
        FLOAT:          5126,
    };

    /* PIXI.settings */

    PIXI.settings = {
        RESOLUTION: 1,
        SCALE_MODE: PIXI.SCALE_MODES.NEAREST,
        ROUND_PIXELS: false,
        TARGET_FPMS: 0.06,
        FILTER_RESOLUTION: 1,
        SPRITE_MAX_TEXTURES: 16,
        RENDER_OPTIONS: {
            view: null,
            resolution: 1,
            antialias: false,
            autoDensity: false,
            transparent: false,
            backgroundColor: 0x000000,
            clearBeforeRender: true,
            preserveDrawingBuffer: false,
            width: 800,
            height: 600,
        },
        GC_MODE: 0,
        GC_MAX_IDLE: 3600,
        GC_MAX_CHECK_COUNT: 600,
        MIPMAP_TEXTURES: 1,
        WRAP_MODE: PIXI.WRAP_MODES.CLAMP,
        CREATE_IMAGE_BITMAP: false,
        PREFER_ENV: 0,
        STRICT_TEXTURE_CACHE: false,
    };

    /* PIXI.utils */

    PIXI.utils = {
        skipHello: function() { /* No-op — suppress PIXI hello message. */ },

        isWebGLSupported: function() { return true; },

        string2hex: function(str) {
            if (typeof str === "string" && str.charAt(0) === "#") {
                str = str.substring(1);
            }
            return parseInt(str, 16) || 0;
        },

        hex2string: function(hex) {
            hex = hex.toString(16);
            while (hex.length < 6) hex = "0" + hex;
            return "#" + hex;
        },

        rgb2hex: function(rgb) {
            return ((rgb[0] * 255) << 16) + ((rgb[1] * 255) << 8) + (rgb[2] * 255 | 0);
        },

        hex2rgb: function(hex, out) {
            out = out || [0, 0, 0];
            out[0] = ((hex >> 16) & 0xFF) / 255;
            out[1] = ((hex >> 8) & 0xFF) / 255;
            out[2] = (hex & 0xFF) / 255;
            return out;
        },

        uid: (function() {
            var nextId = 0;
            return function() { return nextId++; };
        })(),

        sign: function(n) {
            if (n === 0) return 0;
            return n < 0 ? -1 : 1;
        },

        premultiplyTint: function(tint, alpha) {
            if (alpha === 1.0) return (alpha * 255 << 24) + tint;
            if (alpha === 0.0) return 0;
            var r = ((tint >> 16) & 0xFF);
            var g = ((tint >> 8) & 0xFF);
            var b = (tint & 0xFF);
            r = (r * alpha + 0.5) | 0;
            g = (g * alpha + 0.5) | 0;
            b = (b * alpha + 0.5) | 0;
            return (alpha * 255 << 24) + (r << 16) + (g << 8) + b;
        },

        premultiplyTintToRgba: function(tint, alpha, out) {
            out = out || new Float32Array(4);
            out[0] = ((tint >> 16) & 0xFF) / 255.0 * alpha;
            out[1] = ((tint >> 8) & 0xFF) / 255.0 * alpha;
            out[2] = (tint & 0xFF) / 255.0 * alpha;
            out[3] = alpha;
            return out;
        },

        TextureCache: {},
        BaseTextureCache: {},

        destroyTextureCache: function() {
            PIXI.utils.TextureCache = {};
            PIXI.utils.BaseTextureCache = {};
        },

        EventEmitter: null, /* Set below. */

        isMobile: {
            apple: { phone: false, ipod: false, tablet: false, device: false },
            amazon: { phone: false, tablet: false, device: false },
            android: { phone: false, tablet: false, device: false },
            windows: { phone: false, tablet: false, device: false },
            other: { blackberry: false, blackberry10: false, opera: false, firefox: false, chrome: false, device: false },
            any: false,
            phone: false,
            tablet: false,
        },
    };

    /* Simple EventEmitter (used as PIXI.utils.EventEmitter) */

    function EventEmitter() {
        this._listeners = {};
    }

    EventEmitter.prototype.on = function(event, fn, context) {
        if (!this._listeners[event]) this._listeners[event] = [];
        this._listeners[event].push({ fn: fn, context: context || this, once: false });
        return this;
    };

    EventEmitter.prototype.once = function(event, fn, context) {
        if (!this._listeners[event]) this._listeners[event] = [];
        this._listeners[event].push({ fn: fn, context: context || this, once: true });
        return this;
    };

    EventEmitter.prototype.off = function(event, fn, context) {
        if (!event) { this._listeners = {}; return this; }
        var list = this._listeners[event];
        if (!list) return this;
        if (!fn) { delete this._listeners[event]; return this; }
        for (var i = list.length - 1; i >= 0; i--) {
            if (list[i].fn === fn && (!context || list[i].context === context)) {
                list.splice(i, 1);
            }
        }
        return this;
    };

    EventEmitter.prototype.emit = function(event) {
        var list = this._listeners[event];
        if (!list || list.length === 0) return false;
        var args = [];
        for (var i = 1; i < arguments.length; i++) args.push(arguments[i]);
        var toRemove = [];
        for (var j = 0; j < list.length; j++) {
            list[j].fn.apply(list[j].context, args);
            if (list[j].once) toRemove.push(j);
        }
        for (var k = toRemove.length - 1; k >= 0; k--) {
            list.splice(toRemove[k], 1);
        }
        return true;
    };

    EventEmitter.prototype.removeAllListeners = function(event) {
        if (event) {
            delete this._listeners[event];
        } else {
            this._listeners = {};
        }
        return this;
    };

    EventEmitter.prototype.addListener = EventEmitter.prototype.on;
    EventEmitter.prototype.removeListener = EventEmitter.prototype.off;
    EventEmitter.prototype.listeners = function(event) {
        return (this._listeners[event] || []).map(function(l) { return l.fn; });
    };
    EventEmitter.prototype.listenerCount = function(event) {
        return (this._listeners[event] || []).length;
    };

    PIXI.utils.EventEmitter = EventEmitter;

    /* PIXI.Rectangle */

    function Rectangle(x, y, width, height) {
        this.x = x || 0;
        this.y = y || 0;
        this.width = width || 0;
        this.height = height || 0;
        this.type = 1; /* SHAPES.RECT */
    }

    Object.defineProperty(Rectangle.prototype, "left", {
        get: function() { return this.x; }
    });

    Object.defineProperty(Rectangle.prototype, "right", {
        get: function() { return this.x + this.width; }
    });

    Object.defineProperty(Rectangle.prototype, "top", {
        get: function() { return this.y; }
    });

    Object.defineProperty(Rectangle.prototype, "bottom", {
        get: function() { return this.y + this.height; }
    });

    Rectangle.prototype.clone = function() {
        return new Rectangle(this.x, this.y, this.width, this.height);
    };

    Rectangle.prototype.copyFrom = function(rect) {
        this.x = rect.x;
        this.y = rect.y;
        this.width = rect.width;
        this.height = rect.height;
        return this;
    };

    Rectangle.prototype.copyTo = function(rect) {
        rect.x = this.x;
        rect.y = this.y;
        rect.width = this.width;
        rect.height = this.height;
        return rect;
    };

    Rectangle.prototype.contains = function(x, y) {
        if (this.width <= 0 || this.height <= 0) return false;
        return x >= this.x && x < this.x + this.width &&
               y >= this.y && y < this.y + this.height;
    };

    Rectangle.prototype.pad = function(paddingX, paddingY) {
        paddingX = paddingX || 0;
        paddingY = paddingY || ((paddingY !== 0) ? paddingX : 0);
        this.x -= paddingX;
        this.y -= paddingY;
        this.width += paddingX * 2;
        this.height += paddingY * 2;
        return this;
    };

    Rectangle.prototype.fit = function(rectangle) {
        var x1 = Math.max(this.x, rectangle.x);
        var y1 = Math.max(this.y, rectangle.y);
        var x2 = Math.min(this.x + this.width, rectangle.x + rectangle.width);
        var y2 = Math.min(this.y + this.height, rectangle.y + rectangle.height);
        this.x = x1;
        this.y = y1;
        this.width = Math.max(x2 - x1, 0);
        this.height = Math.max(y2 - y1, 0);
        return this;
    };

    Rectangle.prototype.ceil = function(resolution, eps) {
        resolution = resolution || 1;
        eps = eps || 0.001;
        var x2 = Math.ceil((this.x + this.width - eps) * resolution) / resolution;
        var y2 = Math.ceil((this.y + this.height - eps) * resolution) / resolution;
        this.x = Math.floor((this.x + eps) * resolution) / resolution;
        this.y = Math.floor((this.y + eps) * resolution) / resolution;
        this.width = x2 - this.x;
        this.height = y2 - this.y;
        return this;
    };

    Rectangle.prototype.enlarge = function(rectangle) {
        var x1 = Math.min(this.x, rectangle.x);
        var y1 = Math.min(this.y, rectangle.y);
        var x2 = Math.max(this.x + this.width, rectangle.x + rectangle.width);
        var y2 = Math.max(this.y + this.height, rectangle.y + rectangle.height);
        this.x = x1;
        this.y = y1;
        this.width = x2 - x1;
        this.height = y2 - y1;
        return this;
    };

    Rectangle.EMPTY = new Rectangle(0, 0, 0, 0);

    PIXI.Rectangle = Rectangle;

    /* The other shapes of PIXI.SHAPES. Plugins subclass them (CGMZ_Circle extends PIXI.Circle) or build hit areas. */
    PIXI.SHAPES = { POLY: 0, RECT: 1, CIRC: 2, ELIP: 3, RREC: 4 };

    function Circle(x, y, radius) {
        this.x = x || 0;
        this.y = y || 0;
        this.radius = radius || 0;
        this.type = 2;
    }
    Circle.prototype.clone = function() { return new Circle(this.x, this.y, this.radius); };
    Circle.prototype.contains = function(x, y) {
        if (this.radius <= 0) return false;
        var dx = this.x - x, dy = this.y - y;
        return dx * dx + dy * dy <= this.radius * this.radius;
    };
    Circle.prototype.getBounds = function() {
        return new Rectangle(this.x - this.radius, this.y - this.radius, this.radius * 2, this.radius * 2);
    };
    PIXI.Circle = Circle;

    function Ellipse(x, y, halfWidth, halfHeight) {
        this.x = x || 0;
        this.y = y || 0;
        this.width = halfWidth || 0;
        this.height = halfHeight || 0;
        this.type = 3;
    }
    Ellipse.prototype.clone = function() { return new Ellipse(this.x, this.y, this.width, this.height); };
    Ellipse.prototype.contains = function(x, y) {
        if (this.width <= 0 || this.height <= 0) return false;
        var nx = (x - this.x) / this.width, ny = (y - this.y) / this.height;
        return nx * nx + ny * ny <= 1;
    };
    Ellipse.prototype.getBounds = function() {
        return new Rectangle(this.x - this.width, this.y - this.height, this.width * 2, this.height * 2);
    };
    PIXI.Ellipse = Ellipse;

    function Polygon() {
        var points = Array.prototype.slice.call(arguments);
        if (Array.isArray(points[0])) points = points[0];
        if (points.length && typeof points[0] === "object") {
            var flat = [];
            for (var i = 0; i < points.length; i++) flat.push(points[i].x, points[i].y);
            points = flat;
        }
        this.points = points;
        this.closeStroke = true;
        this.type = 0;
    }
    Polygon.prototype.clone = function() {
        var p = new Polygon(this.points.slice());
        p.closeStroke = this.closeStroke;
        return p;
    };
    Polygon.prototype.contains = function(x, y) {
        var inside = false, n = this.points.length / 2;
        for (var i = 0, j = n - 1; i < n; j = i++) {
            var xi = this.points[i * 2], yi = this.points[i * 2 + 1];
            var xj = this.points[j * 2], yj = this.points[j * 2 + 1];
            if (((yi > y) !== (yj > y)) && (x < (xj - xi) * ((y - yi) / (yj - yi)) + xi)) inside = !inside;
        }
        return inside;
    };
    PIXI.Polygon = Polygon;

    function RoundedRectangle(x, y, width, height, radius) {
        this.x = x || 0;
        this.y = y || 0;
        this.width = width || 0;
        this.height = height || 0;
        this.radius = radius === undefined ? 20 : radius;
        this.type = 4;
    }
    RoundedRectangle.prototype.clone = function() {
        return new RoundedRectangle(this.x, this.y, this.width, this.height, this.radius);
    };
    RoundedRectangle.prototype.contains = function(x, y) {
        if (this.width <= 0 || this.height <= 0) return false;
        return x >= this.x && x <= this.x + this.width && y >= this.y && y <= this.y + this.height;
    };
    PIXI.RoundedRectangle = RoundedRectangle;

    /* PIXI.BaseTexture */

    var _baseTextureId = 0;

    function BaseTexture(resource, options) {
        EventEmitter.call(this);

        this._id = _baseTextureId++;
        this.uid = PIXI.utils.uid();

        options = options || {};

        this.width = 0;
        this.height = 0;
        this.resolution = options.resolution || PIXI.settings.RESOLUTION;
        this.mipmap = options.mipmap !== undefined ? options.mipmap : PIXI.settings.MIPMAP_TEXTURES;
        this.wrapMode = options.wrapMode || PIXI.settings.WRAP_MODE;
        this.scaleMode = options.scaleMode !== undefined ? options.scaleMode : PIXI.settings.SCALE_MODE;
        this.format = options.format || PIXI.FORMATS.RGBA;
        this.type = options.type || PIXI.TYPES.UNSIGNED_BYTE;
        this.alphaMode = options.alphaMode !== undefined ? options.alphaMode : 1;

        this.realWidth = 0;
        this.realHeight = 0;

        this.valid = false;
        this.isPowerOfTwo = false;

        this._glTextures = {};
        this._glTexture = 0;  /* Native GL texture ID. */
        this.dirtyId = 0;
        this.dirtyStyleId = 0;
        this.cacheId = null;
        this.textureCacheIds = [];

        this.destroyed = false;
        this.resource = null;

        if (resource) {
            this.setResource(resource);
        }
    }

    BaseTexture.prototype = Object.create(EventEmitter.prototype);
    BaseTexture.prototype.constructor = BaseTexture;

    BaseTexture.prototype.setResource = function(resource) {
        this.resource = resource;
        if (resource && resource.width) {
            this.setRealSize(resource.width, resource.height);
        }
        /* Image resources carry a loader-owned GL texture; record the ownership
           so destroy() never deletes a texture this BaseTexture does not own. */
        if (resource && resource._glTextureId) {
            this._glTexture = resource._glTextureId;
            this._glTextureIsImageOwned = true;
        }
        return this;
    };

    BaseTexture.prototype.setRealSize = function(realWidth, realHeight, resolution) {
        this.resolution = resolution || this.resolution;
        this.realWidth = realWidth;
        this.realHeight = realHeight;
        this.width = Math.round(realWidth / this.resolution);
        this.height = Math.round(realHeight / this.resolution);
        this.isPowerOfTwo = _isPow2(this.realWidth) && _isPow2(this.realHeight);
        this.update();
        return this;
    };

    BaseTexture.prototype.setSize = function(desiredWidth, desiredHeight, resolution) {
        this.resolution = resolution || this.resolution;
        this.width = desiredWidth;
        this.height = desiredHeight;
        this.realWidth = Math.round(desiredWidth * this.resolution);
        this.realHeight = Math.round(desiredHeight * this.resolution);
        this.isPowerOfTwo = _isPow2(this.realWidth) && _isPow2(this.realHeight);
        this.update();
        return this;
    };

    BaseTexture.prototype.setStyle = function(scaleMode, mipmap) {
        if (scaleMode !== undefined) this.scaleMode = scaleMode;
        if (mipmap !== undefined) this.mipmap = mipmap;
        this.dirtyStyleId++;
        return this;
    };

    BaseTexture.prototype.update = function() {
        if (this.destroyed) return;
        if (this.realWidth > 0 && this.realHeight > 0) {
            this.valid = true;
        }
        this.dirtyId++;
        this.emit("update", this);
    };

    BaseTexture.prototype.dispose = function() {
        this.emit("dispose", this);
    };

    /* Push the BaseTexture's scaleMode to the GL texture when either changed.
       RPG Maker bitmaps default to LINEAR while the native side defaults to
       NEAREST, which is only visible on scaled-up bitmaps. */
    function _applyTextureScaleMode(bt) {
        if (!bt._glTexture || typeof __native_renderer === "undefined" ||
            typeof __native_renderer.setTextureFilter !== "function") return;
        if (bt._glAppliedScaleMode === bt.scaleMode && bt._glAppliedScaleTex === bt._glTexture) return;
        __native_renderer.setTextureFilter(bt._glTexture, bt.scaleMode === PIXI.SCALE_MODES.LINEAR);
        bt._glAppliedScaleMode = bt.scaleMode;
        bt._glAppliedScaleTex = bt._glTexture;
    }

    /* Ensure a native GL texture exists: image resources alias the loader's
       texture directly, canvas resources upload their pixels. */
    BaseTexture.prototype._ensureGLTexture = function() {
        if (typeof __native_renderer === "undefined") return;

        /* Fast path: use the image loader's GL texture (from the resource or
           the _sourceImage set by Bitmap._createBaseTexture), avoiding canvas
           pixel readback. */
        var res = this.resource;
        var imgSrc = (res && res._glTextureId) ? res : this._sourceImage;
        var canvas = this._canvas || ((res && res.tagName === "CANVAS") ? res : null);

        /* Once the bitmap has been drawn on since the image texture was
           aliased, switch to the canvas path so the modifications show. */
        var drawnOnSinceAlias = imgSrc && canvas && canvas._context2d && canvas._context2d._handle &&
            this._glTextureIsImageOwned && this._glTexture === imgSrc._glTextureId &&
            this._glTextureDirtyId !== undefined && this._glTextureDirtyId !== this.dirtyId;

        if (imgSrc && imgSrc._glTextureId && !drawnOnSinceAlias) {
            if (this._glTexture !== imgSrc._glTextureId) {
                this._glTexture = imgSrc._glTextureId;
            }
            this._glTextureIsImageOwned = true;
            this._glTextureDirtyId = this.dirtyId;
            _applyTextureScaleMode(this);
            return;
        }

        /* Canvas path: read pixel data and upload. */
        if (typeof __native_canvas2d === "undefined") return;
        if (!canvas) canvas = res;
        if (!canvas) return;
        var ctx2d = canvas._context2d || (typeof canvas.getContext === "function" ? canvas.getContext("2d") : null);
        if (!ctx2d || !ctx2d._handle) return;
        var w = canvas.width || canvas._width || 0;
        var h = canvas.height || canvas._height || 0;
        if (w <= 0 || h <= 0) return;

        /* GL storage is immutable: recreate the texture when there is none,
           when the current one is loader-owned, or when the canvas was resized. */
        var needCreate = this._glTexture === 0 || this._glTextureIsImageOwned ||
                         w !== this._glTexW || h !== this._glTexH;
        if (needCreate || this._glTextureDirtyId !== this.dirtyId) {
            var fastUpload = __native_renderer.updateTextureFromCanvas;
            var pixels = fastUpload ? null : __native_canvas2d.getPixels(ctx2d._handle);
            if (!fastUpload && !pixels) return;
            if (needCreate) {
                if (this._glTexture && !this._glTextureIsImageOwned) {
                    __dropGlTexture(this);
                }
                this._glTexture = __native_renderer.createTexture(w, h);
                /* frees the texture when this object is garbage collected */
                this._glToken = __native_renderer.createTextureToken ? __native_renderer.createTextureToken(this._glTexture) : null;
                this._glTextureIsImageOwned = false;
                this._glTexW = w;
                this._glTexH = h;
            }
            if (!fastUpload || !fastUpload(this._glTexture, w, h, ctx2d._handle)) {
                if (!pixels) pixels = __native_canvas2d.getPixels(ctx2d._handle);
                if (!pixels) return;
                __native_renderer.updateTexture(this._glTexture, w, h, pixels);
            }
            this._glTextureDirtyId = this.dirtyId;
        }
        _applyTextureScaleMode(this);
    };

    /* Deletes the GL texture of a canvas-backed BaseTexture now (and disarms its garbage-collection token). */
    globalThis.__dropGlTexture = function(bt) {
        if (bt._glToken && __native_renderer.releaseTextureToken) {
            __native_renderer.releaseTextureToken(bt._glToken);
        } else if (bt._glTexture) {
            __native_renderer.deleteTexture(bt._glTexture);
        }
        bt._glToken = null;
    };

    BaseTexture.prototype.destroy = function() {
        if (this.destroyed) return;

        if (this.cacheId) {
            delete PIXI.utils.BaseTextureCache[this.cacheId];
            delete PIXI.utils.TextureCache[this.cacheId];
            this.cacheId = null;
        }
        for (var i = 0; i < this.textureCacheIds.length; i++) {
            delete PIXI.utils.BaseTextureCache[this.textureCacheIds[i]];
        }
        this.textureCacheIds.length = 0;

        this.dispose();
        this.resource = null;
        this.valid = false;
        this.destroyed = true;

        /* Never delete a loader-owned texture; it may be shared by other consumers. */
        if (this._glTexture && typeof __native_renderer !== "undefined") {
            if (this._glTextureIsImageOwned) {
                this._glTexture = 0;
            } else {
                __dropGlTexture(this);
                this._glTexture = 0;
            }
        }
    };

    BaseTexture.from = function(source, options) {
        if (source instanceof BaseTexture) return source;

        /* Element sources: PIXI wraps a canvas or image element as the resource
           (e.g. per-frame BaseTexture.from(canvas) in APNG plugins). */
        if (source && typeof source === "object") {
            var isCanvas = typeof source.getContext === "function" ||
                           source.tagName === "CANVAS" || !!source._context2d;
            if (isCanvas) {
                return BaseTexture.fromCanvas(source, options && options.scaleMode);
            }
            var isImage = source.tagName === "IMG" || source._glTextureId !== undefined ||
                          (("src" in source) && ("complete" in source));
            if (isImage) {
                var bti = new BaseTexture(null, options);
                bti.setResource(source);
                return bti;
            }
        }

        var cacheId = null;
        if (typeof source === "string") {
            cacheId = source;
            var cached = PIXI.utils.BaseTextureCache[cacheId];
            if (cached) return cached;
            if (/\.(webm|mp4|ogv|m4v|mov)(\?|$)/i.test(source)) return BaseTexture.fromVideoUrl(source, options);
        }

        var bt = new BaseTexture(null, options);
        if (cacheId) {
            bt.cacheId = cacheId;
            PIXI.utils.BaseTextureCache[cacheId] = bt;
            bt.textureCacheIds.push(cacheId);
        }
        return bt;
    };

    /* A movie used as a texture (video plugins draw movies through PIXI sprites). The <video> element stands in for
       the movie: it reports "loaded", can be "played" and ends, so scripts that wait for the end of a cutscene carry
       on. The picture itself is not drawn (the texture stays empty). */
    BaseTexture.fromVideoUrl = function(url, options) {
        var video = document.createElement("video");
        video.preload = "auto";
        video.src = url;
        var bt = new BaseTexture(null, options);
        bt.cacheId = url;
        PIXI.utils.BaseTextureCache[url] = bt;
        bt.textureCacheIds.push(url);
        bt.source = video;                        /* PIXI 4 */
        bt.resource = { source: video, url: url, width: 0, height: 0 };   /* PIXI 5 */
        /* With the native player the pictures are copied into a GL texture every frame and the full-screen overlay is
           switched off (opacity 0); without it the texture stays empty and the movie just "ends". */
        video.style.opacity = 0;
        video._frameHook = function() {
            if (!video._handle || typeof __native_video.updateTexture !== "function") return;
            if (!bt._glTexture && video.videoWidth > 0 && typeof __native_renderer !== "undefined") {
                bt._glTexture = __native_renderer.createTexture(video.videoWidth, video.videoHeight);
                bt._glTextureIsImageOwned = false;
                bt._glTexW = video.videoWidth;
                bt._glTexH = video.videoHeight;
                bt._glTextureDirtyId = bt.dirtyId;
                bt._glToken = __native_renderer.createTextureToken ? __native_renderer.createTextureToken(bt._glTexture) : null;
            }
            if (bt._glTexture) __native_video.updateTexture(video._handle, bt._glTexture);
        };
        var ready = function() {
            if (bt.valid) return;
            bt.setRealSize(video.videoWidth || 2, video.videoHeight || 2);
            bt.valid = true;
            bt.emit("update", bt);
            bt.emit("loaded", bt);
        };
        video.addEventListener("loadeddata", ready);
        video.addEventListener("canplay", ready);
        if (typeof video.load === "function") video.load();
        return bt;
    };

    BaseTexture.fromCanvas = function(canvas, scaleMode) {
        var bt = new BaseTexture(null, { scaleMode: scaleMode });
        bt.width = canvas.width || canvas._width || 0;
        bt.height = canvas.height || canvas._height || 0;
        bt.realWidth = bt.width;
        bt.realHeight = bt.height;
        bt.valid = bt.width > 0 && bt.height > 0;
        bt._canvas = canvas;
        return bt;
    };

    function _isPow2(v) {
        return v > 0 && (v & (v - 1)) === 0;
    }

    PIXI.BaseTexture = BaseTexture;

    /* PIXI.Texture */

    var _textureId = 0;

    function Texture(baseTexture, frame, orig, trim, rotate, anchor) {
        EventEmitter.call(this);

        this._id = _textureId++;
        this.uid = PIXI.utils.uid();

        this.noFrame = false;
        if (!frame) {
            this.noFrame = true;
            frame = new Rectangle(0, 0, 1, 1);
        }

        if (baseTexture instanceof Texture) {
            baseTexture = baseTexture.baseTexture;
        }

        this._baseTexture = baseTexture || null;
        this._frame = frame;
        this.trim = trim || null;
        this.valid = false;
        this.requiresUpdate = false;
        this._uvs = { x0: 0, y0: 0, x1: 1, y1: 0, x2: 1, y2: 1, x3: 0, y3: 1 };
        this.uvMatrix = null;
        this.orig = orig || frame;
        this._rotate = rotate || 0;
        this.defaultAnchor = anchor ? { x: anchor.x, y: anchor.y } : { x: 0, y: 0 };
        this._updateID = 0;

        this.textureCacheIds = [];

        this.destroyed = false;

        if (baseTexture) {
            if (baseTexture.valid) {
                if (this.noFrame) {
                    this._frame = new Rectangle(0, 0, baseTexture.width, baseTexture.height);
                    this.orig = this._frame;
                }
                this._updateUvs();
                this.valid = true;
            } else {
                var self = this;
                baseTexture.once("update", function() {
                    if (self.noFrame) {
                        self._frame = new Rectangle(0, 0, baseTexture.width, baseTexture.height);
                        self.orig = self._frame;
                    }
                    self._updateUvs();
                    self.valid = true;
                    self.emit("update", self);
                });
            }
        }
    }

    Texture.prototype = Object.create(EventEmitter.prototype);
    Texture.prototype.constructor = Texture;

    Object.defineProperty(Texture.prototype, "baseTexture", {
        get: function() { return this._baseTexture; },
        set: function(bt) {
            if (this._baseTexture === bt) return;
            this._baseTexture = bt;
            if (bt && bt.valid) {
                if (this.noFrame) {
                    this._frame = new Rectangle(0, 0, bt.width, bt.height);
                    this.orig = this._frame;
                }
                this._updateUvs();
                this.valid = true;
            }
        }
    });

    Object.defineProperty(Texture.prototype, "frame", {
        get: function() { return this._frame; },
        set: function(frame) {
            this._frame = frame;
            this.noFrame = false;
            if (!this.trim && !this._rotate) {
                this.orig = frame;
            }
            this._updateUvs();
            if (this._baseTexture && this._baseTexture.valid) {
                this.valid = true;
            }
        }
    });

    Object.defineProperty(Texture.prototype, "rotate", {
        get: function() { return this._rotate; },
        set: function(val) {
            this._rotate = val;
            this._updateUvs();
        }
    });

    Object.defineProperty(Texture.prototype, "width", {
        get: function() { return this.orig.width; }
    });

    Object.defineProperty(Texture.prototype, "height", {
        get: function() { return this.orig.height; }
    });

    Texture.prototype.update = function() {
        if (this.baseTexture && this.baseTexture.resource) {
            this.baseTexture.update();
        }
    };

    Texture.prototype._updateUvs = function() {
        var frame = this._frame;
        var bt = this.baseTexture;
        if (!bt) return;

        var tw = bt.width;
        var th = bt.height;
        if (tw <= 0 || th <= 0) return;

        this._uvs.x0 = frame.x / tw;
        this._uvs.y0 = frame.y / th;
        this._uvs.x1 = (frame.x + frame.width) / tw;
        this._uvs.y1 = frame.y / th;
        this._uvs.x2 = (frame.x + frame.width) / tw;
        this._uvs.y2 = (frame.y + frame.height) / th;
        this._uvs.x3 = frame.x / tw;
        this._uvs.y3 = (frame.y + frame.height) / th;

        this._updateID++;
    };

    Texture.prototype.onBaseTextureUpdated = function(baseTexture) {
        if (this.noFrame) {
            this._frame.width = baseTexture.width;
            this._frame.height = baseTexture.height;
        }
        this._updateUvs();
        this.valid = true;
        this.emit("update", this);
    };

    Texture.prototype.clone = function() {
        return new Texture(this.baseTexture, this._frame.clone(),
                          this.orig.clone(), this.trim ? this.trim.clone() : null,
                          this._rotate, this.defaultAnchor);
    };

    Texture.prototype.castToBaseTexture = function() {
        return this.baseTexture;
    };

    Texture.prototype.destroy = function(destroyBase) {
        if (this.destroyed) return;

        for (var i = 0; i < this.textureCacheIds.length; i++) {
            delete PIXI.utils.TextureCache[this.textureCacheIds[i]];
        }
        this.textureCacheIds.length = 0;

        if (destroyBase && this.baseTexture) {
            this.baseTexture.destroy();
        }
        this.baseTexture = null;
        this._frame = null;
        this.orig = null;
        this.trim = null;
        this._uvs = null;
        this.destroyed = true;
    };

    Texture.from = function(source, options, strict) {
        var cacheId = null;
        if (typeof source === "string") {
            cacheId = source;
            var cached = PIXI.utils.TextureCache[cacheId];
            if (cached) return cached;
        }

        var baseTexture;
        if (source instanceof BaseTexture) {
            baseTexture = source;
        } else {
            baseTexture = BaseTexture.from(source, options);
        }

        var texture = new Texture(baseTexture);
        if (cacheId) {
            texture.textureCacheIds.push(cacheId);
            PIXI.utils.TextureCache[cacheId] = texture;
        }
        return texture;
    };

    Texture.fromCanvas = function(canvas, scaleMode) {
        var baseTexture = BaseTexture.fromCanvas(canvas, scaleMode);
        return new Texture(baseTexture);
    };

    Texture.addToCache = function(texture, id) {
        if (id) {
            if (texture.textureCacheIds.indexOf(id) === -1) {
                texture.textureCacheIds.push(id);
            }
            PIXI.utils.TextureCache[id] = texture;
        }
    };

    Texture.removeFromCache = function(texture) {
        if (typeof texture === "string") {
            var cached = PIXI.utils.TextureCache[texture];
            if (cached) {
                var idx = cached.textureCacheIds.indexOf(texture);
                if (idx >= 0) cached.textureCacheIds.splice(idx, 1);
                delete PIXI.utils.TextureCache[texture];
                return cached;
            }
            return null;
        }
        if (texture && texture.textureCacheIds) {
            for (var i = 0; i < texture.textureCacheIds.length; i++) {
                delete PIXI.utils.TextureCache[texture.textureCacheIds[i]];
            }
            texture.textureCacheIds.length = 0;
        }
        return texture;
    };

    /* Texture.EMPTY — a valid but empty texture. */
    Texture.EMPTY = new Texture(new BaseTexture());
    Texture.EMPTY.destroy = function() {};
    Texture.EMPTY.on = function() { return this; };
    Texture.EMPTY.once = function() { return this; };
    Texture.EMPTY.emit = function() { return false; };

    /* Texture.WHITE — 1x1 white texture for untextured draws. */
    var _whiteBt = new BaseTexture();
    _whiteBt.setSize(1, 1);
    _whiteBt.valid = true;
    Texture.WHITE = new Texture(_whiteBt);
    Texture.WHITE.destroy = function() {};
    Texture.WHITE.on = function() { return this; };
    Texture.WHITE.once = function() { return this; };
    Texture.WHITE.emit = function() { return false; };

    PIXI.Texture = Texture;

    /* PIXI.RenderTexture */

    function RenderTexture(baseRenderTexture, frame) {
        Texture.call(this, baseRenderTexture, frame);

        this.valid = true;
        this._canvasContext = null;
        this.filterFrame = null;
        this.filterPoolKey = null;

        /* Native FBO state. */
        this._fbo = 0;
        this._fboTexture = 0;
    }

    RenderTexture.prototype = Object.create(Texture.prototype);
    RenderTexture.prototype.constructor = RenderTexture;

    Object.defineProperty(RenderTexture.prototype, "framebuffer", {
        get: function() { return { width: this.width, height: this.height }; }
    });

    RenderTexture.prototype.resize = function(desiredWidth, desiredHeight, resizeBaseTexture) {
        desiredWidth = Math.ceil(desiredWidth);
        desiredHeight = Math.ceil(desiredHeight);

        this._frame.width = desiredWidth;
        this._frame.height = desiredHeight;

        if (resizeBaseTexture !== false && this.baseTexture) {
            this.baseTexture.setSize(desiredWidth, desiredHeight);
        }

        /* Recreate FBO if native renderer is available. */
        if (typeof __native_renderer !== "undefined" && this._fbo) {
            __native_renderer.deleteRenderTexture(this._fbo, this._fboTexture);
            var rt = __native_renderer.createRenderTexture(desiredWidth, desiredHeight);
            this._fbo = rt.fbo;
            this._fboTexture = rt.texture;
            if (this.baseTexture) {
                this.baseTexture._glTexture = this._fboTexture;
            }
        }

        this.valid = desiredWidth > 0 && desiredHeight > 0;
        this._updateUvs();
    };

    RenderTexture.prototype.destroy = function(destroyBase) {
        if (typeof __native_renderer !== "undefined" && this._fbo) {
            __native_renderer.deleteRenderTexture(this._fbo, this._fboTexture);
            this._fbo = 0;
            this._fboTexture = 0;
        }
        Texture.prototype.destroy.call(this, destroyBase);
    };

    RenderTexture.create = function(options) {
        if (typeof options === "number") {
            /* Legacy: create(width, height, ...) */
            options = {
                width: arguments[0],
                height: arguments[1],
                scaleMode: arguments[2],
                resolution: arguments[3],
            };
        }
        options = options || {};
        var width = options.width || 100;
        var height = options.height || 100;
        var resolution = options.resolution || PIXI.settings.RESOLUTION;
        var scaleMode = options.scaleMode !== undefined ? options.scaleMode : PIXI.settings.SCALE_MODE;

        var bt = new BaseTexture(null, {
            scaleMode: scaleMode,
            resolution: resolution,
        });
        bt.setSize(width, height, resolution);
        bt.valid = true;

        var rt = new RenderTexture(bt);

        if (typeof __native_renderer !== "undefined") {
            var native = __native_renderer.createRenderTexture(width, height);
            if (native) {
                rt._fbo = native.fbo;
                rt._fboTexture = native.texture;
                bt._glTexture = native.texture;
            }
        }

        return rt;
    };

    PIXI.RenderTexture = RenderTexture;

    /* PIXI.GroupD8 (texture rotation utility) */

    PIXI.groupD8 = {
        E: 0, SE: 1, S: 2, SW: 3, W: 4, NW: 5, N: 6, NE: 7,
        MIRROR_VERTICAL: 8,
        MIRROR_HORIZONTAL: 12,
        uX: function(ind) { return [1, 1, 0, -1, -1, -1, 0, 1][ind & 7]; },
        uY: function(ind) { return [0, 1, 1, 1, 0, -1, -1, -1][ind & 7]; },
        vX: function(ind) { return [0, -1, -1, -1, 0, 1, 1, 1][ind & 7]; },
        vY: function(ind) { return [1, 1, 0, -1, -1, -1, 0, 1][ind & 7]; },
        isVertical: function(rotation) { return (rotation & 3) === 2; },
        byDirection: function(dx, dy) { return 0; },
        matrixAppendRotationInv: function(matrix, rotation, tx, ty) { /* stub */ },
        inv: function(rotation) { return rotation & 7 ? 8 - rotation : rotation; },
        add: function(rotationFirst, rotationSecond) { return (rotationFirst + rotationSecond) & 7; },
        sub: function(rotationFirst, rotationSecond) { return (rotationFirst - rotationSecond + 8) & 7; },
        rotate180: function(rotation) { return rotation ^ 4; },
    };

    /* PIXI.Matrix (2D affine transform) */

    function Matrix(a, b, c, d, tx, ty) {
        this.a  = (a !== undefined) ? a : 1;
        this.b  = b || 0;
        this.c  = c || 0;
        this.d  = (d !== undefined) ? d : 1;
        this.tx = tx || 0;
        this.ty = ty || 0;
    }

    Matrix.prototype.set = function(a, b, c, d, tx, ty) {
        this.a = a; this.b = b; this.c = c; this.d = d;
        this.tx = tx; this.ty = ty;
        return this;
    };

    Matrix.prototype.clone = function() {
        return new Matrix(this.a, this.b, this.c, this.d, this.tx, this.ty);
    };

    Matrix.prototype.copyTo = function(m) {
        m.a = this.a; m.b = this.b; m.c = this.c; m.d = this.d;
        m.tx = this.tx; m.ty = this.ty;
        return m;
    };

    Matrix.prototype.copyFrom = function(m) {
        this.a = m.a; this.b = m.b; this.c = m.c; this.d = m.d;
        this.tx = m.tx; this.ty = m.ty;
        return this;
    };

    Matrix.prototype.identity = function() {
        this.a = 1; this.b = 0; this.c = 0; this.d = 1;
        this.tx = 0; this.ty = 0;
        return this;
    };

    Matrix.prototype.translate = function(x, y) {
        this.tx += x;
        this.ty += y;
        return this;
    };

    Matrix.prototype.scale = function(x, y) {
        this.a *= x; this.b *= y;
        this.c *= x; this.d *= y;
        this.tx *= x; this.ty *= y;
        return this;
    };

    Matrix.prototype.rotate = function(angle) {
        var cos = Math.cos(angle);
        var sin = Math.sin(angle);
        var a = this.a, b = this.b, c = this.c, d = this.d;
        var tx = this.tx, ty = this.ty;
        this.a  = a * cos - b * sin;
        this.b  = a * sin + b * cos;
        this.c  = c * cos - d * sin;
        this.d  = c * sin + d * cos;
        this.tx = tx * cos - ty * sin;
        this.ty = tx * sin + ty * cos;
        return this;
    };

    Matrix.prototype.append = function(m) {
        var a = this.a, b = this.b, c = this.c, d = this.d;
        this.a  = m.a * a + m.b * c;
        this.b  = m.a * b + m.b * d;
        this.c  = m.c * a + m.d * c;
        this.d  = m.c * b + m.d * d;
        this.tx = m.tx * a + m.ty * c + this.tx;
        this.ty = m.tx * b + m.ty * d + this.ty;
        return this;
    };

    Matrix.prototype.prepend = function(m) {
        var tx = this.tx;
        if (m.a !== 1 || m.b !== 0 || m.c !== 0 || m.d !== 1) {
            var a = this.a, c = this.c;
            this.a = a * m.a + this.b * m.c;
            this.b = a * m.b + this.b * m.d;
            this.c = c * m.a + this.d * m.c;
            this.d = c * m.b + this.d * m.d;
        }
        this.tx = tx * m.a + this.ty * m.c + m.tx;
        this.ty = tx * m.b + this.ty * m.d + m.ty;
        return this;
    };

    Matrix.prototype.invert = function() {
        var a = this.a, b = this.b, c = this.c, d = this.d;
        var tx = this.tx, det = a * d - b * c;
        if (det === 0) return this;
        this.a = d / det; this.b = -b / det;
        this.c = -c / det; this.d = a / det;
        this.tx = (c * this.ty - d * tx) / det;
        this.ty = -(a * this.ty - b * tx) / det;
        return this;
    };

    Matrix.prototype.apply = function(pos, newPos) {
        newPos = newPos || { x: 0, y: 0 };
        newPos.x = this.a * pos.x + this.c * pos.y + this.tx;
        newPos.y = this.b * pos.x + this.d * pos.y + this.ty;
        return newPos;
    };

    Matrix.prototype.applyInverse = function(pos, newPos) {
        newPos = newPos || { x: 0, y: 0 };
        var denom = this.a * this.d + this.c * -this.b;
        var id = denom !== 0 ? 1 / denom : 0;
        newPos.x = this.d * id * pos.x + -this.c * id * pos.y +
                   (this.ty * this.c - this.tx * this.d) * id;
        newPos.y = this.a * id * pos.y + -this.b * id * pos.x +
                   (-this.ty * this.a + this.tx * this.b) * id;
        return newPos;
    };

    Matrix.prototype.decompose = function(transform) {
        var a = this.a, b = this.b, c = this.c, d = this.d;
        var skewX = -Math.atan2(-c, d);
        var skewY = Math.atan2(b, a);
        var delta = Math.abs(skewX + skewY);
        if (delta < 0.00001 || Math.abs(Math.PI * 2 - delta) < 0.00001) {
            transform.rotation = skewY;
            if (a < 0 && d >= 0) transform.rotation += Math.PI;
            transform.skew.x = transform.skew.y = 0;
        } else {
            transform.rotation = 0;
            transform.skew.x = skewX;
            transform.skew.y = skewY;
        }
        transform.scale.x = Math.sqrt(a * a + b * b);
        transform.scale.y = Math.sqrt(c * c + d * d);
        transform.position.x = this.tx;
        transform.position.y = this.ty;
        return transform;
    };

    Matrix.IDENTITY = new Matrix();
    Matrix.TEMP_MATRIX = new Matrix();

    PIXI.Matrix = Matrix;

    /* PIXI.Transform */

    function Transform() {
        this.worldTransform = new Matrix();
        this.localTransform = new Matrix();
        this.position = new ObservablePoint(this.onChange, this, 0, 0);
        this.scale = new ObservablePoint(this.onChange, this, 1, 1);
        this.pivot = new ObservablePoint(this.onChange, this, 0, 0);
        this.skew = new ObservablePoint(this.onSkewChange, this, 0, 0);
        this._rotation = 0;
        this._cx = 1;
        this._sx = 0;
        this._cy = 0;
        this._sy = 1;
        this._localID = 0;
        this._currentLocalID = -1;
        this._worldID = 0;
        this._parentID = -1;
    }

    Transform.prototype.onChange = function() {
        this._localID++;
    };

    Transform.prototype.onSkewChange = function() {
        this._cx = Math.cos(this._rotation + this.skew._y);
        this._sx = Math.sin(this._rotation + this.skew._y);
        this._cy = -Math.sin(this._rotation - this.skew._x);
        this._sy = Math.cos(this._rotation - this.skew._x);
        this._localID++;
    };

    Object.defineProperty(Transform.prototype, "rotation", {
        get: function() { return this._rotation; },
        set: function(val) {
            if (this._rotation !== val) {
                this._rotation = val;
                this._cx = Math.cos(val + this.skew._y);
                this._sx = Math.sin(val + this.skew._y);
                this._cy = -Math.sin(val - this.skew._x);
                this._sy = Math.cos(val - this.skew._x);
                this._localID++;
            }
        }
    });

    Transform.prototype.updateLocalTransform = function() {
        var lt = this.localTransform;
        if (this._localID !== this._currentLocalID) {
            lt.a  =  this._cx * this.scale.x;
            lt.b  =  this._sx * this.scale.x;
            lt.c  =  this._cy * this.scale.y;
            lt.d  =  this._sy * this.scale.y;
            lt.tx  = this.position.x - (this.pivot.x * lt.a + this.pivot.y * lt.c);
            lt.ty  = this.position.y - (this.pivot.x * lt.b + this.pivot.y * lt.d);
            this._currentLocalID = this._localID;
            this._parentID = -1;
        }
    };

    Transform.prototype.updateTransform = function(parentTransform) {
        var lt = this.localTransform;
        if (this._localID !== this._currentLocalID) {
            lt.a  =  this._cx * this.scale.x;
            lt.b  =  this._sx * this.scale.x;
            lt.c  =  this._cy * this.scale.y;
            lt.d  =  this._sy * this.scale.y;
            lt.tx  = this.position.x - (this.pivot.x * lt.a + this.pivot.y * lt.c);
            lt.ty  = this.position.y - (this.pivot.x * lt.b + this.pivot.y * lt.d);
            this._currentLocalID = this._localID;
            this._parentID = -1;
        }

        if (this._parentID !== parentTransform._worldID) {
            var pt = parentTransform.worldTransform;
            var wt = this.worldTransform;
            wt.a  = lt.a * pt.a + lt.b * pt.c;
            wt.b  = lt.a * pt.b + lt.b * pt.d;
            wt.c  = lt.c * pt.a + lt.d * pt.c;
            wt.d  = lt.c * pt.b + lt.d * pt.d;
            wt.tx = lt.tx * pt.a + lt.ty * pt.c + pt.tx;
            wt.ty = lt.tx * pt.b + lt.ty * pt.d + pt.ty;
            this._parentID = parentTransform._worldID;
            this._worldID++;
        }
    };

    Transform.prototype.setFromMatrix = function(matrix) {
        matrix.decompose(this);
        this._localID++;
    };

    Transform.IDENTITY = new Transform();

    PIXI.Transform = Transform;

    /* PIXI.Point / PIXI.ObservablePoint */

    function Point(x, y) {
        this.x = x || 0;
        this.y = y || 0;
    }

    Point.prototype.clone = function() {
        return new Point(this.x, this.y);
    };

    Point.prototype.copyFrom = function(p) {
        this.x = p.x;
        this.y = p.y;
        return this;
    };

    Point.prototype.copyTo = function(p) {
        p.x = this.x;
        p.y = this.y;
        return p;
    };

    Point.prototype.equals = function(p) {
        return this.x === p.x && this.y === p.y;
    };

    Point.prototype.set = function(x, y) {
        this.x = x || 0;
        this.y = (y === undefined) ? (x || 0) : (y || 0);
    };

    PIXI.Point = Point;

    function ObservablePoint(cb, scope, x, y) {
        this._x = x || 0;
        this._y = y || 0;
        this.cb = cb;
        this.scope = scope;
    }

    Object.defineProperty(ObservablePoint.prototype, "x", {
        get: function() { return this._x; },
        set: function(v) {
            if (this._x !== v) {
                this._x = v;
                if (this.cb) this.cb.call(this.scope);
            }
        }
    });

    Object.defineProperty(ObservablePoint.prototype, "y", {
        get: function() { return this._y; },
        set: function(v) {
            if (this._y !== v) {
                this._y = v;
                if (this.cb) this.cb.call(this.scope);
            }
        }
    });

    ObservablePoint.prototype.clone = function(cb, scope) {
        return new ObservablePoint(cb || this.cb, scope || this.scope, this._x, this._y);
    };

    ObservablePoint.prototype.copyFrom = function(p) {
        if (this._x !== p.x || this._y !== p.y) {
            this._x = p.x;
            this._y = p.y;
            if (this.cb) this.cb.call(this.scope);
        }
        return this;
    };

    ObservablePoint.prototype.copyTo = function(p) {
        p.set(this._x, this._y);
        return p;
    };

    ObservablePoint.prototype.equals = function(p) {
        return this._x === p.x && this._y === p.y;
    };

    ObservablePoint.prototype.set = function(x, y) {
        var nx = x || 0;
        var ny = (y === undefined) ? nx : (y || 0);
        if (this._x !== nx || this._y !== ny) {
            this._x = nx;
            this._y = ny;
            if (this.cb) this.cb.call(this.scope);
        }
    };

    PIXI.ObservablePoint = ObservablePoint;

    /* PIXI.Bounds */

    function Bounds() {
        this.minX = Infinity;
        this.minY = Infinity;
        this.maxX = -Infinity;
        this.maxY = -Infinity;
        this.rect = null;
        this.updateID = -1;
    }

    Bounds.prototype.isEmpty = function() {
        return this.minX > this.maxX || this.minY > this.maxY;
    };

    Bounds.prototype.clear = function() {
        this.minX = Infinity;
        this.minY = Infinity;
        this.maxX = -Infinity;
        this.maxY = -Infinity;
        this.updateID = -1;
    };

    Bounds.prototype.getRectangle = function(rect) {
        if (this.minX > this.maxX || this.minY > this.maxY) {
            return Rectangle.EMPTY;
        }
        rect = rect || new Rectangle(0, 0, 1, 1);
        rect.x = this.minX;
        rect.y = this.minY;
        rect.width = this.maxX - this.minX;
        rect.height = this.maxY - this.minY;
        return rect;
    };

    Bounds.prototype.addPoint = function(point) {
        this.minX = Math.min(this.minX, point.x);
        this.maxX = Math.max(this.maxX, point.x);
        this.minY = Math.min(this.minY, point.y);
        this.maxY = Math.max(this.maxY, point.y);
    };

    Bounds.prototype.addQuad = function(vertices) {
        var minX = this.minX, minY = this.minY, maxX = this.maxX, maxY = this.maxY;
        for (var i = 0; i < vertices.length; i += 2) {
            var x = vertices[i], y = vertices[i + 1];
            minX = x < minX ? x : minX;
            minY = y < minY ? y : minY;
            maxX = x > maxX ? x : maxX;
            maxY = y > maxY ? y : maxY;
        }
        this.minX = minX;
        this.minY = minY;
        this.maxX = maxX;
        this.maxY = maxY;
    };

    Bounds.prototype.addFrame = function(transform, x0, y0, x1, y1) {
        this.addFrameMatrix(transform.worldTransform, x0, y0, x1, y1);
    };

    Bounds.prototype.addFrameMatrix = function(matrix, x0, y0, x1, y1) {
        var a = matrix.a, b = matrix.b, c = matrix.c, d = matrix.d;
        var tx = matrix.tx, ty = matrix.ty;

        var minX = this.minX, minY = this.minY, maxX = this.maxX, maxY = this.maxY;

        var px = a * x0 + c * y0 + tx;
        var py = b * x0 + d * y0 + ty;
        if (px < minX) minX = px; if (px > maxX) maxX = px;
        if (py < minY) minY = py; if (py > maxY) maxY = py;

        px = a * x1 + c * y0 + tx;
        py = b * x1 + d * y0 + ty;
        if (px < minX) minX = px; if (px > maxX) maxX = px;
        if (py < minY) minY = py; if (py > maxY) maxY = py;

        px = a * x1 + c * y1 + tx;
        py = b * x1 + d * y1 + ty;
        if (px < minX) minX = px; if (px > maxX) maxX = px;
        if (py < minY) minY = py; if (py > maxY) maxY = py;

        px = a * x0 + c * y1 + tx;
        py = b * x0 + d * y1 + ty;
        if (px < minX) minX = px; if (px > maxX) maxX = px;
        if (py < minY) minY = py; if (py > maxY) maxY = py;

        this.minX = minX;
        this.minY = minY;
        this.maxX = maxX;
        this.maxY = maxY;
    };

    Bounds.prototype.addBounds = function(bounds) {
        var minX = this.minX, minY = this.minY, maxX = this.maxX, maxY = this.maxY;
        this.minX = bounds.minX < minX ? bounds.minX : minX;
        this.minY = bounds.minY < minY ? bounds.minY : minY;
        this.maxX = bounds.maxX > maxX ? bounds.maxX : maxX;
        this.maxY = bounds.maxY > maxY ? bounds.maxY : maxY;
    };

    Bounds.prototype.addBoundsMask = function(bounds, mask) {
        this.addFrameMatrix(Matrix.IDENTITY, Math.max(bounds.minX, mask.minX),
            Math.max(bounds.minY, mask.minY), Math.min(bounds.maxX, mask.maxX),
            Math.min(bounds.maxY, mask.maxY));
    };

    PIXI.Bounds = Bounds;

    /* PIXI.DisplayObject */

    var _displayObjectId = 0;

    function DisplayObject() {
        EventEmitter.call(this);

        this._id = _displayObjectId++;

        this.transform = new Transform();
        this.alpha = 1;
        this.visible = true;
        this.renderable = true;
        this.parent = null;
        this.worldAlpha = 1;

        this._lastSortedIndex = 0;
        this._zIndex = 0;

        this.filterArea = null;
        this.filters = null;

        this._enabledFilters = null;
        this._bounds = new Bounds();
        this._boundsID = 0;
        this._lastBoundsID = -1;
        this._boundsRect = null;
        this._localBoundsRect = null;
        this._mask = null;

        this._destroyed = false;

        this.isMask = false;
        this.isSprite = false;

        this.interactive = false;
        this.interactiveChildren = true;
        this.hitArea = null;
        this.buttonMode = false;
        this.cursor = null;

        /* PIXI's DisplayObject has no blendMode of its own; subclasses add
           one. Plugins (VisuMZ TiledMZ) define it as an accessor on their
           class, backed by state that does not exist yet in the base
           constructor, so only seed a plain value where nothing defines it. */
        if (!("blendMode" in this)) {
            this.blendMode = PIXI.BLEND_MODES.NORMAL;
        }
    }

    DisplayObject.prototype = Object.create(EventEmitter.prototype);
    DisplayObject.prototype.constructor = DisplayObject;

    Object.defineProperty(DisplayObject.prototype, "x", {
        get: function() { return this.position.x; },
        set: function(v) { this.transform.position.x = v; }
    });

    Object.defineProperty(DisplayObject.prototype, "y", {
        get: function() { return this.position.y; },
        set: function(v) { this.transform.position.y = v; }
    });

    /* PIXI: assigning a point copies it (sprite.scale = otherPoint) */
    Object.defineProperty(DisplayObject.prototype, "position", {
        get: function() { return this.transform.position; },
        set: function(v) { if (v && typeof v === "object") this.transform.position.copyFrom(v); },
        configurable: true
    });

    /* PIXI: assigning a point copies it (sprite.scale = otherPoint) */
    Object.defineProperty(DisplayObject.prototype, "scale", {
        get: function() { return this.transform.scale; },
        set: function(v) { if (v && typeof v === "object") this.transform.scale.copyFrom(v); },
        configurable: true
    });

    /* PIXI: assigning a point copies it (sprite.scale = otherPoint) */
    Object.defineProperty(DisplayObject.prototype, "pivot", {
        get: function() { return this.transform.pivot; },
        set: function(v) { if (v && typeof v === "object") this.transform.pivot.copyFrom(v); },
        configurable: true
    });

    /* PIXI: assigning a point copies it (sprite.scale = otherPoint) */
    Object.defineProperty(DisplayObject.prototype, "skew", {
        get: function() { return this.transform.skew; },
        set: function(v) { if (v && typeof v === "object") this.transform.skew.copyFrom(v); },
        configurable: true
    });

    Object.defineProperty(DisplayObject.prototype, "rotation", {
        get: function() { return this.transform.rotation; },
        set: function(v) { this.transform.rotation = v; }
    });

    Object.defineProperty(DisplayObject.prototype, "worldTransform", {
        get: function() { return this.transform.worldTransform; }
    });

    Object.defineProperty(DisplayObject.prototype, "localTransform", {
        get: function() { return this.transform.localTransform; }
    });

    Object.defineProperty(DisplayObject.prototype, "zIndex", {
        get: function() { return this._zIndex; },
        set: function(v) { this._zIndex = v; if (this.parent) this.parent.sortDirty = true; }
    });

    Object.defineProperty(DisplayObject.prototype, "worldVisible", {
        get: function() {
            var item = this;
            do {
                if (!item.visible) return false;
                item = item.parent;
            } while (item);
            return true;
        }
    });

    Object.defineProperty(DisplayObject.prototype, "mask", {
        get: function() { return this._mask; },
        set: function(v) {
            if (this._mask) { this._mask.isMask = false; }
            this._mask = v;
            if (this._mask) { this._mask.isMask = true; }
        }
    });

    DisplayObject.prototype.updateTransform = function() {
        this._boundsID++;
        this.transform.updateTransform(this.parent ? this.parent.transform : Transform.IDENTITY);
        this.worldAlpha = this.alpha * (this.parent ? this.parent.worldAlpha : 1);
    };

    DisplayObject.prototype._recursivePostUpdateTransform = function() {
        if (this.parent) {
            this.parent._recursivePostUpdateTransform();
            this.transform.updateTransform(this.parent.transform);
        } else {
            this.transform.updateTransform(Transform.IDENTITY);
        }
    };

    DisplayObject.prototype.getBounds = function(skipUpdate, rect) {
        if (!skipUpdate) {
            if (!this.parent) {
                this.parent = _tempDisplayObjectParent;
                this.updateTransform();
                this.parent = null;
            } else {
                this._recursivePostUpdateTransform();
                this.updateTransform();
            }
        }
        if (this._boundsID !== this._lastBoundsID) {
            this.calculateBounds();
            this._lastBoundsID = this._boundsID;
        }
        if (!rect) {
            if (!this._boundsRect) this._boundsRect = new Rectangle();
            rect = this._boundsRect;
        }
        return this._bounds.getRectangle(rect);
    };

    DisplayObject.prototype.getLocalBounds = function(rect) {
        var transformRef = this.transform;
        var parentRef = this.parent;
        this.parent = null;
        this.transform = _tempDisplayObjectParent.transform;
        if (!rect) {
            if (!this._localBoundsRect) this._localBoundsRect = new Rectangle();
            rect = this._localBoundsRect;
        }
        var bounds = this.getBounds(false, rect);
        this.parent = parentRef;
        this.transform = transformRef;
        return bounds;
    };

    DisplayObject.prototype.toGlobal = function(position, point, skipUpdate) {
        if (!skipUpdate) {
            this._recursivePostUpdateTransform();
            if (!this.parent) {
                this.parent = _tempDisplayObjectParent;
                this.displayObjectUpdateTransform();
                this.parent = null;
            } else {
                this.displayObjectUpdateTransform();
            }
        }
        return this.worldTransform.apply(position, point);
    };

    DisplayObject.prototype.toLocal = function(position, from, point, skipUpdate) {
        if (from) {
            position = from.toGlobal(position, point, skipUpdate);
        }
        if (!skipUpdate) {
            this._recursivePostUpdateTransform();
            if (!this.parent) {
                this.parent = _tempDisplayObjectParent;
                this.displayObjectUpdateTransform();
                this.parent = null;
            } else {
                this.displayObjectUpdateTransform();
            }
        }
        return this.worldTransform.applyInverse(position, point);
    };

    DisplayObject.prototype.render = function(renderer) {
        /* Override in subclasses. */
    };

    DisplayObject.prototype.setParent = function(container) {
        if (!container || !container.addChild) {
            throw new Error("setParent: argument must be a Container");
        }
        container.addChild(this);
        return container;
    };

    DisplayObject.prototype.setTransform = function(x, y, scaleX, scaleY, rotation, skewX, skewY, pivotX, pivotY) {
        this.position.x = x || 0;
        this.position.y = y || 0;
        this.scale.x = scaleX !== undefined ? (scaleX || 0) : 1;
        this.scale.y = scaleY !== undefined ? (scaleY || 0) : 1;
        this.rotation = rotation || 0;
        this.skew.x = skewX || 0;
        this.skew.y = skewY || 0;
        this.pivot.x = pivotX || 0;
        this.pivot.y = pivotY || 0;
        return this;
    };

    DisplayObject.prototype.calculateBounds = function() {
        /* Override in subclasses. */
    };

    DisplayObject.prototype.displayObjectUpdateTransform = DisplayObject.prototype.updateTransform;

    DisplayObject.prototype.destroy = function() {
        if (this.parent) {
            this.parent.removeChild(this);
        }
        this.removeAllListeners();
        this.transform = null;
        this.parent = null;
        this._bounds = null;
        this._mask = null;
        this.filters = null;
        this.filterArea = null;
        this.hitArea = null;
        this.interactive = false;
        this.interactiveChildren = false;
        this._destroyed = true;
    };

    /* Temporary parent used during bounds calculation. */
    var _tempDisplayObjectParent = new DisplayObject();
    _tempDisplayObjectParent.transform = new Transform();

    PIXI.DisplayObject = DisplayObject;

    /* PIXI.Container */

    function Container() {
        DisplayObject.call(this);

        this.children = [];
        this.sortableChildren = false;
        this.sortDirty = false;
    }

    Container.prototype = Object.create(DisplayObject.prototype);
    Container.prototype.constructor = Container;

    Object.defineProperty(Container.prototype, "width", {
        get: function() {
            return this.scale.x * this.getLocalBounds().width;
        },
        set: function(v) {
            var w = this.getLocalBounds().width;
            if (w !== 0) {
                this.scale.x = v / w;
            } else {
                this.scale.x = 1;
            }
            this._width = v;
        }
    });

    Object.defineProperty(Container.prototype, "height", {
        get: function() {
            return this.scale.y * this.getLocalBounds().height;
        },
        set: function(v) {
            var h = this.getLocalBounds().height;
            if (h !== 0) {
                this.scale.y = v / h;
            } else {
                this.scale.y = 1;
            }
            this._height = v;
        }
    });

    Container.prototype.onChildrenChange = function() { /* stub */ };

    Container.prototype.addChild = function(child) {
        var argumentsLength = arguments.length;
        if (argumentsLength > 1) {
            for (var i = 0; i < argumentsLength; i++) {
                this.addChild(arguments[i]);
            }
        } else {
            if (child.parent) {
                child.parent.removeChild(child);
            }
            child.parent = this;
            this.sortDirty = true;
            child.transform._parentID = -1;
            this.children.push(child);
            this._boundsID++;
            this.onChildrenChange(this.children.length - 1);
            child.emit("added", this);
            this.emit("childAdded", child, this, this.children.length - 1);
        }
        return child;
    };

    Container.prototype.addChildAt = function(child, index) {
        if (index < 0 || index > this.children.length) {
            throw new Error("addChildAt: index " + index + " out of range [0, " + this.children.length + "]");
        }
        if (child.parent) {
            child.parent.removeChild(child);
        }
        child.parent = this;
        this.sortDirty = true;
        child.transform._parentID = -1;
        this.children.splice(index, 0, child);
        this._boundsID++;
        this.onChildrenChange(index);
        child.emit("added", this);
        this.emit("childAdded", child, this, index);
        return child;
    };

    Container.prototype.swapChildren = function(child1, child2) {
        if (child1 === child2) return;
        var i1 = this.children.indexOf(child1);
        var i2 = this.children.indexOf(child2);
        if (i1 < 0 || i2 < 0) {
            throw new Error("swapChildren: children not in container");
        }
        this.children[i1] = child2;
        this.children[i2] = child1;
        this.onChildrenChange(i1 < i2 ? i1 : i2);
    };

    Container.prototype.getChildIndex = function(child) {
        var idx = this.children.indexOf(child);
        if (idx < 0) throw new Error("getChildIndex: child not found");
        return idx;
    };

    Container.prototype.setChildIndex = function(child, index) {
        if (index < 0 || index >= this.children.length) {
            throw new Error("setChildIndex: index out of range");
        }
        var curIdx = this.getChildIndex(child);
        this.children.splice(curIdx, 1);
        this.children.splice(index, 0, child);
        this.onChildrenChange(index);
    };

    Container.prototype.getChildAt = function(index) {
        if (index < 0 || index >= this.children.length) {
            throw new Error("getChildAt: index out of range");
        }
        return this.children[index];
    };

    Container.prototype.getChildByName = function(name) {
        for (var i = 0; i < this.children.length; i++) {
            if (this.children[i].name === name) return this.children[i];
        }
        return null;
    };

    Container.prototype.removeChild = function(child) {
        var argumentsLength = arguments.length;
        if (argumentsLength > 1) {
            for (var i = 0; i < argumentsLength; i++) {
                this.removeChild(arguments[i]);
            }
        } else {
            var idx = this.children.indexOf(child);
            if (idx < 0) return null;
            child.parent = null;
            child.transform._parentID = -1;
            this.children.splice(idx, 1);
            this._boundsID++;
            this.onChildrenChange(idx);
            child.emit("removed", this);
            this.emit("childRemoved", child, this, idx);
        }
        return child;
    };

    Container.prototype.removeChildAt = function(index) {
        var child = this.getChildAt(index);
        child.parent = null;
        child.transform._parentID = -1;
        this.children.splice(index, 1);
        this._boundsID++;
        this.onChildrenChange(index);
        child.emit("removed", this);
        this.emit("childRemoved", child, this, index);
        return child;
    };

    Container.prototype.removeChildren = function(beginIndex, endIndex) {
        beginIndex = beginIndex || 0;
        endIndex = endIndex !== undefined ? endIndex : this.children.length;
        var range = endIndex - beginIndex;
        if (range <= 0 || range > this.children.length) return [];
        var removed = this.children.splice(beginIndex, range);
        for (var i = 0; i < removed.length; i++) {
            removed[i].parent = null;
            removed[i].transform._parentID = -1;
            removed[i].emit("removed", this);
        }
        this.onChildrenChange(beginIndex);
        return removed;
    };

    Container.prototype.sortChildren = function() {
        if (!this.sortDirty) return;
        this.sortDirty = false;
        this.children.sort(function(a, b) {
            return a._zIndex - b._zIndex || a._lastSortedIndex - b._lastSortedIndex;
        });
        for (var i = 0; i < this.children.length; i++) {
            this.children[i]._lastSortedIndex = i;
        }
    };

    Container.prototype.updateTransform = function() {
        if (this.sortableChildren && this.sortDirty) {
            this.sortChildren();
        }
        this._boundsID++;
        this.transform.updateTransform(this.parent ? this.parent.transform : Transform.IDENTITY);
        this.worldAlpha = this.alpha * (this.parent ? this.parent.worldAlpha : 1);

        for (var i = 0; i < this.children.length; i++) {
            var child = this.children[i];
            if (child.visible) {
                child.updateTransform();
            }
        }
    };

    Container.prototype.calculateBounds = function() {
        this._bounds.clear();
        if (!this.visible) return;
        this._calculateBounds();
        for (var i = 0; i < this.children.length; i++) {
            var child = this.children[i];
            if (!child.visible || !child.renderable) continue;
            child.calculateBounds();
            if (child._bounds.isEmpty()) continue;
            this._bounds.addBounds(child._bounds);
        }
        this._boundsID = this._lastBoundsID;
    };

    Container.prototype._calculateBounds = function() {
        /* Override in subclasses. */
    };

    Container.prototype._render = function(/*renderer*/) {
        /* Override in subclasses (e.g. Tilemap.Layer). */
    };

    /* True when every filter is effectively a no-op (disabled, AlphaFilter, or a
       ColorFilter with default uniforms), so the FBO pipeline can be skipped. */
    function _isAlphaFilter(filter) {
        return PIXI.filters.AlphaFilter &&
               filter.constructor === PIXI.filters.AlphaFilter;
    }

    function _filtersAreNoOp(filters) {
        for (var i = 0; i < filters.length; i++) {
            var f = filters[i];
            if (f.enabled === false || f._rmBroken) continue;
            /* AlphaFilter needs no FBO pass; filterArea clipping uses GL scissor. */
            if (_isAlphaFilter(f)) continue;
            if (_isColorFilter(f)) {
                var u = f.uniforms;
                if (u.hue !== 0) return false;
                if (u.brightness !== 255) return false;
                var ct = u.colorTone;
                if (ct[0] !== 0 || ct[1] !== 0 || ct[2] !== 0 || ct[3] !== 0) return false;
                var bc = u.blendColor;
                if (bc[0] !== 0 || bc[1] !== 0 || bc[2] !== 0 || bc[3] !== 0) return false;
            } else {
                /* Unknown filter type (e.g. BlurFilter) — needs FBO rendering. */
                return false;
            }
        }
        return true;
    }

    Container.prototype.render = function(renderer) {
        if (!this.visible || this.worldAlpha <= 0 || !this.renderable) return;
        /* A display object used as another's mask is not drawn itself; an
           object with a mask is drawn through it (see _renderMasked). */
        if (this.isMask) return;
        if (this._mask && !this._renderingMasked && _canRenderMasked(this._mask)) {
            this._renderMasked(renderer);
            return;
        }

        if (this.filters && this.filters.length > 0 &&
            !_filtersAreNoOp(this.filters) &&
            typeof __native_filters !== "undefined" &&
            typeof __native_renderer !== "undefined" && __native_renderer._active) {
            this._renderFiltered(renderer);
            return;
        }

        /* Apply filterArea scissor clipping (used by Window._clientArea). */
        var fa = this.filterArea;
        var useScissor = fa && fa.width > 0 && fa.height > 0 &&
            typeof __native_renderer !== "undefined" && __native_renderer._active;
        if (useScissor) {
            var screenH = renderer && renderer.screen ? renderer.screen.height : 624;
            __native_renderer.setScissor(
                Math.round(fa.x), Math.round(screenH - fa.y - fa.height),
                Math.round(fa.width), Math.round(fa.height)
            );
        }

        this._render(renderer);

        for (var i = 0; i < this.children.length; i++) {
            this.children[i].render(renderer);
        }

        if (useScissor) {
            __native_renderer.clearScissor();
        }
    };

    Container.prototype.renderAdvanced = function(renderer) {
        this.render(renderer);
    };

    Container.prototype.destroy = function(options) {
        DisplayObject.prototype.destroy.call(this);
        var destroyChildren = (typeof options === "boolean") ? options : (options && options.children);
        var oldChildren = this.removeChildren(0, this.children.length);
        if (destroyChildren) {
            for (var i = 0; i < oldChildren.length; i++) {
                oldChildren[i].destroy(options);
            }
        }
    };

    Container.prototype.containerUpdateTransform = Container.prototype.updateTransform;

    PIXI.Container = Container;

    /* PIXI.Sprite */

    function Sprite(texture) {
        Container.call(this);

        this._anchor = new ObservablePoint(this._onAnchorUpdate, this, 0, 0);
        this._texture = null;
        this._width = 0;
        this._height = 0;
        this._tint = null;
        this._tintRGB = null;
        this.tint = 0xFFFFFF;
        this.blendMode = PIXI.BLEND_MODES.NORMAL;
        this.pluginName = "batch";

        this._cachedTint = 0xFFFFFF;
        this.uvs = null;
        this.indices = null;

        this.vertexData = new Float32Array(8);
        this.vertexTrimmedData = null;
        this._transformID = -1;
        this._textureID = -1;
        this._transformTrimmedID = -1;
        this._textureTrimmedID = -1;

        this.isSprite = true;

        this.roundPixels = false;

        this.texture = texture || Texture.EMPTY;
    }

    Sprite.prototype = Object.create(Container.prototype);
    Sprite.prototype.constructor = Sprite;

    Object.defineProperty(Sprite.prototype, "anchor", {
        get: function() { return this._anchor; },
        set: function(v) { this._anchor.copyFrom(v); }
    });

    Object.defineProperty(Sprite.prototype, "tint", {
        get: function() { return this._tint; },
        set: function(v) {
            this._tint = v;
            this._tintRGB = (v >> 16) + (v & 0xff00) + ((v & 0xff) << 16);
        }
    });

    Object.defineProperty(Sprite.prototype, "texture", {
        get: function() { return this._texture; },
        set: function(v) {
            if (this._texture === v) return;
            if (this._texture) {
                this._texture.off("update", this._onTextureUpdate, this);
            }
            this._texture = v || Texture.EMPTY;
            this._cachedTint = 0xFFFFFF;
            this._textureID = -1;
            this._textureTrimmedID = -1;
            if (v) {
                if (v.baseTexture.valid) {
                    this._onTextureUpdate();
                } else {
                    v.once("update", this._onTextureUpdate, this);
                }
            }
        }
    });

    Object.defineProperty(Sprite.prototype, "width", {
        get: function() {
            return Math.abs(this.scale.x) * this._texture.orig.width;
        },
        set: function(v) {
            var s = PIXI.utils.sign(this.scale.x) || 1;
            this.scale.x = s * v / this._texture.orig.width;
            this._width = v;
        }
    });

    Object.defineProperty(Sprite.prototype, "height", {
        get: function() {
            return Math.abs(this.scale.y) * this._texture.orig.height;
        },
        set: function(v) {
            var s = PIXI.utils.sign(this.scale.y) || 1;
            this.scale.y = s * v / this._texture.orig.height;
            this._height = v;
        }
    });

    Sprite.prototype._onTextureUpdate = function() {
        this._textureID = -1;
        this._textureTrimmedID = -1;
        this._cachedTint = 0xFFFFFF;
        if (this._width) this.scale.x = PIXI.utils.sign(this.scale.x) * this._width / this._texture.orig.width;
        if (this._height) this.scale.y = PIXI.utils.sign(this.scale.y) * this._height / this._texture.orig.height;
    };

    Sprite.prototype._onAnchorUpdate = function() {
        this._transformID = -1;
        this._transformTrimmedID = -1;
    };

    Sprite.prototype.calculateVertices = function() {
        var texture = this._texture;
        if (this._transformID === this.transform._worldID && this._textureID === texture._updateID) return;
        this._transformID = this.transform._worldID;
        this._textureID = texture._updateID;

        var wt = this.transform.worldTransform;
        var a = wt.a, b = wt.b, c = wt.c, d = wt.d, tx = wt.tx, ty = wt.ty;
        var orig = texture.orig;
        var trim = texture.trim;

        var w0, w1, h0, h1;
        if (trim) {
            w1 = trim.x - (this._anchor._x * orig.width);
            w0 = w1 + trim.width;
            h1 = trim.y - (this._anchor._y * orig.height);
            h0 = h1 + trim.height;
        } else {
            w0 = orig.width * (1 - this._anchor._x);
            w1 = orig.width * -this._anchor._x;
            h0 = orig.height * (1 - this._anchor._y);
            h1 = orig.height * -this._anchor._y;
        }

        var vd = this.vertexData;
        vd[0] = a * w1 + c * h1 + tx;
        vd[1] = d * h1 + b * w1 + ty;
        vd[2] = a * w0 + c * h1 + tx;
        vd[3] = d * h1 + b * w0 + ty;
        vd[4] = a * w0 + c * h0 + tx;
        vd[5] = d * h0 + b * w0 + ty;
        vd[6] = a * w1 + c * h0 + tx;
        vd[7] = d * h0 + b * w1 + ty;

        if (this.roundPixels) {
            for (var i = 0; i < 8; i++) vd[i] = Math.round(vd[i]);
        }
    };

    Sprite.prototype.calculateTrimmedVertices = function() {
        if (!this.vertexTrimmedData) {
            this.vertexTrimmedData = new Float32Array(8);
        }
        var texture = this._texture;
        var vd = this.vertexTrimmedData;
        var orig = texture.orig;
        var anchor = this._anchor;

        var wt = this.transform.worldTransform;
        var a = wt.a, b = wt.b, c = wt.c, d = wt.d, tx = wt.tx, ty = wt.ty;

        var w0 = orig.width * (1 - anchor._x);
        var w1 = orig.width * -anchor._x;
        var h0 = orig.height * (1 - anchor._y);
        var h1 = orig.height * -anchor._y;

        vd[0] = a * w1 + c * h1 + tx;
        vd[1] = d * h1 + b * w1 + ty;
        vd[2] = a * w0 + c * h1 + tx;
        vd[3] = d * h1 + b * w0 + ty;
        vd[4] = a * w0 + c * h0 + tx;
        vd[5] = d * h0 + b * w0 + ty;
        vd[6] = a * w1 + c * h0 + tx;
        vd[7] = d * h0 + b * w1 + ty;
    };

    Sprite.prototype._calculateBounds = function() {
        var trim = this._texture.trim;
        var orig = this._texture.orig;
        if (!trim || (trim.width === orig.width && trim.height === orig.height)) {
            this.calculateVertices();
            this._bounds.addQuad(this.vertexData);
        } else {
            this.calculateTrimmedVertices();
            this._bounds.addQuad(this.vertexTrimmedData);
        }
    };

    Sprite.prototype.getLocalBounds = function(rect) {
        if (this.children.length === 0) {
            this._bounds.minX = this._texture.orig.width * -this._anchor._x;
            this._bounds.minY = this._texture.orig.height * -this._anchor._y;
            this._bounds.maxX = this._texture.orig.width * (1 - this._anchor._x);
            this._bounds.maxY = this._texture.orig.height * (1 - this._anchor._y);
            if (!rect) {
                if (!this._localBoundsRect) this._localBoundsRect = new Rectangle();
                rect = this._localBoundsRect;
            }
            return this._bounds.getRectangle(rect);
        }
        return Container.prototype.getLocalBounds.call(this, rect);
    };

    Sprite.prototype.containsPoint = function(point) {
        this.worldTransform.applyInverse(point, _tempPoint);
        var w = this._texture.orig.width;
        var h = this._texture.orig.height;
        var x1 = -w * this.anchor.x;
        var y1 = -h * this.anchor.y;
        if (_tempPoint.x >= x1 && _tempPoint.x < x1 + w) {
            if (_tempPoint.y >= y1 && _tempPoint.y < y1 + h) {
                return true;
            }
        }
        return false;
    };

    Sprite.prototype.render = function(renderer) {
        if (!this.visible || this.worldAlpha <= 0 || !this.renderable) return;
        /* A display object used as another's mask is not drawn itself; an
           object with a mask is drawn through it (see _renderMasked). */
        if (this.isMask) return;
        if (this._mask && !this._renderingMasked && _canRenderMasked(this._mask)) {
            this._renderMasked(renderer);
            return;
        }

        /* Apply filterArea scissor clipping (used by Window._clientArea). */
        var fa = this.filterArea;
        var useScissor = fa && fa.width > 0 && fa.height > 0 &&
            typeof __native_renderer !== "undefined" && __native_renderer._active;
        if (useScissor) {
            var screenH = renderer && renderer.screen ? renderer.screen.height : 624;
            __native_renderer.setScissor(
                Math.round(fa.x), Math.round(screenH - fa.y - fa.height),
                Math.round(fa.width), Math.round(fa.height)
            );
        }

        /* Filters come first: MV's scene root (Spriteset_Base._baseSprite, which carries the tone filter that every
           Tint Screen event uses) is a Sprite with no texture, and the filter must still apply to its children. */
        if (this.filters && this.filters.length > 0 &&
            !_filtersAreNoOp(this.filters) &&
            typeof __native_filters !== "undefined" &&
            typeof __native_renderer !== "undefined" && __native_renderer._active) {
            if (useScissor) __native_renderer.clearScissor();
            this._renderFiltered(renderer);
            return;
        }

        if (!this._texture || !this._texture.valid) {
            /* Still render children even if this sprite has no valid texture. */
            for (var i = 0; i < this.children.length; i++) {
                this.children[i].render(renderer);
            }
            if (useScissor) __native_renderer.clearScissor();
            return;
        }

        this._renderSelf(renderer);

        for (var k = 0; k < this.children.length; k++) {
            this.children[k].render(renderer);
        }

        if (useScissor) {
            __native_renderer.clearScissor();
        }
    };

    Sprite.prototype._renderSelf = function(renderer) {
        this.calculateVertices();

        if (typeof __native_renderer !== "undefined" && __native_renderer._active) {
            /* RPG Maker MV marks a bitmap dirty when something is drawn on it and expects the renderer to call checkDirty()
               (which updates the texture) when it draws the sprite; dialogue text appears letter by letter this way. */
            var dirtyBitmap = this._bitmap;
            if (dirtyBitmap && dirtyBitmap._dirty && typeof dirtyBitmap.checkDirty === "function") dirtyBitmap.checkDirty();
            var tex = this._texture;
            var bt = tex.baseTexture;
            var glTex = bt._glTexture || 0;

            /* Upload canvas-backed textures that are missing or dirty. */
            if ((glTex === 0 || bt._glTextureDirtyId !== bt.dirtyId) && (bt._canvas || bt.resource)) {
                bt._ensureGLTexture();
                glTex = bt._glTexture || 0;
            }

            /* Skip placeholder textures; they would draw as white quads. */
            if (glTex === 0) {
                return;
            }

            var vd = this.vertexData;
            var uvs = tex._uvs;
            __native_renderer.setBlendMode(this.blendMode);

            if (typeof __native_renderer.drawQuadVertices === "function") {
                /* Pass all four corners (TL, TR, BR, BL) so rotation, skew and
                   mirroring are preserved. */
                __native_renderer.drawQuadVertices(
                    glTex,
                    vd[0], vd[1], vd[2], vd[3], vd[4], vd[5], vd[6], vd[7],
                    uvs.x0, uvs.y0, uvs.x2, uvs.y2,
                    this._tint, this.worldAlpha
                );
            } else {
                /* Fallback: axis-aligned bounding box (no rotation). */
                var minX = vd[0], minY = vd[1], maxX = vd[0], maxY = vd[1];
                for (var j = 2; j < 8; j += 2) {
                    if (vd[j] < minX) minX = vd[j];
                    if (vd[j] > maxX) maxX = vd[j];
                    if (vd[j + 1] < minY) minY = vd[j + 1];
                    if (vd[j + 1] > maxY) maxY = vd[j + 1];
                }
                __native_renderer.drawQuad(
                    glTex,
                    minX, minY, maxX - minX, maxY - minY,
                    uvs.x0, uvs.y0, uvs.x2, uvs.y2,
                    this._tint, this.worldAlpha
                );
            }
        }
    };

    Sprite.prototype.destroy = function(options) {
        Container.prototype.destroy.call(this, options);
        this._anchor = null;
        var destroyTexture = (typeof options === "boolean") ? options : (options && options.texture);
        if (destroyTexture) {
            var destroyBaseTexture = (typeof options === "boolean") ? options : (options && options.baseTexture);
            this._texture.destroy(!!destroyBaseTexture);
        }
        this._texture = null;
    };

    Sprite.from = function(source, options) {
        var texture = (source instanceof Texture) ? source : Texture.from(source, options);
        return new Sprite(texture);
    };

    var _tempPoint = { x: 0, y: 0 };

    PIXI.Sprite = Sprite;

    /* PIXI.TilingSprite */

    function TilingSprite(texture, width, height) {
        Sprite.call(this, texture);

        this.tileTransform = new Transform();
        this._width = width || 100;
        this._height = height || 100;
        this.uvMatrix = null;
        this.pluginName = "tilingSprite";
        this.uvRespectAnchor = false;

        this.clampMargin = 0.5;

        this.tileScale = this.tileTransform.scale;
        this.tilePosition = this.tileTransform.position;
    }

    TilingSprite.prototype = Object.create(Sprite.prototype);
    TilingSprite.prototype.constructor = TilingSprite;

    Object.defineProperty(TilingSprite.prototype, "width", {
        get: function() { return this._width; },
        set: function(v) { this._width = v; }
    });

    Object.defineProperty(TilingSprite.prototype, "height", {
        get: function() { return this._height; },
        set: function(v) { this._height = v; }
    });

    TilingSprite.prototype._calculateBounds = function() {
        var minX = this._width * -this._anchor._x;
        var minY = this._height * -this._anchor._y;
        var maxX = this._width * (1 - this._anchor._x);
        var maxY = this._height * (1 - this._anchor._y);
        this._bounds.addFrameMatrix(this.worldTransform, minX, minY, maxX, maxY);
    };

    TilingSprite.prototype.getLocalBounds = function(rect) {
        if (this.children.length === 0) {
            this._bounds.minX = this._width * -this._anchor._x;
            this._bounds.minY = this._height * -this._anchor._y;
            this._bounds.maxX = this._width * (1 - this._anchor._x);
            this._bounds.maxY = this._height * (1 - this._anchor._y);
            if (!rect) {
                if (!this._localBoundsRect) this._localBoundsRect = new Rectangle();
                rect = this._localBoundsRect;
            }
            return this._bounds.getRectangle(rect);
        }
        return Sprite.prototype.getLocalBounds.call(this, rect);
    };

    TilingSprite.prototype.containsPoint = function(point) {
        this.worldTransform.applyInverse(point, _tempPoint);
        var w = this._width;
        var h = this._height;
        var x1 = -w * this.anchor.x;
        if (_tempPoint.x >= x1 && _tempPoint.x < x1 + w) {
            var y1 = -h * this.anchor.y;
            if (_tempPoint.y >= y1 && _tempPoint.y < y1 + h) return true;
        }
        return false;
    };

    TilingSprite.prototype.render = function(renderer) {
        if (!this.visible || this.worldAlpha <= 0 || !this.renderable) return;
        /* A display object used as another's mask is not drawn itself; an
           object with a mask is drawn through it (see _renderMasked). */
        if (this.isMask) return;
        if (this._mask && !this._renderingMasked && _canRenderMasked(this._mask)) {
            this._renderMasked(renderer);
            return;
        }
        if (!this._texture || !this._texture.valid) return;

        if (typeof __native_renderer !== "undefined" && __native_renderer._active) {
            /* RPG Maker MV marks a bitmap dirty when something is drawn on it and expects the renderer to call checkDirty()
               (which updates the texture) when it draws the sprite; dialogue text appears letter by letter this way. */
            var dirtyBitmap = this._bitmap;
            if (dirtyBitmap && dirtyBitmap._dirty && typeof dirtyBitmap.checkDirty === "function") dirtyBitmap.checkDirty();
            var tex = this._texture;
            var bt = tex.baseTexture;
            var glTex = bt._glTexture || 0;

            if ((glTex === 0 || bt._glTextureDirtyId !== bt.dirtyId) && (bt._canvas || bt.resource)) {
                bt._ensureGLTexture();
                glTex = bt._glTexture || 0;
            }

            if (glTex !== 0) {
                var wt = this.worldTransform;
                var spriteW = this._width;
                var spriteH = this._height;
                var ancX = this._anchor._x;
                var ancY = this._anchor._y;

                var tileW = tex._frame.width * this.tileTransform.scale._x;
                var tileH = tex._frame.height * this.tileTransform.scale._y;

                if (tileW > 0 && tileH > 0) {
                    var u0 = tex._uvs.x0, v0 = tex._uvs.y0;
                    var u1 = tex._uvs.x2, v1 = tex._uvs.y2;

                    var ox = -ancX * spriteW;
                    var oy = -ancY * spriteH;

                    __native_renderer.setBlendMode(this.blendMode);

                    /* Draw a tile grid unless the tile count is excessive; then stretch. */
                    var tilesX = Math.ceil(spriteW / tileW) + 1;
                    var tilesY = Math.ceil(spriteH / tileH) + 1;

                    if (tilesX * tilesY > 256) {
                        var sx = ox, sy = oy;
                        __native_renderer.drawQuad(
                            glTex,
                            wt.a * sx + wt.c * sy + wt.tx,
                            wt.b * sx + wt.d * sy + wt.ty,
                            spriteW * Math.abs(wt.a), spriteH * Math.abs(wt.d),
                            u0, v0, u1, v1,
                            this._tint, this.worldAlpha
                        );
                    } else {
                        /* Tile position offset (wrap into one-tile range). */
                        var offX = this.tileTransform.position._x % tileW;
                        var offY = this.tileTransform.position._y % tileH;
                        if (offX > 0) offX -= tileW;
                        if (offY > 0) offY -= tileH;

                        /* Draw a grid of tiles, clipping edge tiles to sprite bounds. */
                        for (var ty = offY; ty < spriteH; ty += tileH) {
                            for (var tx = offX; tx < spriteW; tx += tileW) {
                                var dx = Math.max(tx, 0);
                                var dy = Math.max(ty, 0);
                                var dw = Math.min(tx + tileW, spriteW) - dx;
                                var dh = Math.min(ty + tileH, spriteH) - dy;
                                if (dw <= 0 || dh <= 0) continue;

                                var cl = (dx - tx) / tileW;
                                var ct = (dy - ty) / tileH;
                                var cu0 = u0 + (u1 - u0) * cl;
                                var cv0 = v0 + (v1 - v0) * ct;
                                var cu1 = u0 + (u1 - u0) * (cl + dw / tileW);
                                var cv1 = v0 + (v1 - v0) * (ct + dh / tileH);

                                var lx = ox + dx, ly = oy + dy;
                                var wx = wt.a * lx + wt.c * ly + wt.tx;
                                var wy = wt.b * lx + wt.d * ly + wt.ty;

                                __native_renderer.drawQuad(
                                    glTex,
                                    wx, wy,
                                    dw * Math.abs(wt.a), dh * Math.abs(wt.d),
                                    cu0, cv0, cu1, cv1,
                                    this._tint, this.worldAlpha
                                );
                            }
                        }
                    }
                }
            }
        }

        for (var i = 0; i < this.children.length; i++) {
            this.children[i].render(renderer);
        }
    };

    TilingSprite.prototype.destroy = function(options) {
        Sprite.prototype.destroy.call(this, options);
        this.tileTransform = null;
        this.uvMatrix = null;
    };

    TilingSprite.from = function(source, options) {
        var texture = (source instanceof Texture) ? source : Texture.from(source);
        return new TilingSprite(texture,
            options ? options.width : texture.width,
            options ? options.height : texture.height);
    };

    PIXI.TilingSprite = TilingSprite;

    /* PIXI.Graphics */

    function Graphics(geometry) {
        Container.call(this);

        this._geometry = geometry || null;
        this._fillStyle = { color: 0xFFFFFF, alpha: 1, visible: false, texture: Texture.WHITE };
        this._lineStyle = { color: 0x0, alpha: 1, visible: false, width: 0, alignment: 0.5, native: false, cap: "butt", join: "miter", miterLimit: 10 };

        this.tint = 0xFFFFFF;
        this.blendMode = PIXI.BLEND_MODES.NORMAL;

        this._currentPath = null;
        this._spriteRect = null;
        this._matrix = null;
        this._holeMode = false;

        this.state = { blend: true };
        this.pluginName = "batch";

        /* Internal draw commands list for deferred rendering. */
        this._commands = [];
        this._dirty = 0;
        this.batchDirty = -1;
        this.batches = [];
        this.vertexData = null;

        this.currentPath = null;
    }

    Graphics.prototype = Object.create(Container.prototype);
    Graphics.prototype.constructor = Graphics;

    Graphics.prototype.clone = function() {
        var g = new Graphics();
        g._commands = this._commands.slice(0);
        g._fillStyle = { color: this._fillStyle.color, alpha: this._fillStyle.alpha, visible: this._fillStyle.visible, texture: this._fillStyle.texture, matrix: this._fillStyle.matrix };
        g._lineStyle = { color: this._lineStyle.color, alpha: this._lineStyle.alpha, visible: this._lineStyle.visible, width: this._lineStyle.width, alignment: this._lineStyle.alignment, native: this._lineStyle.native, cap: this._lineStyle.cap, join: this._lineStyle.join, miterLimit: this._lineStyle.miterLimit };
        return g;
    };

    Graphics.prototype.lineStyle = function(width, color, alpha, alignment, native) {
        if (typeof width === "object") {
            var opts = width;
            width = opts.width; color = opts.color; alpha = opts.alpha;
            alignment = opts.alignment; native = opts.native;
        }
        this._lineStyle.width = width !== undefined ? width : 0;
        this._lineStyle.color = color !== undefined ? color : 0x0;
        this._lineStyle.alpha = alpha !== undefined ? alpha : 1;
        this._lineStyle.alignment = alignment !== undefined ? alignment : 0.5;
        this._lineStyle.native = !!native;
        this._lineStyle.visible = this._lineStyle.width > 0 && this._lineStyle.alpha > 0;
        if (this.currentPath) {
            if (this.currentPath.points.length <= 2) {
                this.currentPath.lineStyle = {
                    width: this._lineStyle.width,
                    color: this._lineStyle.color,
                    alpha: this._lineStyle.alpha
                };
            }
        }
        return this;
    };

    Graphics.prototype.beginFill = function(color, alpha) {
        this._fillStyle.color = color !== undefined ? color : 0xFFFFFF;
        this._fillStyle.alpha = alpha !== undefined ? alpha : 1;
        this._fillStyle.visible = this._fillStyle.alpha > 0;
        this._fillStyle.texture = Texture.WHITE;
        this._fillStyle.matrix = null;
        if (this.currentPath) {
            this.startPoly();
        }
        return this;
    };

    /* beginTextureFill({texture, color, alpha, matrix}): fill shapes with a
       texture. As in PIXI, `matrix` maps texture space to local space; its
       inverse turns each vertex into texture coordinates. */
    Graphics.prototype.beginTextureFill = function(options) {
        options = options || {};
        var texture = options.texture || Texture.WHITE;
        this._fillStyle.color = options.color !== undefined ? options.color : 0xFFFFFF;
        this._fillStyle.alpha = options.alpha !== undefined ? options.alpha : 1;
        this._fillStyle.visible = this._fillStyle.alpha > 0;
        this._fillStyle.texture = texture;
        this._fillStyle.matrix = options.matrix ? options.matrix.clone().invert() : null;
        if (this.currentPath) {
            this.startPoly();
        }
        return this;
    };

    Graphics.prototype.endFill = function() {
        this.finishPoly();
        this._fillStyle.visible = false;
        this._fillStyle.texture = Texture.WHITE;
        this._fillStyle.matrix = null;
        return this;
    };

    /* Snapshot of the fill style for a draw command. */
    Graphics.prototype._fillSnapshot = function() {
        var f = this._fillStyle;
        var snap = { color: f.color, alpha: f.alpha, visible: f.visible };
        if (f.texture && f.texture !== Texture.WHITE) {
            snap.texture = f.texture;
            snap.matrix = f.matrix;
        }
        return snap;
    };

    Graphics.prototype.drawRect = function(x, y, width, height) {
        this._commands.push({ type: "rect", x: x, y: y, width: width, height: height,
            fill: this._fillSnapshot(),
            line: { width: this._lineStyle.width, color: this._lineStyle.color, alpha: this._lineStyle.alpha, visible: this._lineStyle.visible }
        });
        this._dirty++;
        return this;
    };

    Graphics.prototype.drawRoundedRect = function(x, y, width, height, radius) {
        this._commands.push({ type: "roundedRect", x: x, y: y, width: width, height: height,
            radius: (radius === undefined || radius === null) ? 20 : radius,  /* PIXI default */
            fill: this._fillSnapshot(),
            line: { width: this._lineStyle.width, color: this._lineStyle.color, alpha: this._lineStyle.alpha, visible: this._lineStyle.visible }
        });
        this._dirty++;
        return this;
    };

    Graphics.prototype.drawCircle = function(x, y, radius) {
        this._commands.push({ type: "circle", x: x, y: y, radius: radius,
            fill: this._fillSnapshot(),
            line: { width: this._lineStyle.width, color: this._lineStyle.color, alpha: this._lineStyle.alpha, visible: this._lineStyle.visible }
        });
        this._dirty++;
        return this;
    };

    Graphics.prototype.drawEllipse = function(x, y, width, height) {
        this._commands.push({ type: "ellipse", x: x, y: y, width: width, height: height,
            fill: this._fillSnapshot(),
            line: { width: this._lineStyle.width, color: this._lineStyle.color, alpha: this._lineStyle.alpha, visible: this._lineStyle.visible }
        });
        this._dirty++;
        return this;
    };

    Graphics.prototype.drawPolygon = function(path) {
        var points;
        if (Array.isArray(path)) {
            points = path.slice();
        } else if (path && path.points) {
            points = path.points.slice();
        } else {
            points = [];
            for (var i = 0; i < arguments.length; i++) {
                if (typeof arguments[i] === "number") {
                    points.push(arguments[i]);
                } else {
                    points.push(arguments[i].x, arguments[i].y);
                }
            }
        }
        this._commands.push({ type: "polygon", points: points,
            fill: this._fillSnapshot(),
            line: { width: this._lineStyle.width, color: this._lineStyle.color, alpha: this._lineStyle.alpha, visible: this._lineStyle.visible }
        });
        this._dirty++;
        return this;
    };

    Graphics.prototype.moveTo = function(x, y) {
        this.startPoly();
        this.currentPath = { points: [x, y], lineStyle: { width: this._lineStyle.width, color: this._lineStyle.color, alpha: this._lineStyle.alpha } };
        return this;
    };

    Graphics.prototype.lineTo = function(x, y) {
        if (!this.currentPath) {
            this.moveTo(0, 0);
        }
        this.currentPath.points.push(x, y);
        this._dirty++;
        return this;
    };

    Graphics.prototype.closePath = function() {
        this.finishPoly();
        return this;
    };

    Graphics.prototype.startPoly = function() {
        if (this.currentPath) {
            var points = this.currentPath.points;
            if (points.length > 2) {
                this._commands.push({ type: "polyline", points: points,
                    line: { width: this.currentPath.lineStyle.width, color: this.currentPath.lineStyle.color, alpha: this.currentPath.lineStyle.alpha, visible: this.currentPath.lineStyle.width > 0 },
                    fill: this._fillSnapshot()
                });
            }
        }
        this.currentPath = null;
    };

    Graphics.prototype.finishPoly = function() {
        this.startPoly();
    };

    Graphics.prototype.clear = function() {
        this._commands = [];
        this._dirty++;
        this._fillStyle.visible = false;
        this._lineStyle.visible = false;
        this._lineStyle.width = 0;
        this.currentPath = null;
        return this;
    };

    /* Graphics tessellation: the native renderer only draws quads, and
       collapsing one corner of drawQuadVertices yields a triangle, so shapes
       and strokes are tessellated here. */

    function _gxf(wt, px, py) {
        return [px * wt.a + py * wt.c + wt.tx, px * wt.b + py * wt.d + wt.ty];
    }

    /* Texture fill state for one command: the GL texture plus a function
       turning a local point into texture coordinates. Null when the fill
       is a plain colour or the texture has nothing uploaded yet. */
    function _gTextureFill(fill) {
        if (!fill || !fill.texture) return null;
        var tex = fill.texture;
        var bt = tex.baseTexture;
        if (!bt) return null;
        var glTex = bt._glTexture || 0;
        if ((glTex === 0 || bt._glTextureDirtyId !== bt.dirtyId) &&
            (bt._canvas || bt.resource) && bt._ensureGLTexture) {
            bt._ensureGLTexture();
            glTex = bt._glTexture || 0;
        }
        if (glTex === 0 || !(bt.width > 0) || !(bt.height > 0)) return null;
        var frame = tex._frame || tex.frame || { x: 0, y: 0 };
        var m = fill.matrix;
        var bw = bt.width, bh = bt.height;
        return {
            gl: glTex,
            uv: function(x, y) {
                var tx = x, ty = y;
                if (m) {
                    tx = m.a * x + m.c * y + m.tx;
                    ty = m.b * x + m.d * y + m.ty;
                }
                return [(frame.x + tx) / bw, (frame.y + ty) / bh];
            }
        };
    }

    /* Emit a triangle (a,b,c) in local space, transformed by wt. */
    function _gTri(wt, ax, ay, bx, by, cx, cy, color, alpha, tex) {
        var A = _gxf(wt, ax, ay), B = _gxf(wt, bx, by), C = _gxf(wt, cx, cy);
        if (tex) {
            var ua = tex.uv(ax, ay), ub = tex.uv(bx, by), uc = tex.uv(cx, cy);
            __native_renderer.drawQuadVerticesUV(
                tex.gl, A[0], A[1], B[0], B[1], C[0], C[1], C[0], C[1],
                ua[0], ua[1], ub[0], ub[1], uc[0], uc[1], uc[0], uc[1], color, alpha);
            return;
        }
        __native_renderer.drawQuadVertices(
            0, A[0], A[1], B[0], B[1], C[0], C[1], C[0], C[1],
            0, 0, 1, 1, color, alpha);
    }

    /* Emit a filled quad from four local corners (TL, TR, BR, BL). */
    function _gQuad(wt, x0, y0, x1, y1, x2, y2, x3, y3, color, alpha, tex) {
        var A = _gxf(wt, x0, y0), B = _gxf(wt, x1, y1),
            C = _gxf(wt, x2, y2), D = _gxf(wt, x3, y3);
        if (tex) {
            var u0 = tex.uv(x0, y0), u1 = tex.uv(x1, y1), u2 = tex.uv(x2, y2), u3 = tex.uv(x3, y3);
            __native_renderer.drawQuadVerticesUV(
                tex.gl, A[0], A[1], B[0], B[1], C[0], C[1], D[0], D[1],
                u0[0], u0[1], u1[0], u1[1], u2[0], u2[1], u3[0], u3[1], color, alpha);
            return;
        }
        __native_renderer.drawQuadVertices(
            0, A[0], A[1], B[0], B[1], C[0], C[1], D[0], D[1],
            0, 0, 1, 1, color, alpha);
    }

    function _gEllipsePoints(cx, cy, rx, ry, segs) {
        var pts = [];
        for (var i = 0; i < segs; i++) {
            var a = (i / segs) * Math.PI * 2;
            pts.push([cx + Math.cos(a) * rx, cy + Math.sin(a) * ry]);
        }
        return pts;
    }

    function _gEllipseSegs(rx, ry) {
        return Math.max(12, Math.min(64, Math.ceil(Math.max(Math.abs(rx), Math.abs(ry)) * 0.5)));
    }

    /* Fill a closed loop of points as a fan around (cx,cy). */
    function _gFillFan(wt, cx, cy, pts, color, alpha, tex) {
        for (var i = 0; i < pts.length; i++) {
            var p0 = pts[i], p1 = pts[(i + 1) % pts.length];
            _gTri(wt, cx, cy, p0[0], p0[1], p1[0], p1[1], color, alpha, tex);
        }
    }

    /* Fill a simple (convex) polygon as a fan from the first vertex. */
    function _gFillPolygon(wt, pts, color, alpha, tex) {
        for (var i = 1; i < pts.length - 1; i++) {
            _gTri(wt, pts[0][0], pts[0][1], pts[i][0], pts[i][1],
                  pts[i + 1][0], pts[i + 1][1], color, alpha, tex);
        }
    }

    /* Stroke a single segment as a rectangle of the given width. */
    function _gStrokeSeg(wt, ax, ay, bx, by, width, color, alpha) {
        var dx = bx - ax, dy = by - ay;
        var len = Math.sqrt(dx * dx + dy * dy);
        if (len === 0) return;
        var nx = -dy / len * (width / 2);
        var ny = dx / len * (width / 2);
        _gQuad(wt, ax + nx, ay + ny, bx + nx, by + ny,
               bx - nx, by - ny, ax - nx, ay - ny, color, alpha);
    }

    /* Stroke a polyline (optionally closed). */
    function _gStrokePath(wt, pts, closed, width, color, alpha) {
        for (var i = 0; i < pts.length - 1; i++) {
            _gStrokeSeg(wt, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], width, color, alpha);
        }
        if (closed && pts.length > 2) {
            var n = pts.length - 1;
            _gStrokeSeg(wt, pts[n][0], pts[n][1], pts[0][0], pts[0][1], width, color, alpha);
        }
    }

    function _gArcFan(wt, cx, cy, r, a0, a1, segs, color, alpha, tex) {
        var prev = null;
        for (var i = 0; i <= segs; i++) {
            var a = a0 + (a1 - a0) * (i / segs);
            var px = cx + Math.cos(a) * r, py = cy + Math.sin(a) * r;
            if (prev) _gTri(wt, cx, cy, prev[0], prev[1], px, py, color, alpha, tex);
            prev = [px, py];
        }
    }

    /* Fill a rounded rect via a non-overlapping decomposition (middle band +
       left/right strips + four corner arcs) so alpha blending stays correct. */
    function _gFillRoundedRect(wt, x, y, w, h, r, color, alpha, tex) {
        r = Math.min(r, w / 2, h / 2);
        if (r <= 0) {
            _gQuad(wt, x, y, x + w, y, x + w, y + h, x, y + h, color, alpha, tex);
            return;
        }
        _gQuad(wt, x + r, y, x + w - r, y, x + w - r, y + h, x + r, y + h, color, alpha, tex);
        _gQuad(wt, x, y + r, x + r, y + r, x + r, y + h - r, x, y + h - r, color, alpha, tex);
        _gQuad(wt, x + w - r, y + r, x + w, y + r, x + w, y + h - r, x + w - r, y + h - r, color, alpha, tex);
        var segs = Math.max(4, Math.min(16, Math.ceil(r * 0.5)));
        _gArcFan(wt, x + r,     y + r,     r, Math.PI,       Math.PI * 1.5, segs, color, alpha, tex);
        _gArcFan(wt, x + w - r, y + r,     r, Math.PI * 1.5, Math.PI * 2,   segs, color, alpha, tex);
        _gArcFan(wt, x + w - r, y + h - r, r, 0,             Math.PI * 0.5, segs, color, alpha, tex);
        _gArcFan(wt, x + r,     y + h - r, r, Math.PI * 0.5, Math.PI,       segs, color, alpha, tex);
    }

    function _gRectCorners(cmd) {
        return [[cmd.x, cmd.y], [cmd.x + cmd.width, cmd.y],
                [cmd.x + cmd.width, cmd.y + cmd.height], [cmd.x, cmd.y + cmd.height]];
    }

    Graphics.prototype.render = function(renderer) {
        if (!this.visible || this.worldAlpha <= 0 || !this.renderable) return;
        /* A display object used as another's mask is not drawn itself; an
           object with a mask is drawn through it (see _renderMasked). */
        if (this.isMask) return;
        if (this._mask && !this._renderingMasked && _canRenderMasked(this._mask)) {
            this._renderMasked(renderer);
            return;
        }

        if (typeof __native_renderer !== "undefined" && __native_renderer._active &&
            typeof __native_renderer.drawQuadVertices === "function") {
            __native_renderer.setBlendMode(this.blendMode);
            var wt = this.worldTransform;
            var wa = this.worldAlpha;
            for (var i = 0; i < this._commands.length; i++) {
                var cmd = this._commands[i];
                var fill = cmd.fill, line = cmd.line;
                var fa = fill ? fill.alpha * wa : 0;
                var la = line ? line.alpha * wa : 0;
                var tex = (fill && fill.visible) ? _gTextureFill(fill) : null;
                var pts, k;

                if (cmd.type === "rect") {
                    if (fill && fill.visible) {
                        _gQuad(wt, cmd.x, cmd.y, cmd.x + cmd.width, cmd.y,
                               cmd.x + cmd.width, cmd.y + cmd.height, cmd.x, cmd.y + cmd.height,
                               fill.color, fa, tex);
                    }
                    if (line && line.visible) {
                        _gStrokePath(wt, _gRectCorners(cmd), true, line.width, line.color, la);
                    }
                } else if (cmd.type === "roundedRect") {
                    if (fill && fill.visible) {
                        _gFillRoundedRect(wt, cmd.x, cmd.y, cmd.width, cmd.height, cmd.radius, fill.color, fa, tex);
                    }
                    if (line && line.visible) {
                        _gStrokePath(wt, _gRectCorners(cmd), true, line.width, line.color, la);
                    }
                } else if (cmd.type === "circle" || cmd.type === "ellipse") {
                    var rx = (cmd.type === "circle") ? cmd.radius : cmd.width;
                    var ry = (cmd.type === "circle") ? cmd.radius : cmd.height;
                    var segs = _gEllipseSegs(rx, ry);
                    pts = _gEllipsePoints(cmd.x, cmd.y, rx, ry, segs);
                    if (fill && fill.visible) _gFillFan(wt, cmd.x, cmd.y, pts, fill.color, fa, tex);
                    if (line && line.visible) _gStrokePath(wt, pts, true, line.width, line.color, la);
                } else if (cmd.type === "polygon") {
                    pts = [];
                    for (k = 0; k + 1 < cmd.points.length; k += 2) pts.push([cmd.points[k], cmd.points[k + 1]]);
                    if (pts.length >= 3) {
                        if (fill && fill.visible) _gFillPolygon(wt, pts, fill.color, fa, tex);
                        if (line && line.visible) _gStrokePath(wt, pts, true, line.width, line.color, la);
                    }
                } else if (cmd.type === "polyline") {
                    pts = [];
                    for (k = 0; k + 1 < cmd.points.length; k += 2) pts.push([cmd.points[k], cmd.points[k + 1]]);
                    if (pts.length >= 2 && line && line.visible) {
                        _gStrokePath(wt, pts, false, line.width, line.color, la);
                    }
                }
            }
        }

        for (var j = 0; j < this.children.length; j++) {
            this.children[j].render(renderer);
        }
    };

    Graphics.prototype._calculateBounds = function() {
        for (var i = 0; i < this._commands.length; i++) {
            var cmd = this._commands[i];
            if (cmd.type === "rect") {
                this._bounds.addFrameMatrix(this.worldTransform, cmd.x, cmd.y,
                    cmd.x + cmd.width, cmd.y + cmd.height);
            } else if (cmd.type === "circle") {
                this._bounds.addFrameMatrix(this.worldTransform,
                    cmd.x - cmd.radius, cmd.y - cmd.radius,
                    cmd.x + cmd.radius, cmd.y + cmd.radius);
            }
        }
    };

    Graphics.prototype.destroy = function(options) {
        Container.prototype.destroy.call(this, options);
        this._commands = null;
        this._fillStyle = null;
        this._lineStyle = null;
        this._geometry = null;
    };

    PIXI.Graphics = Graphics;

    /* PIXI.Text: renders text to an off-screen canvas used as a sprite texture. */

    var _textStyleDefaults = {
        fontFamily: "Arial",
        fontSize: 26,
        fontStyle: "normal",
        fontWeight: "normal",
        fill: "black",
        stroke: "black",
        strokeThickness: 0,
        dropShadow: false,
        dropShadowColor: "black",
        dropShadowBlur: 0,
        dropShadowAngle: Math.PI / 6,
        dropShadowDistance: 5,
        wordWrap: false,
        wordWrapWidth: 100,
        align: "left",
        padding: 0,
        trim: false,
        lineHeight: 0,
        lineJoin: "miter",
        miterLimit: 10,
        leading: 0,
    };

    function TextStyle(style) {
        style = style || {};
        for (var key in _textStyleDefaults) {
            if (_textStyleDefaults.hasOwnProperty(key)) {
                this[key] = (style[key] !== undefined) ? style[key] : _textStyleDefaults[key];
            }
        }
    }

    TextStyle.prototype.clone = function() {
        var s = new TextStyle();
        for (var key in _textStyleDefaults) {
            if (_textStyleDefaults.hasOwnProperty(key)) {
                s[key] = this[key];
            }
        }
        return s;
    };

    TextStyle.prototype.toFontString = function() {
        var parts = [];
        if (this.fontStyle && this.fontStyle !== "normal") parts.push(this.fontStyle);
        if (this.fontWeight && this.fontWeight !== "normal") parts.push(this.fontWeight);
        parts.push((typeof this.fontSize === "number" ? this.fontSize + "px" : this.fontSize));
        parts.push(this.fontFamily);
        return parts.join(" ");
    };

    PIXI.TextStyle = TextStyle;

    function Text(text, style, canvas) {
        Sprite.call(this, Texture.EMPTY);

        this._text = String(text || "");
        this._style = (style instanceof TextStyle) ? style : new TextStyle(style);
        this._canvas = canvas || null;
        this._context = null;
        this._resolution = 1;
        this._autoResolution = true;
        this.dirty = true;

        this._generateCanvas();
    }

    Text.prototype = Object.create(Sprite.prototype);
    Text.prototype.constructor = Text;

    Object.defineProperty(Text.prototype, "text", {
        get: function() { return this._text; },
        set: function(v) {
            v = String(v || "");
            if (this._text === v) return;
            this._text = v;
            this.dirty = true;
        }
    });

    Object.defineProperty(Text.prototype, "style", {
        get: function() { return this._style; },
        set: function(v) {
            this._style = (v instanceof TextStyle) ? v : new TextStyle(v);
            this.dirty = true;
        }
    });

    Object.defineProperty(Text.prototype, "resolution", {
        get: function() { return this._resolution; },
        set: function(v) {
            this._autoResolution = false;
            if (this._resolution === v) return;
            this._resolution = v;
            this.dirty = true;
        }
    });

    Text.prototype._generateCanvas = function() {
        if (!this._canvas) {
            if (typeof document !== "undefined") {
                this._canvas = document.createElement("canvas");
            } else {
                return;
            }
        }
        this._context = this._canvas.getContext("2d");
    };

    Text.prototype.updateText = function(respectDirty) {
        if (respectDirty && !this.dirty) return;

        var style = this._style;
        var ctx = this._context;
        if (!ctx) return;

        var fontString = style.toFontString();
        ctx.font = fontString;

        var lines = this._text.split(/\n/);
        var lineHeight = style.lineHeight || (style.fontSize * 1.2);
        var maxWidth = 0;

        for (var i = 0; i < lines.length; i++) {
            var m = ctx.measureText(lines[i]);
            if (m.width > maxWidth) maxWidth = m.width;
        }

        var totalHeight = lineHeight * lines.length;
        var padding = style.padding || 0;

        var cw = Math.ceil(maxWidth + padding * 2 + style.strokeThickness * 2) || 1;
        var ch = Math.ceil(totalHeight + padding * 2 + style.strokeThickness * 2) || 1;

        this._canvas.width = cw;
        this._canvas.height = ch;

        /* Re-set font after canvas resize (browser clears context state). */
        ctx.font = fontString;
        ctx.textBaseline = "alphabetic";

        ctx.clearRect(0, 0, cw, ch);

        var x = padding + style.strokeThickness;
        var y = padding + style.strokeThickness + (style.fontSize * 0.85);

        for (var j = 0; j < lines.length; j++) {
            if (style.strokeThickness > 0 && style.stroke) {
                ctx.strokeStyle = style.stroke;
                ctx.lineWidth = style.strokeThickness;
                ctx.strokeText(lines[j], x, y);
            }
            if (style.fill) {
                ctx.fillStyle = (typeof style.fill === "object" && style.fill[0]) ? style.fill[0] : style.fill;
                ctx.fillText(lines[j], x, y);
            }
            y += lineHeight;
        }

        this._updateTexture();
        this.dirty = false;
    };

    Text.prototype._updateTexture = function() {
        if (!this._canvas) return;

        var canvas = this._canvas;
        var bt = this._texture.baseTexture;

        bt.width = canvas.width;
        bt.height = canvas.height;
        bt.realWidth = canvas.width;
        bt.realHeight = canvas.height;
        bt.valid = true;

        bt._canvas = canvas;

        this._texture._frame.width = canvas.width;
        this._texture._frame.height = canvas.height;
        this._texture.orig.width = canvas.width;
        this._texture.orig.height = canvas.height;
        this._texture.trim = null;
        this._texture.valid = true;
        this._texture.update();

        this._width = canvas.width;
        this._height = canvas.height;
    };

    Text.prototype.render = function(renderer) {
        if (this.dirty) this.updateText(false);
        Sprite.prototype.render.call(this, renderer);
    };

    Text.prototype.destroy = function(options) {
        Sprite.prototype.destroy.call(this, options);
        this._canvas = null;
        this._context = null;
        this._style = null;
    };

    Text.prototype.getLocalBounds = function(rect) {
        this.updateText(true);
        return Sprite.prototype.getLocalBounds.call(this, rect);
    };

    PIXI.Text = Text;

    /* PIXI.Ticker */

    function Ticker() {
        this._callbacks = [];    /* { fn, context, once } */
        this.autoStart = true;
        this.deltaTime = 1;
        this.deltaMS = 1000 / 60;
        this.elapsedMS = 1000 / 60;
        this.lastTime = -1;
        this.speed = 1;
        this.started = false;
        this.FPS = 60;
        this.minFPS = 10;
        this.maxFPS = 0;

        this._requestId = null;
        this._maxElapsedMS = 100;
        this._minElapsedMS = 0;

        var self = this;
        this._tick = function(time) {
            self._requestId = null;
            if (self.started) {
                self.update(time);
                if (self.started && self._requestId === null && self._callbacks.length > 0) {
                    self._requestId = requestAnimationFrame(self._tick);
                }
            }
        };
    }

    Ticker.prototype.add = function(fn, context, priority) {
        context = context || null;
        for (var i = 0; i < this._callbacks.length; i++) {
            if (this._callbacks[i].fn === fn && this._callbacks[i].context === context) {
                return this;
            }
        }
        this._callbacks.push({ fn: fn, context: context, once: false });
        if (this.started && this._requestId === null) {
            this._requestId = requestAnimationFrame(this._tick);
        }
        return this;
    };

    Ticker.prototype.addOnce = function(fn, context, priority) {
        context = context || null;
        this._callbacks.push({ fn: fn, context: context, once: true });
        if (this.started && this._requestId === null) {
            this._requestId = requestAnimationFrame(this._tick);
        }
        return this;
    };

    Ticker.prototype.remove = function(fn, context) {
        context = context || null;
        for (var i = this._callbacks.length - 1; i >= 0; i--) {
            var cb = this._callbacks[i];
            if (cb.fn === fn && cb.context === context) {
                this._callbacks.splice(i, 1);
            }
        }
        return this;
    };

    Ticker.prototype.start = function() {
        if (!this.started) {
            this.started = true;
            this.lastTime = -1;
            this._requestId = requestAnimationFrame(this._tick);
        }
    };

    Ticker.prototype.stop = function() {
        if (this.started) {
            this.started = false;
            if (this._requestId !== null) {
                cancelAnimationFrame(this._requestId);
                this._requestId = null;
            }
        }
    };

    Ticker.prototype.update = function(currentTime) {
        if (currentTime === undefined) {
            currentTime = (typeof performance !== "undefined") ? performance.now() : Date.now();
        }

        if (this.lastTime < 0) {
            this.lastTime = currentTime;
        }

        var elapsedMS = currentTime - this.lastTime;
        if (elapsedMS > this._maxElapsedMS) {
            elapsedMS = this._maxElapsedMS;
        }
        if (this._minElapsedMS > 0 && elapsedMS < this._minElapsedMS) {
            return;
        }

        this.deltaMS = elapsedMS;
        this.deltaTime = (elapsedMS / (1000 / 60)) * this.speed;
        this.elapsedMS = elapsedMS;
        this.lastTime = currentTime;

        if (elapsedMS > 0) {
            this.FPS = 1000 / elapsedMS;
        }

        var cbs = this._callbacks.slice();
        var toRemove = [];
        for (var i = 0; i < cbs.length; i++) {
            var cb = cbs[i];
            cb.fn.call(cb.context, this.deltaTime);
            if (cb.once) toRemove.push(cb);
        }
        for (var j = 0; j < toRemove.length; j++) {
            var idx = this._callbacks.indexOf(toRemove[j]);
            if (idx >= 0) this._callbacks.splice(idx, 1);
        }
    };

    Ticker.prototype.destroy = function() {
        this.stop();
        this._callbacks = [];
    };

    Object.defineProperty(Ticker, "shared", {
        get: function() {
            if (!Ticker._shared) {
                Ticker._shared = new Ticker();
                Ticker._shared.autoStart = true;
                /* PIXI's shared ticker runs from creation, but _tick only re-arms
                   rAF while listeners exist; pin a no-op listener so elapsedMS
                   stays current for plugins that read it. */
                Ticker._shared.add(function() {}, null);
                Ticker._shared.start();
            }
            return Ticker._shared;
        }
    });

    Object.defineProperty(Ticker, "system", {
        get: function() {
            if (!Ticker._system) {
                Ticker._system = new Ticker();
                Ticker._system.autoStart = true;
                Ticker._system.add(function() {}, null);
                Ticker._system.start();
            }
            return Ticker._system;
        }
    });

    PIXI.Ticker = Ticker;

    /* PIXI.Renderer (wraps __native_renderer) */

    function PixiRenderer(options) {
        EventEmitter.call(this);

        options = options || {};
        this.type = 1; /* RENDERER_TYPE.WEBGL */
        this.view = options.view || null;
        this.screen = new Rectangle(0, 0, options.width || 800, options.height || 600);
        this.resolution = options.resolution || PIXI.settings.RESOLUTION;
        this.transparent = !!options.transparent;
        this.autoDensity = !!options.autoDensity;
        this.preserveDrawingBuffer = !!options.preserveDrawingBuffer;
        this.clearBeforeRender = options.clearBeforeRender !== false;
        this.backgroundColor = options.backgroundColor !== undefined ? options.backgroundColor : 0x000000;
        this._backgroundColorRgba = [0, 0, 0, 1];
        this._backgroundColorString = "#000000";
        this._lastObjectRendered = null;

        this._updateBackgroundColor();

        if (typeof __native_renderer !== "undefined") {
            var initResult = __native_renderer.init(this.screen.width, this.screen.height);
            if (!initResult) console.log("[WARN] __native_renderer.init failed for " + this.screen.width + "x" + this.screen.height);
        }
        /* Filter system needs the GL context, so it is initialized after the renderer. */
        if (typeof __native_filters !== "undefined" && __native_filters.init) {
            __native_filters.init();
        }

        /* Stub GL context for Tilemap.Renderer and effekseer compatibility. */
        this.gl = {
            canvas: this.view,
            TRIANGLES: 4,
            RGBA: 6408,
            UNSIGNED_BYTE: 5121,
            TEXTURE_2D: 3553,
            UNPACK_PREMULTIPLY_ALPHA_WEBGL: 37441,
            STENCIL_TEST: 0x0B90,
            STENCIL_BUFFER_BIT: 0x0400,
            EQUAL: 0x0202,
            ALWAYS: 0x0207,
            KEEP: 0x1E00,
            REPLACE: 0x1E01,
            INCR: 0x1E02,
            ZERO: 0,
            ONE: 1,
            SRC_ALPHA: 0x0302,
            ONE_MINUS_SRC_ALPHA: 0x0303,
            enable: function() {},
            disable: function() {},
            stencilFunc: function() {},
            stencilOp: function() {},
            stencilMask: function() {},
            clearStencil: function() {},
            clear: function() {},
            blendFunc: function() {},
            pixelStorei: function() {},
            texSubImage2D: function() {}
        };

        /* Renderer subsystems (stubs for Tilemap.Layer/Renderer compatibility). */
        var self = this;
        this.batch = {
            currentRenderer: null,
            setObjectRenderer: function(renderer) {
                this.currentRenderer = renderer;
            },
            flush: function() {}
        };
        this.framebuffer = {
            forceStencil: function() {},
            bind: function() {}
        };
        this.projection = {
            projectionMatrix: new Matrix()
        };
        this.shader = {
            bind: function(shader) {}
        };
        this.geometry = {
            bind: function(vao, shader) {},
            updateBuffers: function() {},
            draw: function(type, count, start) {}
        };
        this.texture = {
            bind: function(texture, location) {}
        };
        this.state = {
            set: function(state) {}
        };

        /* Extract plugin — used by Bitmap.snap(). */
        this.plugins = {
            extract: {
                canvas: function(target) {
                    return self._extractCanvas(target);
                },
                pixels: function(target) {
                    return self._extractPixels(target);
                },
                destroy: function() {}
            }
        };

        /* Initialize registered plugins (e.g., rpgtilemap from Tilemap.Renderer). */
        for (var pluginName in PixiRenderer._plugins) {
            if (PixiRenderer._plugins.hasOwnProperty(pluginName)) {
                var PluginCtor = PixiRenderer._plugins[pluginName];
                try {
                    this.plugins[pluginName] = new PluginCtor(this);
                } catch(e) {
                    this.plugins[pluginName] = null;
                }
            }
        }

        this.extract = this.plugins.extract;
    }

    PixiRenderer.prototype = Object.create(EventEmitter.prototype);
    PixiRenderer.prototype.constructor = PixiRenderer;

    Object.defineProperty(PixiRenderer.prototype, "width", {
        get: function() { return this.screen.width; }
    });

    Object.defineProperty(PixiRenderer.prototype, "height", {
        get: function() { return this.screen.height; }
    });

    PixiRenderer.prototype._updateBackgroundColor = function() {
        var hex = this.backgroundColor;
        this._backgroundColorRgba[0] = ((hex >> 16) & 0xFF) / 255;
        this._backgroundColorRgba[1] = ((hex >> 8) & 0xFF) / 255;
        this._backgroundColorRgba[2] = (hex & 0xFF) / 255;
        this._backgroundColorRgba[3] = this.transparent ? 0 : 1;
        this._backgroundColorString = PIXI.utils.hex2string(hex);
    };

    PixiRenderer.prototype.render = function(displayObject, renderTexture, clear, transform, skipUpdateTransform) {
        if (!displayObject) return;

        if (renderTexture) {
            /* Render to an FBO (render-to-texture). */
            var fbo = renderTexture._fbo;
            var w = renderTexture.baseTexture ? renderTexture.baseTexture.width : renderTexture.width;
            var h = renderTexture.baseTexture ? renderTexture.baseTexture.height : renderTexture.height;

            if (typeof __native_renderer !== "undefined" && fbo) {
                var prevActive = __native_renderer._active;
                __native_renderer.bindRenderTexture(fbo, w, h);
                __native_renderer._active = true;

                if (clear !== false) {
                    __native_renderer.beginFrameTransparent();
                }

                if (!skipUpdateTransform) {
                    displayObject.updateTransform();
                }
                /* Register the target so a filtered node inside the tree
                   (every Scene_Base has a ColorFilter) composites back into
                   this texture rather than onto the screen. */
                _fboStack.push({ fbo: fbo, w: w, h: h });
                displayObject.render(this);
                for (var si = _fboStack.length - 1; si >= 0; si--) {
                    if (_fboStack[si].fbo === fbo) {
                        _fboStack.splice(si, 1);
                        break;
                    }
                }

                __native_renderer.endFrame();
                __native_renderer.unbindRenderTexture();
                __native_renderer._active = prevActive;
            }
        } else {
            /* Render to the default framebuffer (screen). */
            this._lastObjectRendered = displayObject;

            /* PIXI sizes screen rendering from the canvas itself (renderer
               .width is view.width), so a game may just set canvas.width and
               never call renderer.resize(): rmmz_core 1.2 does exactly that
               in Graphics.resize(). Follow the canvas like PIXI does. */
            if (this.view && this.view.width > 0 && this.view.height > 0) {
                var viewW = this.view.width / this.resolution;
                var viewH = this.view.height / this.resolution;
                if (viewW !== this.screen.width || viewH !== this.screen.height) {
                    this.resize(viewW, viewH);
                }
            }

            if (typeof __native_renderer !== "undefined") {
                var prevActive = __native_renderer._active;
                __native_renderer._active = true;

                if (this.clearBeforeRender) {
                    __native_renderer.beginFrame();
                }

                if (!skipUpdateTransform) {
                    displayObject.updateTransform();
                }

                displayObject.render(this);

                __native_renderer.endFrame();
                __native_renderer._active = prevActive;
            }
        }
    };

    PixiRenderer.prototype.resize = function(screenWidth, screenHeight) {
        this.screen.width = screenWidth;
        this.screen.height = screenHeight;

        if (this.view) {
            this.view.width = screenWidth * this.resolution;
            this.view.height = screenHeight * this.resolution;
        }

        if (typeof __native_renderer !== "undefined") {
            __native_renderer.resize(screenWidth, screenHeight);

            /* Recompute the letterbox viewport inline rather than via
               __dom_setWindowSize, whose resize event would re-enter the game's
               own resize flow. */
            var ww = window.innerWidth;
            var wh = window.innerHeight;
            if (screenWidth > 0 && screenHeight > 0 && ww > 0 && wh > 0) {
                var lbScale = Math.min(ww / screenWidth, wh / screenHeight);
                var vpW = Math.round(screenWidth * lbScale);
                var vpH = Math.round(screenHeight * lbScale);
                var vpX = Math.round((ww - vpW) / 2);
                var vpY = Math.round((wh - vpH) / 2);
                var vpY_gl = wh - vpY - vpH;
                __native_renderer.setScreenViewport(vpX, vpY_gl, vpW, vpH);

                var canvas = document.getElementById("gameCanvas");
                if (canvas) {
                    canvas.offsetLeft = vpX;
                    canvas.offsetTop = vpY;
                    canvas.offsetWidth = vpW;
                    canvas.offsetHeight = vpH;
                    canvas.clientWidth = vpW;
                    canvas.clientHeight = vpH;
                }
            }
        }
    };

    PixiRenderer.prototype.clear = function() {
        if (typeof __native_renderer !== "undefined") {
            __native_renderer.beginFrame();
        }
    };

    PixiRenderer.prototype.reset = function() {
        /* No-op for our implementation. */
    };

    /* Read back a render target (or the screen) into a canvas element. */
    PixiRenderer.prototype._extractCanvas = function(target) {
        var canvas = document.createElement("canvas");

        if (!target || !target._fbo) {
            var size = ((typeof __native_renderer !== "undefined") && __native_renderer.getSize()) || { width: 0, height: 0 };
            canvas.width = size.width;
            canvas.height = size.height;

            if (typeof __native_renderer !== "undefined" && size.width > 0 && size.height > 0) {
                var pixels = __native_renderer.readPixels(0, size.width, size.height);
                if (pixels) {
                    var ctx = canvas.getContext("2d");
                    var imageData = ctx.createImageData(size.width, size.height);
                    /* Bulk copy: a per-element loop over a full screen is
                       millions of interpreted iterations per snapshot. */
                    imageData.data.set(new Uint8Array(pixels));
                    ctx.putImageData(imageData, 0, 0);
                }
            }
            return canvas;
        }

        var w = target.baseTexture ? target.baseTexture.width : (target.width || 0);
        var h = target.baseTexture ? target.baseTexture.height : (target.height || 0);
        canvas.width = w;
        canvas.height = h;

        if (typeof __native_renderer !== "undefined" && w > 0 && h > 0) {
            var pxData = __native_renderer.readPixels(target._fbo, w, h);
            if (pxData) {
                var ctx2 = canvas.getContext("2d");
                var imgData = ctx2.createImageData(w, h);
                imgData.data.set(new Uint8Array(pxData));
                ctx2.putImageData(imgData, 0, 0);
            }
        }
        return canvas;
    };

    PixiRenderer.prototype._extractPixels = function(target) {
        if (!target || !target._fbo) return new Uint8Array(0);
        var w = target.baseTexture ? target.baseTexture.width : (target.width || 0);
        var h = target.baseTexture ? target.baseTexture.height : (target.height || 0);
        if (typeof __native_renderer !== "undefined" && w > 0 && h > 0) {
            var pxData = __native_renderer.readPixels(target._fbo, w, h);
            if (pxData) return new Uint8Array(pxData);
        }
        return new Uint8Array(w * h * 4);
    };

    PixiRenderer.prototype.destroy = function(removeView) {
        if (typeof __native_renderer !== "undefined") {
            __native_renderer.shutdown();
        }
        this.view = null;
        this.gl = null;
        this.screen = null;
        this.plugins = null;
        this.extract = null;
    };

    /* Static plugin registry for renderer. */
    PixiRenderer._plugins = {};

    PixiRenderer.registerPlugin = function(name, ctor) {
        PixiRenderer._plugins[name] = ctor;
    };

    PIXI.Renderer = PixiRenderer;
    PIXI.WebGLRenderer = PixiRenderer;

    /* PIXI.State (stub for Tilemap.Layer compatibility) */

    function State() {
        this.blend = true;
        this.offsets = false;
        this.culling = false;
        this.depthTest = false;
        this.clockwiseFrontFace = false;
        this.blendMode = 0;
        this.polygonOffset = 0;
    }

    State.for2d = function() {
        var s = new State();
        s.blend = true;
        return s;
    };

    PIXI.State = State;

    /* PIXI.Buffer (stub for Tilemap.Layer VAO creation) */

    function PIXIBuffer(data, isStatic, isIndex) {
        this.data = data;
        this.static = !!isStatic;
        this.index = !!isIndex;
        this._updateID = 0;
    }

    PIXIBuffer.prototype.update = function(data) {
        if (data !== undefined) this.data = data;
        this._updateID++;
    };

    PIXIBuffer.prototype.destroy = function() {
        this.data = null;
    };

    PIXIBuffer.prototype.dispose = function() {
        this.data = null;
    };

    PIXI.Buffer = PIXIBuffer;

    /* PIXI.Geometry (stub for Tilemap.Layer VAO creation) */

    function Geometry() {
        this.attributes = {};
        this.indexBuffer = null;
        this.buffers = [];
    }

    Geometry.prototype.addIndex = function(buffer) {
        this.indexBuffer = buffer;
        return this;
    };

    Geometry.prototype.addAttribute = function(id, buffer, size, normalized, type, stride, start) {
        this.attributes[id] = {
            buffer: buffer,
            size: size || 1,
            normalized: !!normalized,
            type: type || 5126,
            stride: stride || 0,
            start: start || 0
        };
        if (this.buffers.indexOf(buffer) < 0) {
            this.buffers.push(buffer);
        }
        return this;
    };

    Geometry.prototype.destroy = function() {
        this.attributes = {};
        this.indexBuffer = null;
        this.buffers = [];
    };

    Geometry.prototype.dispose = function() {
        this.destroy();
    };

    PIXI.Geometry = Geometry;

    /* PIXI.Program (stub for Tilemap.Renderer shader creation) */

    function PIXIProgram(vertexSrc, fragmentSrc) {
        this.vertexSrc = vertexSrc || "";
        this.fragmentSrc = fragmentSrc || "";
    }

    PIXIProgram.from = function(vertexSrc, fragmentSrc) {
        return new PIXIProgram(vertexSrc, fragmentSrc);
    };

    PIXI.Program = PIXIProgram;

    /* PIXI.Shader (stub for Tilemap.Renderer shader creation) */

    function PIXIShader(program, uniforms) {
        this.program = program;
        this.uniforms = uniforms || {};
    }

    PIXIShader.prototype.destroy = function() {
        this.program = null;
        this.uniforms = null;
    };

    PIXI.Shader = PIXIShader;

    /* PIXI.ObjectRenderer (base class for Tilemap.Renderer) */

    function ObjectRenderer(renderer) {
        this.renderer = renderer;
    }

    ObjectRenderer.prototype.flush = function() {};
    ObjectRenderer.prototype.destroy = function() {
        this.renderer = null;
    };
    ObjectRenderer.prototype.start = function() {};
    ObjectRenderer.prototype.stop = function() {};
    ObjectRenderer.prototype.render = function(object) {};

    PIXI.ObjectRenderer = ObjectRenderer;

    /* PIXI.BaseRenderTexture (for Tilemap.Renderer internal textures) */

    function BaseRenderTexture(options) {
        BaseTexture.call(this);
        options = options || {};
        this.width = options.width || 0;
        this.height = options.height || 0;
        this.resolution = options.resolution || 1;
        this.scaleMode = options.scaleMode !== undefined ? options.scaleMode : PIXI.SCALE_MODES.NEAREST;
        this.valid = false;
    }

    BaseRenderTexture.prototype = Object.create(BaseTexture.prototype);
    BaseRenderTexture.prototype.constructor = BaseRenderTexture;

    BaseRenderTexture.prototype.resize = function(width, height) {
        this.width = width;
        this.height = height;
        this.valid = true;
    };

    BaseRenderTexture.prototype.destroy = function() {
        this.valid = false;
    };

    PIXI.BaseRenderTexture = BaseRenderTexture;

    /* PIXI.utils.createIndicesForQuads (for Tilemap.Layer index buffer) */

    PIXI.utils.createIndicesForQuads = function(size, outBuffer) {
        var totalIndices = size * 6;
        var buffer = outBuffer || new Uint16Array(totalIndices);
        if (buffer.length < totalIndices) {
            buffer = new Uint16Array(totalIndices);
        }
        for (var i = 0, j = 0; i < totalIndices; i += 6, j += 4) {
            buffer[i + 0] = j + 0;
            buffer[i + 1] = j + 1;
            buffer[i + 2] = j + 2;
            buffer[i + 3] = j + 0;
            buffer[i + 4] = j + 2;
            buffer[i + 5] = j + 3;
        }
        return buffer;
    };

    /* PIXI.Application */

    function Application(options) {
        options = options || {};

        this.stage = new Container();

        var rendererOptions = {
            view: options.view || null,
            width: options.width || (options.view ? (options.view.width || 800) : 800),
            height: options.height || (options.view ? (options.view.height || 600) : 600),
            backgroundColor: options.backgroundColor !== undefined ? options.backgroundColor : 0x000000,
            resolution: options.resolution || PIXI.settings.RESOLUTION,
            transparent: !!options.transparent,
            autoDensity: !!options.autoDensity,
            antialias: !!options.antialias,
            preserveDrawingBuffer: !!options.preserveDrawingBuffer,
            clearBeforeRender: options.clearBeforeRender !== false,
        };

        this.renderer = new PixiRenderer(rendererOptions);

        this._ticker = new Ticker();

        /* Register the prototype method with `this` as context (as PIXI does) so
           RPG Maker's `ticker.remove(this._app.render, this._app)` can unhook
           the auto-render. */
        this._ticker.add(this.render, this);

        if (options.autoStart !== false) {
            this.start();
        }
    }

    Object.defineProperty(Application.prototype, "ticker", {
        get: function() { return this._ticker; },
        set: function(ticker) {
            if (this._ticker) {
                this._ticker.remove(this.render, this);
            }
            this._ticker = ticker;
            if (ticker) {
                ticker.add(this.render, this);
            }
        }
    });

    Object.defineProperty(Application.prototype, "view", {
        get: function() { return this.renderer.view; }
    });

    Object.defineProperty(Application.prototype, "screen", {
        get: function() { return this.renderer.screen; }
    });

    Application.prototype.render = function() {
        this.renderer.render(this.stage);
    };

    Application.prototype.start = function() {
        this._ticker.start();
    };

    Application.prototype.stop = function() {
        this._ticker.stop();
    };

    Application.prototype.destroy = function(removeView, stageOptions) {
        this._ticker.destroy();
        this._ticker = null;

        if (this.stage) {
            this.stage.destroy(stageOptions);
            this.stage = null;
        }

        if (this.renderer) {
            this.renderer.destroy(removeView);
            this.renderer = null;
        }
    };

    PIXI.Application = Application;

    /* PIXI.Loader / PIXI.LoaderResource (resource-loader v3 subset), used by
       plugins such as ApngPicture. Signals dispatch in add-order with PIXI v5
       argument shapes: onStart(loader), onProgress/onLoad(loader, resource),
       onError(error, loader, resource), onComplete(loader, resources). */

    function LoaderSignal() { this._handlers = []; }
    LoaderSignal.prototype.add = function(fn, ctx) {
        this._handlers.push({ fn: fn, ctx: ctx || null, once: false });
        return this;
    };
    LoaderSignal.prototype.once = function(fn, ctx) {
        this._handlers.push({ fn: fn, ctx: ctx || null, once: true });
        return this;
    };
    LoaderSignal.prototype.detach = function(fn) {
        for (var i = this._handlers.length - 1; i >= 0; i--) {
            if (this._handlers[i].fn === fn) this._handlers.splice(i, 1);
        }
        return this;
    };
    LoaderSignal.prototype.detachAll = function() { this._handlers.length = 0; return this; };
    LoaderSignal.prototype.dispatch = function() {
        var list = this._handlers.slice(0);
        for (var i = 0; i < list.length; i++) {
            var h = list[i];
            if (h.once) {
                var idx = this._handlers.indexOf(h);
                if (idx >= 0) this._handlers.splice(idx, 1);
            }
            h.fn.apply(h.ctx, arguments);
        }
    };

    function _resourceExtension(url) {
        var u = String(url).split("?")[0].split("#")[0];
        var dot = u.lastIndexOf("."), slash = u.lastIndexOf("/");
        return dot > slash ? u.substring(dot + 1) : "";
    }

    function LoaderResource(name, url, options) {
        options = options || {};
        this.name = name;
        this.url = url;
        this.extension = _resourceExtension(url);
        this.data = null;
        this.crossOrigin = options.crossOrigin;
        this.timeout = options.timeout || 0;
        this.loadType = options.loadType || LoaderResource._determineLoadType(this.extension);
        this.xhrType = options.xhrType || null;
        this.metadata = options.metadata || {};
        this.error = null;
        this.xhr = null;
        this.children = [];
        this.type = LoaderResource.TYPE.UNKNOWN;
        this.progressChunk = 0;
        this.isLoading = false;
        this.isComplete = false;
        this.texture = null;
        this.onStart = new LoaderSignal();
        this.onProgress = new LoaderSignal();
        this.onComplete = new LoaderSignal();
        this.onAfterMiddleware = new LoaderSignal();
    }
    LoaderResource.STATUS_FLAGS = { NONE: 0, DATA_URL: 1, COMPLETE: 2, LOADING: 4 };
    LoaderResource.TYPE = { UNKNOWN: 0, JSON: 1, XML: 2, IMAGE: 3, AUDIO: 4, VIDEO: 5, TEXT: 6 };
    LoaderResource.LOAD_TYPE = { XHR: 1, IMAGE: 2, AUDIO: 3, VIDEO: 4 };
    LoaderResource.XHR_RESPONSE_TYPE = {
        DEFAULT: "text", BUFFER: "arraybuffer", BLOB: "blob",
        DOCUMENT: "document", JSON: "json", TEXT: "text"
    };
    LoaderResource._imageExts = { png: 1, jpg: 1, jpeg: 1, gif: 1, webp: 1, bmp: 1, svg: 1 };
    LoaderResource._determineLoadType = function(ext) {
        return LoaderResource._imageExts[String(ext).toLowerCase()]
            ? LoaderResource.LOAD_TYPE.IMAGE : LoaderResource.LOAD_TYPE.XHR;
    };
    LoaderResource.setExtensionLoadType = function() {};
    LoaderResource.setExtensionXhrType = function() {};
    LoaderResource.prototype.complete = function() {
        this.isLoading = false;
        this.isComplete = true;
        this.onComplete.dispatch(this);
    };
    LoaderResource.prototype.abort = function(message) {
        this.error = new Error(message || "aborted");
        this.complete();
    };

    function _runMiddleware(loader, fns, res, done) {
        var i = 0;
        (function next() {
            if (i >= fns.length) return done();
            var fn = fns[i++];
            fn.call(loader, res, next);
        })();
    }

    function Loader(baseUrl, concurrency) {
        this.baseUrl = baseUrl || "";
        this.concurrency = concurrency || 10;
        this.defaultQueryString = "";
        this.progress = 0;
        this.loading = false;
        this.resources = {};
        this._queue = [];
        this._remaining = 0;
        this._beforeMiddleware = [];
        this._afterMiddleware = [];
        this.onProgress = new LoaderSignal();
        this.onError = new LoaderSignal();
        this.onLoad = new LoaderSignal();
        this.onStart = new LoaderSignal();
        this.onComplete = new LoaderSignal();
        for (var i = 0; i < Loader._plugins.length; i++) {
            var p = Loader._plugins[i];
            if (p.pre) this.pre(p.pre);
            if (p.use) this.use(p.use);
        }
    }
    Loader._plugins = [];
    Loader.registerPlugin = function(plugin) { Loader._plugins.push(plugin); return Loader; };
    Loader.prototype.pre = function(fn) { this._beforeMiddleware.push(fn); return this; };
    Loader.prototype.use = function(fn) { this._afterMiddleware.push(fn); return this; };

    Loader.prototype.reset = function() {
        this.progress = 0;
        this.loading = false;
        this._queue.length = 0;
        this._remaining = 0;
        for (var k in this.resources) {
            var r = this.resources[k];
            if (r.xhr && typeof r.xhr.abort === "function" && r.isLoading) r.xhr.abort();
        }
        this.resources = {};
        return this;
    };

    /* add(url) | add(url, opts) | add(url, cb) | add(name, url) |
       add(name, url, opts) | add(name, url, opts, cb) | add({name,url,...}) | add([...]) */
    Loader.prototype.add = function(name, url, options, cb) {
        if (Array.isArray(name)) {
            for (var i = 0; i < name.length; i++) this.add(name[i]);
            return this;
        }
        if (name !== null && typeof name === "object") {
            cb = url;
            options = name;
            url = options.url;
            name = options.name || url;
        } else {
            if (typeof url === "function") {            /* add(url, cb) */
                cb = url; url = name; options = null;
            } else if (url === undefined || (url !== null && typeof url === "object")) {
                cb = options; options = url; url = name;   /* add(url[, opts]) */
            }
            if (typeof options === "function") { cb = options; options = null; }
        }
        if (this.resources[name]) return this;   /* PIXI throws; be lenient. */

        var res = new LoaderResource(name, url, options || {});
        if (typeof cb === "function") res.onAfterMiddleware.once(cb);
        this.resources[name] = res;
        if (this.loading) {
            this._loadResource(res);
        } else {
            this._queue.push(res);
        }
        return this;
    };

    Loader.prototype.load = function(cb) {
        if (typeof cb === "function") this.onComplete.once(cb);
        if (this.loading) return this;
        this.loading = true;
        this.progress = 0;
        this.onStart.dispatch(this);
        var pending = this._queue.slice(0);
        this._queue.length = 0;
        if (pending.length === 0) {
            this._finish();
            return this;
        }
        for (var i = 0; i < pending.length; i++) this._loadResource(pending[i]);
        return this;
    };

    Loader.prototype._finish = function() {
        if (!this.loading) return;
        this.loading = false;
        var self = this;
        Promise.resolve().then(function() {
            self.progress = 100;
            self.onComplete.dispatch(self, self.resources);
        });
    };

    Loader.prototype._loadResource = function(res) {
        var self = this;
        this._remaining++;
        res.isLoading = true;
        res.onStart.dispatch(res);

        function finishRes() {
            self._remaining--;
            var done = 0, total = 0;
            for (var k in self.resources) { total++; if (self.resources[k].isComplete) done++; }
            self.progress = total ? (done / total) * 100 : 100;
            if (res.error) self.onError.dispatch(res.error, self, res);
            self.onLoad.dispatch(self, res);
            self.onProgress.dispatch(self, res);
            if (self._remaining <= 0 && self._queue.length === 0) self._finish();
        }

        function afterLoad() {
            /* Equivalent of PIXI's texture middleware for image resources. */
            if (!res.error && res.type === LoaderResource.TYPE.IMAGE && res.data &&
                typeof Texture !== "undefined" && Texture.from) {
                try { res.texture = Texture.from(res.data); } catch (e) { /* ignore */ }
            }
            _runMiddleware(self, self._afterMiddleware, res, function() {
                res.isLoading = false;
                res.isComplete = true;
                res.onAfterMiddleware.dispatch(res);
                res.onComplete.dispatch(res);
                finishRes();
            });
        }

        function loadXhr() {
            var xhr = new XMLHttpRequest();
            res.xhr = xhr;
            var rt = res.xhrType || LoaderResource.XHR_RESPONSE_TYPE.DEFAULT;
            xhr.open("GET", self.baseUrl + res.url + self.defaultQueryString, true);
            xhr.responseType = (rt === "text") ? "" : rt;
            xhr.onload = function() {
                if (xhr.status !== 200) {
                    res.error = new Error("[" + xhr.status + "] " + (xhr.statusText || "") + ": " + res.url);
                } else {
                    res.data = (rt === "text") ? xhr.responseText : xhr.response;
                    res.type = (rt === "json") ? LoaderResource.TYPE.JSON
                             : (rt === "text") ? LoaderResource.TYPE.TEXT
                             : LoaderResource.TYPE.UNKNOWN;
                }
                afterLoad();
            };
            xhr.onerror = function() {
                res.error = new Error("Load error: " + res.url);
                afterLoad();
            };
            xhr.send();
        }

        function loadImage() {
            var img = new Image();
            res.data = img;
            img.onload = function() { res.type = LoaderResource.TYPE.IMAGE; afterLoad(); };
            img.onerror = function() { res.error = new Error("Image load error: " + res.url); afterLoad(); };
            img.src = self.baseUrl + res.url;
        }

        _runMiddleware(this, this._beforeMiddleware, res, function() {
            if (res.isComplete) { finishRes(); return; }
            if (res.loadType === LoaderResource.LOAD_TYPE.IMAGE) loadImage();
            else loadXhr();
        });
    };

    Loader.prototype.destroy = function() {
        this.reset();
        this.onProgress.detachAll(); this.onError.detachAll(); this.onLoad.detachAll();
        this.onStart.detachAll(); this.onComplete.detachAll();
    };

    Object.defineProperty(Loader, "shared", {
        get: function() {
            if (!Loader._shared) Loader._shared = new Loader();
            return Loader._shared;
        },
        configurable: true
    });

    PIXI.Loader = Loader;
    PIXI.LoaderResource = LoaderResource;
    /* PIXI v4 alias some plugins still use. */
    Object.defineProperty(PIXI, "loader", { get: function() { return Loader.shared; }, configurable: true });

    /* Scene graph render helper */

    PIXI._renderSceneGraph = function(root) {
        if (!root) return;
        root.updateTransform();
        root.render(null);
    };

    /* PIXI.Filter base class */

    function Filter(vertexSrc, fragmentSrc, uniforms) {
        this.vertexSrc = vertexSrc || null;
        this.fragmentSrc = fragmentSrc || null;
        this.uniforms = uniforms || {};
        this.enabled = true;
        this.autoFit = true;
        this.padding = 0;
        this.resolution = PIXI.settings.FILTER_RESOLUTION || PIXI.settings.RESOLUTION;
        this.blendMode = PIXI.BLEND_MODES.NORMAL;
        this._glShader = 0;
    }

    /* Detect RMMZ ColorFilter by its uniform signature. */
    function _isColorFilter(filter) {
        var u = filter.uniforms;
        return u && u.hue !== undefined && u.colorTone !== undefined &&
               u.blendColor !== undefined && u.brightness !== undefined;
    }

    Filter.prototype.apply = function(renderer, inputTexture, outputW, outputH) {
        if (typeof __native_filters === "undefined" || !inputTexture) return;

        /* RMMZ ColorFilter uses the built-in native shader instead of compiling
           the PIXI-style GLSL from rmmz_core.js. */
        if (_isColorFilter(this) && __native_filters.colorFilterShader) {
            var shader = __native_filters.colorFilterShader;
            __native_filters.beginFilter(shader, inputTexture, outputW, outputH);
            __native_filters.setUniform1f(shader, "hue", this.uniforms.hue);
            var ct = this.uniforms.colorTone;
            __native_filters.setUniform4f(shader, "colorTone", ct[0], ct[1], ct[2], ct[3]);
            var bc = this.uniforms.blendColor;
            __native_filters.setUniform4f(shader, "blendColor", bc[0], bc[1], bc[2], bc[3]);
            __native_filters.setUniform1f(shader, "brightness", this.uniforms.brightness);
            __native_filters.drawQuad();
            __native_filters.endFilter();
            return;
        }

        /* Try to compile the shader on first use. */
        if (!this._glShader && this.fragmentSrc) {
            this._glShader = __native_filters.compileShader(this.vertexSrc, this.fragmentSrc);
        }

        if (this._glShader) {
            __native_filters.beginFilter(this._glShader, inputTexture, outputW, outputH);
            if (this.uniforms) {
                var s = this._glShader;
                for (var uname in this.uniforms) {
                    if (!this.uniforms.hasOwnProperty(uname)) continue;
                    var val = this.uniforms[uname];
                    if (typeof val === "number") {
                        __native_filters.setUniform1f(s, uname, val);
                    } else if (Array.isArray(val)) {
                        if (val.length === 2) {
                            __native_filters.setUniform2f(s, uname, val[0], val[1]);
                        } else if (val.length === 4) {
                            __native_filters.setUniform4f(s, uname, val[0], val[1], val[2], val[3]);
                        } else if (val.length === 16) {
                            __native_filters.setUniformMat4(s, uname, val);
                        }
                    }
                }
            }
            __native_filters.drawQuad();
            __native_filters.endFilter();
        } else {
            /* Passthrough copy via the filter pipeline, not the sprite batch: a
               prior endFilter() leaves the batch's VAO/shader unbound. */
            if (__native_filters.alphaShader) {
                __native_filters.beginFilter(
                    __native_filters.alphaShader, inputTexture, outputW, outputH);
                __native_filters.setUniform1f(
                    __native_filters.alphaShader, "u_alpha", 1.0);
                __native_filters.drawQuad();
                __native_filters.endFilter();
            }
        }
    };

    Filter.prototype.destroy = function() {
        if (this._glShader && typeof __native_filters !== "undefined") {
            __native_filters.deleteShader(this._glShader);
        }
        this._glShader = 0;
        this.uniforms = null;
    };

    Filter.defaultVertexSrc = "";
    Filter.defaultFragmentSrc = "";

    PIXI.Filter = Filter;

    /* PIXI.filters.ColorMatrixFilter */

    function ColorMatrixFilter() {
        Filter.call(this);
        /* 4x5 color matrix stored as a 20-element array:
           [m0, m1, m2, m3, m4,    <- R row (4 multipliers + offset)
            m5, m6, m7, m8, m9,    <- G row
            m10,m11,m12,m13,m14,   <- B row
            m15,m16,m17,m18,m19]   <- A row */
        this.matrix = [
            1, 0, 0, 0, 0,
            0, 1, 0, 0, 0,
            0, 0, 1, 0, 0,
            0, 0, 0, 1, 0
        ];
        this.alpha = 1;
        this.uniforms.uColorMatrix = this.matrix;
    }

    ColorMatrixFilter.prototype = Object.create(Filter.prototype);
    ColorMatrixFilter.prototype.constructor = ColorMatrixFilter;

    ColorMatrixFilter.prototype.apply = function(renderer, inputTexture, outputW, outputH) {
        if (typeof __native_filters === "undefined" || !inputTexture) return;

        var shader = __native_filters.colorMatrixShader;
        if (!shader) return;

        var m = this.matrix;
        /* Extract 4x4 matrix (column-major for GL): indices [0-3, 5-8, 10-13, 15-18] */
        var mat4 = new Float32Array([
            m[0],  m[5],  m[10], m[15],
            m[1],  m[6],  m[11], m[16],
            m[2],  m[7],  m[12], m[17],
            m[3],  m[8],  m[13], m[18]
        ]);
        /* Extract offset: indices [4, 9, 14, 19] */
        var offset = [m[4] / 255.0, m[9] / 255.0, m[14] / 255.0, m[19] / 255.0];

        __native_filters.beginFilter(shader, inputTexture, outputW, outputH);
        __native_filters.setUniformMat4(shader, "u_colorMatrix", mat4);
        __native_filters.setUniform4f(shader, "u_colorOffset",
            offset[0], offset[1], offset[2], offset[3]);
        __native_filters.drawQuad();
        __native_filters.endFilter();
    };

    /* Multiply current matrix by another. */
    ColorMatrixFilter.prototype._multiply = function(out, a, b) {
        /* a and b are 4x5, out is 4x5. Row-major 5-column layout. */
        out[0]  = a[0]*b[0]  + a[1]*b[5]  + a[2]*b[10] + a[3]*b[15];
        out[1]  = a[0]*b[1]  + a[1]*b[6]  + a[2]*b[11] + a[3]*b[16];
        out[2]  = a[0]*b[2]  + a[1]*b[7]  + a[2]*b[12] + a[3]*b[17];
        out[3]  = a[0]*b[3]  + a[1]*b[8]  + a[2]*b[13] + a[3]*b[18];
        out[4]  = a[0]*b[4]  + a[1]*b[9]  + a[2]*b[14] + a[3]*b[19] + a[4];

        out[5]  = a[5]*b[0]  + a[6]*b[5]  + a[7]*b[10] + a[8]*b[15];
        out[6]  = a[5]*b[1]  + a[6]*b[6]  + a[7]*b[11] + a[8]*b[16];
        out[7]  = a[5]*b[2]  + a[6]*b[7]  + a[7]*b[12] + a[8]*b[17];
        out[8]  = a[5]*b[3]  + a[6]*b[8]  + a[7]*b[13] + a[8]*b[18];
        out[9]  = a[5]*b[4]  + a[6]*b[9]  + a[7]*b[14] + a[8]*b[19] + a[9];

        out[10] = a[10]*b[0] + a[11]*b[5] + a[12]*b[10] + a[13]*b[15];
        out[11] = a[10]*b[1] + a[11]*b[6] + a[12]*b[11] + a[13]*b[16];
        out[12] = a[10]*b[2] + a[11]*b[7] + a[12]*b[12] + a[13]*b[17];
        out[13] = a[10]*b[3] + a[11]*b[8] + a[12]*b[13] + a[13]*b[18];
        out[14] = a[10]*b[4] + a[11]*b[9] + a[12]*b[14] + a[13]*b[19] + a[14];

        out[15] = a[15]*b[0] + a[16]*b[5] + a[17]*b[10] + a[18]*b[15];
        out[16] = a[15]*b[1] + a[16]*b[6] + a[17]*b[11] + a[18]*b[16];
        out[17] = a[15]*b[2] + a[16]*b[7] + a[17]*b[12] + a[18]*b[17];
        out[18] = a[15]*b[3] + a[16]*b[8] + a[17]*b[13] + a[18]*b[18];
        out[19] = a[15]*b[4] + a[16]*b[9] + a[17]*b[14] + a[18]*b[19] + a[19];
    };

    /* Load a new matrix, optionally multiplying with the current one. */
    ColorMatrixFilter.prototype._loadMatrix = function(matrix, multiply) {
        var newMatrix = matrix;
        if (multiply) {
            var temp = new Array(20);
            this._multiply(temp, matrix, this.matrix);
            newMatrix = temp;
        }
        for (var i = 0; i < 20; i++) {
            this.matrix[i] = newMatrix[i];
        }
    };

    ColorMatrixFilter.prototype._copyMatrix = function(m) {
        var out = new Array(20);
        for (var i = 0; i < 20; i++) out[i] = m[i];
        return out;
    };

    ColorMatrixFilter.prototype.reset = function() {
        this.matrix = [
            1, 0, 0, 0, 0,
            0, 1, 0, 0, 0,
            0, 0, 1, 0, 0,
            0, 0, 0, 1, 0
        ];
    };

    /* Brightness: 0 = black, 1 = normal, >1 = brighter. */
    ColorMatrixFilter.prototype.brightness = function(b, multiply) {
        var matrix = [
            b, 0, 0, 0, 0,
            0, b, 0, 0, 0,
            0, 0, b, 0, 0,
            0, 0, 0, 1, 0
        ];
        this._loadMatrix(matrix, multiply);
    };

    /* Tint with a color (PIXI format). */
    ColorMatrixFilter.prototype.tint = function(color, multiply) {
        var r = ((color >> 16) & 0xFF) / 255.0;
        var g = ((color >> 8) & 0xFF) / 255.0;
        var b = (color & 0xFF) / 255.0;
        var matrix = [
            r, 0, 0, 0, 0,
            0, g, 0, 0, 0,
            0, 0, b, 0, 0,
            0, 0, 0, 1, 0
        ];
        this._loadMatrix(matrix, multiply);
    };

    ColorMatrixFilter.prototype.greyscale = function(scale, multiply) {
        var s = scale || 1;
        var r = 0.2126 * s;
        var g = 0.7152 * s;
        var b = 0.0722 * s;
        var matrix = [
            r, g, b, 0, 0,
            r, g, b, 0, 0,
            r, g, b, 0, 0,
            0, 0, 0, 1, 0
        ];
        this._loadMatrix(matrix, multiply);
    };

    ColorMatrixFilter.prototype.grayscale = ColorMatrixFilter.prototype.greyscale;

    /* Saturation: 0 = greyscale, 1 = normal, >1 = supersaturated. */
    ColorMatrixFilter.prototype.saturate = function(amount, multiply) {
        if (amount === undefined) amount = 0;
        var x = (amount * 2.0 / 3.0) + 1;
        var y = ((x - 1) * -0.5);
        var matrix = [
            x, y, y, 0, 0,
            y, x, y, 0, 0,
            y, y, x, 0, 0,
            0, 0, 0, 1, 0
        ];
        this._loadMatrix(matrix, multiply);
    };

    /* Desaturate — shortcut for fully greyscale. */
    ColorMatrixFilter.prototype.desaturate = function() {
        this.saturate(-1);
    };

    /* Hue rotation in degrees. */
    ColorMatrixFilter.prototype.hue = function(rotation, multiply) {
        rotation = (rotation || 0) / 180 * Math.PI;
        var cos = Math.cos(rotation);
        var sin = Math.sin(rotation);
        var lumR = 0.213;
        var lumG = 0.715;
        var lumB = 0.072;
        var matrix = [
            lumR + cos * (1 - lumR) + sin * (-lumR),
            lumG + cos * (-lumG) + sin * (-lumG),
            lumB + cos * (-lumB) + sin * (1 - lumB), 0, 0,
            lumR + cos * (-lumR) + sin * 0.143,
            lumG + cos * (1 - lumG) + sin * 0.140,
            lumB + cos * (-lumB) + sin * (-0.283), 0, 0,
            lumR + cos * (-lumR) + sin * (-(1 - lumR)),
            lumG + cos * (-lumG) + sin * lumG,
            lumB + cos * (1 - lumB) + sin * lumB, 0, 0,
            0, 0, 0, 1, 0
        ];
        this._loadMatrix(matrix, multiply);
    };

    /* Contrast: 0 = grey, 1 = normal, >1 = more contrast. */
    ColorMatrixFilter.prototype.contrast = function(amount, multiply) {
        var v = (amount || 0) + 1;
        var o = -0.5 * (v - 1) * 255;
        var matrix = [
            v, 0, 0, 0, o,
            0, v, 0, 0, o,
            0, 0, v, 0, o,
            0, 0, 0, 1, 0
        ];
        this._loadMatrix(matrix, multiply);
    };

    ColorMatrixFilter.prototype.negative = function(multiply) {
        var matrix = [
            -1, 0,  0, 1, 255,
             0,-1,  0, 1, 255,
             0, 0, -1, 1, 255,
             0, 0,  0, 1, 0
        ];
        this._loadMatrix(matrix, multiply);
    };

    ColorMatrixFilter.prototype.sepia = function(multiply) {
        var matrix = [
            0.393, 0.769, 0.189, 0, 0,
            0.349, 0.686, 0.168, 0, 0,
            0.272, 0.534, 0.131, 0, 0,
            0,     0,     0,     1, 0
        ];
        this._loadMatrix(matrix, multiply);
    };

    PIXI.filters = PIXI.filters || {};
    PIXI.filters.ColorMatrixFilter = ColorMatrixFilter;

    /* PIXI.filters.BlurFilter */

    function BlurFilter(strength, quality, resolution, kernelSize) {
        Filter.call(this);
        this._strength = strength !== undefined ? strength : 8;
        this.quality = quality || 4;
        this.resolution = resolution || PIXI.settings.RESOLUTION;
        this.kernelSize = kernelSize || 5;
    }

    BlurFilter.prototype = Object.create(Filter.prototype);
    BlurFilter.prototype.constructor = BlurFilter;

    Object.defineProperty(BlurFilter.prototype, "blur", {
        get: function() { return this._strength; },
        set: function(v) { this._strength = v; }
    });

    Object.defineProperty(BlurFilter.prototype, "blurX", {
        get: function() { return this._strength; },
        set: function(v) { this._strength = v; }
    });

    Object.defineProperty(BlurFilter.prototype, "blurY", {
        get: function() { return this._strength; },
        set: function(v) { this._strength = v; }
    });

    BlurFilter.prototype.apply = function(renderer, inputTexture, outputW, outputH) {
        if (typeof __native_filters === "undefined" || !inputTexture) return;

        var shader = __native_filters.blurShader;
        if (!shader) return;

        /* Multi-pass: horizontal then vertical, repeated for quality. A blur
           of 0 still has to copy the input through: the caller's output FBO
           starts cleared, and skipping it would draw the object as nothing
           (Scene_MenuBase backgrounds keep a BlurFilter at blur 0 in some
           games). */
        var passes = this._strength > 0 ? Math.max(1, this.quality) : 0;
        var strength = passes > 0 ? this._strength / passes : 0;
        var currentTex = inputTexture;
        var pooledRTs = [];

        for (var p = 0; p < passes; p++) {
            /* Horizontal pass. */
            var hRT = _acquireFilterFBO(outputW, outputH);
            pooledRTs.push(hRT);
            __native_renderer.bindRenderTexture(hRT.fbo, outputW, outputH);
            /* Pooled FBOs keep stale contents and the filter pass blends, so clear first. */
            __native_renderer.beginFrameTransparent();
            __native_filters.beginFilter(shader, currentTex, outputW, outputH);
            __native_filters.setUniform2f(shader, "u_direction", 1.0 / outputW, 0);
            __native_filters.setUniform1f(shader, "u_strength", strength);
            __native_filters.drawQuad();
            __native_filters.endFilter();

            /* Vertical pass. */
            var vRT = _acquireFilterFBO(outputW, outputH);
            pooledRTs.push(vRT);
            __native_renderer.bindRenderTexture(vRT.fbo, outputW, outputH);
            __native_renderer.beginFrameTransparent();
            __native_filters.beginFilter(shader, hRT.texture, outputW, outputH);
            __native_filters.setUniform2f(shader, "u_direction", 0, 1.0 / outputH);
            __native_filters.setUniform1f(shader, "u_strength", strength);
            __native_filters.drawQuad();
            __native_filters.endFilter();

            currentTex = vRT.texture;
        }

        /* Rebind the caller's output FBO (the H/V passes replaced its binding)
           and copy the result into it. */
        if (_filterOutputTarget) {
            __native_renderer.bindRenderTexture(
                _filterOutputTarget.fbo, _filterOutputTarget.w, _filterOutputTarget.h);
        }
        __native_filters.beginFilter(shader, currentTex, outputW, outputH);
        __native_filters.setUniform2f(shader, "u_direction", 0, 0);
        __native_filters.setUniform1f(shader, "u_strength", 0);
        __native_filters.drawQuad();
        __native_filters.endFilter();

        for (var t = 0; t < pooledRTs.length; t++) {
            _releaseFilterFBO(pooledRTs[t]);
        }
    };

    PIXI.filters.BlurFilter = BlurFilter;

    /* PIXI.filters.BlurFilterPass: one direction of PIXI's blur. Not used by
       this renderer's blur, but plugins patch its prototype (Archeia's
       SystemAdjust) or build their own pass, so the class must exist. */
    function BlurFilterPass(horizontal, strength, quality, resolution, kernelSize) {
        Filter.call(this);
        this.horizontal = !!horizontal;
        this.strength = strength !== undefined ? strength : 8;
        this.quality = quality !== undefined ? quality : 4;
        this.resolution = resolution !== undefined ? resolution : 1;
        this.kernelSize = kernelSize !== undefined ? kernelSize : 5;
        this.passes = this.quality;
        this.uniforms.strength = 0;
    }
    BlurFilterPass.prototype = Object.create(Filter.prototype);
    BlurFilterPass.prototype.constructor = BlurFilterPass;
    Object.defineProperty(BlurFilterPass.prototype, "blur", {
        get: function() { return this.strength; },
        set: function(v) { this.padding = 1 + Math.abs(v) * 2; this.strength = v; }
    });
    BlurFilterPass.prototype.apply = function() {};
    PIXI.filters.BlurFilterPass = BlurFilterPass;

    /* PIXI.filters.AlphaFilter */

    function AlphaFilter(alpha) {
        Filter.call(this);
        this.alpha = alpha !== undefined ? alpha : 1.0;
    }

    AlphaFilter.prototype = Object.create(Filter.prototype);
    AlphaFilter.prototype.constructor = AlphaFilter;

    AlphaFilter.prototype.apply = function(renderer, inputTexture, outputW, outputH) {
        if (typeof __native_filters === "undefined" || !inputTexture) return;

        var shader = __native_filters.alphaShader;
        if (!shader) return;

        __native_filters.beginFilter(shader, inputTexture, outputW, outputH);
        __native_filters.setUniform1f(shader, "u_alpha", this.alpha);
        __native_filters.drawQuad();
        __native_filters.endFilter();
    };

    PIXI.filters.AlphaFilter = AlphaFilter;

    /* Filter rendering pipeline (shared by Container and Sprite) */

    /* Temporary FBO pool to avoid per-frame allocations. Capped to prevent GPU memory leaks. */
    var _filterFBOPool = [];
    var _filterFBOPoolMax = 8;

    function _acquireFilterFBO(w, h) {
        for (var i = 0; i < _filterFBOPool.length; i++) {
            var item = _filterFBOPool[i];
            if (!item.inUse && item.width === w && item.height === h) {
                item.inUse = true;
                return item;
            }
        }
        /* Evict oldest unused entry if pool is at capacity. */
        if (_filterFBOPool.length >= _filterFBOPoolMax) {
            for (var j = 0; j < _filterFBOPool.length; j++) {
                if (!_filterFBOPool[j].inUse) {
                    __native_renderer.deleteRenderTexture(
                        _filterFBOPool[j].fbo, _filterFBOPool[j].texture);
                    _filterFBOPool.splice(j, 1);
                    break;
                }
            }
        }
        var rt = __native_renderer.createRenderTexture(w, h);
        var item = { fbo: rt.fbo, texture: rt.texture, width: w, height: h, inUse: true };
        _filterFBOPool.push(item);
        return item;
    }

    function _releaseFilterFBO(item) {
        item.inUse = false;
    }

    /* Stack of {fbo, w, h} render targets for nested _renderFiltered calls. */
    var _fboStack = [];

    /* Output target of the filter currently being applied ({fbo, w, h} or null),
       so multi-pass filters can rebind it after their internal passes. */
    var _filterOutputTarget = null;

    /* Render this display object with its filter chain applied. Supports
       nesting: a child's filter pass draws back into the parent's FBO. */
    DisplayObject.prototype._renderFiltered = function(renderer) {
        var screenW = renderer && renderer.screen ? renderer.screen.width : 816;
        var screenH = renderer && renderer.screen ? renderer.screen.height : 624;

        __native_renderer.flush();

        /* Peek (not pop) the parent's target so sibling calls can also see it. */
        var restoreEntry = _fboStack.length > 0
            ? _fboStack[_fboStack.length - 1] : null;

        /* Render the subtree to a temporary FBO; push it so nested calls
           restore back to it. */
        var inputFBO = _acquireFilterFBO(screenW, screenH);
        _fboStack.push({ fbo: inputFBO.fbo, w: screenW, h: screenH });
        __native_renderer.bindRenderTexture(inputFBO.fbo, screenW, screenH);

        /* Clear to transparent black so filters preserve alpha. */
        __native_renderer.beginFrameTransparent();

        if (this._renderSelf && (!this.isSprite || (this._texture && this._texture.valid))) {
            this._renderSelf(renderer);
        }
        for (var i = 0; i < this.children.length; i++) {
            this.children[i].render(renderer);
        }

        __native_renderer.flush();

        /* Remove our entry; nested calls may have shifted the stack, so search. */
        for (var si = _fboStack.length - 1; si >= 0; si--) {
            if (_fboStack[si].fbo === inputFBO.fbo) {
                _fboStack.splice(si, 1);
                break;
            }
        }

        /* Apply the filter chain; each filter reads currentTex and writes a new FBO. */
        var currentTex = inputFBO.texture;
        var enabledFilters = [];
        for (var f = 0; f < this.filters.length; f++) {
            if (this.filters[f].enabled !== false && !this.filters[f]._rmBroken) {
                enabledFilters.push(this.filters[f]);
            }
        }

        if (enabledFilters.length > 0) {
            for (var fi = 0; fi < enabledFilters.length; fi++) {
                var outputFBO = _acquireFilterFBO(screenW, screenH);
                __native_renderer.bindRenderTexture(outputFBO.fbo, screenW, screenH);
                __native_renderer.beginFrameTransparent();

                /* Tell multi-pass filters where to draw their final result. */
                _filterOutputTarget = { fbo: outputFBO.fbo, w: screenW, h: screenH };
                try {
                    enabledFilters[fi].apply(renderer, currentTex, screenW, screenH);
                } catch (filterError) {
                    /* A filter this renderer cannot run (for example a multi-pass pixi-filters effect) must not take the
                       whole screen down: switch it off, log it once and pass the picture through unchanged. */
                    var failed = enabledFilters[fi];
                    failed._rmBroken = true; /* stays off even when a plugin sets enabled = true again */
                    if (typeof console !== "undefined") {
                        console.warn("filter switched off (" + ((failed.constructor && failed.constructor.name) || "?") + "): " +
                                     (filterError && filterError.message ? filterError.message : filterError));
                    }
                    __native_renderer.bindRenderTexture(outputFBO.fbo, screenW, screenH);
                    __native_renderer.beginFrameTransparent();
                    __native_renderer.setBlendMode(4); /* premultiplied normal: a plain copy onto the cleared target */
                    __native_renderer.drawQuad(currentTex, 0, 0, screenW, screenH, 0, 1, 1, 0, 0xFFFFFF, 1.0);
                }
                _filterOutputTarget = null;

                __native_renderer.flush();

                /* Release previous input (except the very first). */
                if (fi > 0) {
                    for (var p = 0; p < _filterFBOPool.length; p++) {
                        if (_filterFBOPool[p].texture === currentTex && _filterFBOPool[p].inUse) {
                            _releaseFilterFBO(_filterFBOPool[p]);
                            break;
                        }
                    }
                }
                currentTex = outputFBO.texture;
            }
        }

        /* Restore the parent's target from the saved entry, not the stack, so
           siblings do not consume each other's restore point. */
        if (restoreEntry) {
            __native_renderer.bindRenderTexture(
                restoreEntry.fbo, restoreEntry.w, restoreEntry.h);
        } else {
            __native_renderer.unbindRenderTexture();
        }
        __native_renderer.rebindBatch();

        /* The FBO already holds premultiplied colour scaled by each node's
           worldAlpha, so composite it with alpha 1 and premultiplied blending.
           Non-NORMAL object blend modes are kept. */
        var BLEND_NORMAL_PREMULT = 4;
        var objBlend = this.blendMode || 0;
        __native_renderer.setBlendMode(objBlend === 0 ? BLEND_NORMAL_PREMULT : objBlend);

        /* Clip to filterArea if set (used by Window._clientArea). */
        var fa = this.filterArea;
        if (fa && fa.width > 0 && fa.height > 0) {
            var u0 = fa.x / screenW;
            var u1 = (fa.x + fa.width) / screenW;
            var v0 = 1.0 - fa.y / screenH;
            var v1 = 1.0 - (fa.y + fa.height) / screenH;
            __native_renderer.drawQuad(
                currentTex, fa.x, fa.y, fa.width, fa.height,
                u0, v0, u1, v1,
                0xFFFFFF, 1.0
            );
        } else {
            __native_renderer.drawQuad(
                currentTex, 0, 0, screenW, screenH,
                0, 1, 1, 0,
                0xFFFFFF, 1.0
            );
        }
        __native_renderer.setBlendMode(objBlend);

        _releaseFilterFBO(inputFBO);
        for (var r = 0; r < _filterFBOPool.length; r++) {
            if (_filterFBOPool[r].texture === currentTex && _filterFBOPool[r].inUse) {
                _releaseFilterFBO(_filterFBOPool[r]);
                break;
            }
        }

    };

    /* --- Sprite / Graphics masks ---------------------------------------
       PIXI multiplies the masked object's pixels by the mask's alpha. The
       object (with its own filters) and the mask are each rendered to a
       screen-sized texture, combined with the mask shader into a third one,
       and that is composited onto the parent's target like a filter result. */
    function _canRenderMasked(mask) {
        return typeof mask.render === "function" &&
            typeof __native_filters !== "undefined" && __native_filters.maskShader &&
            typeof __native_renderer !== "undefined" && __native_renderer._active;
    }

    DisplayObject.prototype._renderMasked = function(renderer) {
        var screenW = renderer && renderer.screen ? renderer.screen.width : 816;
        var screenH = renderer && renderer.screen ? renderer.screen.height : 624;
        var mask = this._mask;

        __native_renderer.flush();
        var restoreEntry = _fboStack.length > 0 ? _fboStack[_fboStack.length - 1] : null;

        var renderInto = function(target, drawFn) {
            var fbo = _acquireFilterFBO(screenW, screenH);
            _fboStack.push({ fbo: fbo.fbo, w: screenW, h: screenH });
            __native_renderer.bindRenderTexture(fbo.fbo, screenW, screenH);
            __native_renderer.beginFrameTransparent();
            try {
                drawFn();
            } finally {
                __native_renderer.flush();
                for (var si = _fboStack.length - 1; si >= 0; si--) {
                    if (_fboStack[si].fbo === fbo.fbo) { _fboStack.splice(si, 1); break; }
                }
            }
            return fbo;
        };

        var self = this;
        var contentFBO = renderInto(null, function() {
            self._renderingMasked = true;
            try { self.render(renderer); } finally { self._renderingMasked = false; }
        });
        var maskFBO = renderInto(null, function() {
            /* The mask draws with its own transform whatever its visibility. */
            var wasMask = mask.isMask, wasVisible = mask.visible, wasRenderable = mask.renderable;
            mask.isMask = false; mask.visible = true; mask.renderable = true;
            try { mask.render(renderer); }
            finally { mask.isMask = wasMask; mask.visible = wasVisible; mask.renderable = wasRenderable; }
        });
        var outFBO = renderInto(null, function() {
            var shader = __native_filters.maskShader;
            __native_filters.beginFilter(shader, contentFBO.texture, screenW, screenH);
            __native_filters.setUniformTexture(shader, "u_mask", maskFBO.texture, 1);
            __native_filters.drawQuad();
            __native_filters.endFilter();
        });

        if (restoreEntry) {
            __native_renderer.bindRenderTexture(restoreEntry.fbo, restoreEntry.w, restoreEntry.h);
        } else {
            __native_renderer.unbindRenderTexture();
        }
        __native_renderer.rebindBatch();

        var BLEND_NORMAL_PREMULT = 4;
        var objBlend = this.blendMode || 0;
        __native_renderer.setBlendMode(objBlend === 0 ? BLEND_NORMAL_PREMULT : objBlend);
        __native_renderer.drawQuad(outFBO.texture, 0, 0, screenW, screenH, 0, 1, 1, 0, 0xFFFFFF, 1.0);
        __native_renderer.setBlendMode(objBlend);

        _releaseFilterFBO(contentFBO);
        _releaseFilterFBO(maskFBO);
        _releaseFilterFBO(outFBO);
    };

    globalThis.PIXI = PIXI;

})();
