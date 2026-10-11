/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

/*
 * tilemap_shim.js — Post-core overrides: native Tilemap.Layer rendering via
 * __native_tilemap.drawTiles(), plus Bitmap/Window/Scene resource fixes.
 * Must load after rmmz_core.js.
 */
(function() {
    "use strict";

    if (typeof Tilemap === "undefined") {
        return;
    }

    try {

    /* ---- Tilemap.Layer override ---- */

    /* Caches per-setNumber {glTexture, width, height} for render. */
    Tilemap.Layer.prototype.setBitmaps = function(bitmaps) {
        this._images = bitmaps.map(function(bitmap) {
            return bitmap.image || bitmap.canvas;
        });
        this._tilesetTextures = [];
        this._needsTexturesUpdate = true;
        this._updateTilesetTextures();
    };

    Tilemap.Layer.prototype._updateTilesetTextures = function() {
        if (!this._images) return;
        this._tilesetTextures = [];
        this._hasMissingTextures = false;
        for (var i = 0; i < this._images.length; i++) {
            var img = this._images[i];
            var info = { glTexture: 0, width: 0, height: 0 };
            if (img) {
                if (img._glTextureId) {
                    info.glTexture = img._glTextureId;
                    info.width = img.naturalWidth || img.width || 0;
                    info.height = img.naturalHeight || img.height || 0;
                } else if (img._imageHandle !== undefined && typeof __native_image !== "undefined") {
                    var ninfo = __native_image.getImageInfo(img._imageHandle);
                    if (ninfo) {
                        info.glTexture = ninfo.glTexture || 0;
                        info.width = ninfo.width || 0;
                        info.height = ninfo.height || 0;
                    }
                } else if (img.width && img.height) {
                    info.width = img.width;
                    info.height = img.height;
                }
                if (info.glTexture === 0 && (img.width > 0 || img._imageHandle !== undefined)) {
                    this._hasMissingTextures = true;
                }
            }
            this._tilesetTextures.push(info);
        }
    };

    Tilemap.Layer.prototype.isReady = function() {
        if (!this._images || this._images.length === 0) {
            return false;
        }
        for (var i = 0; i < this._images.length; i++) {
            var img = this._images[i];
            if (!img) return false;
            if (img.complete === false) return false;
            if (img.valid === false) return false;
        }
        return true;
    };

    /* Packs all tile elements into a Float32Array and hands them to the native
       sprite batch instead of PIXI's vertex buffer + custom shader path.
       FilterControllerMZ moves this to Layer._render, which Container.render
       still calls, so the override survives. */
    Tilemap.Layer.prototype.render = function(renderer) {
        if (!this.visible || this.worldAlpha <= 0) return;

        var numElements = this._elements.length;
        if (numElements === 0) return;

        if (typeof __native_tilemap === "undefined" ||
            typeof __native_renderer === "undefined" ||
            !__native_renderer._active) {
            return;
        }

        /* Images load async, so re-resolve textures while any are missing. */
        if (this._needsTexturesUpdate || this._hasMissingTextures) {
            this._updateTilesetTextures();
            this._needsTexturesUpdate = false;
        }

        /* 7 floats per element. */
        var bufSize = numElements * 7;
        if (!this._nativeBuffer || this._nativeBuffer.length < bufSize) {
            this._nativeBuffer = new Float32Array(bufSize * 2);
        }
        var buf = this._nativeBuffer;
        var idx = 0;
        for (var i = 0; i < numElements; i++) {
            var item = this._elements[i];
            buf[idx++] = item[0]; /* setNumber */
            buf[idx++] = item[1]; /* sx */
            buf[idx++] = item[2]; /* sy */
            buf[idx++] = item[3]; /* dx */
            buf[idx++] = item[4]; /* dy */
            buf[idx++] = item[5]; /* w */
            buf[idx++] = item[6]; /* h */
        }

        var ts = this._tilesetTextures || [];
        var numTs = ts.length;
        var texIds = [];
        var texWidths = [];
        var texHeights = [];
        for (var j = 0; j < numTs; j++) {
            texIds.push(ts[j].glTexture);
            texWidths.push(ts[j].width);
            texHeights.push(ts[j].height);
        }

        var wt = this.worldTransform;
        var ox = wt ? wt.tx : 0;
        var oy = wt ? wt.ty : 0;
        /* the layer may sit inside a scaled container (map zoom): tiles are scaled with it */
        var sx = wt && wt.a ? wt.a : 1;
        var sy = wt && wt.d ? wt.d : 1;

        __native_tilemap.drawTiles(
            buf.buffer, numElements,
            texIds, texWidths, texHeights, numTs,
            ox, oy, sx, sy
        );
    };

    /* ---- Tilemap.Renderer: no-op, the PIXI plugin path is bypassed ---- */

    if (Tilemap.Renderer && Tilemap.Renderer.prototype) {
        Tilemap.Renderer.prototype.initialize = function(renderer) {
            this.renderer = renderer;
            this._shader = null;
            this._images = [];
            this._internalTextures = [];
        };

        Tilemap.Renderer.prototype.destroy = function() {
            this.renderer = null;
            this._shader = null;
            this._images = [];
            this._internalTextures = [];
        };

        Tilemap.Renderer.prototype.getShader = function() {
            return this._shader;
        };

        Tilemap.Renderer.prototype.contextChange = function() {
        };

        Tilemap.Renderer.prototype.updateTextures = function() {
        };

        Tilemap.Renderer.prototype.bindTextures = function() {
        };
    }

    /* ---- WindowLayer.render ---- */

    /* The original uses raw WebGL stencil masking, which the native renderer
       lacks; render visible children in order instead. */
    if (typeof WindowLayer !== "undefined") {
        WindowLayer.prototype.render = function(renderer) {
            if (!this.visible) return;
            for (var i = 0; i < this.children.length; i++) {
                var child = this.children[i];
                if (child.visible) {
                    child.render(renderer);
                }
            }
        };
    }

    /* ---- Native resource release for Bitmap ---- */
    /* Bitmap.destroy/_destroyCanvas never free native handles (browsers GC
       them), so Canvas2D context slots (max 256) and the image cache would
       otherwise grow until exhausted. */

    if (typeof Bitmap !== "undefined") {
        /* Runs after the original so BaseTexture.destroy has already released
           the loader-owned GL texture; the loader deletes it here. */
        var _origBitmapDestroy = Bitmap.prototype.destroy;
        Bitmap.prototype.destroy = function() {
            _origBitmapDestroy.call(this);
            if (this._image && typeof this._image._releaseNativeImage === "function") {
                this._image._releaseNativeImage();
            }
        };

        var _origDestroyCanvas = Bitmap.prototype._destroyCanvas;
        Bitmap.prototype._destroyCanvas = function() {
            if (this._canvas && this._canvas._context2d &&
                this._canvas._context2d._handle &&
                typeof __native_canvas2d !== "undefined") {
                __native_canvas2d.destroy(this._canvas._context2d._handle);
                this._canvas._context2d._handle = 0;
            }
            /* Every Bitmap creates an unattached canvas; drop it from the element
               registry so it can be GC'd. */
            if (this._canvas && typeof __dom_removeFromElementRegistry === "function") {
                __dom_removeFromElementRegistry(this._canvas);
            }
            _origDestroyCanvas.call(this);
        };
    }

    /* Sprite.bitmap replacement abandons the old Bitmap without destroying it
       (browsers rely on GC). Free its Canvas2D context and GL texture once no
       sprite references it any more, and only after a grace period: plugins
       draw into a bitmap through a scratch window, detach it there and keep
       showing it from another sprite (DTextPicture), or park bitmaps to
       re-attach later. Never the whole baseTexture, which a sprite may still
       reference when the new bitmap is 0x0. */
    if (typeof Sprite !== "undefined") {
        var _origBitmapDesc = Object.getOwnPropertyDescriptor(Sprite.prototype, "bitmap");
        var RELEASE_GRACE_FRAMES = 120;
        var _pendingReleases = [];

        var _releaseBitmapNative = function(bitmap) {
            if (bitmap._canvas && bitmap._canvas._context2d && bitmap._canvas._context2d._handle &&
                typeof __native_canvas2d !== "undefined") {
                __native_canvas2d.destroy(bitmap._canvas._context2d._handle);
                bitmap._canvas._context2d._handle = 0;
            }
            if (bitmap._baseTexture && bitmap._baseTexture._glTexture &&
                !bitmap._baseTexture._glTextureIsImageOwned &&
                typeof __native_renderer !== "undefined") {
                if (typeof __dropGlTexture === "function") __dropGlTexture(bitmap._baseTexture);
                else __native_renderer.deleteTexture(bitmap._baseTexture._glTexture);
                bitmap._baseTexture._glTexture = 0;
            }
        };

        var _flushPendingReleases = function(now) {
            var keep = [];
            for (var i = 0; i < _pendingReleases.length; i++) {
                var entry = _pendingReleases[i];
                if ((entry.bitmap._spriteRefs | 0) > 0) {            /* re-attached */
                    entry.bitmap._releasePending = false;
                    continue;
                }
                if (now - entry.frame < RELEASE_GRACE_FRAMES) {
                    keep.push(entry);
                } else {
                    entry.bitmap._releasePending = false;
                    _releaseBitmapNative(entry.bitmap);
                }
            }
            _pendingReleases = keep;
        };

        if (_origBitmapDesc && _origBitmapDesc.set) {
            Object.defineProperty(Sprite.prototype, "bitmap", {
                get: _origBitmapDesc.get,
                set: function(value) {
                    var old = this._bitmap;
                    _origBitmapDesc.set.call(this, value);
                    if (old === value) return;
                    if (value) value._spriteRefs = (value._spriteRefs | 0) + 1;
                    /* Only anonymous canvas-backed bitmaps (new Bitmap(w, h));
                       loaded images belong to the ImageManager cache. */
                    if (old && old._canvas && !old._url) {
                        old._spriteRefs = Math.max(0, (old._spriteRefs | 0) - 1);
                        if (old._spriteRefs === 0 && !old._releasePending) {
                            old._releasePending = true;
                            _pendingReleases.push({ bitmap: old,
                                frame: typeof Graphics !== "undefined" ? Graphics.frameCount : 0 });
                        }
                    }
                    if (_pendingReleases.length) {
                        _flushPendingReleases(typeof Graphics !== "undefined" ? Graphics.frameCount : 0);
                    }
                },
                configurable: true
            });
        }
    }

    /* ---- Transfer fade-in while the screen is faded out ---- */

    /* If $gameScreen brightness is 0 (a prior Fadeout Screen command), the
       event will fade in explicitly, so hold the scene ColorFilter at black
       instead of animating, and release it when startFadeIn is called. */
    if (typeof Scene_Map !== "undefined" && Scene_Map.prototype.fadeInForTransfer) {
        var _origFadeInForTransfer = Scene_Map.prototype.fadeInForTransfer;
        Scene_Map.prototype.fadeInForTransfer = function() {
            if ($gameScreen && $gameScreen.brightness() <= 0) {
                this._fadeOpacity = 255;
                this._fadeDuration = 0;
                this._fadeSign = 0;
                if (this.updateColorFilter) this.updateColorFilter();
                return;
            }
            _origFadeInForTransfer.call(this);
        };
    }

    if (typeof Game_Screen !== "undefined" && Game_Screen.prototype.startFadeIn) {
        var _origScreenFadeIn = Game_Screen.prototype.startFadeIn;
        Game_Screen.prototype.startFadeIn = function(duration) {
            _origScreenFadeIn.call(this, duration);
            var scene = SceneManager._scene;
            if (scene && scene._fadeOpacity > 0 && scene._fadeDuration === 0) {
                scene._fadeSign = 1;
                scene._fadeDuration = duration;
            }
        };
    }

    /* ---- Bitmap._createBaseTexture ---- */

    /* A BaseTexture created from the canvas loses the loaded image's native
       GL texture; keep a reference so _ensureGLTexture can use it directly. */
    if (typeof Bitmap !== "undefined" && Bitmap.prototype._createBaseTexture) {
        var _orig_createBaseTexture = Bitmap.prototype._createBaseTexture;
        Bitmap.prototype._createBaseTexture = function(source) {
            _orig_createBaseTexture.call(this, source);
            if (this._image && this._image._glTextureId && this._baseTexture) {
                this._baseTexture._sourceImage = this._image;
            }
        };
    }

    /* ---- Bitmap._ensureCanvas: size a fresh canvas from the loaded image ---- */

    if (typeof Bitmap !== "undefined" && Bitmap.prototype._ensureCanvas) {
        var _orig_ensureCanvas = Bitmap.prototype._ensureCanvas;
        Bitmap.prototype._ensureCanvas = function() {
            var hadCanvas = !!this._canvas;
            _orig_ensureCanvas.call(this);
            if (!hadCanvas && this._canvas && this._image && this._image._imageHandle) {
                var w = this._canvas.width || this._canvas._width || 0;
                var h = this._canvas.height || this._canvas._height || 0;
                if (w <= 0 || h <= 0) {
                    var iw = this._image._naturalWidth || this._image._width || 0;
                    var ih = this._image._naturalHeight || this._image._height || 0;
                    if (iw > 0 && ih > 0) {
                        this._canvas.width = iw;
                        this._canvas.height = ih;
                        if (this._context) {
                            this._context.drawImage(this._image, 0, 0);
                        }
                    }
                }
            }
        };
    }

    /* ---- Game_Interpreter: log and skip failing event commands ---- */

    if (typeof Game_Interpreter !== "undefined") {
        if (Game_Interpreter.prototype.command355) {
            var _origCmd355 = Game_Interpreter.prototype.command355;
            Game_Interpreter.prototype.command355 = function(params) {
                try {
                    return _origCmd355.call(this, params);
                } catch(e) {
                    console.log("[EVT_SCRIPT] Error: " + e.message);
                    return true;
                }
            };
        }

        var _origExecCmd = Game_Interpreter.prototype.executeCommand;
        Game_Interpreter.prototype.executeCommand = function() {
            try {
                return _origExecCmd.call(this);
            } catch(e) {
                console.log("[EVT_CMD] EXCEPTION at idx=" + this._index +
                    " code=" + (this.currentCommand() ? this.currentCommand().code : "?") +
                    ": " + e.message);
                this._index++;
                return true;
            }
        };
    }

    /* ---- SceneManager.catchException: log before the game stops ---- */

    if (typeof SceneManager !== "undefined" && SceneManager.catchException) {
        var _origCatchException = SceneManager.catchException;
        SceneManager.catchException = function(e) {
            if (e instanceof Error) {
                console.log("[SCENE_ERR] " + e.name + ": " + e.message);
                console.log("[SCENE_ERR] " + (e.stack || ""));
            } else if (e instanceof Array && e[0] === "LoadError") {
                console.log("[SCENE_ERR] LoadError: " + e[1]);
            } else {
                console.log("[SCENE_ERR] Unknown: " + String(e));
            }
            _origCatchException.call(this, e);
        };
    }

    console.log("[tilemap_shim] overrides applied successfully");

    } catch(e) {
        console.log("[tilemap_shim] ERROR: " + e.message + "\n" + (e.stack || ""));
    }

})();
