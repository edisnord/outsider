/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the PS5 port contributors */
/*
 * Plugin de-obfuscation: many plugins (all of VisuMZ) are run through obfuscator.io, which turns every property
 * name and string into a call of a "string array" decoder, e.g. this[_0x3158c1(0x3cd)](x). The decoder is a pure
 * lookup, but calling it for every property access makes such plugins several times slower in an interpreter.
 *
 * The native loader evaluates the plugin once in a scratch context (its own globals, so nothing leaks), which
 * leaves the plugin's decoder function defined, and then calls __deobf(source) there. The result is the source with
 * every decoder call that has literal arguments replaced by the string it returns. Calls that cannot be resolved
 * safely are left alone, so the worst case is the plugin running as slowly as before.
 */
(function () {
    const stringify = JSON.stringify;
    const MapC = Map;
    const NumberC = Number;
    const H = "_0x[0-9a-fA-F]+";

    globalThis.__deobf_count = 0;
    globalThis.__deobf = function (src) {
        // 1. the decoders: global functions with an obfuscated name (function declarations are hoisted, so they
        //    exist even when the plugin body failed in this bare context)
        const decoders = new MapC();
        const declRe = new RegExp("function\\s+(" + H + ")\\s*\\(", "g");
        let m;
        while ((m = declRe.exec(src)) !== null) {
            const f = globalThis[m[1]];
            if (typeof f === "function" && !decoders.has(m[1])) decoders.set(m[1], f);
        }
        if (decoders.size === 0) return undefined;

        // 2. aliases: const _0x5bf39b=_0x2e74;  and, inside functions,  const _0x3158c1=_0x5bf39b;
        const target = new MapC();
        for (const n of decoders.keys()) target.set(n, n);
        const pairs = [];
        const aliasRe = new RegExp("(?:const|let|var|,)\\s*(" + H + ")\\s*=\\s*(" + H + ")\\s*(?=[,;)])", "g");
        while ((m = aliasRe.exec(src)) !== null) pairs.push([m[1], m[2]]);
        const conflicts = new MapC();
        for (let round = 0; round < 8; round++) {
            let changed = false;
            for (let i = 0; i < pairs.length; i++) {
                const a = pairs[i][0], t = target.get(pairs[i][1]);
                if (t === undefined) continue;
                const cur = target.get(a);
                if (cur === undefined) { target.set(a, t); changed = true; }
                else if (cur !== t) conflicts.set(a, true);
            }
            if (!changed) break;
        }
        for (const a of conflicts.keys()) target.delete(a);

        // 3. the array shuffling function at the top runs the decoder while the array is still being rotated, so
        //    its calls have to stay
        const skip = [];
        const shufRe = new RegExp("\\(function\\(\\s*(" + H + ")\\s*,\\s*(" + H + ")\\s*\\)\\s*\\{\\s*(?:const|let|var)\\s+"
            + H + "\\s*=\\s*" + H + "\\s*,\\s*" + H + "\\s*=\\s*\\1\\(\\)\\s*;\\s*while\\s*\\(\\s*!!\\[\\]\\s*\\)", "g");
        const endRe = new RegExp("\\}\\(\\s*" + H + "\\s*,\\s*(?:0x[0-9a-fA-F]+|\\d+)\\s*\\)\\s*\\)\\s*;?", "g");
        while ((m = shufRe.exec(src)) !== null) {
            endRe.lastIndex = m.index + m[0].length;
            const e = endRe.exec(src);
            if (!e) return undefined;                       // cannot tell where it ends: leave the plugin alone
            skip.push(m.index, e.index + e[0].length);
            shufRe.lastIndex = e.index + e[0].length;
        }

        // 4. replace the calls
        let count = 0;
        const callRe = new RegExp("(?<![.\\w$])(" + H + ")\\(\\s*(0x[0-9a-fA-F]+|\\d+)\\s*(?:,\\s*('[^'\\\\\\n]*'|\"[^\"\\\\\\n]*\")\\s*)?\\)", "g");
        const out = src.replace(callRe, function (all, name, idx, key, off) {
            const d = target.get(name);
            if (d === undefined) return all;
            for (let i = 0; i < skip.length; i += 2) if (off >= skip[i] && off < skip[i + 1]) return all;
            let v;
            try {
                const fn = decoders.get(d);
                v = key === undefined ? fn(NumberC(idx)) : fn(NumberC(idx), key.slice(1, -1));
            } catch (e) {
                return all;
            }
            if (typeof v !== "string") return all;
            count++;
            return stringify(v);
        });
        globalThis.__deobf_count = count;
        return count > 0 ? out : undefined;
    };
})();
