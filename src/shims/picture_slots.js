/*
 * Idle picture slots cost nothing.
 *
 * RPG Maker creates one Sprite_Picture for every picture slot the game allows (Game_Screen.maxPictures(), 100 by
 * default) and updates, transforms and draws all of them every frame; plugins that react to touches loop over them as
 * well. Some games raise the limit to 10000 with a plugin and use only a few slots. A browser's compiled JavaScript
 * copes with ten thousand idle sprites; this interpreter needs half a second per frame for them.
 *
 * With many slots, the work that is done for every picture sprite each frame (the update, the transform pass, drawing,
 * and the touch plugin's loop) is only done for the sprites that have something to show: a picture in their slot, or a
 * picture that was just erased and still has to disappear. Nothing is removed or reordered: container.children keeps
 * all sprites, with their usual index (a plugin may sort them by depth; then the order it chose is kept), so scripts that
 * look a picture sprite up by number still work.
 *
 * Which sprites are in use is followed through Game_Screen.showPicture (new pictures) and the sprites themselves (a
 * sprite stays on the list until it has cleared itself); a complete look at all slots every second catches anything
 * a script put in place some other way.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
(function() {
    "use strict";

    if (typeof Spriteset_Base === "undefined" || typeof Sprite_Picture === "undefined" ||
        typeof Game_Screen === "undefined" || !Spriteset_Base.prototype.createPictures) return;

    var MANY = 300;           /* up to this many slots the normal code is cheap enough and stays as it is */
    var RESCAN_FRAMES = 60;   /* every this many frames all slots are looked at, whatever was reported */

    /* pictures shown since the lists were last made (picture numbers as the event commands give them) */
    var showPicture = Game_Screen.prototype.showPicture;
    Game_Screen.prototype.showPicture = function(pictureId) {
        var result = showPicture.apply(this, arguments);
        if (!this._rmNewPictures) this._rmNewPictures = [];
        this._rmNewPictures.push(pictureId);
        return result;
    };

    function byRank(a, b) { return a._rmRank - b._rmRank; }

    /* remembers where every sprite stands in container.children (its draw order) */
    function numberChildren(container) {
        var kids = container.children, n = kids.length;
        for (var i = 0; i < n; i++) kids[i]._rmRank = i;
        container._sig0 = kids[0];
        container._sigM = kids[n >> 1];
        container._sigN = kids[n - 1];
        container._ranksDirty = false;
    }

    /* the sprites of the container that need work this frame, in children order (made once per frame) */
    function activeSprites(container) {
        var frame = (typeof Graphics !== "undefined" && Graphics.frameCount !== undefined) ? Graphics.frameCount : -1;
        if (container._activeFrame === frame && container._active) return container._active;

        var count = container._count, byId = container._spriteById;
        var kids = container.children;
        var screen = $gameScreen;
        var pics = screen._pictures;
        var offset = ($gameParty && $gameParty.inBattle()) ? screen.maxPictures() : 0;
        var fresh = screen._rmNewPictures;
        screen._rmNewPictures = null;
        var mark = container._mark = (container._mark || 0) + 1;
        var out = [], i, sprite, id;

        var sameSet = kids.length === count;       /* nothing added or removed by a script */
        var rescan = !container._active || screen !== container._screen || (frame >= 0 && frame % RESCAN_FRAMES === 0);
        if (sameSet && (container._ranksDirty || kids[0] !== container._sig0 || kids[count >> 1] !== container._sigM ||
                        kids[count - 1] !== container._sigN)) {
            numberChildren(container);             /* a plugin sorted the sprites (by depth, usually) */
            rescan = true;
        }

        if (!sameSet) {
            /* children were added or removed by a script: look at every child */
            for (i = 0; i < kids.length; i++) {
                sprite = kids[i];
                if (!sprite._isPicture || pics[sprite._pictureId + offset] || sprite._bitmap || sprite._pictureName) out.push(sprite);
            }
        } else if (rescan) {
            for (id = 1; id < byId.length; id++) {
                if (pics[id + offset]) {
                    sprite = byId[id];
                    sprite._rmMark = mark;
                    out.push(sprite);
                }
            }
            var previous = container._active || [];
            for (i = 0; i < previous.length; i++) {
                sprite = previous[i];
                if (sprite._rmMark !== mark && sprite._isPicture && (sprite._bitmap || sprite._pictureName)) {
                    sprite._rmMark = mark;
                    out.push(sprite);
                }
            }
            /* the numbers must still match the real order (this catches sorts that were not noticed) */
            for (i = 0; i < out.length; i++) {
                if (kids[out[i]._rmRank] !== out[i]) { numberChildren(container); break; }
            }
            out.sort(byRank);
        } else {
            var candidates = container._active;
            var added = false;
            for (i = 0; i < candidates.length; i++) {
                sprite = candidates[i];
                if (sprite._rmMark !== mark && (!sprite._isPicture || pics[sprite._pictureId + offset] || sprite._bitmap ||
                                                sprite._pictureName)) {
                    sprite._rmMark = mark;
                    out.push(sprite);
                }
            }
            if (fresh) {
                for (i = 0; i < fresh.length; i++) {
                    sprite = byId[fresh[i]];
                    if (sprite && sprite._rmMark !== mark) {
                        sprite._rmMark = mark;
                        out.push(sprite);
                        added = true;
                    }
                }
            }
            if (added) out.sort(byRank);
        }
        container._screen = screen;
        container._active = out;
        container._activeFrame = frame;
        return out;
    }

    var createPictures = Spriteset_Base.prototype.createPictures;
    Spriteset_Base.prototype.createPictures = function() {
        createPictures.apply(this, arguments);
        var container = this._pictureContainer;
        if (!container || !container.children || container.children.length <= MANY) return;

        /* sprite number n must be the n-th child (it is, as created); otherwise nothing is changed */
        var byId = [null];
        var kids = container.children;
        for (var i = 0; i < kids.length; i++) {
            if (kids[i]._pictureId !== i + 1) return;
            byId.push(kids[i]);
        }
        container._count = kids.length;
        container._spriteById = byId;
        container._manyPictures = true;
        numberChildren(container);

        container.update = function() {
            var act = activeSprites(this);
            for (var i = 0; i < act.length; i++) {
                if (act[i].update) act[i].update();
            }
        };

        /* the transform pass and drawing look at the sprites in the list only; the full list is put back afterwards */
        var transform = container.updateTransform;
        container.updateTransform = function() {
            var everything = this.children;
            this.children = activeSprites(this);
            try { return transform.apply(this, arguments); } finally { this.children = everything; }
        };
        var render = container.render;
        container.render = function() {
            var everything = this.children;
            this.children = activeSprites(this);
            try { return render.apply(this, arguments); } finally { this.children = everything; }
        };
    };

    /* a plugin that sorts the picture sprites says so: the order has to be taken over */
    var sortPictures = Spriteset_Base.prototype.sortPictures;
    if (typeof sortPictures === "function") {
        Spriteset_Base.prototype.sortPictures = function() {
            var result = sortPictures.apply(this, arguments);
            if (this._pictureContainer) this._pictureContainer._ranksDirty = true;
            return result;
        };
    }

    /* the touch plugin loops over all picture sprites every frame; idle ones have nothing to report */
    var iterateSub = Spriteset_Base.prototype._iteratePicturesSub;
    if (typeof iterateSub === "function") {
        Spriteset_Base.prototype._iteratePicturesSub = function(children, callback) {
            var container = this._pictureContainer;
            if (container && container._manyPictures && children === container.children) {
                var act = activeSprites(container);
                for (var i = act.length - 1; i >= 0; i--) {
                    if (!callback(act[i])) break;
                }
                return;
            }
            return iterateSub.apply(this, arguments);
        };
    }
})();

/*
 * Game_Interpreter.setup() (every event or common event that starts) calls Game_Interpreter.requestImages(list), which
 * walks every command of the list and of all common events it calls, only to preload pictures. Games that start
 * common events all the time (a card game's interface) spend most of their frame on that walk in this interpreter.
 * The preloading only saves a little waiting later, so a list that was walked within the last minute is not walked again.
 */
(function() {
    "use strict";
    if (typeof Game_Interpreter === "undefined" || typeof Game_Interpreter.requestImages !== "function" ||
        typeof WeakMap === "undefined") return;
    var requestImages = Game_Interpreter.requestImages;
    var walked = new WeakMap();
    Game_Interpreter.requestImages = function(list, commonList) {
        if (list && commonList === undefined) {          /* the call that starts a walk (not the nested ones) */
            var now = (typeof Graphics !== "undefined" && Graphics.frameCount) || 0;
            var last = walked.get(list);
            if (last !== undefined && now - last < 3600) return;
            walked.set(list, now);
        }
        return requestImages.apply(this, arguments);
    };
})();
