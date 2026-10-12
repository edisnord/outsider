/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

/*
 * Canvas 2D shim: CanvasRenderingContext2D (fills, strokes, paths, text,
 * transforms, drawImage) on top of the native software rasterizer.
 */

(function() {
    "use strict";

    /* ---- CSS color parsing ---- */

    var _colorCache = {};
    var _colorCacheSize = 0;
    var _colorCacheMax = 4096;

    var _namedColors = {
        "black":   [0,0,0,255],       "white":   [255,255,255,255],
        "red":     [255,0,0,255],      "green":   [0,128,0,255],
        "blue":    [0,0,255,255],      "yellow":  [255,255,0,255],
        "cyan":    [0,255,255,255],    "magenta": [255,0,255,255],
        "gray":    [128,128,128,255],  "grey":    [128,128,128,255],
        "orange":  [255,165,0,255],    "purple":  [128,0,128,255],
        "pink":    [255,192,203,255],  "brown":   [165,42,42,255],
        "silver":  [192,192,192,255],  "gold":    [255,215,0,255],
        "lime":    [0,255,0,255],      "navy":    [0,0,128,255],
        "teal":    [0,128,128,255],    "maroon":  [128,0,0,255],
        "olive":   [128,128,0,255],    "aqua":    [0,255,255,255],
        "fuchsia": [255,0,255,255],    "transparent": [0,0,0,0],
    };

    function parseColor(str) {
        if (!str) return [0, 0, 0, 255];
        if (typeof str !== "string") return [0, 0, 0, 255];

        return _parseColorShared(str).slice();
    }

    /* Like parseColor but returns the cached array itself: callers must not modify it. */
    function _parseColorShared(str) {
        if (!str) return [0, 0, 0, 255];
        if (typeof str !== "string") return [0, 0, 0, 255];

        var cached = _colorCache[str];
        if (cached) return cached;

        var result = _parseColorUncached(str);
        if (_colorCacheSize >= _colorCacheMax) {
            /* Lighting plugins generate thousands of distinct rgba() strings; start over rather than
               stop caching. */
            _colorCache = {};
            _colorCacheSize = 0;
        }
        _colorCache[str] = result;
        _colorCacheSize++;
        return result;
    }

    function _parseColorUncached(str) {
        str = str.trim().toLowerCase();

        if (_namedColors[str]) return _namedColors[str];

        /* Fast paths for the common forms, without regular expressions (they are slow to run here). */
        var len = str.length, c0 = str.charCodeAt(0);
        if (c0 === 35 && (len === 7 || len === 9)) {
            var hr = parseInt(str.substr(1, 2), 16), hg = parseInt(str.substr(3, 2), 16);
            var hb = parseInt(str.substr(5, 2), 16), ha = len === 9 ? parseInt(str.substr(7, 2), 16) : 255;
            if (hr === hr && hg === hg && hb === hb && ha === ha) return [hr, hg, hb, ha];
        } else if (c0 === 114 && str.charCodeAt(1) === 103 && str.charCodeAt(2) === 98 &&
                   str.charCodeAt(len - 1) === 41) {
            var lp = str.indexOf("(");
            if (lp > 0) {
                var parts = str.substring(lp + 1, len - 1).split(",");
                if (parts.length === 3 || parts.length === 4) {
                    var fr = parseInt(parts[0], 10), fg = parseInt(parts[1], 10), fb = parseInt(parts[2], 10);
                    var fa = parts.length === 4 ? Math.round(parseFloat(parts[3]) * 255) : 255;
                    if (fr === fr && fg === fg && fb === fb && fa === fa) return [fr, fg, fb, fa];
                }
            }
        }

        var m = str.match(/^#([0-9a-f])([0-9a-f])([0-9a-f])$/);
        if (m) {
            return [
                parseInt(m[1] + m[1], 16),
                parseInt(m[2] + m[2], 16),
                parseInt(m[3] + m[3], 16),
                255
            ];
        }

        m = str.match(/^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/);
        if (m) {
            return [parseInt(m[1], 16), parseInt(m[2], 16), parseInt(m[3], 16), 255];
        }

        m = str.match(/^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/);
        if (m) {
            return [parseInt(m[1], 16), parseInt(m[2], 16), parseInt(m[3], 16), parseInt(m[4], 16)];
        }

        m = str.match(/^rgb\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)$/);
        if (m) {
            return [parseInt(m[1]), parseInt(m[2]), parseInt(m[3]), 255];
        }

        m = str.match(/^rgba\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*([\d.]+)\s*\)$/);
        if (m) {
            return [parseInt(m[1]), parseInt(m[2]), parseInt(m[3]),
                    Math.round(parseFloat(m[4]) * 255)];
        }

        return [0, 0, 0, 255];
    }

    /* ---- CSS font string parsing ---- */

    /* Extract pixel size and family from a CSS font string ("bold 24px sans-serif"). */
    function parseFont(fontStr) {
        var size = 10;
        var family = "sans-serif";

        if (!fontStr) return { size: size, family: family };

        var m = fontStr.match(/(\d+(?:\.\d+)?)\s*px/);
        if (m) {
            size = parseFloat(m[1]);
        }

        var sizeIdx = fontStr.indexOf("px");
        if (sizeIdx >= 0) {
            var rest = fontStr.substring(sizeIdx + 2).trim();
            if (rest.length > 0) {
                family = rest.replace(/['"]/g, "").trim();
                /* Native canvas2d takes a single family name; use the first. */
                var commaIdx = family.indexOf(",");
                if (commaIdx >= 0) {
                    family = family.substring(0, commaIdx).trim();
                }
            }
        }

        return { size: size, family: family };
    }

    /* ---- Enum mappings to native values ---- */

    /* What this context cannot do goes to the compatibility log (error_handler.h), once per kind of request. */
    function compat(key, detail) {
        if (typeof globalThis.__compat_report === "function") globalThis.__compat_report("canvas2d", key, detail);
    }

    var _compOps = {
        "source-over": 0,
        "source-atop": 1,
        "source-in":   2,
        "source-out":  3,
        "destination-over": 4,
        "destination-atop": 5,
        "destination-in":   6,
        "destination-out":  7,
        "lighter":     8,
        "copy":        9,
        "xor":         10,
        "multiply":    11,
        "screen":      12,
        "difference":  13,
        "saturation":  14,
    };

    var _textAligns = {
        "left":   0,
        "center": 1,
        "right":  2,
        "start":  3,
        "end":    4,
    };

    var _textBaselines = {
        "top":         0,
        "hanging":     1,
        "middle":      2,
        "alphabetic":  3,
        "ideographic": 4,
        "bottom":      5,
    };

    /* ---- CanvasRenderingContext2D ---- */

    function CanvasRenderingContext2D(canvas) {
        this.canvas = canvas;
        this._handle = 0;  /* native Canvas2DHandle */

        /* Drawing state is kept here and synced to native on change. */
        this._fillStyle = "#000000";
        this._strokeStyle = "#000000";
        this._globalAlpha = 1.0;
        this._globalCompositeOperation = "source-over";
        this._font = "10px sans-serif";
        this._textAlign = "start";
        this._textBaseline = "alphabetic";
        this._lineWidth = 1;
        this.lineCap = "butt";
        this.lineJoin = "miter";
        this.imageSmoothingEnabled = true;
        this._stateStack = [];

        /* CTM [a, b, c, d, e, f]: x' = a*x + c*y + e ; y' = b*x + d*y + f. */
        this._ctm = [1, 0, 0, 1, 0, 0];
        /* Current path: subpaths of device-space points (transformed as added, per spec). */
        this._path = [];
        this._pathCur = null;
        this._curPt = null;     /* last point in user space, for curve flattening */
        this._clip = null;      /* {x,y,w,h} device-space clip rect, or null */

        this._ensureNative();
    }

    CanvasRenderingContext2D.prototype._ensureNative = function() {
        if (this._handle) return;
        if (typeof __native_canvas2d !== "undefined") {
            var w = this.canvas._width || 1;
            var h = this.canvas._height || 1;
            this._handle = __native_canvas2d.create(w, h);
            /* frees the native canvas when this context is garbage collected */
            if (this._handle && __native_canvas2d.createToken) this._token = __native_canvas2d.createToken(this._handle);
        }
    };

    CanvasRenderingContext2D.prototype._syncFillColor = function() {
        if (!this._handle) return;
        if (this._fillStyle instanceof CanvasGradient) return;
        var c = _parseColorShared(this._fillStyle);   /* read only */
        __native_canvas2d.setFillColor(this._handle, c[0], c[1], c[2], c[3]);
    };

    CanvasRenderingContext2D.prototype._syncStrokeColor = function() {
        if (!this._handle) return;
        var c = _parseColorShared(this._strokeStyle);   /* read only */
        __native_canvas2d.setStrokeColor(this._handle, c[0], c[1], c[2], c[3]);
    };

    /* ---- Properties ---- */

    Object.defineProperty(CanvasRenderingContext2D.prototype, "fillStyle", {
        get: function() { return this._fillStyle; },
        set: function(v) {
            this._fillStyle = v;
            this._syncFillColor();
        }
    });

    Object.defineProperty(CanvasRenderingContext2D.prototype, "strokeStyle", {
        get: function() { return this._strokeStyle; },
        set: function(v) {
            this._strokeStyle = v;
            this._syncStrokeColor();
        }
    });

    Object.defineProperty(CanvasRenderingContext2D.prototype, "globalAlpha", {
        get: function() { return this._globalAlpha; },
        set: function(v) {
            this._globalAlpha = v;
            if (this._handle) __native_canvas2d.setGlobalAlpha(this._handle, v);
        }
    });

    Object.defineProperty(CanvasRenderingContext2D.prototype, "globalCompositeOperation", {
        get: function() { return this._globalCompositeOperation; },
        set: function(v) {
            this._globalCompositeOperation = v;
            if (this._handle) {
                var op = _compOps[v];
                if (op === undefined) {
                    compat("globalCompositeOperation=" + v, "operation not supported, drawn as source-over");
                    op = 0;
                }
                __native_canvas2d.setCompositeOp(this._handle, op);
            }
        }
    });

    Object.defineProperty(CanvasRenderingContext2D.prototype, "font", {
        get: function() { return this._font; },
        set: function(v) {
            this._font = v;
            if (this._handle) {
                var f = parseFont(v);
                __native_canvas2d.setFont(this._handle, f.family, f.size);
            }
        }
    });

    Object.defineProperty(CanvasRenderingContext2D.prototype, "textAlign", {
        get: function() { return this._textAlign; },
        set: function(v) {
            this._textAlign = v;
            if (this._handle) {
                var a = _textAligns[v];
                if (a === undefined) { compat("textAlign=" + v, "unknown value, treated as start"); a = 3; }
                __native_canvas2d.setTextAlign(this._handle, a);
            }
        }
    });

    Object.defineProperty(CanvasRenderingContext2D.prototype, "textBaseline", {
        get: function() { return this._textBaseline; },
        set: function(v) {
            this._textBaseline = v;
            if (this._handle) {
                var b = _textBaselines[v];
                if (b === undefined) { compat("textBaseline=" + v, "unknown value, treated as alphabetic"); b = 3; }
                __native_canvas2d.setTextBaseline(this._handle, b);
            }
        }
    });

    Object.defineProperty(CanvasRenderingContext2D.prototype, "lineWidth", {
        get: function() { return this._lineWidth; },
        set: function(v) {
            this._lineWidth = v;
            if (this._handle) __native_canvas2d.setLineWidth(this._handle, v);
        }
    });

    /* ---- State stack ---- */

    CanvasRenderingContext2D.prototype.save = function() {
        this._stateStack.push({
            fillStyle: this._fillStyle,
            strokeStyle: this._strokeStyle,
            globalAlpha: this._globalAlpha,
            globalCompositeOperation: this._globalCompositeOperation,
            font: this._font,
            textAlign: this._textAlign,
            textBaseline: this._textBaseline,
            lineWidth: this._lineWidth,
            imageSmoothingEnabled: this.imageSmoothingEnabled,
            ctm: this._ctm.slice(0),
            clip: this._clip
        });
        if (this._handle) __native_canvas2d.save(this._handle);
    };

    CanvasRenderingContext2D.prototype.restore = function() {
        var state = this._stateStack.pop();
        if (state) {
            this._fillStyle = state.fillStyle;
            this._strokeStyle = state.strokeStyle;
            this._globalAlpha = state.globalAlpha;
            this._globalCompositeOperation = state.globalCompositeOperation;
            this._font = state.font;
            this._textAlign = state.textAlign;
            this._textBaseline = state.textBaseline;
            this._lineWidth = state.lineWidth;
            this.imageSmoothingEnabled = state.imageSmoothingEnabled;
            this._ctm = state.ctm;
            this._clip = state.clip;
        }
        if (this._handle) __native_canvas2d.restore(this._handle);
    };

    /* ---- Drawing operations ---- */

    /* Rect primitives honour the CTM and clip: translate/scale map to a native
       rect directly, rotation/skew goes through the path rasteriser. */
    CanvasRenderingContext2D.prototype._withTempPath = function(fn) {
        var saved = this._path, savedCur = this._pathCur, savedPt = this._curPt;
        this._path = []; this._pathCur = null; this._curPt = null;
        fn.call(this);
        this._path = saved; this._pathCur = savedCur; this._curPt = savedPt;
    };

    CanvasRenderingContext2D.prototype.fillRect = function(x, y, w, h) {
        if (!this._handle) return;
        if (!this._ctmIsAxisAligned()) {
            this._withTempPath(function() { this.rect(x, y, w, h); this.fill(); });
            return;
        }
        var r = this._deviceRect(x, y, w, h);
        if (!r) return;
        if (this._fillStyle instanceof CanvasGradient) {
            this._fillGradientRect(r[0], r[1], r[2], r[3]);
            return;
        }
        this._syncFillColor();
        __native_canvas2d.fillRect(this._handle, r[0], r[1], r[2], r[3]);
    };

    CanvasRenderingContext2D.prototype.strokeRect = function(x, y, w, h) {
        if (!this._handle) return;
        if (!this._ctmIsAxisAligned()) {
            this._withTempPath(function() { this.rect(x, y, w, h); this.stroke(); });
            return;
        }
        var r = this._deviceRect(x, y, w, h, true);
        if (!r) return;
        this._syncStrokeColor();
        __native_canvas2d.strokeRect(this._handle, r[0], r[1], r[2], r[3]);
    };

    CanvasRenderingContext2D.prototype.clearRect = function(x, y, w, h) {
        if (!this._handle) return;
        /* A rotated clear is approximated by the transformed bounding box. */
        var r = this._deviceRect(x, y, w, h);
        if (!r) return;
        __native_canvas2d.clearRect(this._handle, r[0], r[1], r[2], r[3]);
    };

    /* Text is placed at the transformed anchor; glyphs are not rotated/scaled
       (native text has no transform support). */
    CanvasRenderingContext2D.prototype.fillText = function(text, x, y, maxWidth) {
        if (!this._handle) return;
        var p = this._tx(x, y);
        this._syncFillColor();
        __native_canvas2d.fillText(this._handle, String(text), p[0], p[1]);
    };

    CanvasRenderingContext2D.prototype.strokeText = function(text, x, y, maxWidth) {
        if (!this._handle) return;
        var p = this._tx(x, y);
        this._syncStrokeColor();
        __native_canvas2d.strokeText(this._handle, String(text), p[0], p[1]);
    };

    CanvasRenderingContext2D.prototype.measureText = function(text) {
        if (!this._handle) {
            var fontSize = 10;
            var m = this._font.match(/(\d+)px/);
            if (m) fontSize = parseInt(m[1]);
            return { width: (text || "").length * fontSize * 0.6 };
        }
        var str = String(text || "");
        var w = __native_canvas2d.measureText(this._handle, str);
        var res = { width: w };
        var fm = /(\d+(?:\.\d+)?)px/.exec(this._font || "");
        var handle = this._handle, size = fm ? parseFloat(fm[1]) : 10, cache = null;
        /* actualBoundingBox*: the real glyph box, worked out only when a script asks for it (text-fitting plugins do) */
        function bounds() {
            if (!cache) cache = __native_canvas2d.measureTextBounds ? __native_canvas2d.measureTextBounds(handle, str) : [0, w, size * 0.8, size * 0.2];
            return cache;
        }
        Object.defineProperty(res, "actualBoundingBoxLeft", { enumerable: true, get: function() { return bounds()[0]; } });
        Object.defineProperty(res, "actualBoundingBoxRight", { enumerable: true, get: function() { return bounds()[1]; } });
        Object.defineProperty(res, "actualBoundingBoxAscent", { enumerable: true, get: function() { return bounds()[2]; } });
        Object.defineProperty(res, "actualBoundingBoxDescent", { enumerable: true, get: function() { return bounds()[3]; } });
        res.fontBoundingBoxAscent = size * 0.88;
        res.fontBoundingBoxDescent = size * 0.12;
        res.emHeightAscent = size * 0.8;
        res.emHeightDescent = size * 0.2;
        return res;
    };

    CanvasRenderingContext2D.prototype.drawImage = function(source) {
        if (!this._handle || !source) return;

        /* Supports the 3-, 5- and 9-argument forms. */
        var a = arguments, n = a.length;
        var srcW = 0, srcH = 0, srcPixels = null, srcCanvas = 0;
        var useHandle = !!(source._imageHandle && typeof __native_canvas2d.drawImageHandle !== "undefined");

        if (useHandle) {
            /* Image with a native handle: fast path. */
            srcW = source._naturalWidth || source._width || 0;
            srcH = source._naturalHeight || source._height || 0;
        } else if (source.tagName === "CANVAS" && source._context2d) {
            if (source._context2d._handle) {
                if (typeof __native_canvas2d.drawCanvas === "function") {
                    /* canvas to canvas inside the native layer: no copy of the source's pixels into JS */
                    srcCanvas = source._context2d._handle;
                } else {
                    srcPixels = __native_canvas2d.getPixels(source._context2d._handle);
                }
                srcW = source._width;
                srcH = source._height;
            }
        } else if (source._imageHandle && typeof __native_image !== "undefined") {
            srcPixels = __native_image.getImagePixels(source._imageHandle);
            srcW = source._naturalWidth || source._width || 0;
            srcH = source._naturalHeight || source._height || 0;
        }
        if (!srcW || !srcH || (!useHandle && !srcPixels && !srcCanvas)) return;

        var sx = 0, sy = 0, sw = srcW, sh = srcH, dx, dy, dw, dh;
        if (n <= 3) {
            dx = +a[1] || 0; dy = +a[2] || 0; dw = srcW; dh = srcH;
        } else if (n <= 5) {
            dx = +a[1] || 0; dy = +a[2] || 0; dw = +a[3] || 0; dh = +a[4] || 0;
        } else {
            sx = +a[1] || 0; sy = +a[2] || 0; sw = +a[3] || 0; sh = +a[4] || 0;
            dx = +a[5] || 0; dy = +a[6] || 0; dw = +a[7] || 0; dh = +a[8] || 0;
        }

        var r = this._mapDrawRect(sx, sy, sw, sh, dx, dy, dw, dh);
        if (!r) return;

        if (useHandle) {
            __native_canvas2d.drawImageHandle(this._handle, source._imageHandle,
                r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
        } else if (srcCanvas) {
            __native_canvas2d.drawCanvas(this._handle, srcCanvas,
                r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
        } else {
            __native_canvas2d.drawImage(this._handle, srcPixels, srcW, srcH,
                r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
        }
    };

    /* ---- Pixel manipulation ---- */

    var MAX_IMAGEDATA_BYTES = 64 * 1024 * 1024; /* matches the native binding's cap */

    CanvasRenderingContext2D.prototype.getImageData = function(x, y, w, h) {
        w = w | 0;
        h = h | 0;
        if (!this._handle || w <= 0 || h <= 0) {
            var len = w * h * 4;
            if (len <= 0 || len > MAX_IMAGEDATA_BYTES) {
                return { width: w, height: h, data: new Uint8ClampedArray(0) };
            }
            return { width: w, height: h, data: new Uint8ClampedArray(len) };
        }
        var ab = __native_canvas2d.getImageData(this._handle, x | 0, y | 0, w, h);
        if (!ab) {
            var len2 = w * h * 4;
            if (len2 <= 0 || len2 > MAX_IMAGEDATA_BYTES) {
                return { width: w, height: h, data: new Uint8ClampedArray(0) };
            }
            return { width: w, height: h, data: new Uint8ClampedArray(len2) };
        }
        return { width: w, height: h, data: new Uint8ClampedArray(ab) };
    };

    CanvasRenderingContext2D.prototype.putImageData = function(imageData, dx, dy) {
        if (!this._handle || !imageData || !imageData.data) return;
        var w = imageData.width;
        var h = imageData.height;
        var ab;
        if (imageData.data.buffer instanceof ArrayBuffer) {
            ab = imageData.data.buffer;
        } else {
            ab = new ArrayBuffer(imageData.data.length);
            var view = new Uint8Array(ab);
            for (var i = 0; i < imageData.data.length; i++) {
                view[i] = imageData.data[i];
            }
        }
        __native_canvas2d.putImageData(this._handle, ab, dx | 0, dy | 0, w, h);
    };

    CanvasRenderingContext2D.prototype.createImageData = function(w, h) {
        if (typeof w === "object") { h = w.height; w = w.width; }
        w = Math.abs(w | 0);
        h = Math.abs(h | 0);
        if (w > 16384 || h > 16384 || w * h * 4 > MAX_IMAGEDATA_BYTES) {
            return { width: 0, height: 0, data: new Uint8ClampedArray(0) };
        }
        var len = w * h * 4;
        return { width: w, height: h, data: new Uint8ClampedArray(len) };
    };

    /* ---- Gradient / pattern ---- */

    function CanvasGradient(x0, y0, x1, y1) {
        this._x0 = x0;
        this._y0 = y0;
        this._x1 = x1;
        this._y1 = y1;
        this._stops = [];
    }

    CanvasGradient.prototype.addColorStop = function(offset, color) {
        var stops = this._stops, n = stops.length;
        stops.push({ offset: offset, color: _parseColorShared(color) });
        /* Stops nearly always arrive in order; sorting every time was a hot spot. */
        if (n > 0 && stops[n - 1].offset > offset) {
            stops.sort(function(a, b) { return a.offset - b.offset; });
        }
    };

    function _interpolateGradientColor(stops, t) {
        if (stops.length === 0) return [0, 0, 0, 255];
        if (t <= stops[0].offset) return stops[0].color;
        if (t >= stops[stops.length - 1].offset) return stops[stops.length - 1].color;
        for (var i = 0; i < stops.length - 1; i++) {
            if (t >= stops[i].offset && t <= stops[i + 1].offset) {
                var range = stops[i + 1].offset - stops[i].offset;
                var localT = range > 0 ? (t - stops[i].offset) / range : 0;
                var c0 = stops[i].color;
                var c1 = stops[i + 1].color;
                return [
                    (c0[0] + (c1[0] - c0[0]) * localT + 0.5) | 0,
                    (c0[1] + (c1[1] - c0[1]) * localT + 0.5) | 0,
                    (c0[2] + (c1[2] - c0[2]) * localT + 0.5) | 0,
                    (c0[3] + (c1[3] - c0[3]) * localT + 0.5) | 0
                ];
            }
        }
        return stops[stops.length - 1].color;
    }

    CanvasRenderingContext2D.prototype.createLinearGradient = function(x0, y0, x1, y1) {
        return new CanvasGradient(x0, y0, x1, y1);
    };

    /* Approximated as concentric rings around the outer circle's center
       (the common r0=0, same-center usage). */
    CanvasRenderingContext2D.prototype.createRadialGradient = function(x0, y0, r0, x1, y1, r1) {
        var g = new CanvasGradient(x0, y0, x1, y1);
        g._radial = true;
        g._cx = x1;
        g._cy = y1;
        g._r0 = r0;
        g._r1 = r1;
        return g;
    };

    CanvasRenderingContext2D.prototype.createPattern = function(image, rep) {
        compat("createPattern", "returns an empty object; filling with it draws nothing");
        return {};
    };

    /* Fill a device-space rect with the current gradient, one span at a time. */
    CanvasRenderingContext2D.prototype._fillGradientRect = function(x, y, w, h) {
        var grad = this._gradientToDevice(this._fillStyle);
        var stops = grad._stops;
        if (stops.length === 0) return;

        x = x | 0; y = y | 0; w = w | 0; h = h | 0;
        if (w <= 0 || h <= 0) return;

        /* Native fill: computing a colour per pixel is far too slow in JS (a lighting plugin fills
           whole-screen radial gradients every frame). */
        if (typeof __native_canvas2d.fillGradient === 'function') {
            var flat = [];
            for (var si = 0; si < stops.length; si++) {
                var sc = stops[si].color;
                flat.push(stops[si].offset, sc[0], sc[1], sc[2], sc[3]);
            }
            if (grad._radial) {
                __native_canvas2d.fillGradient(this._handle, x, y, w, h, 1, grad._cx, grad._cy, grad._r0, grad._r1, flat);
            } else {
                __native_canvas2d.fillGradient(this._handle, x, y, w, h, 0, grad._x0, grad._y0, grad._x1, grad._y1, flat);
            }
            return;
        }

        if (grad._radial) {
            var cx = grad._cx, cy = grad._cy;
            var r0 = grad._r0, r1 = grad._r1;
            var denom = r1 - r0;
            for (var row = 0; row < h; row++) {
                var py = y + row + 0.5;
                var runStart = 0;
                var runColor = null;
                for (var col = 0; col < w; col++) {
                    var px = x + col + 0.5;
                    var ddx = px - cx, ddy = py - cy;
                    var dist = Math.sqrt(ddx * ddx + ddy * ddy);
                    var rt = denom !== 0 ? (dist - r0) / denom : (dist <= r0 ? 0 : 1);
                    if (rt < 0) rt = 0; else if (rt > 1) rt = 1;
                    var rc = _interpolateGradientColor(stops, rt);
                    if (runColor === null) {
                        runColor = rc; runStart = col;
                    } else if (rc[0] !== runColor[0] || rc[1] !== runColor[1] ||
                               rc[2] !== runColor[2] || rc[3] !== runColor[3]) {
                        __native_canvas2d.setFillColor(this._handle, runColor[0], runColor[1], runColor[2], runColor[3]);
                        __native_canvas2d.fillRect(this._handle, x + runStart, y + row, col - runStart, 1);
                        runColor = rc; runStart = col;
                    }
                }
                if (runColor !== null) {
                    __native_canvas2d.setFillColor(this._handle, runColor[0], runColor[1], runColor[2], runColor[3]);
                    __native_canvas2d.fillRect(this._handle, x + runStart, y + row, w - runStart, 1);
                }
            }
            return;
        }

        var dx = grad._x1 - grad._x0;
        var dy = grad._y1 - grad._y0;
        var lenSq = dx * dx + dy * dy;

        if (lenSq === 0) {
            var c = stops[0].color;
            __native_canvas2d.setFillColor(this._handle, c[0], c[1], c[2], c[3]);
            __native_canvas2d.fillRect(this._handle, x, y, w, h);
            return;
        }

        var isVertical = (dx === 0);
        var isHorizontal = (dy === 0);

        if (isVertical) {
            for (var row = 0; row < h; row++) {
                var t = (y + row + 0.5 - grad._y0) / dy;
                if (t < 0) t = 0; else if (t > 1) t = 1;
                var c = _interpolateGradientColor(stops, t);
                __native_canvas2d.setFillColor(this._handle, c[0], c[1], c[2], c[3]);
                __native_canvas2d.fillRect(this._handle, x, y + row, w, 1);
            }
        } else if (isHorizontal) {
            for (var col = 0; col < w; col++) {
                var t = (x + col + 0.5 - grad._x0) / dx;
                if (t < 0) t = 0; else if (t > 1) t = 1;
                var c = _interpolateGradientColor(stops, t);
                __native_canvas2d.setFillColor(this._handle, c[0], c[1], c[2], c[3]);
                __native_canvas2d.fillRect(this._handle, x + col, y, 1, h);
            }
        } else {
            /* Diagonal: project each row center onto the gradient vector. */
            for (var row = 0; row < h; row++) {
                var py = y + row + 0.5;
                var px = x + w * 0.5;
                var t = ((px - grad._x0) * dx + (py - grad._y0) * dy) / lenSq;
                if (t < 0) t = 0; else if (t > 1) t = 1;
                var c = _interpolateGradientColor(stops, t);
                __native_canvas2d.setFillColor(this._handle, c[0], c[1], c[2], c[3]);
                __native_canvas2d.fillRect(this._handle, x, y + row, w, 1);
            }
        }
    };

    /* ---- Transforms ---- */
    /* The native context has no transform support, so the CTM lives here.
       Path points are transformed as added; rect/image primitives are mapped
       at draw time (rotated image blits are approximated as axis-aligned). */

    /* CTM = CTM x M (M applied first, per spec). */
    function _mulMat(m, n) {
        return [
            m[0] * n[0] + m[2] * n[1],        m[1] * n[0] + m[3] * n[1],
            m[0] * n[2] + m[2] * n[3],        m[1] * n[2] + m[3] * n[3],
            m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]
        ];
    }

    CanvasRenderingContext2D.prototype._tx = function(x, y) {
        var m = this._ctm;
        return [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]];
    };
    CanvasRenderingContext2D.prototype._ctmIsIdentity = function() {
        var m = this._ctm;
        return m[0] === 1 && m[1] === 0 && m[2] === 0 && m[3] === 1 && m[4] === 0 && m[5] === 0;
    };
    CanvasRenderingContext2D.prototype._ctmIsAxisAligned = function() {
        var m = this._ctm;
        return m[1] === 0 && m[2] === 0;
    };
    /* Uniform scale factor of the CTM (sqrt of |det|), for line widths and radii. */
    CanvasRenderingContext2D.prototype._ctmScale = function() {
        var m = this._ctm;
        return Math.sqrt(Math.abs(m[0] * m[3] - m[1] * m[2])) || 1;
    };

    CanvasRenderingContext2D.prototype.setTransform = function(a, b, c, d, e, f) {
        if (typeof a === "object" && a) { var t = a; a = t.a; b = t.b; c = t.c; d = t.d; e = t.e; f = t.f; }
        this._ctm = [+a || 0, +b || 0, +c || 0, +d || 0, +e || 0, +f || 0];
    };
    CanvasRenderingContext2D.prototype.resetTransform = function() { this._ctm = [1, 0, 0, 1, 0, 0]; };
    CanvasRenderingContext2D.prototype.transform = function(a, b, c, d, e, f) {
        this._ctm = _mulMat(this._ctm, [+a || 0, +b || 0, +c || 0, +d || 0, +e || 0, +f || 0]);
    };
    CanvasRenderingContext2D.prototype.translate = function(x, y) {
        this._ctm = _mulMat(this._ctm, [1, 0, 0, 1, +x || 0, +y || 0]);
    };
    CanvasRenderingContext2D.prototype.scale = function(x, y) {
        this._ctm = _mulMat(this._ctm, [+x || 0, 0, 0, +y || 0, 0, 0]);
    };
    CanvasRenderingContext2D.prototype.rotate = function(angle) {
        var cs = Math.cos(angle), sn = Math.sin(angle);
        this._ctm = _mulMat(this._ctm, [cs, sn, -sn, cs, 0, 0]);
    };
    CanvasRenderingContext2D.prototype.getTransform = function() {
        var m = this._ctm;
        return { a: m[0], b: m[1], c: m[2], d: m[3], e: m[4], f: m[5] };
    };

    /* Map a user rect to an integer device rect through the CTM and (unless
       noClip) the clip rect. Returns [x, y, w, h] or null. */
    CanvasRenderingContext2D.prototype._deviceRect = function(x, y, w, h, noClip) {
        var x0, y0, x1, y1;
        if (this._ctmIsIdentity()) {
            x0 = x | 0; y0 = y | 0; x1 = x0 + (w | 0); y1 = y0 + (h | 0);
        } else {
            var p0 = this._tx(x, y), p1 = this._tx(x + w, y + h);
            x0 = Math.round(Math.min(p0[0], p1[0])); x1 = Math.round(Math.max(p0[0], p1[0]));
            y0 = Math.round(Math.min(p0[1], p1[1])); y1 = Math.round(Math.max(p0[1], p1[1]));
        }
        if (x1 < x0) { var t = x0; x0 = x1; x1 = t; }
        if (y1 < y0) { var u = y0; y0 = y1; y1 = u; }
        var c = this._clip;
        if (c && !noClip) {
            x0 = Math.max(x0, c.x); y0 = Math.max(y0, c.y);
            x1 = Math.min(x1, c.x + c.w); y1 = Math.min(y1, c.y + c.h);
        }
        if (x1 <= x0 || y1 <= y0) return null;
        return [x0, y0, x1 - x0, y1 - y0];
    };

    /* Map a drawImage source/dest pair through the CTM and clip; the clip trims
       the destination and shrinks the source proportionally. Returns
       [sx, sy, sw, sh, dx, dy, dw, dh] or null. */
    CanvasRenderingContext2D.prototype._mapDrawRect = function(sx, sy, sw, sh, dx, dy, dw, dh) {
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return null;
        var m = this._ctm;
        if (!this._ctmIsIdentity()) {
            if (m[1] === 0 && m[2] === 0) {
                var ax = m[0] * dx + m[4], ay = m[3] * dy + m[5];
                var bx = m[0] * (dx + dw) + m[4], by = m[3] * (dy + dh) + m[5];
                dx = Math.min(ax, bx); dy = Math.min(ay, by);
                dw = Math.abs(bx - ax); dh = Math.abs(by - ay);
            } else {
                /* Rotation/skew: place the scaled image at the transformed origin. */
                var p = this._tx(dx, dy), s = this._ctmScale();
                dx = p[0]; dy = p[1]; dw *= s; dh *= s;
            }
        }
        var c = this._clip;
        if (c) {
            var nx0 = Math.max(dx, c.x), ny0 = Math.max(dy, c.y);
            var nx1 = Math.min(dx + dw, c.x + c.w), ny1 = Math.min(dy + dh, c.y + c.h);
            if (nx1 <= nx0 || ny1 <= ny0) return null;
            var fx0 = (nx0 - dx) / dw, fx1 = (nx1 - dx) / dw;
            var fy0 = (ny0 - dy) / dh, fy1 = (ny1 - dy) / dh;
            sx += sw * fx0; sw *= (fx1 - fx0);
            sy += sh * fy0; sh *= (fy1 - fy0);
            dx = nx0; dy = ny0; dw = nx1 - nx0; dh = ny1 - ny0;
        }
        var r = [Math.round(sx), Math.round(sy), Math.round(sw), Math.round(sh),
                 Math.round(dx), Math.round(dy), Math.round(dw), Math.round(dh)];
        if (r[2] <= 0 || r[3] <= 0 || r[6] <= 0 || r[7] <= 0) return null;
        return r;
    };

    /* Gradient geometry mapped through the CTM. */
    CanvasRenderingContext2D.prototype._gradientToDevice = function(g) {
        if (this._ctmIsIdentity()) return g;
        var p0 = this._tx(g._x0, g._y0), p1 = this._tx(g._x1, g._y1), s = this._ctmScale();
        var d = { _stops: g._stops, _x0: p0[0], _y0: p0[1], _x1: p1[0], _y1: p1[1], _radial: g._radial };
        if (g._radial) {
            var pc = this._tx(g._cx, g._cy);
            d._cx = pc[0]; d._cy = pc[1]; d._r0 = g._r0 * s; d._r1 = g._r1 * s;
        }
        return d;
    };

    /* ---- Paths ---- */
    /* Subpaths are flattened polylines in device space. fill() uses a
       nonzero-winding scanline rasteriser emitting 1px-high native spans
       (needed for Bitmap.drawCircle). Edges are not anti-aliased. */

    CanvasRenderingContext2D.prototype.beginPath = function() {
        this._path = []; this._pathCur = null; this._curPt = null;
    };

    CanvasRenderingContext2D.prototype.moveTo = function(x, y) {
        var p = this._tx(x, y);
        this._pathCur = { pts: [p[0], p[1]], closed: false };
        this._path.push(this._pathCur);
        this._curPt = [x, y];
    };

    CanvasRenderingContext2D.prototype.lineTo = function(x, y) {
        if (!this._pathCur) { this.moveTo(x, y); return; }
        var m = this._ctm;
        this._pathCur.pts.push(m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]);
        this._curPt = [x, y];
    };

    CanvasRenderingContext2D.prototype.closePath = function() {
        var cur = this._pathCur;
        if (!cur || cur.pts.length < 2) return;
        cur.closed = true;
        /* Per spec a new subpath begins at the closed subpath's start. */
        var m = this._ctm, sx = cur.pts[0], sy = cur.pts[1];
        this._pathCur = { pts: [sx, sy], closed: false };
        this._path.push(this._pathCur);
        /* Recover the user-space start point (inverse CTM) for curves. */
        var det = m[0] * m[3] - m[1] * m[2];
        if (det) {
            var ux = sx - m[4], uy = sy - m[5];
            this._curPt = [(m[3] * ux - m[2] * uy) / det, (-m[1] * ux + m[0] * uy) / det];
        }
    };

    CanvasRenderingContext2D.prototype.rect = function(x, y, w, h) {
        this.moveTo(x, y);
        this.lineTo(x + w, y);
        this.lineTo(x + w, y + h);
        this.lineTo(x, y + h);
        this.closePath();
    };

    CanvasRenderingContext2D.prototype.arc = function(x, y, r, sa, ea, ccw) {
        r = Math.abs(r);
        var TWO_PI = Math.PI * 2;
        var delta = ea - sa;
        if (!ccw) {
            if (delta < 0) delta = (delta % TWO_PI) + TWO_PI;
            if (delta > TWO_PI) delta = TWO_PI;
        } else {
            if (delta > 0) delta = (delta % TWO_PI) - TWO_PI;
            if (delta < -TWO_PI) delta = -TWO_PI;
        }
        var segs = Math.max(8, Math.min(96, Math.ceil(r * this._ctmScale() * 0.5 * Math.abs(delta) / Math.PI * 2)));
        /* Points are generated by rotating a vector step by step (no trig per point) and mapped through the
           CTM straight into the subpath (no per-point arrays), because lighting plugins draw many arcs per
           frame. An open subpath is joined to the arc start by a line. */
        var m = this._ctm, m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3], m4 = m[4], m5 = m[5];
        var step = delta / segs, cs = Math.cos(step), sn = Math.sin(step);
        var ux = Math.cos(sa), uy = Math.sin(sa);
        var px = x + ux * r, py = y + uy * r;
        if (!this._pathCur) this.moveTo(px, py);
        else this._pathCur.pts.push(m0 * px + m2 * py + m4, m1 * px + m3 * py + m5);
        var pts = this._pathCur.pts;
        for (var i = 1; i <= segs; i++) {
            var nx = ux * cs - uy * sn;
            uy = ux * sn + uy * cs;
            ux = nx;
            px = x + ux * r; py = y + uy * r;
            pts.push(m0 * px + m2 * py + m4, m1 * px + m3 * py + m5);
        }
        this._curPt = [px, py];
    };

    CanvasRenderingContext2D.prototype.ellipse = function(x, y, rx, ry, rotation, sa, ea, ccw) {
        this.save();
        this.translate(x, y); this.rotate(rotation || 0); this.scale(rx, ry);
        this.arc(0, 0, 1, sa, ea, ccw);
        this.restore();
    };

    /* Approximated with the tangent corner. */
    CanvasRenderingContext2D.prototype.arcTo = function(x1, y1, x2, y2, r) {
        this.lineTo(x1, y1);
    };

    CanvasRenderingContext2D.prototype.bezierCurveTo = function(cp1x, cp1y, cp2x, cp2y, x, y) {
        var p0 = this._curPt || [cp1x, cp1y];
        for (var i = 1; i <= 16; i++) {
            var t = i / 16, u = 1 - t;
            var bx = u*u*u*p0[0] + 3*u*u*t*cp1x + 3*u*t*t*cp2x + t*t*t*x;
            var by = u*u*u*p0[1] + 3*u*u*t*cp1y + 3*u*t*t*cp2y + t*t*t*y;
            this.lineTo(bx, by);
        }
    };

    CanvasRenderingContext2D.prototype.quadraticCurveTo = function(cpx, cpy, x, y) {
        var p0 = this._curPt || [cpx, cpy];
        for (var i = 1; i <= 12; i++) {
            var t = i / 12, u = 1 - t;
            this.lineTo(u*u*p0[0] + 2*u*t*cpx + t*t*x, u*u*p0[1] + 2*u*t*cpy + t*t*y);
        }
    };

    /* Scanline-rasterise the current path (nonzero winding, subpaths implicitly
       closed), calling emitSpan(x, y, w) per 1px-high run within the clip. */
    CanvasRenderingContext2D.prototype._rasterizePath = function(emitSpan) {
        var edges = [], minY = Infinity, maxY = -Infinity;
        for (var s = 0; s < this._path.length; s++) {
            var pts = this._path[s].pts, n = pts.length >> 1;
            if (n < 3) continue;
            for (var i = 0; i < n; i++) {
                var j = (i + 1) % n;
                var x0 = pts[2*i], y0 = pts[2*i+1], x1 = pts[2*j], y1 = pts[2*j+1];
                if (y0 === y1) continue;
                var dir = 1;
                if (y0 > y1) { var tx = x0; x0 = x1; x1 = tx; var ty = y0; y0 = y1; y1 = ty; dir = -1; }
                edges.push({ y0: y0, y1: y1, x0: x0, dxdy: (x1 - x0) / (y1 - y0), dir: dir });
                if (y0 < minY) minY = y0;
                if (y1 > maxY) maxY = y1;
            }
        }
        if (!edges.length) return;
        var cw = this.canvas._width || 0, ch = this.canvas._height || 0;
        var c = this._clip;
        var yA = Math.max(0, c ? c.y : 0, Math.floor(minY));
        var yB = Math.min(ch, c ? c.y + c.h : ch, Math.ceil(maxY));
        var xA = c ? Math.max(0, c.x) : 0, xB = c ? Math.min(cw, c.x + c.w) : cw;
        var xs = [];
        for (var y = yA; y < yB; y++) {
            var sy = y + 0.5;
            xs.length = 0;
            for (var e = 0; e < edges.length; e++) {
                var ed = edges[e];
                if (sy >= ed.y0 && sy < ed.y1) xs.push({ x: ed.x0 + (sy - ed.y0) * ed.dxdy, dir: ed.dir });
            }
            if (xs.length < 2) continue;
            xs.sort(function(a, b) { return a.x - b.x; });
            var wind = 0, spanStart = 0;
            for (var k = 0; k < xs.length; k++) {
                var prev = wind;
                wind += xs[k].dir;
                if (prev === 0 && wind !== 0) {
                    spanStart = xs[k].x;
                } else if (prev !== 0 && wind === 0) {
                    var xa = Math.max(xA, Math.round(spanStart)), xb = Math.min(xB, Math.round(xs[k].x));
                    if (xb > xa) emitSpan(xa, y, xb - xa);
                }
            }
        }
    };

    /* Native path fill: flattens the subpaths and hands them to the rasteriser in canvas2d.c. `grad` is a
       device-space CanvasGradient or null for the current fill colour. Returns false if not available. */
    CanvasRenderingContext2D.prototype._fillPathNative = function(grad, path) {
        if (typeof __native_canvas2d.fillPath !== 'function') return false;
        var pts, counts = [];
        if (path.length === 1) {
            /* The usual case (one circle or one box): hand the point list over without copying. */
            pts = path[0].pts;
            if ((pts.length >> 1) < 3) return true;
            counts.push(pts.length >> 1);
        } else {
            pts = [];
            for (var s = 0; s < path.length; s++) {
                var sp = path[s].pts, n = sp.length >> 1;
                if (n < 3) continue;
                for (var i = 0; i < sp.length; i++) pts.push(sp[i]);
                counts.push(n);
            }
            if (!counts.length) return true;
        }
        var c = this._clip;
        var flat = [], kind = -1, p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        if (grad) {
            var stops = grad._stops;
            if (stops.length === 0) return true;
            for (var si = 0; si < stops.length; si++) {
                var sc = stops[si].color;
                flat.push(stops[si].offset, sc[0], sc[1], sc[2], sc[3]);
            }
            if (grad._radial) { kind = 1; p0 = grad._cx; p1 = grad._cy; p2 = grad._r0; p3 = grad._r1; }
            else { kind = 0; p0 = grad._x0; p1 = grad._y0; p2 = grad._x1; p3 = grad._y1; }
        }
        __native_canvas2d.fillPath(this._handle, pts, counts,
            c ? c.x : 0, c ? c.y : 0, c ? c.w : 0, c ? c.h : 0, c ? 1 : 0,
            kind, p0, p1, p2, p3, flat);
        return true;
    };

    CanvasRenderingContext2D.prototype.fill = function() {
        if (!this._handle || !this._path.length) return;
        var self = this, h = this._handle;
        if (this._fillStyle instanceof CanvasGradient) {
            if (this._fillPathNative(this._gradientToDevice(this._fillStyle), this._path)) return;
        } else {
            this._syncFillColor();
            if (this._fillPathNative(null, this._path)) return;
        }
        if (this._fillStyle instanceof CanvasGradient) {
            this._rasterizePath(function(x, y, w) { self._fillGradientRect(x, y, w, 1); });
        } else {
            this._syncFillColor();
            this._rasterizePath(function(x, y, w) { __native_canvas2d.fillRect(h, x, y, w, 1); });
        }
    };

    /* Strokes are drawn as one filled quad per segment (no joins/caps). */
    CanvasRenderingContext2D.prototype.stroke = function() {
        if (!this._handle || !this._path.length) return;
        var half = Math.max(1, this._lineWidth * this._ctmScale()) / 2;
        var quads = [];
        for (var s = 0; s < this._path.length; s++) {
            var sp = this._path[s], pts = sp.pts, n = pts.length >> 1;
            var segs = (sp.closed && n > 2) ? n : n - 1;
            for (var i = 0; i < segs; i++) {
                var j = (i + 1) % n;
                var x0 = pts[2*i], y0 = pts[2*i+1], x1 = pts[2*j], y1 = pts[2*j+1];
                var dx = x1 - x0, dy = y1 - y0, len = Math.sqrt(dx*dx + dy*dy);
                if (!len) continue;
                var nx = -dy / len * half, ny = dx / len * half;
                quads.push({ pts: [x0+nx, y0+ny, x1+nx, y1+ny, x1-nx, y1-ny, x0-nx, y0-ny], closed: true });
            }
        }
        if (!quads.length) return;
        var saved = this._path, h = this._handle;
        this._path = quads;
        var c = parseColor(this._strokeStyle);
        __native_canvas2d.setFillColor(h, c[0], c[1], c[2], c[3]);
        if (!this._fillPathNative(null, quads)) {
            this._rasterizePath(function(x, y, w) { __native_canvas2d.fillRect(h, x, y, w, 1); });
        }
        this._path = saved;
        this._syncFillColor();
    };

    /* Clip is approximated by the path's bounding box intersected with any
       existing clip (RPG Maker only uses rectangular clips). */
    CanvasRenderingContext2D.prototype.clip = function() {
        var minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity, any = false;
        for (var s = 0; s < this._path.length; s++) {
            var pts = this._path[s].pts;
            for (var i = 0; i < pts.length; i += 2) {
                any = true;
                if (pts[i] < minX) minX = pts[i]; if (pts[i] > maxX) maxX = pts[i];
                if (pts[i+1] < minY) minY = pts[i+1]; if (pts[i+1] > maxY) maxY = pts[i+1];
            }
        }
        if (!any) return;
        for (var cs = 0; cs < this._path.length; cs++) {
            if (this._path[cs].pts.length > 10) {
                compat("clip() with a curved or polygonal path", "clipped to the path's bounding box only");
                break;
            }
        }
        var r = { x: Math.floor(minX), y: Math.floor(minY), w: Math.ceil(maxX) - Math.floor(minX), h: Math.ceil(maxY) - Math.floor(minY) };
        var c = this._clip;
        if (c) {
            var x0 = Math.max(c.x, r.x), y0 = Math.max(c.y, r.y);
            var x1 = Math.min(c.x + c.w, r.x + r.w), y1 = Math.min(c.y + c.h, r.y + r.h);
            r = { x: x0, y: y0, w: Math.max(0, x1 - x0), h: Math.max(0, y1 - y0) };
        }
        this._clip = r;
    };

    /* ---- Misc ---- */

    CanvasRenderingContext2D.prototype.getContextAttributes = function() {
        return { alpha: true, desynchronized: false };
    };

    CanvasRenderingContext2D.prototype.isPointInPath = function() {
        compat("isPointInPath", "always false");
        return false;
    };

    /* Properties the context stores but nothing here draws with. Reading gives back what was set; setting one to
       anything but its default is logged. */
    [["shadowBlur", 0], ["shadowColor", "rgba(0, 0, 0, 0)"], ["shadowOffsetX", 0], ["shadowOffsetY", 0],
     ["lineCap", "butt"], ["lineJoin", "miter"], ["miterLimit", 10], ["lineDashOffset", 0], ["filter", "none"],
     ["direction", "inherit"], ["imageSmoothingEnabled", true], ["imageSmoothingQuality", "low"],
     ["fontKerning", "auto"], ["letterSpacing", "0px"], ["wordSpacing", "0px"], ["textRendering", "auto"]
    ].forEach(function(p) {
        var name = p[0], dflt = p[1], slot = "_set_" + name;
        Object.defineProperty(CanvasRenderingContext2D.prototype, name, {
            configurable: true,
            get: function() { var v = this[slot]; return v === undefined ? dflt : v; },
            set: function(v) {
                this[slot] = v;
                if (v !== dflt)
                    compat("set " + name + (typeof v === "string" || typeof v === "boolean" ? "=" + String(v).slice(0, 24) : ""),
                           "stored, has no effect on drawing (value " + String(v).slice(0, 40) + ")");
            }
        });
    });

    /* Standard methods this context lacks. They throw as an absent method would, but say so first, so a game that
       catches the error still shows up in the log. */
    ["setLineDash", "getLineDash", "roundRect", "createConicGradient", "isPointInStroke", "drawFocusIfNeeded",
     "reset"].forEach(function(name) {
        if (CanvasRenderingContext2D.prototype[name]) return;
        CanvasRenderingContext2D.prototype[name] = function() {
            compat(name + "()", "method not implemented");
            throw new TypeError("CanvasRenderingContext2D." + name + " is not implemented");
        };
    });

    CanvasRenderingContext2D.prototype._destroy = function() {
        if (this._handle && typeof __native_canvas2d !== "undefined") {
            __native_canvas2d.destroy(this._handle);
        }
        this._handle = 0;
        this._token = null;
    };

    /* Replaces the stub context in dom_shim.js. */
    /* Plugins patch or test against these by name (CanvasGradient.prototype..., instanceof). */
    if (typeof globalThis.CanvasGradient === 'undefined') globalThis.CanvasGradient = CanvasGradient;
    if (typeof globalThis.CanvasRenderingContext2D === 'undefined') globalThis.CanvasRenderingContext2D = CanvasRenderingContext2D;
    globalThis.__Canvas2DContext = CanvasRenderingContext2D;

    /* Exposed for tests. */
    globalThis.__parseColor = parseColor;
    globalThis.__parseFont = parseFont;

})();
