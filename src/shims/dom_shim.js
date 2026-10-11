/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

/*
 * DOM shim: window, document, HTMLElement/canvas/image elements, events,
 * timers and requestAnimationFrame on top of the native platform layer.
 */

(function() {
    "use strict";

    /* EventTarget base */

    function EventTarget() {
        this._listeners = {};
    }

    EventTarget.prototype.addEventListener = function(type, listener, options) {
        if (typeof listener !== "function") return;
        if (!this._listeners[type]) {
            this._listeners[type] = [];
        }
        var list = this._listeners[type];
        for (var i = 0; i < list.length; i++) {
            if (list[i] === listener) return;
        }
        list.push(listener);
    };

    EventTarget.prototype.removeEventListener = function(type, listener, options) {
        if (!this._listeners[type]) return;
        var list = this._listeners[type];
        for (var i = 0; i < list.length; i++) {
            if (list[i] === listener) {
                list.splice(i, 1);
                return;
            }
        }
    };

    EventTarget.prototype.dispatchEvent = function(event) {
        if (!event.type) return false;
        event.target = this;
        event.currentTarget = this;

        var list = this._listeners[event.type];
        if (list) {
            var copy = list.slice();
            for (var i = 0; i < copy.length; i++) {
                copy[i].call(this, event);
            }
        }

        var handler = this["on" + event.type];
        if (typeof handler === "function") {
            handler.call(this, event);
        }

        return !event.defaultPrevented;
    };

    /* Event */

    function Event(type, init) {
        this.type = type;
        this.bubbles = (init && init.bubbles) || false;
        this.cancelable = (init && init.cancelable) || false;
        this.defaultPrevented = false;
        this.target = null;
        this.currentTarget = null;
        this.timeStamp = Date.now();
    }

    Event.prototype.preventDefault = function() {
        if (this.cancelable) {
            this.defaultPrevented = true;
        }
    };

    Event.prototype.stopPropagation = function() {};
    Event.prototype.stopImmediatePropagation = function() {};

    /* KeyboardEvent */

    function KeyboardEvent(type, init) {
        Event.call(this, type, init);
        this.key = (init && init.key) || "";
        this.code = (init && init.code) || "";
        this.keyCode = (init && init.keyCode) || 0;
        this.which = this.keyCode;
        this.charCode = (type === "keypress") ? this.keyCode : 0;
        this.repeat = (init && init.repeat) || false;
        this.shiftKey = (init && init.shiftKey) || false;
        this.ctrlKey = (init && init.ctrlKey) || false;
        this.altKey = (init && init.altKey) || false;
        this.metaKey = (init && init.metaKey) || false;
        this.location = 0;
        this.isComposing = false;
    }

    KeyboardEvent.prototype = Object.create(Event.prototype);
    KeyboardEvent.prototype.constructor = KeyboardEvent;
    KeyboardEvent.prototype.getModifierState = function(key) {
        if (key === "Shift") return this.shiftKey;
        if (key === "Control") return this.ctrlKey;
        if (key === "Alt") return this.altKey;
        if (key === "Meta") return this.metaKey;
        return false;
    };

    /* MouseEvent */

    function MouseEvent(type, init) {
        Event.call(this, type, init);
        this.clientX = (init && init.clientX) || 0;
        this.clientY = (init && init.clientY) || 0;
        this.pageX = (init && init.pageX) || this.clientX;
        this.pageY = (init && init.pageY) || this.clientY;
        this.screenX = (init && init.screenX) || this.clientX;
        this.screenY = (init && init.screenY) || this.clientY;
        this.offsetX = this.clientX;
        this.offsetY = this.clientY;
        this.movementX = (init && init.movementX) || 0;
        this.movementY = (init && init.movementY) || 0;
        this.button = (init && init.button !== undefined) ? init.button : 0;
        this.buttons = (init && init.buttons) || 0;
        this.shiftKey = (init && init.shiftKey) || false;
        this.ctrlKey = (init && init.ctrlKey) || false;
        this.altKey = (init && init.altKey) || false;
        this.metaKey = (init && init.metaKey) || false;
        this.relatedTarget = null;
    }

    MouseEvent.prototype = Object.create(Event.prototype);
    MouseEvent.prototype.constructor = MouseEvent;
    MouseEvent.prototype.getModifierState = function(key) {
        if (key === "Shift") return this.shiftKey;
        if (key === "Control") return this.ctrlKey;
        if (key === "Alt") return this.altKey;
        if (key === "Meta") return this.metaKey;
        return false;
    };

    /* WheelEvent */

    function WheelEvent(type, init) {
        MouseEvent.call(this, type, init);
        this.deltaX = (init && init.deltaX) || 0;
        this.deltaY = (init && init.deltaY) || 0;
        this.deltaZ = (init && init.deltaZ) || 0;
        this.deltaMode = (init && init.deltaMode) || 0;
    }

    WheelEvent.prototype = Object.create(MouseEvent.prototype);
    WheelEvent.prototype.constructor = WheelEvent;

    /* Timers: setTimeout / setInterval, flushed once per frame by the main loop */

    var _timerIdCounter = 1;
    var _timers = {};  /* id -> { callback, delay, interval, nextTime, cleared } */
    var _timerStartTime = Date.now();

    function _now() {
        return Date.now() - _timerStartTime;
    }

    function _setTimeout(callback, delay) {
        if (typeof callback !== "function") return 0;
        var id = _timerIdCounter++;
        var d = Math.max(0, delay || 0);
        _timers[id] = {
            callback: callback,
            delay: d,
            interval: false,
            nextTime: _now() + d,
            cleared: false
        };
        return id;
    }

    function _setInterval(callback, delay) {
        if (typeof callback !== "function") return 0;
        var id = _timerIdCounter++;
        var d = Math.max(0, delay || 0);
        _timers[id] = {
            callback: callback,
            delay: d,
            interval: true,
            nextTime: _now() + d,
            cleared: false
        };
        return id;
    }

    function _clearTimeout(id) {
        if (_timers[id]) {
            _timers[id].cleared = true;
            delete _timers[id];
        }
    }

    function _clearInterval(id) {
        _clearTimeout(id);
    }

    function _flushTimers() {
        var now = _now();
        var ids = Object.keys(_timers);
        for (var i = 0; i < ids.length; i++) {
            var id = ids[i];
            var t = _timers[id];
            if (!t || t.cleared) continue;
            if (now >= t.nextTime) {
                if (t.interval) {
                    t.nextTime = now + t.delay;
                    t.callback();
                } else {
                    delete _timers[id];
                    t.callback();
                }
            }
        }
    }

    /* requestAnimationFrame / cancelAnimationFrame */

    var _rafIdCounter = 1;
    var _rafCallbacks = {};

    function _requestAnimationFrame(callback) {
        if (typeof callback !== "function") return 0;
        var id = _rafIdCounter++;
        _rafCallbacks[id] = callback;
        return id;
    }

    function _cancelAnimationFrame(id) {
        delete _rafCallbacks[id];
    }

    function _flushAnimationFrames(timestamp) {
        if (typeof globalThis.__dom_updateVideos === "function") globalThis.__dom_updateVideos();
        var cbs = _rafCallbacks;
        _rafCallbacks = {};
        var ids = Object.keys(cbs);
        var ts = timestamp || _now();
        for (var i = 0; i < ids.length; i++) {
            var cb = cbs[ids[i]];
            if (typeof cb !== "function") continue;
            /* As in browsers, one throwing callback must not drop the rest of the frame. */
            try {
                cb(ts);
            } catch (e) {
                if (typeof globalThis.__error_handler_dispatch === "function") {
                    try { globalThis.__error_handler_dispatch(String(e && e.message || e), e); } catch (e2) { /* ignore */ }
                } else {
                    console.error("[rAF] callback threw: " + (e && e.stack || e));
                }
            }
        }
    }

    /* CSSStyleDeclaration (minimal) */

    var CSS_PROPERTIES = ("cssText cursor display width height minWidth minHeight maxWidth maxHeight " +
        "position top left right bottom overflow overflowX overflowY margin marginTop marginRight " +
        "marginBottom marginLeft padding paddingTop paddingRight paddingBottom paddingLeft zIndex " +
        "opacity visibility background backgroundColor backgroundImage backgroundSize " +
        "backgroundPosition backgroundRepeat imageRendering color font fontFamily fontSize " +
        "fontWeight fontStyle lineHeight letterSpacing textAlign textDecoration textShadow " +
        "whiteSpace wordBreak border borderTop borderRight borderBottom borderLeft borderWidth " +
        "borderColor borderStyle borderRadius outline boxShadow boxSizing transform " +
        "transformOrigin transformStyle perspective perspectiveOrigin backfaceVisibility " +
        "transition transitionProperty transitionDuration animation filter backdropFilter " +
        "clipPath mixBlendMode willChange pointerEvents userSelect flex flexDirection flexWrap " +
        "justifyContent alignItems alignSelf gap objectFit objectPosition verticalAlign " +
        "float clear content resize webkitTransform webkitTransformOrigin webkitFilter " +
        "webkitUserSelect webkitBackfaceVisibility webkitPerspective webkitTransition").split(" ");

    function CSSStyleDeclaration() {
        for (var i = 0; i < CSS_PROPERTIES.length; i++) this[CSS_PROPERTIES[i]] = "";
    }

    CSSStyleDeclaration.prototype.setProperty = function(name, value) {
        var camel = name.replace(/-([a-z])/g, function(m, c) { return c.toUpperCase(); });
        this[camel] = value;
    };

    CSSStyleDeclaration.prototype.getPropertyValue = function(name) {
        var camel = name.replace(/-([a-z])/g, function(m, c) { return c.toUpperCase(); });
        return this[camel] || "";
    };

    /* HTMLElement */

    function HTMLElement(tagName) {
        EventTarget.call(this);
        this.tagName = (tagName || "DIV").toUpperCase();
        this.nodeName = this.tagName;
        this.nodeType = 1;
        this.id = "";
        this.className = "";
        this.classList = {
            _list: [],
            add: function(c) { if (this._list.indexOf(c) === -1) this._list.push(c); },
            remove: function(c) { var i = this._list.indexOf(c); if (i >= 0) this._list.splice(i, 1); },
            contains: function(c) { return this._list.indexOf(c) >= 0; },
            toggle: function(c) { if (this.contains(c)) { this.remove(c); } else { this.add(c); } }
        };
        this.style = new CSSStyleDeclaration();
        this.children = [];
        this.childNodes = [];
        this.parentNode = null;
        this.parentElement = null;
        this.dataset = {};
        this.attributes = {};
        this._innerHTML = "";
        this.offsetLeft = 0;
        this.offsetTop = 0;
        this.offsetWidth = 0;
        this.offsetHeight = 0;
        this.scrollLeft = 0;
        this.scrollTop = 0;
        this.scrollWidth = 0;
        this.scrollHeight = 0;
        this.clientLeft = 0;
        this.clientTop = 0;
        this.clientWidth = 0;
        this.clientHeight = 0;
    }

    HTMLElement.prototype = Object.create(EventTarget.prototype);
    HTMLElement.prototype.constructor = HTMLElement;

    /* Suppresses registry removal while appendChild/insertBefore reparent via removeChild. */
    var _reparenting = false;

    /* True when the element hangs off the document (body, head or html). */
    function _isConnected(el) {
        for (var p = el; p; p = p.parentNode) {
            if (p === _body || p === _head || p === _documentElement) return true;
        }
        return false;
    }

    /* Scripts the runtime already evaluated (the loader announces them) and
       browser libraries it replaces natively: a <script src> for one of
       these fires load without running the file again. */
    var _executedScripts = {
        "js/libs/pixi.js": true, "js/libs/localforage.min.js": true,
        "js/libs/effekseer.min.js": true, "js/libs/vorbisdecoder.js": true
    };
    function _scriptKey(src) {
        var key = decodeURIComponent(String(src)).replace(/[?#].*$/, "").replace(/\\/g, "/");
        return key.replace(/^\.\//, "");
    }
    globalThis.__dom_noteScriptLoaded = function(src) {
        _executedScripts[_scriptKey(src)] = true;
    };

    /* A <script> inserted into the document runs, as in a browser: inline
       text right away, a src file read from the game directory. Load/error
       events fire on a later tick. Plugins use this to define globals
       (FOSSIL) or pull in helper scripts. */
    function _runInsertedScript(script) {
        if (!script || script.tagName !== "SCRIPT" || script._executed) return;
        if (!_isConnected(script)) return;
        script._executed = true;
        var src = script.src;
        var source = null;
        if (src) {
            var key = _scriptKey(src);
            if (_executedScripts[key]) {
                setTimeout(function() { script.dispatchEvent(new Event("load")); }, 0);
                return;
            }
            try {
                if (typeof __native_io !== "undefined" && __native_io.existsSync(key)) {
                    source = __native_io.readFileSync(key);
                }
            } catch (e) {
                source = null;
            }
            if (source === null) {
                setTimeout(function() { script.dispatchEvent(new Event("error")); }, 0);
                return;
            }
            _executedScripts[key] = true;
        } else {
            source = script._innerHTML || "";
        }
        if (source && source.charCodeAt(0) === 0xFEFF) source = source.slice(1);
        var previous = document.currentScript;
        document.currentScript = script;
        try {
            (0, eval)(source);
        } catch (e) {
            _compatThrown("exception", "script " + (src || "(inline)") + " failed: " + (e && e.message || e), e);
            if (typeof console !== "undefined") {
                console.error("script " + (src || "(inline)") + ": " + (e && e.stack || e));
            }
        }
        document.currentScript = previous;
        if (src) {
            setTimeout(function() { script.dispatchEvent(new Event("load")); }, 0);
        }
    }

    HTMLElement.prototype.appendChild = function(child) {
        if (child.parentNode) {
            _reparenting = true;
            child.parentNode.removeChild(child);
            _reparenting = false;
        }
        this.children.push(child);
        this.childNodes.push(child);
        child.parentNode = this;
        child.parentElement = this;
        /* Re-register an element that was removed earlier. */
        if (_allCreatedElements && _allCreatedElements.indexOf(child) < 0) {
            _allCreatedElements.push(child); _elemGen++;
        }
        _runInsertedScript(child);
        return child;
    };

    /* insertAdjacentElement(position, element): beforebegin / afterbegin /
       beforeend / afterend relative to this element. */
    HTMLElement.prototype.insertAdjacentElement = function(position, element) {
        var pos = String(position || "").toLowerCase();
        var parent = this.parentNode;
        if (pos === "beforebegin" && parent) {
            parent.insertBefore(element, this);
        } else if (pos === "afterbegin") {
            this.insertBefore(element, this.children[0] || null);
        } else if (pos === "afterend" && parent) {
            var idx = parent.children.indexOf(this);
            parent.insertBefore(element, parent.children[idx + 1] || null);
        } else {
            this.appendChild(element);
        }
        return element;
    };

    HTMLElement.prototype.insertAdjacentHTML = function(position, html) {
        /* Markup is not parsed; a text-only fragment keeps the text. */
        var el = new HTMLElement("SPAN");
        el._innerHTML = String(html || "");
        this.insertAdjacentElement(position, el);
    };

    HTMLElement.prototype.removeChild = function(child) {
        if (!child) return child;
        var idx = this.children.indexOf(child);
        if (idx >= 0) {
            this.children.splice(idx, 1);
        }
        idx = this.childNodes.indexOf(child);
        if (idx >= 0) {
            this.childNodes.splice(idx, 1);
        }
        child.parentNode = null;
        child.parentElement = null;
        if (!_reparenting) {
            _removeFromElementRegistry(child);
        }
        return child;
    };

    HTMLElement.prototype.insertBefore = function(newChild, refChild) {
        if (newChild.parentNode) {
            _reparenting = true;
            newChild.parentNode.removeChild(newChild);
            _reparenting = false;
        }
        if (refChild) {
            var idx = this.children.indexOf(refChild);
            if (idx >= 0) {
                this.children.splice(idx, 0, newChild);
                this.childNodes.splice(idx, 0, newChild);
            } else {
                this.children.push(newChild);
                this.childNodes.push(newChild);
            }
        } else {
            this.children.push(newChild);
            this.childNodes.push(newChild);
        }
        newChild.parentNode = this;
        newChild.parentElement = this;
        if (_allCreatedElements && _allCreatedElements.indexOf(newChild) < 0) {
            _allCreatedElements.push(newChild); _elemGen++;
        }
        _runInsertedScript(newChild);
        return newChild;
    };

    HTMLElement.prototype.getAttribute = function(name) {
        if (name === "id") return this.id;
        if (name === "class") return this.className;
        return this.attributes[name] || null;
    };

    HTMLElement.prototype.setAttribute = function(name, value) {
        if (name === "id") { this.id = value; }
        else if (name === "class") { this.className = value; }
        this.attributes[name] = String(value);
    };

    HTMLElement.prototype.removeAttribute = function(name) {
        delete this.attributes[name];
    };

    HTMLElement.prototype.hasAttribute = function(name) {
        return name in this.attributes || (name === "id" && this.id) || (name === "class" && this.className);
    };

    HTMLElement.prototype.getBoundingClientRect = function() {
        var w = this.offsetWidth || this._width || 0;
        var h = this.offsetHeight || this._height || 0;
        return { x: 0, y: 0, width: w, height: h, top: 0, left: 0, right: w, bottom: h };
    };

    HTMLElement.prototype.querySelector = function(selector) {
        return null;
    };

    HTMLElement.prototype.querySelectorAll = function(selector) {
        return [];
    };

    /* HTMLCollection stand-in: an array with item() and namedItem() (MV does head.item(0).appendChild(style)) */
    function _asCollection(arr) {
        Object.defineProperty(arr, "item", { value: function(i) { return this[i] || null; }, enumerable: false });
        Object.defineProperty(arr, "namedItem", {
            value: function(n) {
                for (var i = 0; i < this.length; i++) if (this[i].id === n || this[i].name === n) return this[i];
                return null;
            }, enumerable: false
        });
        return arr;
    }

    HTMLElement.prototype.getElementsByTagName = function(tag) {
        var upper = String(tag).toUpperCase(), out = [];
        (function walk(el) {
            var kids = el.children || [];
            for (var i = 0; i < kids.length; i++) {
                if (upper === "*" || kids[i].tagName === upper) out.push(kids[i]);
                walk(kids[i]);
            }
        })(this);
        return _asCollection(out);
    };

    HTMLElement.prototype.focus = function() {};
    HTMLElement.prototype.blur = function() {};
    HTMLElement.prototype.click = function() {};
    HTMLElement.prototype.remove = function() {
        if (this.parentNode) {
            this.parentNode.removeChild(this);
        }
    };

    HTMLElement.prototype.requestFullscreen = function() {
        if (typeof __native_platform !== "undefined") {
            __native_platform.setFullscreen(true);
        }
        document.fullscreenElement = this;
        document.fullScreenElement = this;
        document.webkitFullscreenElement = this;
        document.mozFullScreen = true;
        var evt = new Event("fullscreenchange");
        evt.target = document;
        document.dispatchEvent(evt);
        return Promise.resolve();
    };

    HTMLElement.prototype.requestFullScreen = HTMLElement.prototype.requestFullscreen;
    HTMLElement.prototype.mozRequestFullScreen = HTMLElement.prototype.requestFullscreen;
    HTMLElement.prototype.webkitRequestFullscreen = HTMLElement.prototype.requestFullscreen;
    HTMLElement.prototype.webkitRequestFullScreen = HTMLElement.prototype.requestFullscreen;

    Object.defineProperty(HTMLElement.prototype, "innerHTML", {
        get: function() { return this._innerHTML; },
        set: function(v) { this._innerHTML = v; this.children = []; this.childNodes = []; }
    });

    /* textContent is the text of the markup set with innerHTML: <br> becomes a line break, other tags disappear
       (the engines' error messages are set as HTML and must show up as readable text) */
    Object.defineProperty(HTMLElement.prototype, "textContent", {
        get: function() {
            var h = this._innerHTML;
            if (typeof h !== "string" || h.indexOf("<") < 0) return h;
            return h.replace(/<br\s*\/?>/gi, "\n").replace(/<[^>]*>/g, "").replace(/&nbsp;/g, " ").replace(/&lt;/g, "<")
                    .replace(/&gt;/g, ">").replace(/&quot;/g, '"').replace(/&#39;/g, "'").replace(/&amp;/g, "&");
        },
        set: function(v) { this._innerHTML = v; }
    });

    Object.defineProperty(HTMLElement.prototype, "firstChild", {
        get: function() { return this.childNodes[0] || null; }
    });

    Object.defineProperty(HTMLElement.prototype, "lastChild", {
        get: function() { return this.childNodes[this.childNodes.length - 1] || null; }
    });

    /* HTMLCanvasElement (2D context stub used when canvas2d_shim.js is absent) */

    function CanvasRenderingContext2DStub(canvas) {
        this.canvas = canvas;
        this.fillStyle = "#000000";
        this.strokeStyle = "#000000";
        this.globalAlpha = 1.0;
        this.globalCompositeOperation = "source-over";
        this.font = "10px sans-serif";
        this.textAlign = "start";
        this.textBaseline = "alphabetic";
        this.lineWidth = 1;
        this.lineCap = "butt";
        this.lineJoin = "miter";
        this.imageSmoothingEnabled = true;
        this._stateStack = [];
    }

    CanvasRenderingContext2DStub.prototype.save = function() {
        this._stateStack.push({
            fillStyle: this.fillStyle,
            strokeStyle: this.strokeStyle,
            globalAlpha: this.globalAlpha,
            globalCompositeOperation: this.globalCompositeOperation,
            font: this.font,
            textAlign: this.textAlign,
            textBaseline: this.textBaseline,
            lineWidth: this.lineWidth,
            imageSmoothingEnabled: this.imageSmoothingEnabled
        });
    };

    CanvasRenderingContext2DStub.prototype.restore = function() {
        var state = this._stateStack.pop();
        if (state) {
            for (var key in state) {
                this[key] = state[key];
            }
        }
    };

    CanvasRenderingContext2DStub.prototype.fillRect = function(x, y, w, h) {};
    CanvasRenderingContext2DStub.prototype.strokeRect = function(x, y, w, h) {};
    CanvasRenderingContext2DStub.prototype.clearRect = function(x, y, w, h) {};
    CanvasRenderingContext2DStub.prototype.fillText = function(text, x, y, maxWidth) {};
    CanvasRenderingContext2DStub.prototype.strokeText = function(text, x, y, maxWidth) {};
    CanvasRenderingContext2DStub.prototype.measureText = function(text) {
        /* Rough width estimate from the font size. */
        var fontSize = 10;
        var m = this.font.match(/(\d+)px/);
        if (m) fontSize = parseInt(m[1]);
        return { width: (text || "").length * fontSize * 0.6 };
    };
    CanvasRenderingContext2DStub.prototype.drawImage = function() {};
    CanvasRenderingContext2DStub.prototype.createLinearGradient = function(x0, y0, x1, y1) {
        return { addColorStop: function() {} };
    };
    CanvasRenderingContext2DStub.prototype.createRadialGradient = function(x0, y0, r0, x1, y1, r1) {
        return { addColorStop: function() {} };
    };
    CanvasRenderingContext2DStub.prototype.createPattern = function(image, rep) {
        return {};
    };
    CanvasRenderingContext2DStub.prototype.beginPath = function() {};
    CanvasRenderingContext2DStub.prototype.closePath = function() {};
    CanvasRenderingContext2DStub.prototype.moveTo = function(x, y) {};
    CanvasRenderingContext2DStub.prototype.lineTo = function(x, y) {};
    CanvasRenderingContext2DStub.prototype.arc = function(x, y, r, sa, ea, cc) {};
    CanvasRenderingContext2DStub.prototype.arcTo = function(x1, y1, x2, y2, r) {};
    CanvasRenderingContext2DStub.prototype.bezierCurveTo = function(cp1x, cp1y, cp2x, cp2y, x, y) {};
    CanvasRenderingContext2DStub.prototype.quadraticCurveTo = function(cpx, cpy, x, y) {};
    CanvasRenderingContext2DStub.prototype.rect = function(x, y, w, h) {};
    CanvasRenderingContext2DStub.prototype.fill = function() {};
    CanvasRenderingContext2DStub.prototype.stroke = function() {};
    CanvasRenderingContext2DStub.prototype.clip = function() {};
    CanvasRenderingContext2DStub.prototype.setTransform = function(a, b, c, d, e, f) {};
    CanvasRenderingContext2DStub.prototype.transform = function(a, b, c, d, e, f) {};
    CanvasRenderingContext2DStub.prototype.resetTransform = function() {};
    CanvasRenderingContext2DStub.prototype.scale = function(x, y) {};
    CanvasRenderingContext2DStub.prototype.rotate = function(angle) {};
    CanvasRenderingContext2DStub.prototype.translate = function(x, y) {};
    CanvasRenderingContext2DStub.prototype.getImageData = function(x, y, w, h) {
        var len = w * h * 4;
        return { width: w, height: h, data: new Uint8ClampedArray(len) };
    };
    CanvasRenderingContext2DStub.prototype.putImageData = function(imageData, x, y) {};
    CanvasRenderingContext2DStub.prototype.createImageData = function(w, h) {
        if (typeof w === "object") { h = w.height; w = w.width; }
        var len = w * h * 4;
        return { width: w, height: h, data: new Uint8ClampedArray(len) };
    };
    CanvasRenderingContext2DStub.prototype.getContextAttributes = function() {
        return { alpha: true, desynchronized: false };
    };
    CanvasRenderingContext2DStub.prototype.isPointInPath = function() { return false; };

    function HTMLCanvasElement(width, height) {
        HTMLElement.call(this, "CANVAS");
        this._width = width || 300;
        this._height = height || 150;
        this._context2d = null;
        this._contextWebGL = null;
    }

    HTMLCanvasElement.prototype = Object.create(HTMLElement.prototype);
    HTMLCanvasElement.prototype.constructor = HTMLCanvasElement;

    Object.defineProperty(HTMLCanvasElement.prototype, "width", {
        get: function() { return this._width; },
        set: function(v) {
            this._width = v;
            if (this._context2d) {
                this._context2d.canvas = this;
                if (this._context2d._handle && typeof __native_canvas2d !== "undefined" &&
                    this._width > 0 && this._height > 0) {
                    __native_canvas2d.resize(this._context2d._handle, this._width, this._height);
                }
            }
        }
    });

    Object.defineProperty(HTMLCanvasElement.prototype, "height", {
        get: function() { return this._height; },
        set: function(v) {
            this._height = v;
            if (this._context2d) {
                this._context2d.canvas = this;
                if (this._context2d._handle && typeof __native_canvas2d !== "undefined" &&
                    this._width > 0 && this._height > 0) {
                    __native_canvas2d.resize(this._context2d._handle, this._width, this._height);
                }
            }
        }
    });

    HTMLCanvasElement.prototype.getContext = function(type, attrs) {
        if (type === "2d") {
            if (!this._context2d) {
                if (typeof globalThis.__Canvas2DContext === "function") {
                    this._context2d = new globalThis.__Canvas2DContext(this);
                } else {
                    this._context2d = new CanvasRenderingContext2DStub(this);
                }
            }
            return this._context2d;
        }
        if (type === "webgl" || type === "webgl2" || type === "experimental-webgl") {
            /* WebGL context stub; rendering goes through the native renderer. Anything a game does with the
               context beyond these members is a use of WebGL this runtime cannot honour, so it is logged. */
            if (!this._contextWebGL) {
                var report = function(key, detail) {
                    if (typeof globalThis.__compat_report === "function") globalThis.__compat_report("webgl", key, detail);
                };
                report("getContext(" + type + ")", "a game script asked for a WebGL context; only a stub is available");
                var stub = {
                    canvas: this,
                    drawingBufferWidth: this._width,
                    drawingBufferHeight: this._height,
                    getParameter: function(p) { report("getParameter(" + p + ")", "returned null"); return null; },
                    getExtension: function(n) { report("getExtension(" + n + ")", "returned null"); return null; },
                    isContextLost: function() { return false; }
                };
                this._contextWebGL = typeof Proxy === "function" ? new Proxy(stub, {
                    get: function(target, prop, receiver) {
                        if (typeof prop === "string" && !(prop in target) && prop !== "then" && prop !== "toJSON")
                            report("gl." + prop, "WebGL member read on the stub context, not implemented");
                        return Reflect.get(target, prop, receiver);
                    }
                }) : stub;
            }
            return this._contextWebGL;
        }
        if (typeof globalThis.__compat_report === "function")
            globalThis.__compat_report("api", "getContext(" + type + ")", "context type not supported, returned null");
        return null;
    };

    HTMLCanvasElement.prototype.toDataURL = function(type, quality) {
        if (typeof globalThis.__compat_report === "function")
            globalThis.__compat_report("canvas2d", "HTMLCanvasElement.toDataURL", "returns an empty data URL");
        return "data:image/png;base64,";
    };

    HTMLCanvasElement.prototype.toBlob = function(callback, type, quality) {
        if (typeof globalThis.__compat_report === "function")
            globalThis.__compat_report("canvas2d", "HTMLCanvasElement.toBlob", "calls back with an empty blob");
        var blob = new Blob([], { type: type || "image/png" });
        if (typeof callback === "function") {
            callback(blob);
        }
    };

    HTMLCanvasElement.prototype.transferControlToOffscreen = function() {
        return this;
    };

    HTMLCanvasElement.prototype._destroyNativeContext = function() {
        if (this._context2d && typeof this._context2d._destroy === "function") {
            this._context2d._destroy();
        }
    };

    /* HTMLImageElement */

    function HTMLImageElement() {
        HTMLElement.call(this, "IMG");
        this._src = "";
        this._width = 0;
        this._height = 0;
        this._naturalWidth = 0;
        this._naturalHeight = 0;
        this._complete = false;
        this.crossOrigin = null;
        this.onload = null;
        this.onerror = null;
    }

    HTMLImageElement.prototype = Object.create(HTMLElement.prototype);
    HTMLImageElement.prototype.constructor = HTMLImageElement;

    Object.defineProperty(HTMLImageElement.prototype, "src", {
        get: function() { return this._src; },
        set: function(v) {
            this._src = v;
            this._complete = false;
            var self = this;
            /* Async load through the native image loader; falls back to an
               existence check when the image binding is unavailable. */
            Promise.resolve().then(function() {
                /* blob: URLs decode from the in-memory blob store. */
                if (v && v.indexOf("blob:") === 0 && typeof __native_image !== "undefined") {
                    var blob = _blobStore[v];
                    if (blob && blob._parts && blob._parts.length > 0) {
                        var totalSize = 0;
                        for (var i = 0; i < blob._parts.length; i++) {
                            var p = blob._parts[i];
                            if (p instanceof ArrayBuffer) totalSize += p.byteLength;
                            else if (p && p.buffer instanceof ArrayBuffer) totalSize += p.byteLength;
                        }
                        if (totalSize > 0) {
                            var combined = new ArrayBuffer(totalSize);
                            var view = new Uint8Array(combined);
                            var offset = 0;
                            for (var j = 0; j < blob._parts.length; j++) {
                                var part = blob._parts[j];
                                if (part instanceof ArrayBuffer) {
                                    view.set(new Uint8Array(part), offset);
                                    offset += part.byteLength;
                                } else if (part && part.buffer instanceof ArrayBuffer) {
                                    view.set(new Uint8Array(part.buffer, part.byteOffset, part.byteLength), offset);
                                    offset += part.byteLength;
                                }
                            }
                            var result = __native_image.loadImageFromMemory(v, combined);
                            if (result) {
                                if (self._released) {
                                    /* Owner destroyed while decoding: free, and do not fire onload. */
                                    __native_image.freeImage(result.handle);
                                    self._complete = true;
                                    return;
                                }
                                self._imageHandle = result.handle;
                                self._imageToken = __native_image.createImageToken ? __native_image.createImageToken(result.handle) : null;
                                self._glTextureId = result.glTexture || 0;
                                self._naturalWidth = result.width;
                                self._naturalHeight = result.height;
                                self._width = result.width;
                                self._height = result.height;
                                self._complete = true;
                                if (typeof self.onload === "function") self.onload.call(self);
                                self.dispatchEvent(new Event("load"));
                                return;
                            }
                        }
                    }
                    self._complete = true;
                    self._loadFailed = true;
                    if (typeof self.onerror === "function") self.onerror.call(self, new Event("error"));
                    self.dispatchEvent(new Event("error"));
                    return;
                }

                /* data: URLs and empty src complete immediately. */
                if (v === "" || v === "data:" || (v && v.indexOf("data:") === 0)) {
                    self._complete = true;
                    if (typeof self.onload === "function") self.onload.call(self);
                    self.dispatchEvent(new Event("load"));
                    return;
                }

                if (typeof __native_image !== "undefined") {
                    var info = __native_image.loadImage(v);
                    if (info) {
                        if (self._released) {
                            __native_image.freeImage(info.handle);
                            self._complete = true;
                            return;
                        }
                        self._imageHandle = info.handle;
                        self._imageToken = __native_image.createImageToken ? __native_image.createImageToken(info.handle) : null;
                        self._glTextureId = info.glTexture || 0;
                        self._naturalWidth = info.width;
                        self._naturalHeight = info.height;
                        self._width = info.width;
                        self._height = info.height;
                        self._complete = true;
                        if (typeof self.onload === "function") self.onload.call(self);
                        self.dispatchEvent(new Event("load"));
                        return;
                    }
                    self._complete = true;
                    self._loadFailed = true;
                    if (typeof self.onerror === "function") self.onerror.call(self, new Event("error"));
                    self.dispatchEvent(new Event("error"));
                    return;
                }

                /* No image binding: succeed if the file exists. */
                if (typeof __native_io !== "undefined" && __native_io.existsSync(v)) {
                    self._complete = true;
                    self._naturalWidth = self._naturalWidth || 1;
                    self._naturalHeight = self._naturalHeight || 1;
                    self._width = self._naturalWidth;
                    self._height = self._naturalHeight;
                    if (typeof self.onload === "function") self.onload.call(self);
                    self.dispatchEvent(new Event("load"));
                } else if (typeof __native_io !== "undefined") {
                    self._complete = true;
                    self._loadFailed = true;
                    if (typeof self.onerror === "function") self.onerror.call(self, new Event("error"));
                    self.dispatchEvent(new Event("error"));
                } else {
                    /* No native bindings at all. */
                    self._complete = true;
                    if (typeof self.onload === "function") self.onload.call(self);
                    self.dispatchEvent(new Event("load"));
                }
            });
        }
    });

    Object.defineProperty(HTMLImageElement.prototype, "width", {
        get: function() { return this._width; },
        set: function(v) { this._width = v; }
    });

    Object.defineProperty(HTMLImageElement.prototype, "height", {
        get: function() { return this._height; },
        set: function(v) { this._height = v; }
    });

    Object.defineProperty(HTMLImageElement.prototype, "naturalWidth", {
        get: function() { return this._naturalWidth; }
    });

    Object.defineProperty(HTMLImageElement.prototype, "naturalHeight", {
        get: function() { return this._naturalHeight; }
    });

    Object.defineProperty(HTMLImageElement.prototype, "complete", {
        get: function() { return this._complete; }
    });

    /* Release the native pixels + GL texture behind this element (called when the
       owning Bitmap is destroyed). Loads still in flight free on arrival. */
    HTMLImageElement.prototype._releaseNativeImage = function() {
        this._released = true;
        if (this._imageToken && typeof __native_image !== "undefined" && __native_image.releaseImageToken) {
            __native_image.releaseImageToken(this._imageToken);
        } else if (this._imageHandle && typeof __native_image !== "undefined" &&
            typeof __native_image.freeImage === "function") {
            __native_image.freeImage(this._imageHandle);
        }
        this._imageToken = null;
        this._imageHandle = 0;
        this._glTextureId = 0;
    };

    HTMLImageElement.prototype.decode = function() {
        var self = this;
        return new Promise(function(resolve, reject) {
            if (self._complete) {
                /* Only a failed load rejects; data: URLs complete with no intrinsic size. */
                if (self._loadFailed) reject(new Error("The image cannot be decoded."));
                else resolve();
                return;
            }
            /* Chain onto existing handlers rather than replacing them. */
            var origOnload = self.onload;
            var origOnerror = self.onerror;
            self.onload = function() {
                if (typeof origOnload === "function") origOnload.call(self);
                resolve();
            };
            self.onerror = function(e) {
                if (typeof origOnerror === "function") origOnerror.call(self, e);
                reject(new Error("The image cannot be decoded."));
            };
        });
    };

    /* HTMLVideoElement stub */

    /* HTMLVideoElement backed by the native MPEG-1 player (__native_video).
       Games ship WebM/MP4 movies; the packaging tools transcode them to .mpg,
       so a source URL is mapped to the .mpg next to it. When no playable
       file exists (unconverted game, or the runtime built without the
       player) the element reports the movie as finished right away instead:
       rmmz_core's Video.play() sets _loading until "loadeddata" fires, and
       Video.isPlaying() gates the "video" wait mode and plugins such as
       title-movie scenes, so a silent element would hang the game.

       Events are delayed by a few frames on purpose. A real video takes
       time to load, and plugins depend on that: a title-movie scene starts
       playback in its constructor, and its ended handler only acts if that
       scene is already current, which happens one frame later. */

    var VIDEO_EVENT_DELAY_MS = 150;
    var _activeVideos = [];

    function _hasNativeVideo() {
        return typeof __native_video !== "undefined" &&
               typeof __native_video.open === "function";
    }

    /* "movies/scene1.webm?x" -> "movies/scene1.mpg" */
    function _toMpegPath(src) {
        var u = String(src).split("?")[0].split("#")[0];
        return u.replace(/\.(webm|mp4|m4v|ogv|ogg)$/i, ".mpg");
    }

    function HTMLVideoElement() {
        HTMLElement.call(this, "VIDEO");
        this._src = "";
        this._volume = 1;
        this._loop = false;
        this.muted = false;
        this.paused = true;
        this.ended = false;
        this.currentTime = 0;
        this.duration = 0;
        this.readyState = 0;
        this.videoWidth = 0;
        this.videoHeight = 0;
        this._handle = 0;
        this._loadedDispatched = false;
        this._finishPending = false;
    }

    HTMLVideoElement.prototype = Object.create(HTMLElement.prototype);
    HTMLVideoElement.prototype.constructor = HTMLVideoElement;

    Object.defineProperty(HTMLVideoElement.prototype, "src", {
        get: function() { return this._src; },
        set: function(value) {
            this._closeNative();
            this._src = value ? String(value) : "";
            this.ended = false;
            this.paused = true;
            this.currentTime = 0;
            this.readyState = 0;
            this._loadedDispatched = false;
        }
    });

    Object.defineProperty(HTMLVideoElement.prototype, "volume", {
        get: function() { return this._volume; },
        set: function(value) {
            this._volume = Number(value);
            if (this._handle) __native_video.setVolume(this._handle, this._volume);
        }
    });

    Object.defineProperty(HTMLVideoElement.prototype, "loop", {
        get: function() { return this._loop; },
        set: function(value) {
            this._loop = !!value;
            if (this._handle) __native_video.setLoop(this._handle, this._loop);
        }
    });

    HTMLVideoElement.prototype._closeNative = function() {
        if (this._handle) {
            __native_video.close(this._handle);
            this._handle = 0;
        }
        var idx = _activeVideos.indexOf(this);
        if (idx >= 0) _activeVideos.splice(idx, 1);
    };

    /* Open the native player for the current source. Returns true when a
       playable file was found. */
    HTMLVideoElement.prototype._openNative = function() {
        if (this._handle) return true;
        if (!this._src || !_hasNativeVideo()) return false;
        var candidates = [_toMpegPath(this._src), this._src];
        for (var i = 0; i < candidates.length && !this._handle; i++) {
            var h = 0;
            try { h = __native_video.open(candidates[i]); } catch (e) { h = 0; }
            if (h) this._handle = h;
        }
        if (!this._handle) {
            console.warn("[video] no playable movie for " + this._src +
                         " (expected " + _toMpegPath(this._src) + "); skipping");
            return false;
        }
        var st = __native_video.getState(this._handle);
        if (st) {
            this.duration = st.duration;
            this.videoWidth = st.width;
            this.videoHeight = st.height;
        }
        __native_video.setVolume(this._handle, this._volume);
        __native_video.setLoop(this._handle, this._loop);
        _activeVideos.push(this);
        return true;
    };

    HTMLVideoElement.prototype._dispatchLoadedOnce = function() {
        if (this._loadedDispatched) return;
        this._loadedDispatched = true;
        this.readyState = 4;
        this.dispatchEvent(new Event("loadedmetadata"));
        this.dispatchEvent(new Event("loadeddata"));
        this.dispatchEvent(new Event("canplay"));
    };

    /* Fallback when nothing can be played: report loadeddata, then ended. */
    /* A movie that cannot be played "ends" quickly so cutscenes do not hang. A title-screen movie that the game
       restarts every time it ends would then spin, so the same file ending again soon gets a longer and longer wait. */
    var _skippedMovies = {};

    HTMLVideoElement.prototype._finishAsync = function() {
        if (!this._src || this._finishPending) return;
        this._finishPending = true;
        var self = this;
        var now = Date.now();
        var rec = _skippedMovies[this._src] || (_skippedMovies[this._src] = { last: 0, n: 0 });
        rec.n = (now - rec.last < 15000) ? rec.n + 1 : 0;
        var endDelay = Math.min(VIDEO_EVENT_DELAY_MS * Math.pow(2, rec.n), 20000);
        setTimeout(function() {
            if (!self._src || self._handle) { self._finishPending = false; return; }
            self.paused = false;
            self._dispatchLoadedOnce();
            setTimeout(function() {
                self._finishPending = false;
                self.paused = true;
                self.ended = true;
                rec.last = Date.now();
                self.dispatchEvent(new Event("ended"));
            }, endDelay);
        }, VIDEO_EVENT_DELAY_MS);
    };

    HTMLVideoElement.prototype.load = function() {
        this.ended = false;
        if (this._openNative()) {
            /* Asynchronous like a browser: Video.play() sets _loading after
               load() returns and _onLoad clears it. */
            var self = this;
            setTimeout(function() {
                if (self._handle) self._dispatchLoadedOnce();
            }, VIDEO_EVENT_DELAY_MS);
        } else {
            this._finishAsync();
        }
    };

    HTMLVideoElement.prototype.play = function() {
        if (!this._handle && !this._finishPending) {
            if (!this._openNative()) this._finishAsync();
        }
        if (this._handle) {
            if (this.ended) {
                /* Replaying a finished movie: reopen from the start. */
                this._closeNative();
                this.ended = false;
                this._loadedDispatched = false;
                if (!this._openNative()) { this._finishAsync(); return Promise.resolve(); }
            }
            __native_video.play(this._handle);
            this.paused = false;
            this._dispatchLoadedOnce();
            this.dispatchEvent(new Event("play"));
            this.dispatchEvent(new Event("playing"));
        } else {
            this.paused = false;
        }
        return Promise.resolve();
    };

    HTMLVideoElement.prototype.pause = function() {
        this.paused = true;
        if (this._handle) __native_video.pause(this._handle);
        this.dispatchEvent(new Event("pause"));
    };

    /* "maybe" for the formats games ship when the native player exists,
       so Utils.canPlayWebm() picks a source; "" (cannot play) otherwise. */
    HTMLVideoElement.prototype.canPlayType = function(type) {
        if (!_hasNativeVideo()) return "";
        return /video\/(webm|mp4|ogg)/i.test(String(type)) ? "maybe" : "";
    };

    /* Per frame: mirror native state and fire "ended". Visibility follows
       style.opacity, which rmmz_core toggles in Video._updateVisibility. */
    function _updateVideos() {
        for (var i = _activeVideos.length - 1; i >= 0; i--) {
            var el = _activeVideos[i];
            if (!el._handle) { _activeVideos.splice(i, 1); continue; }
            var op = el.style ? el.style.opacity : "";
            var visible = !(op === 0 || op === "0");
            __native_video.setVisible(el._handle, visible);
            var st = __native_video.getState(el._handle);
            if (!st) continue;
            el.currentTime = st.time;
            if (el._frameHook) el._frameHook(st);
            if (st.ended && !el.ended) {
                el.ended = true;
                el.paused = true;
                el.dispatchEvent(new Event("ended"));
            }
        }
    }
    globalThis.__dom_updateVideos = _updateVideos;

    /* HTMLAudioElement stub */

    function HTMLAudioElement() {
        HTMLElement.call(this, "AUDIO");
        this.src = "";
        this.volume = 1;
        this.muted = false;
        this.currentTime = 0;
        this.duration = 0;
        this.paused = true;
    }

    HTMLAudioElement.prototype = Object.create(HTMLElement.prototype);
    HTMLAudioElement.prototype.constructor = HTMLAudioElement;
    HTMLAudioElement.prototype.play = function() { return Promise.resolve(); };
    HTMLAudioElement.prototype.pause = function() {};
    HTMLAudioElement.prototype.load = function() {};
    HTMLAudioElement.prototype.canPlayType = function(type) {
        if (type && type.indexOf("ogg") >= 0) return "probably";
        if (type && type.indexOf("wav") >= 0) return "probably";
        if (type && type.indexOf("mp3") >= 0) return "probably";
        return "";
    };

    /* Blob / URL */

    var _blobStore = {};
    var _blobIdCounter = 1;

    function Blob(parts, options) {
        this._parts = parts || [];
        this.type = (options && options.type) || "";
        this.size = 0;
        for (var i = 0; i < this._parts.length; i++) {
            var p = this._parts[i];
            if (typeof p === "string") { this.size += p.length; }
            else if (p instanceof ArrayBuffer) { this.size += p.byteLength; }
            else if (p && p.byteLength !== undefined) { this.size += p.byteLength; }
        }
    }

    Blob.prototype.slice = function(start, end, type) {
        return new Blob([], { type: type || this.type });
    };

    Blob.prototype.arrayBuffer = function() {
        var self = this;
        return new Promise(function(resolve) {
            var buf = new ArrayBuffer(self.size);
            resolve(buf);
        });
    };

    Blob.prototype.text = function() {
        var self = this;
        return new Promise(function(resolve) {
            var result = "";
            for (var i = 0; i < self._parts.length; i++) {
                var p = self._parts[i];
                if (typeof p === "string") result += p;
            }
            resolve(result);
        });
    };

    var URL = {
        createObjectURL: function(blob) {
            var id = "blob:internal/" + (_blobIdCounter++);
            _blobStore[id] = blob;
            return id;
        },
        revokeObjectURL: function(url) {
            delete _blobStore[url];
        }
    };

    /* document */

    var _documentElement = new HTMLElement("HTML");
    _documentElement.style.width = "816px";
    _documentElement.style.height = "624px";

    var _body = new HTMLElement("BODY");
    _documentElement.appendChild(_body);

    var _head = new HTMLElement("HEAD");

    /* Pre-create elements from index.html that plugins expect to exist. */
    var _refresherCanvas = new HTMLCanvasElement();
    _refresherCanvas.id = "refresher";
    _body.appendChild(_refresherCanvas);

    var _title = "";
    var _allCreatedElements = [_refresherCanvas];
    var _elemGen = 0;      /* changes whenever an element is added to or removed from the registry */
    var _tagCache = {};    /* tag name -> { gen, results } */

    var document = {
        _listeners: {},
        nodeType: 9,
        readyState: "complete",
        title: _title,
        documentElement: _documentElement,
        body: _body,
        head: _head,
        fonts: {
            _loaded: true,
            ready: Promise.resolve(),
            check: function() { return true; },
            load: function() { return Promise.resolve([]); },
            add: function() {},
            forEach: function() {}
        },
        currentScript: null,
        fullscreenElement: null,
        fullScreenElement: null,
        webkitFullscreenElement: null,
        mozFullScreen: false,
        hidden: false,
        visibilityState: "visible",

        createElement: function(tagName) {
            var tag = (tagName || "").toLowerCase();
            var el;
            if (tag === "canvas") {
                el = new HTMLCanvasElement();
            } else if (tag === "img") {
                el = new HTMLImageElement();
            } else if (tag === "video") {
                el = new HTMLVideoElement();
            } else if (tag === "audio") {
                el = new HTMLAudioElement();
            } else {
                el = new HTMLElement(tagName);
            }
            /* Canvases, images, videos and audio hold big native buffers and are almost never looked up by id or tag
               name before they are attached; appendChild() registers them when they are. Keeping every one of them in
               the registry for ever leaked every bitmap of a game that leaves cleaning up to the garbage collector. */
            if (tag !== "canvas" && tag !== "img" && tag !== "video" && tag !== "audio") {
                _allCreatedElements.push(el); _elemGen++;
            }
            return el;
        },

        createElementNS: function(ns, tagName) {
            return this.createElement(tagName);
        },

        createTextNode: function(text) {
            var node = new HTMLElement("#text");
            node.nodeType = 3;
            node._innerHTML = text;
            return node;
        },

        createDocumentFragment: function() {
            var frag = new HTMLElement("#document-fragment");
            frag.nodeType = 11;
            return frag;
        },

        getElementById: function(id) {
            for (var i = 0; i < _allCreatedElements.length; i++) {
                if (_allCreatedElements[i].id === id) return _allCreatedElements[i];
            }
            return null;
        },

        getElementsByTagName: function(tag) {
            var upper = String(tag).toUpperCase();
            if (upper === "HEAD") return _asCollection([_head]);
            if (upper === "BODY") return _asCollection([_body]);
            if (upper === "HTML") return _asCollection([_documentElement]);
            var cached = _tagCache[upper];
            if (cached && cached.gen === _elemGen) return _asCollection(cached.results.slice());
            var results = [];
            for (var i = 0; i < _allCreatedElements.length; i++) {
                if (_allCreatedElements[i].tagName === upper) results.push(_allCreatedElements[i]);
            }
            _tagCache[upper] = { gen: _elemGen, results: results };
            return _asCollection(results.slice());
        },

        getElementsByClassName: function(cls) {
            return [];
        },

        querySelector: function(selector) {
            if (selector === "body") return _body;
            if (selector === "html") return _documentElement;
            if (selector === "head") return _head;
            if (selector.charAt(0) === "#") {
                return this.getElementById(selector.substring(1));
            }
            return null;
        },

        querySelectorAll: function(selector) {
            var single = this.querySelector(selector);
            return single ? [single] : [];
        },

        addEventListener: function(type, listener, options) {
            if (typeof listener !== "function") return;
            if (!this._listeners[type]) {
                this._listeners[type] = [];
            }
            var list = this._listeners[type];
            for (var i = 0; i < list.length; i++) {
                if (list[i] === listener) return;
            }
            list.push(listener);
        },

        removeEventListener: function(type, listener, options) {
            if (!this._listeners[type]) return;
            var list = this._listeners[type];
            for (var i = 0; i < list.length; i++) {
                if (list[i] === listener) {
                    list.splice(i, 1);
                    return;
                }
            }
        },

        dispatchEvent: function(event) {
            event.target = this;
            event.currentTarget = this;
            var list = this._listeners[event.type];
            if (list) {
                var copy = list.slice();
                for (var i = 0; i < copy.length; i++) {
                    copy[i].call(this, event);
                }
            }
            return true;
        },

        exitFullscreen: function() {
            if (typeof __native_platform !== "undefined") {
                __native_platform.setFullscreen(false);
            }
            this.fullscreenElement = null;
            this.fullScreenElement = null;
            this.webkitFullscreenElement = null;
            this.mozFullScreen = false;
            var evt = new Event("fullscreenchange");
            evt.target = this;
            this.dispatchEvent(evt);
            return Promise.resolve();
        },

        cancelFullScreen: function() {
            return this.exitFullscreen();
        },

        mozCancelFullScreen: function() {
            return this.exitFullscreen();
        },

        webkitExitFullscreen: function() {
            return this.exitFullscreen();
        },

        webkitCancelFullScreen: function() {
            return this.exitFullscreen();
        },

        exitPointerLock: function() {},

        createEvent: function(type) {
            return new Event(type);
        },

        getComputedStyle: null /* defined on window below */
    };

    Object.defineProperty(document, "title", {
        get: function() { return _title; },
        set: function(v) {
            _title = String(v);
            if (typeof __native_platform !== "undefined" &&
                typeof __native_platform.setWindowTitle === "function") {
                __native_platform.setWindowTitle(_title);
            }
        }
    });

    /* window */

    var _windowWidth = 816;
    var _windowHeight = 624;

    var window = globalThis;

    window._listeners = {};

    window.addEventListener = function(type, listener, options) {
        if (typeof listener !== "function") return;
        if (!this._listeners[type]) this._listeners[type] = [];
        var list = this._listeners[type];
        for (var i = 0; i < list.length; i++) {
            if (list[i] === listener) return;
        }
        list.push(listener);
    };

    window.removeEventListener = function(type, listener, options) {
        if (!this._listeners[type]) return;
        var list = this._listeners[type];
        for (var i = 0; i < list.length; i++) {
            if (list[i] === listener) {
                list.splice(i, 1);
                return;
            }
        }
    };

    window.dispatchEvent = function(event) {
        event.target = this;
        event.currentTarget = this;
        var list = this._listeners[event.type];
        if (list) {
            var copy = list.slice();
            for (var i = 0; i < copy.length; i++) {
                copy[i].call(this, event);
            }
        }
        var handler = this["on" + event.type];
        if (typeof handler === "function") {
            handler.call(this, event);
        }
        return true;
    };

    window.innerWidth = _windowWidth;
    window.innerHeight = _windowHeight;
    window.outerWidth = _windowWidth;
    window.outerHeight = _windowHeight;
    window.devicePixelRatio = 1;

    window.document = document;
    window.navigator = null;  /* set by navigator_shim.js */

    window.location = {
        href: "file:///game/index.html",
        origin: "file://",
        protocol: "file:",
        host: "",
        hostname: "",
        port: "",
        pathname: "/game/index.html",
        search: "",
        hash: "",
        reload: function() {},
        assign: function() {},
        replace: function() {}
    };

    window.screen = {
        width: _windowWidth,
        height: _windowHeight,
        availWidth: _windowWidth,
        availHeight: _windowHeight,
        colorDepth: 32,
        pixelDepth: 32,
        orientation: {
            type: "landscape-primary",
            angle: 0,
            addEventListener: function() {},
            removeEventListener: function() {}
        }
    };

    window.performance = {
        now: function() { return _now(); },
        mark: function() {},
        measure: function() {},
        getEntries: function() { return []; },
        getEntriesByName: function() { return []; },
        getEntriesByType: function() { return []; },
        clearMarks: function() {},
        clearMeasures: function() {}
    };

    window.getComputedStyle = function(el) {
        return el.style || new CSSStyleDeclaration();
    };
    document.getComputedStyle = window.getComputedStyle;

    window.getSelection = function() {
        return { removeAllRanges: function() {}, addRange: function() {} };
    };

    window.matchMedia = function(query) {
        return {
            matches: false,
            media: query,
            addEventListener: function() {},
            removeEventListener: function() {},
            addListener: function() {},
            removeListener: function() {}
        };
    };

    window.open = function() { return null; };
    window.close = function() {};
    window.focus = function() {};
    window.blur = function() {};
    window.scrollTo = function() {};
    window.moveTo = function() {};
    window.moveBy = function() {};
    window.resizeTo = function() {};
    window.resizeBy = function() {};
    window.alert = function(msg) { console.log("[alert] " + msg); };
    window.confirm = function(msg) { return true; };
    window.prompt = function(msg) { return null; };
    window.print = function() {};

    window.atob = function(b64) {
        if (typeof globalThis.__atob === "function") return globalThis.__atob(b64);
        var chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        var str = b64.replace(/[=]+$/, "");
        var output = "";
        for (var i = 0, len = str.length; i < len; i += 4) {
            var a = chars.indexOf(str[i]);
            var b = chars.indexOf(str[i + 1] || "A");
            var c = chars.indexOf(str[i + 2] || "A");
            var d = chars.indexOf(str[i + 3] || "A");
            var bits = (a << 18) | (b << 12) | (c << 6) | d;
            output += String.fromCharCode((bits >> 16) & 0xFF);
            if (str[i + 2]) output += String.fromCharCode((bits >> 8) & 0xFF);
            if (str[i + 3]) output += String.fromCharCode(bits & 0xFF);
        }
        return output;
    };

    window.btoa = function(str) {
        if (typeof globalThis.__btoa === "function") return globalThis.__btoa(str);
        var chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        var output = "";
        for (var i = 0; i < str.length; i += 3) {
            var a = str.charCodeAt(i);
            var b = str.charCodeAt(i + 1);
            var c = str.charCodeAt(i + 2);
            var bits = (a << 16) | ((b || 0) << 8) | (c || 0);
            output += chars[(bits >> 18) & 0x3F];
            output += chars[(bits >> 12) & 0x3F];
            output += (i + 1 < str.length) ? chars[(bits >> 6) & 0x3F] : "=";
            output += (i + 2 < str.length) ? chars[bits & 0x3F] : "=";
        }
        return output;
    };

    window.setTimeout = _setTimeout;
    window.setInterval = _setInterval;
    window.clearTimeout = _clearTimeout;
    window.clearInterval = _clearInterval;
    window.requestAnimationFrame = _requestAnimationFrame;
    window.cancelAnimationFrame = _cancelAnimationFrame;

    globalThis.setTimeout = _setTimeout;
    globalThis.setInterval = _setInterval;
    globalThis.clearTimeout = _clearTimeout;
    globalThis.clearInterval = _clearInterval;
    globalThis.requestAnimationFrame = _requestAnimationFrame;
    globalThis.cancelAnimationFrame = _cancelAnimationFrame;

    /* Globals */

    globalThis.window = window;
    globalThis.self = window;
    globalThis.document = document;
    globalThis.Event = Event;
    /* window.name: the browsing context name, "" by default. Scripts read
       it as a bare global; without it that is a ReferenceError. */
    if (typeof globalThis.name === "undefined") {
        globalThis.name = "";
    }

    globalThis.KeyboardEvent = KeyboardEvent;
    globalThis.MouseEvent = MouseEvent;
    globalThis.WheelEvent = WheelEvent;
    globalThis.EventTarget = EventTarget;
    globalThis.HTMLElement = HTMLElement;
    globalThis.HTMLCanvasElement = HTMLCanvasElement;
    globalThis.HTMLImageElement = HTMLImageElement;
    globalThis.HTMLVideoElement = HTMLVideoElement;
    globalThis.HTMLAudioElement = HTMLAudioElement;
    globalThis.Audio = HTMLAudioElement;
    globalThis.CSSStyleDeclaration = CSSStyleDeclaration;
    globalThis.Blob = Blob;
    globalThis.URL = URL;
    globalThis.Image = HTMLImageElement;

    /* Internal API called from C each frame */

    globalThis.__dom_flushTimers = _flushTimers;
    globalThis.__dom_flushAnimationFrames = _flushAnimationFrames;

    /* Drop an element from the registry so GC can reclaim it. */
    function _removeFromElementRegistry(el) {
        var idx = _allCreatedElements.indexOf(el);
        if (idx >= 0) {
            _allCreatedElements.splice(idx, 1); _elemGen++;
        }
    }
    globalThis.__dom_removeFromElementRegistry = _removeFromElementRegistry;
    globalThis.__dom_getElementRegistryLength = function() {
        return _allCreatedElements.length;
    };

    /* Called from C on window resize. */
    globalThis.__dom_setWindowSize = function(w, h) {
        _windowWidth = w;
        _windowHeight = h;
        window.innerWidth = w;
        window.innerHeight = h;
        window.outerWidth = w;
        window.outerHeight = h;
        if (document.documentElement) {
            document.documentElement.style.width = w + "px";
            document.documentElement.style.height = h + "px";
            document.documentElement.clientWidth = w;
            document.documentElement.clientHeight = h;
        }

        /* Letterbox the logical game resolution into the window, preserving aspect ratio. */
        if (typeof __native_renderer !== "undefined" && __native_renderer.getSize) {
            var gameSize = __native_renderer.getSize();
            if (gameSize && gameSize.width > 0 && gameSize.height > 0) {
                var gw = gameSize.width;
                var gh = gameSize.height;
                var scale = Math.min(w / gw, h / gh);
                var vpW = Math.round(gw * scale);
                var vpH = Math.round(gh * scale);
                var vpX = Math.round((w - vpW) / 2);
                var vpY = Math.round((h - vpH) / 2);
                /* GL viewport origin is bottom-left. */
                var vpY_gl = h - vpY - vpH;
                __native_renderer.setScreenViewport(vpX, vpY_gl, vpW, vpH);

                /* Canvas layout drives Graphics.pageToCanvasX/Y mouse mapping. */
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

        var evt = new Event("resize");
        window.dispatchEvent(evt);
    };

    /* Populate window.screen / devicePixelRatio from the native display. */
    globalThis.__dom_initDisplayInfo = function() {
        if (typeof __native_platform === "undefined") return;
        var ds = __native_platform.getDisplaySize();
        if (ds) {
            window.screen.width = ds.width;
            window.screen.height = ds.height;
            window.screen.availWidth = ds.width;
            window.screen.availHeight = ds.height;
        }
        var scale = __native_platform.getDisplayScale();
        if (scale && scale > 0) {
            window.devicePixelRatio = scale;
        }
    };

    /* Called by the script loader before each plugin is evaluated so
       document.currentScript.src matches the plugin path, as in a browser. */
    globalThis.__dom_setCurrentScript = function(src) {
        if (src) {
            __dom_noteScriptLoaded(src);
            var scriptEl = new HTMLElement("SCRIPT");
            scriptEl.tagName = "SCRIPT";
            scriptEl.src = src;
            scriptEl.type = "text/javascript";
            document.currentScript = scriptEl;
            /* Findable via getElementsByTagName("script"). */
            _allCreatedElements.push(scriptEl); _elemGen++;
        } else {
            document.currentScript = null;
        }
    };

    /* Error handling (window.onerror, onunhandledrejection) */

    window.onerror = null;

    window.onunhandledrejection = null;

    /* Puts an uncaught error or unhandled rejection in the compatibility log (see error_handler.h): one entry per
       message shape and throw site, so a plugin that throws every frame is one line with a count. The stack is the
       engine's own format ("fn@file:line:col" on JavaScriptCore, "at fn (file:line)" on QuickJS). */
    function _compatThrown(category, message, errorObj) {
        if (typeof globalThis.__compat_report !== "function") return;
        try {
            var msg = String(message === undefined ? "" : message).replace(/\s+/g, " ");
            var frames = [];
            var st = errorObj && typeof errorObj === "object" ? errorObj.stack : null;
            if (typeof st === "string") {
                var lines = st.split("\n");
                for (var i = 0; i < lines.length && frames.length < 4; i++) {
                    var l = lines[i].trim();
                    if (!l || l.indexOf("[native code]") >= 0) continue;
                    if (!/:\d+/.test(l)) continue;
                    frames.push(l.length > 110 ? l.slice(0, 110) : l);
                }
            }
            var key = msg.replace(/\d+/g, "#").slice(0, 100) + " @ " + (frames[0] || "?");
            globalThis.__compat_report(category, key, msg.slice(0, 120) + (frames.length > 1 ? " << " + frames.slice(1).join(" < ") : ""));
        } catch (ignore) { /* logging must never throw */ }
    }

    /* Called from the native error handler; routes to window.onerror
       (browser signature) and then 'error' event listeners. */
    globalThis.__error_handler_dispatch = function(message, errorObj) {
        _compatThrown("exception", message, errorObj);
        if (typeof window.onerror === "function") {
            try {
                var handled = window.onerror(message, "", 0, 0, errorObj);
                if (handled === true) return;
            } catch (e) {
                console.error("[error_handler] window.onerror threw:", e);
            }
        }

        var evt = new Event("error");
        evt.message = message;
        evt.error = errorObj;
        evt.filename = "";
        evt.lineno = 0;
        evt.colno = 0;
        window.dispatchEvent(evt);
    };

    /* Called from C for unhandled promise rejections. */
    globalThis.__error_handler_dispatchRejection = function(reason) {
        _compatThrown("rejection", reason && typeof reason === "object" && reason.message !== undefined ?
            (reason.name ? reason.name + ": " : "") + reason.message : reason, reason);
        if (typeof window.onunhandledrejection === "function") {
            try {
                var evt = {
                    type: "unhandledrejection",
                    reason: reason,
                    promise: null,
                    preventDefault: function() { this.defaultPrevented = true; },
                    defaultPrevented: false
                };
                window.onunhandledrejection(evt);
                if (evt.defaultPrevented) return;
            } catch (e) {
                console.error("[error_handler] onunhandledrejection threw:", e);
            }
        }

        var evt2 = new Event("unhandledrejection");
        evt2.reason = reason;
        evt2.promise = null;
        window.dispatchEvent(evt2);
    };

    /* Crash-recovery auto-save via DataManager. Returns 1 on success, 0 on failure. */
    globalThis.__error_handler_autoSave = function() {
        try {
            if (typeof DataManager !== "undefined" &&
                typeof DataManager.saveGame === "function" &&
                typeof $gameSystem !== "undefined") {
                /* Slot 0 is the RPG Maker MZ auto-save slot. */
                DataManager.saveGame(0);
                return 1;
            }
        } catch (e) {
            console.error("[error_handler] Auto-save failed:", e);
        }
        return 0;
    };

    globalThis.ErrorEvent = function ErrorEvent(type, init) {
        var e = new Event(type || "error");
        e.message = (init && init.message) || "";
        e.filename = (init && init.filename) || "";
        e.lineno = (init && init.lineno) || 0;
        e.colno = (init && init.colno) || 0;
        e.error = (init && init.error) || null;
        return e;
    };

    globalThis.PromiseRejectionEvent = function PromiseRejectionEvent(type, init) {
        var e = new Event(type || "unhandledrejection");
        e.reason = (init && init.reason) || undefined;
        e.promise = (init && init.promise) || null;
        return e;
    };

    /* Input dispatch (called from C each frame) */

    /* Translate queued native input events into DOM events on document. */
    globalThis.__dom_flushInputEvents = function() {
        if (typeof __native_input === "undefined") return;

        var events = __native_input.pollEvents();
        if (!events || events.length === 0) return;

        for (var i = 0; i < events.length; i++) {
            var raw = events[i];
            var evt;
            var type = raw.type;

            /* HTML overlay widgets (dom_overlay.js) get first pick: a focused
               text input takes keyboard and text events, buttons take clicks. */
            if (typeof __dom_overlay !== "undefined" && __dom_overlay.handleRawEvent(raw)) {
                continue;
            }
            if (type === "textinput") continue;

            if (type === "keydown" || type === "keyup") {
                evt = new KeyboardEvent(type, {
                    key: raw.key,
                    code: raw.code,
                    keyCode: raw.keyCode,
                    repeat: raw.repeat,
                    shiftKey: raw.shiftKey,
                    ctrlKey: raw.ctrlKey,
                    altKey: raw.altKey,
                    metaKey: raw.metaKey,
                    bubbles: true,
                    cancelable: true
                });
                document.dispatchEvent(evt);
            } else if (type === "mousedown" || type === "mouseup" || type === "mousemove") {
                evt = new MouseEvent(type, {
                    clientX: raw.clientX,
                    clientY: raw.clientY,
                    pageX: raw.pageX,
                    pageY: raw.pageY,
                    screenX: raw.screenX,
                    screenY: raw.screenY,
                    button: raw.button,
                    bubbles: true,
                    cancelable: true
                });
                document.dispatchEvent(evt);
            } else if (type === "wheel") {
                evt = new WheelEvent(type, {
                    clientX: raw.clientX,
                    clientY: raw.clientY,
                    pageX: raw.pageX,
                    pageY: raw.pageY,
                    deltaX: raw.deltaX,
                    deltaY: raw.deltaY,
                    deltaZ: raw.deltaZ,
                    deltaMode: raw.deltaMode,
                    bubbles: true,
                    cancelable: true
                });
                document.dispatchEvent(evt);
            }
        }
    };

    /* Push native gamepad state into the navigator shim. */
    globalThis.__dom_updateGamepads = function() {
        if (typeof __native_input === "undefined") return;
        if (typeof __navigator_setGamepad !== "function") return;
        if (typeof __navigator_clearGamepad !== "function") return;

        var count = __native_input.getGamepadCount();
        for (var i = 0; i < count; i++) {
            var gp = __native_input.getGamepad(i);
            if (gp) {
                __navigator_setGamepad(i, gp);
            } else {
                __navigator_clearGamepad(i);
            }
        }
    };

})();

/* ---- window globals (browser) that scripts use as plain names ---------------------------------------------------
   A browser's window has many properties that code reads without "window.": `length` (number of frames), `parent`,
   `top`, `status`, ... A missing one is a ReferenceError at the first use. Defined only when absent. */
(function() {
    "use strict";
    var g = globalThis;

    function define(name, value) {
        if (name in g && g[name] !== undefined) return;
        try {
            Object.defineProperty(g, name, { value: value, writable: true, configurable: true, enumerable: false });
        } catch (e) {}
    }

    define("length", 0);
    define("status", "");
    define("defaultStatus", "");
    define("closed", false);
    define("parent", g);
    define("top", g);
    define("frames", g);
    define("opener", null);
    define("origin", "file://");
    define("event", undefined);
    if (!("event" in g)) define("event", undefined);
    define("isSecureContext", true);
    define("fullScreen", false);
    define("screenX", 0);
    define("screenY", 0);
    define("screenLeft", 0);
    define("screenTop", 0);
    define("scrollX", 0);
    define("scrollY", 0);
    define("pageXOffset", 0);
    define("pageYOffset", 0);
    ["menubar", "toolbar", "statusbar", "personalbar", "locationbar", "scrollbars"].forEach(function(n) {
        define(n, { visible: false });
    });
    define("external", {});

    define("history", {
        length: 1,
        state: null,
        scrollRestoration: "auto",
        back: function() {},
        forward: function() {},
        go: function() {},
        pushState: function() {},
        replaceState: function() {}
    });

    /* sessionStorage: kept in memory for the session */
    if (typeof g.sessionStorage === "undefined") {
        var store = {};
        var session = {
            getItem: function(k) { k = String(k); return Object.prototype.hasOwnProperty.call(store, k) ? store[k] : null; },
            setItem: function(k, v) { store[String(k)] = String(v); },
            removeItem: function(k) { delete store[String(k)]; },
            clear: function() { store = {}; },
            key: function(i) { var keys = Object.keys(store); return i < keys.length ? keys[i] : null; }
        };
        Object.defineProperty(session, "length", { get: function() { return Object.keys(store).length; } });
        define("sessionStorage", session);
    }

    /* crypto: random values and UUIDs (no real cryptography) */
    if (typeof g.crypto === "undefined") {
        define("crypto", {
            getRandomValues: function(array) {
                var bytes = new Uint8Array(array.buffer, array.byteOffset, array.byteLength);
                for (var i = 0; i < bytes.length; i++) bytes[i] = Math.floor(Math.random() * 256);
                return array;
            },
            randomUUID: function() {
                return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, function(c) {
                    var r = Math.floor(Math.random() * 16);
                    return (c === "x" ? r : (r & 3) | 8).toString(16);
                });
            },
            subtle: {}
        });
    }

    define("visualViewport", {
        width: 816, height: 624, scale: 1, offsetLeft: 0, offsetTop: 0, pageLeft: 0, pageTop: 0,
        addEventListener: function() {}, removeEventListener: function() {}
    });
    define("customElements", { define: function() {}, get: function() { return undefined; }, whenDefined: function() { return Promise.resolve(); } });
    define("scrollBy", function() {});
    define("stop", function() {});
    define("find", function() { return false; });
    define("reportError", function(e) { if (typeof console !== "undefined") console.error(e); });

    define("requestIdleCallback", function(cb) {
        return setTimeout(function() {
            cb({ didTimeout: false, timeRemaining: function() { return 10; } });
        }, 1);
    });
    define("cancelIdleCallback", function(id) { clearTimeout(id); });

    define("postMessage", function(message) {
        setTimeout(function() {
            try {
                var ev = typeof Event === "function" ? new Event("message") : { type: "message" };
                ev.data = message;
                ev.source = g;
                if (typeof g.onmessage === "function") g.onmessage(ev);
                if (typeof g.dispatchEvent === "function") g.dispatchEvent(ev);
            } catch (e) {}
        }, 0);
    });

    /* structuredClone: a deep copy that keeps cycles, dates, maps, sets and typed arrays */
    if (typeof g.structuredClone === "undefined") {
        define("structuredClone", function(value) {
            var seen = new Map();
            function clone(v) {
                if (v === null || typeof v !== "object") {
                    if (typeof v === "function" || typeof v === "symbol") {
                        var err = new Error("The object can not be cloned.");
                        err.name = "DataCloneError";
                        throw err;
                    }
                    return v;
                }
                if (seen.has(v)) return seen.get(v);
                var out;
                if (v instanceof Date) { out = new Date(v.getTime()); seen.set(v, out); return out; }
                if (v instanceof RegExp) { out = new RegExp(v.source, v.flags); seen.set(v, out); return out; }
                if (v instanceof ArrayBuffer) { out = v.slice(0); seen.set(v, out); return out; }
                if (ArrayBuffer.isView(v)) {
                    var buf = clone(v.buffer);
                    out = v instanceof DataView ? new DataView(buf, v.byteOffset, v.byteLength)
                                                : new v.constructor(buf, v.byteOffset, v.length);
                    seen.set(v, out);
                    return out;
                }
                if (v instanceof Map) {
                    out = new Map(); seen.set(v, out);
                    v.forEach(function(val, key) { out.set(clone(key), clone(val)); });
                    return out;
                }
                if (v instanceof Set) {
                    out = new Set(); seen.set(v, out);
                    v.forEach(function(val) { out.add(clone(val)); });
                    return out;
                }
                if (Array.isArray(v)) {
                    out = new Array(v.length); seen.set(v, out);
                    for (var i = 0; i < v.length; i++) out[i] = clone(v[i]);
                    return out;
                }
                out = {}; seen.set(v, out);
                Object.keys(v).forEach(function(k) { out[k] = clone(v[k]); });
                return out;
            }
            return clone(value);
        });
    }
})();
