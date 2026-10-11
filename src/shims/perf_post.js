/*
 * Faster versions of a few functions of the RPG Maker MZ core that games call very often, installed after the plugins
 * have loaded. Each one replaces a function only if it is still exactly the core's own code (a plugin that changed or
 * wrapped it is left alone), and gives the same results.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the PS5 port contributors
 */
(function() {
    "use strict";

    var applied = [];

    /* The core's own functions are recognised by a fingerprint of their text (FNV-1a over the text without white space),
       so that this file does not have to carry that code. */
    function fingerprint(text) {
        var h = 0x811c9dc5;
        for (var i = 0; i < text.length; i++) {
            h ^= text.charCodeAt(i);
            h = Math.imul(h, 0x01000193) >>> 0;
        }
        return h;
    }

    function same(fn, hash) {
        if (typeof fn !== "function") return false;
        return fingerprint(Function.prototype.toString.call(fn).replace(/\s+/g, "")) === hash;
    }

    /* String.prototype.format: "%1 of %2".format(a, b). The core does it with a regular expression and a callback;
       scanning the string by hand is several times faster in this interpreter. Message plugins wrap the core's
       function (VisuMZ keeps the original as VisuMZ.MessageCore.String_format), so that one is replaced too. */
    function fastFormat() {
        var s = String(this), a = arguments;
        var i = s.indexOf("%");
        if (i < 0) return s;
        var n = s.length, out = "", last = 0;
        while (i >= 0) {
            var j = i + 1;
            while (j < n) {
                var c = s.charCodeAt(j);
                if (c < 48 || c > 57) break;
                j++;
            }
            if (j === i + 1) {
                i = s.indexOf("%", i + 1);
                continue;
            }
            out += s.substring(last, i) + a[Number(s.substring(i + 1, j)) - 1];
            last = j;
            i = s.indexOf("%", last);
        }
        return out + s.substring(last);
    }

    try {
        if (same(String.prototype.format, 0x17eafabf)) {
            String.prototype.format = fastFormat;
            applied.push("format");
        }
        var mc = typeof VisuMZ !== "undefined" && VisuMZ.MessageCore;
        if (mc && same(mc.String_format, 0x17eafabf)) {
            mc.String_format = fastFormat;
            applied.push("MessageCore.String_format");
        }
    } catch (e) {}

    /* Game_Map.checkLayeredTilesFlags: is one of the four tiles at (x, y) flagged? The core builds an array of the four
       tile ids and runs some() over it, for every character that asks whether it stands in a bush. */
    try {
        if (typeof Game_Map !== "undefined" &&
            same(Game_Map.prototype.tileId, 0xd6d9f7a3) &&
            same(Game_Map.prototype.layeredTiles, 0x2ee6871f) &&
            same(Game_Map.prototype.checkLayeredTilesFlags, 0x675d33ae)) {
            Game_Map.prototype.checkLayeredTilesFlags = function(x, y, bit) {
                var flags = this.tilesetFlags();
                var width = $dataMap.width, height = $dataMap.height, data = $dataMap.data;
                var i = y * width + x, layer = width * height;
                for (var z = 3; z >= 0; z--) {
                    if ((flags[data[z * layer + i] || 0] & bit) !== 0) return true;
                }
                return false;
            };
            applied.push("checkLayeredTilesFlags");
        }
    } catch (e) {}

    /* VisuMZ_2_WeatherEffects can pre-generate the images of about 70 weather kinds when the game starts (an option some
       games switch on): 16 canvases each, up to 1920x1920 pixels, 2.2 GB in all (Domina REMAKE). A weather's images are
       also made when it is first shown, so the preloading is skipped: the console cannot hold that much memory. */
    try {
        if (typeof ImageManager !== "undefined" && typeof ImageManager.prepareGeneratedWeatherImages === "function") {
            ImageManager.prepareGeneratedWeatherImages = function() {};
            applied.push("prepareGeneratedWeatherImages");
        }
    } catch (e) {}

    globalThis.__perfPostApplied = applied;
})();
