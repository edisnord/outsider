/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

/*
 * HTML overlay: draws plain DOM widgets that plugins append to
 * document.body (forms with text inputs, buttons and labels, e.g. name
 * entry dialogs) on top of the game and feeds keyboard, text and mouse
 * input to them. Without this such a plugin's event waits forever for a
 * dialog nobody can see.
 *
 * Supported: a subset of CSS on element.style and on tag/id/class rules in
 * <style> elements: block and flex layout (direction, justify-content,
 * align-items), width/height in px or %, padding/margin/border shorthands,
 * border-radius, background-color, color, font-size/font-family, display,
 * position:absolute with left/top, and transform translate(%)/scale().
 * Elements are laid out in game pixels and rendered through the software
 * canvas, then composited after the game's own frame.
 */

(function() {
    "use strict";

    var OVERLAY_TAGS = { FORM: 1, DIV: 1, INPUT: 1, BUTTON: 1, TEXTAREA: 1, SPAN: 1,
                         P: 1, LABEL: 1, SELECT: 1, SECTION: 1, UL: 1, LI: 1, H1: 1, H2: 1, H3: 1 };
    var FOCUSABLE = { INPUT: 1, TEXTAREA: 1, BUTTON: 1, SELECT: 1 };

    var overlay = {
        roots: [],
        focused: null,
        hover: null,
        pressed: null,
        frame: 0,
        canvas: null,
        ctx: null,
        measureCanvas: null,
        measureCtx: null,
        texture: 0,
        texW: 0,
        texH: 0,
        lastRoot: null
    };

    /* --- Element properties the DOM shim lacks --------------------------- */

    if (!Object.getOwnPropertyDescriptor(HTMLElement.prototype, "innerText")) {
        Object.defineProperty(HTMLElement.prototype, "innerText", {
            get: function() { return this.textContent; },
            set: function(v) { this.textContent = v; }
        });
    }

    /* form.elements: descendants that are form controls, by index and name/id. */
    Object.defineProperty(HTMLElement.prototype, "elements", {
        get: function() {
            var list = [];
            (function collect(el) {
                for (var i = 0; i < el.children.length; i++) {
                    var c = el.children[i];
                    if (FOCUSABLE[c.tagName]) {
                        list.push(c);
                        if (c.name && !list[c.name]) list[c.name] = c;
                        if (c.id && !list[c.id]) list[c.id] = c;
                    }
                    collect(c);
                }
            })(this);
            return list;
        },
        configurable: true
    });

    var focusOrig = HTMLElement.prototype.focus;
    HTMLElement.prototype.focus = function() {
        focusOrig.call(this);
        if (FOCUSABLE[this.tagName]) {
            setFocus(this);
        }
    };
    var blurOrig = HTMLElement.prototype.blur;
    HTMLElement.prototype.blur = function() {
        blurOrig.call(this);
        if (overlay.focused === this) setFocus(null);
    };
    HTMLElement.prototype.click = function() {
        fireBubbling(this, makeEvent("click"));
    };

    function setFocus(el) {
        if (overlay.focused === el) return;
        if (overlay.focused) fireBubbling(overlay.focused, makeEvent("blur", false));
        overlay.focused = el;
        document.activeElement = el || document.body;
        if (el) {
            if (el.tagName === "INPUT" || el.tagName === "TEXTAREA") {
                if (el.value === undefined || el.value === null) el.value = "";
                el.value = String(el.value);
                if (typeof el.selectionStart !== "number") el.selectionStart = el.value.length;
                if (typeof el.selectionEnd !== "number") el.selectionEnd = el.value.length;
            }
            fireBubbling(el, makeEvent("focus", false));
        }
    }

    /* --- Root tracking --------------------------------------------------- */

    function isOverlayElement(el) {
        return !!(el && el.tagName && OVERLAY_TAGS[el.tagName]);
    }

    var body = document.body;
    var bodyAppend = body.appendChild;
    var bodyRemove = body.removeChild;
    var bodyInsert = body.insertBefore;
    body.appendChild = function(child) {
        var r = bodyAppend.call(this, child);
        if (isOverlayElement(child) && overlay.roots.indexOf(child) < 0) overlay.roots.push(child);
        return r;
    };
    if (typeof bodyInsert === "function") {
        body.insertBefore = function(child, ref) {
            var r = bodyInsert.call(this, child, ref);
            if (isOverlayElement(child) && overlay.roots.indexOf(child) < 0) overlay.roots.push(child);
            return r;
        };
    }
    body.removeChild = function(child) {
        var r = bodyRemove.call(this, child);
        var i = overlay.roots.indexOf(child);
        if (i >= 0) overlay.roots.splice(i, 1);
        if (overlay.focused && contains(child, overlay.focused)) setFocus(null);
        return r;
    };

    function contains(root, el) {
        for (var p = el; p; p = p.parentNode) if (p === root) return true;
        return false;
    }

    /* --- Events ---------------------------------------------------------- */

    function makeEvent(type, bubbles) {
        var e = new Event(type, { bubbles: bubbles !== false, cancelable: true });
        return e;
    }

    /* Dispatch to el and up its ancestors, then document, like the browser
       (the shim's dispatchEvent only ever hits one target). */
    function fireBubbling(el, evt) {
        var stopped = false;
        evt.stopPropagation = function() { stopped = true; };
        evt.stopImmediatePropagation = function() { stopped = true; };
        var path = [];
        for (var p = el; p; p = p.parentNode) path.push(p);
        if (path[path.length - 1] !== document) path.push(document);
        for (var i = 0; i < path.length && !stopped; i++) {
            var node = path[i];
            var list = node._listeners && node._listeners[evt.type];
            evt.target = el;
            evt.currentTarget = node;
            if (list) {
                var copy = list.slice();
                for (var j = 0; j < copy.length && !stopped; j++) copy[j].call(node, evt);
            }
            var handler = node["on" + evt.type];
            if (typeof handler === "function" && !stopped) handler.call(node, evt);
            if (!evt.bubbles) break;
        }
        return !evt.defaultPrevented;
    }

    function keyEventFromRaw(raw) {
        return new KeyboardEvent(raw.type, {
            key: raw.key, code: raw.code, keyCode: raw.keyCode, repeat: raw.repeat,
            shiftKey: raw.shiftKey, ctrlKey: raw.ctrlKey, altKey: raw.altKey, metaKey: raw.metaKey,
            bubbles: true, cancelable: true
        });
    }

    function insertText(el, text) {
        var v = String(el.value || "");
        var s = clampSel(el, el.selectionStart), e = clampSel(el, el.selectionEnd);
        if (s > e) { var t = s; s = e; e = t; }
        var max = parseInt(el.maxLength, 10);
        if (max > 0) {
            var room = max - (v.length - (e - s));
            if (room <= 0) return;
            text = text.substring(0, room);
        }
        el.value = v.substring(0, s) + text + v.substring(e);
        el.selectionStart = el.selectionEnd = s + text.length;
        fireBubbling(el, makeEvent("input"));
    }

    function clampSel(el, n) {
        var len = String(el.value || "").length;
        if (typeof n !== "number" || n < 0) return len;
        return n > len ? len : n;
    }

    function deleteText(el, dir) {
        var v = String(el.value || "");
        var s = clampSel(el, el.selectionStart), e = clampSel(el, el.selectionEnd);
        if (s > e) { var t = s; s = e; e = t; }
        if (s === e) {
            if (dir < 0 && s > 0) s--;
            else if (dir > 0 && e < v.length) e++;
            else return;
        }
        el.value = v.substring(0, s) + v.substring(e);
        el.selectionStart = el.selectionEnd = s;
        fireBubbling(el, makeEvent("input"));
    }

    /* Browser default actions for a keydown on a text input / button. */
    function defaultKeyAction(el, evt) {
        var isText = el.tagName === "INPUT" || el.tagName === "TEXTAREA";
        if (isText) {
            switch (evt.key) {
                case "Backspace": deleteText(el, -1); return;
                case "Delete": deleteText(el, 1); return;
                case "ArrowLeft":
                    el.selectionStart = el.selectionEnd = Math.max(0, clampSel(el, el.selectionStart) - 1); return;
                case "ArrowRight":
                    el.selectionStart = el.selectionEnd = clampSel(el, el.selectionStart + 1); return;
                case "Home": el.selectionStart = el.selectionEnd = 0; return;
                case "End": el.selectionStart = el.selectionEnd = String(el.value || "").length; return;
                case "Enter":
                    if (el.tagName === "INPUT") {
                        var form = closestTag(el, "FORM");
                        if (form) fireBubbling(form, makeEvent("submit"));
                    }
                    return;
                case "Tab": moveFocus(el, evt.shiftKey ? -1 : 1); return;
            }
        } else if (el.tagName === "BUTTON") {
            if (evt.key === "Enter" || evt.key === " ") {
                fireBubbling(el, makeEvent("click"));
            } else if (evt.key === "Tab") {
                moveFocus(el, evt.shiftKey ? -1 : 1);
            }
        }
    }

    function closestTag(el, tag) {
        for (var p = el; p; p = p.parentNode) if (p.tagName === tag) return p;
        return null;
    }

    function focusables() {
        var list = [];
        for (var r = 0; r < overlay.roots.length; r++) {
            (function collect(el) {
                if (isHidden(el)) return;
                if (FOCUSABLE[el.tagName] && !el.disabled) list.push(el);
                for (var i = 0; i < el.children.length; i++) collect(el.children[i]);
            })(overlay.roots[r]);
        }
        return list;
    }

    function moveFocus(el, dir) {
        var list = focusables();
        if (!list.length) return;
        var i = list.indexOf(el);
        var next = list[(i + dir + list.length) % list.length];
        next.focus();
    }

    /* Called by the DOM shim for every raw native event before it reaches
       document. Returns true when the overlay consumed the event. */
    overlay.handleRawEvent = function(raw) {
        if (overlay.roots.length === 0) return false;
        var type = raw.type;
        if (type === "textinput") {
            var f = overlay.focused;
            if (f && (f.tagName === "INPUT" || f.tagName === "TEXTAREA") && !f.readOnly && !f.disabled) {
                insertText(f, raw.text || "");
            }
            return true;
        }
        if (type === "keydown" || type === "keyup") {
            var target = overlay.focused;
            if (!target) return false;
            var evt = keyEventFromRaw(raw);
            var ok = fireBubbling(target, evt);
            if (type === "keydown" && ok) defaultKeyAction(target, evt);
            return true;                     /* bubbling already reached document */
        }
        if (type === "mousedown" || type === "mouseup" || type === "mousemove") {
            var hit = hitTest(raw.clientX, raw.clientY);
            if (type === "mousemove") {
                overlay.hover = hit;
                return false;
            }
            if (!hit) return false;
            var mevt = new MouseEvent(type, { clientX: raw.clientX, clientY: raw.clientY,
                                              button: raw.button, bubbles: true, cancelable: true });
            fireBubbling(hit, mevt);
            if (type === "mousedown") {
                overlay.pressed = hit;
                var target2 = FOCUSABLE[hit.tagName] ? hit : null;
                if (target2) target2.focus();
            } else {
                if (overlay.pressed === hit) {
                    var clickable = FOCUSABLE[hit.tagName] ? hit : closestTag(hit, "BUTTON") || hit;
                    fireBubbling(clickable, new MouseEvent("click", { clientX: raw.clientX, clientY: raw.clientY,
                                                                    button: raw.button, bubbles: true, cancelable: true }));
                }
                overlay.pressed = null;
            }
            return true;
        }
        return false;
    };

    /* --- CSS ------------------------------------------------------------- */

    var styleCache = { texts: null, rules: null };

    function styleRules() {
        /* Tag, #id and .class rules from <style> elements, latest wins. Parsed again only when the text of a
           style element changed (the overlay asks for the rules on every frame). */
        var sheets = document.getElementsByTagName("style");
        var texts = [];
        for (var t = 0; t < sheets.length; t++) texts.push(sheets[t].textContent || "");
        var cached = styleCache.texts;
        if (cached && cached.length === texts.length) {
            var same = true;
            for (var q = 0; q < texts.length; q++) if (cached[q] !== texts[q]) { same = false; break; }
            if (same) return styleCache.rules;
        }
        var rules = [];
        for (var i = 0; i < sheets.length; i++) {
            var text = texts[i];
            var re = /([^{}]+)\{([^}]*)\}/g, m;
            while ((m = re.exec(text))) {
                if (m[1].indexOf("@") >= 0) continue;
                var decls = {};
                m[2].split(";").forEach(function(d) {
                    var k = d.indexOf(":");
                    if (k > 0) decls[d.substring(0, k).trim().toLowerCase()] = d.substring(k + 1).trim().replace(/!important/g, "").trim();
                });
                m[1].split(",").forEach(function(sel) {
                    rules.push({ sel: sel.trim(), decls: decls });
                });
            }
        }
        styleCache.texts = texts;
        styleCache.rules = rules;
        return rules;
    }

    function matches(el, sel) {
        if (sel.charAt(0) === "#") return el.id === sel.substring(1);
        if (sel.charAt(0) === ".") return el.classList && el.classList.contains(sel.substring(1));
        return el.tagName === sel.toUpperCase();
    }

    var CAMEL = {};
    function camel(name) {
        if (!CAMEL[name]) CAMEL[name] = name.replace(/-([a-z])/g, function(m, c) { return c.toUpperCase(); });
        return CAMEL[name];
    }

    /* Resolved style: sheet rules first, then element.style. */
    function computeStyle(el, rules) {
        var out = {};
        for (var i = 0; i < rules.length; i++) {
            if (matches(el, rules[i].sel)) {
                for (var k in rules[i].decls) out[camel(k)] = rules[i].decls[k];
            }
        }
        var st = el.style || {};
        for (var key in st) {
            if (typeof st[key] === "string" && st[key] !== "" && key !== "cssText") out[key] = st[key];
        }
        if (st.cssText) {
            st.cssText.split(";").forEach(function(d) {
                var k = d.indexOf(":");
                if (k > 0) out[camel(d.substring(0, k).trim())] = d.substring(k + 1).trim();
            });
        }
        return out;
    }

    function px(value, base, scale) {
        if (value === undefined || value === null || value === "" || value === "auto") return null;
        var s = String(value).trim();
        var n = parseFloat(s);
        if (isNaN(n)) return null;
        if (s.slice(-1) === "%") return base !== null && base !== undefined ? n * base / 100 : null;
        if (s.slice(-2) === "em") return n * 16 * scale;
        return n * scale;
    }

    /* 1-4 value shorthand (top right bottom left) plus per-side overrides. */
    function sides(cs, name, scale) {
        var r = { t: 0, r: 0, b: 0, l: 0 };
        var sh = cs[name];
        if (sh) {
            var parts = String(sh).trim().split(/\s+/).map(function(v) { return px(v, 0, scale) || 0; });
            if (parts.length === 1) { r.t = r.r = r.b = r.l = parts[0]; }
            else if (parts.length === 2) { r.t = r.b = parts[0]; r.r = r.l = parts[1]; }
            else if (parts.length === 3) { r.t = parts[0]; r.r = r.l = parts[1]; r.b = parts[2]; }
            else { r.t = parts[0]; r.r = parts[1]; r.b = parts[2]; r.l = parts[3]; }
        }
        var cap = name.charAt(0).toUpperCase() + name.slice(1);
        var v;
        if ((v = px(cs[name + "Top"], 0, scale)) !== null) r.t = v;
        if ((v = px(cs[name + "Right"], 0, scale)) !== null) r.r = v;
        if ((v = px(cs[name + "Bottom"], 0, scale)) !== null) r.b = v;
        if ((v = px(cs[name + "Left"], 0, scale)) !== null) r.l = v;
        void cap;
        return r;
    }

    function border(cs, scale) {
        var b = { width: 0, color: "#000000", style: "none" };
        var v = cs.border || cs.borderTop;
        if (v) {
            var parts = String(v).trim().split(/\s+/);
            for (var i = 0; i < parts.length; i++) {
                var p = parts[i];
                if (/^[\d.]+(px|em)?$/.test(p)) b.width = px(p, 0, scale) || 0;
                else if (p === "solid" || p === "none" || p === "dashed" || p === "dotted" || p === "double") b.style = p;
                else b.color = p;
            }
            if (b.style === "none") b.width = 0;
        }
        var w = px(cs.borderWidth, 0, scale);
        if (w !== null) b.width = w;
        if (cs.borderColor) b.color = cs.borderColor;
        if (cs.borderStyle === "none") b.width = 0;
        return b;
    }

    function transformOf(cs) {
        var t = { tx: 0, ty: 0, scale: 1 };
        var s = cs.transform || "";
        var m;
        var re = /(translate|scale|translateX|translateY)\(([^)]*)\)/g;
        while ((m = re.exec(s))) {
            var args = m[2].split(",").map(function(a) { return a.trim(); });
            if (m[1] === "scale") {
                t.scale *= parseFloat(args[0]) || 1;
            } else if (m[1] === "translate") {
                t.tx += parsePct(args[0]); t.ty += parsePct(args[1] || "0");
            } else if (m[1] === "translateX") { t.tx += parsePct(args[0]); }
            else if (m[1] === "translateY") { t.ty += parsePct(args[0]); }
        }
        return t;
    }

    /* Percent → fraction of own size; px → {px: n}. */
    function parsePct(v) {
        v = String(v || "0").trim();
        if (v.slice(-1) === "%") return parseFloat(v) / 100;
        var n = parseFloat(v);
        return isNaN(n) ? 0 : { px: n };
    }

    function isHidden(el) {
        if (el.hidden) return true;
        var d = el.style && el.style.display;
        if (d === "none") return true;
        if (el.style && el.style.visibility === "hidden") return true;
        /* RPG Maker MV hides its renderer-mode label ("Canvas mode") with opacity 0 */
        var o = el.style && el.style.opacity;
        if (o !== undefined && o !== null && o !== "" && parseFloat(o) === 0) return true;
        return false;
    }

    /* --- Layout ---------------------------------------------------------- */

    var DEFAULT_FONT_SIZE = 16;

    function fontOf(cs, inherited, scale) {
        var size = px(cs.fontSize, inherited.size, scale);
        return {
            size: size !== null ? size : inherited.size,
            family: cs.fontFamily || inherited.family,
            weight: cs.fontWeight || inherited.weight,
            color: cs.color || inherited.color
        };
    }

    function fontString(f) {
        var family = String(f.family || "sans-serif").replace(/['"]/g, "");
        return (f.weight && f.weight !== "normal" ? f.weight + " " : "") + Math.round(f.size) + "px " + family;
    }

    function measureText(text, f) {
        var ctx = overlay.measureCtx;
        ctx.font = fontString(f);
        return ctx.measureText(text).width;
    }

    function textOf(el) {
        if (el.tagName === "INPUT" || el.tagName === "TEXTAREA") return String(el.value === undefined ? "" : el.value);
        if (el.tagName === "SELECT") return String(el.value === undefined ? "" : el.value);
        var t = el.textContent;
        if (!t && el.childNodes) {
            t = "";
            for (var i = 0; i < el.childNodes.length; i++) {
                if (el.childNodes[i].nodeType === 3) t += el.childNodes[i].textContent;
            }
        }
        return t ? String(t) : "";
    }

    /* Wrap text into lines that fit width (words for Latin, characters for CJK). */
    function wrapText(text, f, maxWidth) {
        var lines = [];
        var paragraphs = text.split("\n");
        for (var p = 0; p < paragraphs.length; p++) {
            var words = paragraphs[p].split(/(\s+)/);
            var line = "";
            for (var w = 0; w < words.length; w++) {
                var word = words[w];
                if (!word) continue;
                var candidate = line + word;
                if (maxWidth === null || measureText(candidate, f) <= maxWidth || line === "") {
                    if (maxWidth !== null && measureText(candidate, f) > maxWidth && line === "") {
                        /* single word too long: break by characters */
                        var chunk = "";
                        for (var c = 0; c < word.length; c++) {
                            if (measureText(chunk + word[c], f) > maxWidth && chunk) {
                                lines.push(chunk);
                                chunk = "";
                            }
                            chunk += word[c];
                        }
                        line = chunk;
                    } else {
                        line = candidate;
                    }
                } else {
                    lines.push(line.replace(/\s+$/, ""));
                    line = /^\s+$/.test(word) ? "" : word;
                }
            }
            lines.push(line.replace(/\s+$/, ""));
        }
        return lines;
    }

    /* Build a layout tree: each node {el, cs, x, y, w, h (border box), pad,
       bw, font, lines, children}. `availW` is the containing block's content
       width; `shrink` requests shrink-to-fit width (flex items, inline). */
    function buildNode(el, rules, inherited, availW, shrink, scale) {
        if (isHidden(el)) return null;
        var cs = computeStyle(el, rules);
        var tag = el.tagName;
        var f = fontOf(cs, inherited, scale);
        if (el.id && /^errorprinter$/i.test(el.id)) f.color = "#ffffff";   /* the engines colour the message with <font> tags */
        var pad = sides(cs, "padding", scale);
        var mar = sides(cs, "margin", scale);
        var bd = border(cs, scale);
        var isControl = tag === "INPUT" || tag === "BUTTON" || tag === "TEXTAREA" || tag === "SELECT";
        if (isControl && !cs.padding && !cs.paddingLeft) {
            pad = { t: 1 * scale, r: 6 * scale, b: 1 * scale, l: 6 * scale };
            if (tag === "INPUT" || tag === "TEXTAREA") pad = { t: 1 * scale, r: 2 * scale, b: 1 * scale, l: 2 * scale };
        }
        if (isControl && !cs.border && !cs.borderWidth) bd = { width: 2 * scale, color: "#767676", style: "solid" };
        var boxSizing = cs.boxSizing || (isControl ? "border-box" : "content-box");
        var extraW = pad.l + pad.r + 2 * bd.width;
        var extraH = pad.t + pad.b + 2 * bd.width;
        var innerAvail = availW === null ? null : Math.max(0, availW - mar.l - mar.r);

        var node = { el: el, cs: cs, tag: tag, pad: pad, mar: mar, bd: bd, font: f, children: [],
                     lines: null, x: 0, y: 0, w: 0, h: 0,
                     display: cs.display || defaultDisplay(tag) };

        /* Width */
        var explicitW = px(cs.width, innerAvail, scale);
        var contentW;
        if (explicitW !== null) {
            contentW = boxSizing === "border-box" ? explicitW - extraW : explicitW;
        } else if (isControl) {
            contentW = null;                             /* from content, below */
        } else if (!shrink && innerAvail !== null) {
            contentW = innerAvail - extraW;               /* block fills the line */
        } else {
            contentW = null;
        }
        if (contentW !== null && contentW < 0) contentW = 0;

        /* Content */
        var textContentW = 0, textContentH = 0;
        var text = textOf(el);
        var hasElementChildren = el.children && el.children.some(function(c) { return c.nodeType === 1 && c.tagName !== "STYLE"; });
        /* the engines' error printer holds the message text and the Retry button; the text is what matters */
        if (hasElementChildren && text && el.id && /^errorprinter$/i.test(el.id)) hasElementChildren = false;
        var lineHeight = f.size * 1.25;
        if (tag === "INPUT" || tag === "SELECT") {
            node.lines = [text];
            textContentW = explicitW !== null ? contentW : Math.max(measureText(text, f), 150 * scale);
            var hh = px(cs.height, null, scale);
            textContentH = hh !== null ? (boxSizing === "border-box" ? hh - extraH : hh) : lineHeight;
        } else if (!hasElementChildren) {
            var maxW = contentW !== null ? contentW : (innerAvail !== null ? innerAvail - extraW : null);
            node.lines = text ? wrapText(text, f, maxW && maxW > 0 ? maxW : null) : [];
            var widest = 0;
            for (var i = 0; i < node.lines.length; i++) widest = Math.max(widest, measureText(node.lines[i], f));
            textContentW = widest;
            textContentH = node.lines.length * lineHeight;
        }

        var finalContentW = contentW !== null ? contentW : textContentW;

        /* Children */
        var childrenH = 0, childrenW = 0;
        if (hasElementChildren) {
            var flex = node.display === "flex" || node.display === "inline-flex";
            var row = flex && (cs.flexDirection === "row" || !cs.flexDirection);
            var kids = [];
            for (var c = 0; c < el.children.length; c++) {
                var ch = el.children[c];
                if (ch.nodeType !== 1 || ch.tagName === "STYLE" || ch.tagName === "SCRIPT") continue;
                var childShrink = flex ? (row || (cs.alignItems && cs.alignItems !== "stretch" && cs.alignItems !== "normal")) : false;
                var childAvail = contentW !== null ? contentW : innerAvail === null ? null : innerAvail - extraW;
                var kid = buildNode(ch, rules, f, childAvail, childShrink, scale);
                if (kid) kids.push(kid);
            }
            node.children = kids;
            if (flex && row) {
                var totalW = 0, maxH = 0;
                kids.forEach(function(k) { totalW += k.w + k.mar.l + k.mar.r; maxH = Math.max(maxH, k.h + k.mar.t + k.mar.b); });
                childrenW = totalW; childrenH = maxH;
            } else {
                var totalH = 0, maxW2 = 0;
                kids.forEach(function(k) { totalH += k.h + k.mar.t + k.mar.b; maxW2 = Math.max(maxW2, k.w + k.mar.l + k.mar.r); });
                childrenW = maxW2; childrenH = totalH;
            }
            if (contentW === null) finalContentW = childrenW;
        }

        /* Height */
        var explicitH = px(cs.height, null, scale);
        var contentH;
        if (explicitH !== null) contentH = boxSizing === "border-box" ? explicitH - extraH : explicitH;
        else contentH = hasElementChildren ? childrenH : textContentH;
        if (contentH < 0) contentH = 0;

        node.contentW = finalContentW;
        node.contentH = contentH;
        node.w = finalContentW + extraW;
        node.h = contentH + extraH;

        /* Position children inside the content box */
        if (node.children.length) {
            positionChildren(node, cs);
        }
        return node;
    }

    function defaultDisplay(tag) {
        if (tag === "SPAN" || tag === "LABEL" || tag === "INPUT" || tag === "BUTTON" || tag === "SELECT") return "inline-block";
        return "block";
    }

    function positionChildren(node, cs) {
        var kids = node.children;
        var flex = node.display === "flex" || node.display === "inline-flex";
        var row = flex && (cs.flexDirection === "row" || !cs.flexDirection);
        var justify = cs.justifyContent || "flex-start";
        var align = cs.alignItems || "stretch";
        var x0 = node.bd.width + node.pad.l, y0 = node.bd.width + node.pad.t;
        var W = node.contentW, H = node.contentH;
        var i, k;
        if (flex && row) {
            var used = 0;
            kids.forEach(function(k2) { used += k2.w + k2.mar.l + k2.mar.r; });
            var free = Math.max(0, W - used);
            var gap = 0, start = 0;
            if (justify === "space-around") { gap = free / kids.length; start = gap / 2; }
            else if (justify === "space-between") { gap = kids.length > 1 ? free / (kids.length - 1) : 0; }
            else if (justify === "space-evenly") { gap = free / (kids.length + 1); start = gap; }
            else if (justify === "center") start = free / 2;
            else if (justify === "flex-end") start = free;
            var x = x0 + start;
            for (i = 0; i < kids.length; i++) {
                k = kids[i];
                k.x = x + k.mar.l;
                if (align === "center") k.y = y0 + (H - k.h) / 2;
                else if (align === "flex-end") k.y = y0 + H - k.h - k.mar.b;
                else k.y = y0 + k.mar.t;
                x += k.w + k.mar.l + k.mar.r + gap;
            }
        } else {
            var usedH = 0;
            kids.forEach(function(k2) { usedH += k2.h + k2.mar.t + k2.mar.b; });
            var freeH = Math.max(0, H - usedH);
            var gapV = 0, startV = 0;
            if (flex) {
                if (justify === "space-around") { gapV = freeH / kids.length; startV = gapV / 2; }
                else if (justify === "space-between") { gapV = kids.length > 1 ? freeH / (kids.length - 1) : 0; }
                else if (justify === "space-evenly") { gapV = freeH / (kids.length + 1); startV = gapV; }
                else if (justify === "center") startV = freeH / 2;
                else if (justify === "flex-end") startV = freeH;
            }
            var y = y0 + startV;
            for (i = 0; i < kids.length; i++) {
                k = kids[i];
                k.y = y + k.mar.t;
                if (flex && align === "center") k.x = x0 + (W - k.w) / 2;
                else if (flex && align === "flex-end") k.x = x0 + W - k.w - k.mar.r;
                else if (!flex && (k.display === "inline-block") && k.cs.margin === "auto") k.x = x0 + (W - k.w) / 2;
                else k.x = x0 + k.mar.l;
                y += k.h + k.mar.t + k.mar.b + gapV;
            }
        }
    }

    /* Absolute placement of a root on the screen (game pixels). */
    function placeRoot(node, screenW, screenH) {
        var cs = node.cs;
        var t = transformOf(cs);
        var left = px(cs.left, screenW, 1), top = px(cs.top, screenH, 1);
        var x = left !== null ? left : (screenW - node.w) / 2;
        var y = top !== null ? top : (screenH - node.h) / 2;
        if (typeof t.tx === "number") x += t.tx * node.w; else x += t.tx.px;
        if (typeof t.ty === "number") y += t.ty * node.h; else y += t.ty.px;
        node.screenX = Math.round(x);
        node.screenY = Math.round(y);
    }

    /* --- Rendering ------------------------------------------------------- */

    function ensureCanvases() {
        if (!overlay.measureCanvas) {
            overlay.measureCanvas = document.createElement("canvas");
            overlay.measureCanvas.width = 8;
            overlay.measureCanvas.height = 8;
            overlay.measureCtx = overlay.measureCanvas.getContext("2d");
            overlay.canvas = document.createElement("canvas");
            overlay.ctx = overlay.canvas.getContext("2d");
        }
        return !!(overlay.measureCtx && overlay.ctx);
    }

    function roundRectPath(ctx, x, y, w, h, r) {
        r = Math.max(0, Math.min(r, w / 2, h / 2));
        ctx.beginPath();
        if (r <= 0) {
            ctx.rect(x, y, w, h);
            return;
        }
        ctx.moveTo(x + r, y);
        ctx.lineTo(x + w - r, y);
        ctx.arcTo(x + w, y, x + w, y + r, r);
        ctx.lineTo(x + w, y + h - r);
        ctx.arcTo(x + w, y + h, x + w - r, y + h, r);
        ctx.lineTo(x + r, y + h);
        ctx.arcTo(x, y + h, x, y + h - r, r);
        ctx.lineTo(x, y + r);
        ctx.arcTo(x, y, x + r, y, r);
        ctx.closePath();
    }

    function drawNode(ctx, node, ox, oy, scale) {
        var x = ox + node.x, y = oy + node.y;
        var cs = node.cs;
        var radius = px(cs.borderRadius, 0, scale) || 0;
        var bg = cs.backgroundColor || cs.background;
        var isControl = node.tag === "INPUT" || node.tag === "BUTTON" || node.tag === "TEXTAREA" || node.tag === "SELECT";
        if (!bg && isControl) bg = node.tag === "BUTTON" ? "#efefef" : "#ffffff";
        if (bg && bg !== "transparent" && bg !== "none") {
            roundRectPath(ctx, x, y, node.w, node.h, radius);
            ctx.fillStyle = bg;
            ctx.fill();
        }
        if (node.bd.width > 0) {
            var hw = node.bd.width / 2;
            roundRectPath(ctx, x + hw, y + hw, node.w - node.bd.width, node.h - node.bd.width, Math.max(0, radius - hw));
            ctx.strokeStyle = node.bd.color;
            ctx.lineWidth = node.bd.width;
            ctx.stroke();
        }
        if (overlay.hover === node.el && node.tag === "BUTTON") {
            roundRectPath(ctx, x, y, node.w, node.h, radius);
            ctx.fillStyle = "rgba(0,0,0,0.08)";
            ctx.fill();
        }

        var cx = x + node.bd.width + node.pad.l, cy = y + node.bd.width + node.pad.t;
        if (node.lines && node.lines.length) {
            var f = node.font;
            ctx.font = fontString(f);
            ctx.fillStyle = f.color || "#000000";
            ctx.textBaseline = "middle";
            var lineHeight = f.size * 1.25;
            var align = cs.textAlign || (node.tag === "BUTTON" ? "center" : "left");
            var isText = node.tag === "INPUT" || node.tag === "TEXTAREA" || node.tag === "SELECT";
            var startY = isText || node.tag === "BUTTON" ? cy + (node.contentH - node.lines.length * lineHeight) / 2 : cy;
            for (var i = 0; i < node.lines.length; i++) {
                var line = node.lines[i];
                var tw = measureText(line, f);
                var tx = cx;
                if (align === "center") tx = cx + (node.contentW - tw) / 2;
                else if (align === "right") tx = cx + node.contentW - tw;
                var ty = startY + i * lineHeight + lineHeight / 2;
                if (isText && node.el.type === "password") line = new Array(line.length + 1).join("•");
                if (isText && node.el.placeholder && !line) {
                    ctx.fillStyle = "#757575";
                    ctx.fillText(String(node.el.placeholder), tx, ty);
                    ctx.fillStyle = f.color || "#000000";
                } else {
                    ctx.fillText(line, tx, ty);
                }
                /* Caret */
                if (isText && overlay.focused === node.el && i === node.lines.length - 1 &&
                    Math.floor(overlay.frame / 30) % 2 === 0) {
                    var pos = clampSel(node.el, node.el.selectionStart);
                    var caretX = tx + measureText(line.substring(0, pos), f);
                    ctx.fillRect(Math.round(caretX), Math.round(ty - f.size * 0.55), Math.max(1, Math.round(scale)), Math.round(f.size * 1.1));
                }
            }
        }
        for (var c = 0; c < node.children.length; c++) {
            drawNode(ctx, node.children[c], x, y, scale);
        }
    }

    var currentLayouts = [];

    function hitTest(sx, sy) {
        for (var r = currentLayouts.length - 1; r >= 0; r--) {
            var root = currentLayouts[r];
            var hit = hitNode(root, root.screenX, root.screenY, sx, sy);
            if (hit) return hit;
        }
        return null;
    }

    function hitNode(node, ox, oy, sx, sy) {
        var x = ox + node.x, y = oy + node.y;
        if (sx < x || sy < y || sx >= x + node.w || sy >= y + node.h) return null;
        for (var c = node.children.length - 1; c >= 0; c--) {
            var hit = hitNode(node.children[c], x, y, sx, sy);
            if (hit) return hit;
        }
        return node.el;
    }

    /* Lay out every root for a screen of the given size (also refreshes
       the boxes used for mouse hit-testing). */
    overlay.updateLayouts = function(screenW, screenH) {
        if (!ensureCanvases()) return [];
        var rules = styleRules();
        var inherited = { size: DEFAULT_FONT_SIZE, family: "sans-serif", weight: "normal", color: "#000000" };
        var layouts = [];
        for (var r = 0; r < overlay.roots.length; r++) {
            var el = overlay.roots[r];
            var cs0 = computeStyle(el, rules);
            var scale = transformOf(cs0).scale || 1;
            var node = buildNode(el, rules, { size: inherited.size * scale, family: inherited.family,
                                              weight: inherited.weight, color: inherited.color },
                                 screenW, cs0.position === "absolute" || cs0.position === "fixed", scale);
            if (!node) continue;
            placeRoot(node, screenW, screenH);
            node.scale = scale;
            layouts.push(node);
        }
        currentLayouts = layouts;
        return layouts;
    };

    overlay.render = function() {
        overlay.frame++;
        if (overlay.roots.length === 0) {
            currentLayouts = [];
            return;
        }
        if (typeof __native_renderer === "undefined" || typeof __native_canvas2d === "undefined") return;
        var size = __native_renderer.getSize();
        if (!size || !(size.width > 0) || !(size.height > 0)) return;

        var layouts = overlay.updateLayouts(size.width, size.height);
        if (!layouts.length) return;

        /* Draw all roots into one screen-sized canvas region covering them. */
        var minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
        layouts.forEach(function(n) {
            minX = Math.min(minX, n.screenX); minY = Math.min(minY, n.screenY);
            maxX = Math.max(maxX, n.screenX + n.w); maxY = Math.max(maxY, n.screenY + n.h);
        });
        minX = Math.max(0, Math.floor(minX)); minY = Math.max(0, Math.floor(minY));
        maxX = Math.min(size.width, Math.ceil(maxX)); maxY = Math.min(size.height, Math.ceil(maxY));
        var w = maxX - minX, h = maxY - minY;
        if (w <= 0 || h <= 0) return;

        var canvas = overlay.canvas, ctx = overlay.ctx;
        if (canvas.width !== w || canvas.height !== h) {
            canvas.width = w;
            canvas.height = h;
        }
        ctx.clearRect(0, 0, w, h);
        for (var i = 0; i < layouts.length; i++) {
            var n = layouts[i];
            drawNode(ctx, n, n.screenX - minX - n.x, n.screenY - minY - n.y, n.scale);
        }

        var handle = ctx._handle;
        if (!handle) return;
        var fastUpload = __native_renderer.updateTextureFromCanvas;
        var pixels = fastUpload ? null : __native_canvas2d.getPixels(handle);
        if (!fastUpload && !pixels) return;
        if (!overlay.texture || overlay.texW !== w || overlay.texH !== h) {
            if (overlay.texture) __native_renderer.deleteTexture(overlay.texture);
            overlay.texture = __native_renderer.createTexture(w, h);
            overlay.texW = w;
            overlay.texH = h;
        }
        if (!fastUpload || !fastUpload(overlay.texture, w, h, handle)) {
            if (!pixels) pixels = __native_canvas2d.getPixels(handle);
            if (!pixels) return;
            __native_renderer.updateTexture(overlay.texture, w, h, pixels);
        }
        __native_renderer.rebindBatch();
        __native_renderer.setBlendMode(0);
        __native_renderer.drawQuad(overlay.texture, minX, minY, w, h, 0, 0, 1, 1, 0xFFFFFF, 1);
        __native_renderer.endFrame();
    };

    /* Hook the frame: draw after every requestAnimationFrame callback ran,
       i.e. after the game's own render. */
    var flushOrig = globalThis.__dom_flushAnimationFrames;
    if (typeof flushOrig === "function") {
        globalThis.__dom_flushAnimationFrames = function(timestamp) {
            flushOrig(timestamp);
            try {
                overlay.render();
            } catch (e) {
                if (typeof console !== "undefined") console.error("dom_overlay: " + (e && e.stack || e));
            }
        };
    }

    /* Exposed for the DOM shim's input flush and for tests. */
    overlay.layout = function(el, screenW, screenH) {
        ensureCanvases();
        var rules = styleRules();
        var cs0 = computeStyle(el, rules);
        var scale = transformOf(cs0).scale || 1;
        var node = buildNode(el, rules, { size: DEFAULT_FONT_SIZE * scale, family: "sans-serif", weight: "normal", color: "#000000" },
                             screenW, cs0.position === "absolute" || cs0.position === "fixed", scale);
        if (node) placeRoot(node, screenW, screenH);
        return node;
    };
    overlay.isOverlayElement = isOverlayElement;
    overlay.fireBubbling = fireBubbling;

    globalThis.__dom_overlay = overlay;
})();
