/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Node.js pieces that RPG Maker MV/MZ games and their plugins use. On a PC those games run in NW.js, which has Node
 * built in; here there is no Node, so this file provides the parts plugins commonly need:
 *
 *   Buffer, TextEncoder/TextDecoder, more of fs (binary files, stat, callbacks, fs.promises, file descriptors),
 *   more of path, os, events, util, buffer, string_decoder, zlib (when the game ships pako), crypto.randomBytes,
 *   process details, setImmediate, a global "nw", and a CommonJS require() for the game's own .js / .json files.
 *
 * Modules that cannot work on the console (child_process, http, net, ...) can be required, but calling them throws a
 * clear error, so that a plugin that only mentions them still loads.
 *
 * Built on the storage shim's require("fs") / require("path") and the native file functions in __native_io.
 */
(function() {
    "use strict";
    var G = globalThis;

    /* ------------------------------------------------------------------ text encodings */

    function utf8Encode(str) {
        var out = [];
        for (var i = 0; i < str.length; i++) {
            var c = str.charCodeAt(i);
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < str.length) {
                var d = str.charCodeAt(i + 1);
                if (d >= 0xDC00 && d <= 0xDFFF) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                    i++;
                } else {
                    c = 0xFFFD;
                }
            } else if (c >= 0xD800 && c <= 0xDFFF) {
                c = 0xFFFD;
            }
            if (c < 0x80) {
                out.push(c);
            } else if (c < 0x800) {
                out.push(0xC0 | (c >> 6), 0x80 | (c & 63));
            } else if (c < 0x10000) {
                out.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
            } else {
                out.push(0xF0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
            }
        }
        return new Uint8Array(out);
    }

    function utf8Decode(b, start, end) {
        var parts = [], chunk = [];
        var i = start;
        while (i < end) {
            var c = b[i++];
            var cp;
            if (c < 0x80) {
                cp = c;
            } else if (c >= 0xC2 && c < 0xE0 && i < end && (b[i] & 0xC0) === 0x80) {
                cp = ((c & 31) << 6) | (b[i++] & 63);
            } else if (c >= 0xE0 && c < 0xF0 && i + 1 < end && (b[i] & 0xC0) === 0x80 && (b[i + 1] & 0xC0) === 0x80) {
                cp = ((c & 15) << 12) | ((b[i] & 63) << 6) | (b[i + 1] & 63);
                i += 2;
                if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
            } else if (c >= 0xF0 && c < 0xF5 && i + 2 < end && (b[i] & 0xC0) === 0x80 && (b[i + 1] & 0xC0) === 0x80 &&
                       (b[i + 2] & 0xC0) === 0x80) {
                cp = ((c & 7) << 18) | ((b[i] & 63) << 12) | ((b[i + 1] & 63) << 6) | (b[i + 2] & 63);
                i += 3;
                if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
            } else {
                cp = 0xFFFD;
            }
            if (cp > 0xFFFF) {
                cp -= 0x10000;
                chunk.push(0xD800 + (cp >> 10), 0xDC00 + (cp & 1023));
            } else {
                chunk.push(cp);
            }
            if (chunk.length >= 8192) {
                parts.push(String.fromCharCode.apply(null, chunk));
                chunk = [];
            }
        }
        parts.push(String.fromCharCode.apply(null, chunk));
        return parts.join("");
    }

    var B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    var B64_INDEX = (function() {
        var t = new Int16Array(256).fill(-1);
        for (var i = 0; i < 64; i++) t[B64.charCodeAt(i)] = i;
        t["-".charCodeAt(0)] = 62; /* base64url */
        t["_".charCodeAt(0)] = 63;
        return t;
    })();

    function base64Encode(b, start, end, url) {
        var out = [], i;
        for (i = start; i + 2 < end; i += 3) {
            var n = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
            out.push(B64[n >> 18], B64[(n >> 12) & 63], B64[(n >> 6) & 63], B64[n & 63]);
        }
        if (end - i === 1) {
            var n1 = b[i] << 16;
            out.push(B64[n1 >> 18], B64[(n1 >> 12) & 63]);
            if (!url) out.push("==");
        } else if (end - i === 2) {
            var n2 = (b[i] << 16) | (b[i + 1] << 8);
            out.push(B64[n2 >> 18], B64[(n2 >> 12) & 63], B64[(n2 >> 6) & 63]);
            if (!url) out.push("=");
        }
        var s = out.join("");
        return url ? s.replace(/\+/g, "-").replace(/\//g, "_") : s;
    }

    function base64Decode(str) {
        var out = [], acc = 0, bits = 0;
        for (var i = 0; i < str.length; i++) {
            var v = B64_INDEX[str.charCodeAt(i) & 255];
            if (v < 0) continue; /* padding, white space */
            acc = (acc << 6) | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push((acc >> bits) & 255);
            }
        }
        return new Uint8Array(out);
    }

    function normEncoding(enc) {
        if (enc === undefined || enc === null) return "utf8";
        var e = String(enc).toLowerCase();
        switch (e) {
        case "utf8": case "utf-8": return "utf8";
        case "ucs2": case "ucs-2": case "utf16le": case "utf-16le": return "utf16le";
        case "latin1": case "binary": return "latin1";
        case "ascii": return "ascii";
        case "base64": return "base64";
        case "base64url": return "base64url";
        case "hex": return "hex";
        }
        throw new TypeError("Unknown encoding: " + enc);
    }

    function encodeString(str, enc) {
        str = String(str);
        switch (normEncoding(enc)) {
        case "utf8": return utf8Encode(str);
        case "base64": case "base64url": return base64Decode(str);
        case "hex": {
            var n = str.length >> 1, h = new Uint8Array(n), k = 0;
            for (var i = 0; i < n; i++) {
                var v = parseInt(str.substr(i * 2, 2), 16);
                if (isNaN(v)) break;
                h[k++] = v;
            }
            return h.subarray(0, k);
        }
        case "utf16le": {
            var u = new Uint8Array(str.length * 2);
            for (var j = 0; j < str.length; j++) {
                var c = str.charCodeAt(j);
                u[j * 2] = c & 255;
                u[j * 2 + 1] = c >> 8;
            }
            return u;
        }
        default: { /* latin1, ascii */
            var l = new Uint8Array(str.length);
            for (var m = 0; m < str.length; m++) l[m] = str.charCodeAt(m) & 255;
            return l;
        }
        }
    }

    function decodeBytes(b, enc, start, end) {
        switch (normEncoding(enc)) {
        case "utf8": return utf8Decode(b, start, end);
        case "base64": return base64Encode(b, start, end, false);
        case "base64url": return base64Encode(b, start, end, true);
        case "hex": {
            var s = [];
            for (var i = start; i < end; i++) s.push((b[i] < 16 ? "0" : "") + b[i].toString(16));
            return s.join("");
        }
        case "utf16le": {
            var u = [];
            for (var j = start; j + 1 < end; j += 2) u.push(String.fromCharCode(b[j] | (b[j + 1] << 8)));
            return u.join("");
        }
        case "ascii": {
            var a = [];
            for (var k = start; k < end; k++) a.push(String.fromCharCode(b[k] & 127));
            return a.join("");
        }
        default: {
            var l = [];
            for (var m = start; m < end; m++) l.push(String.fromCharCode(b[m]));
            return l.join("");
        }
        }
    }

    /* ------------------------------------------------------------------ Buffer */

    class Buffer extends Uint8Array {
        constructor(a, b, c) {
            /* new Buffer(string, encoding) is deprecated in Node but still used by old plugins */
            if (typeof a === "string") {
                var bytes = encodeString(a, b);
                super(bytes.length);
                this.set(bytes);
            } else if (b === undefined && c === undefined) {
                super(a);
            } else {
                super(a, b, c);
            }
        }

        static from(value, encOrOffset, length) {
            if (typeof value === "string") return new Buffer(value, encOrOffset);
            if (value instanceof ArrayBuffer || (typeof SharedArrayBuffer !== "undefined" && value instanceof SharedArrayBuffer)) {
                var off = encOrOffset === undefined ? 0 : encOrOffset >>> 0;
                var len = length === undefined ? value.byteLength - off : length >>> 0;
                return new Buffer(value, off, len); /* shares the memory, as in Node */
            }
            if (ArrayBuffer.isView(value) || Array.isArray(value)) {
                var copy = new Buffer(value.length);
                copy.set(value); /* values are cut to bytes, as in Node */
                return copy;
            }
            if (value && value.type === "Buffer" && Array.isArray(value.data)) return Buffer.from(value.data);
            if (value && typeof value.length === "number") return Buffer.from(Array.prototype.slice.call(value));
            if (value && typeof value.valueOf === "function" && value.valueOf() !== value) {
                return Buffer.from(value.valueOf(), encOrOffset, length);
            }
            throw new TypeError("Buffer.from: unsupported argument");
        }

        static alloc(size, fill, encoding) {
            var b = new Buffer(size >>> 0);
            if (fill !== undefined && fill !== 0) b.fill(fill, 0, b.length, encoding);
            return b;
        }

        static allocUnsafe(size) { return new Buffer(size >>> 0); }
        static allocUnsafeSlow(size) { return new Buffer(size >>> 0); }
        static isBuffer(x) { return x instanceof Buffer; }

        static isEncoding(enc) {
            try { normEncoding(enc); return true; } catch (e) { return false; }
        }

        static byteLength(value, encoding) {
            if (typeof value === "string") return encodeString(value, encoding).length;
            return value.byteLength;
        }

        static concat(list, totalLength) {
            var total = 0, i;
            if (totalLength === undefined) {
                for (i = 0; i < list.length; i++) total += list[i].length;
            } else {
                total = totalLength >>> 0;
            }
            var out = new Buffer(total), pos = 0;
            for (i = 0; i < list.length && pos < total; i++) {
                var item = list[i];
                var n = Math.min(item.length, total - pos);
                out.set(n === item.length ? item : item.subarray(0, n), pos);
                pos += n;
            }
            return out;
        }

        static compare(a, b) { return Buffer.prototype.compare.call(a, b); }

        toString(encoding, start, end) {
            var s = start === undefined ? 0 : Math.max(0, start | 0);
            var e = end === undefined ? this.length : Math.min(this.length, end | 0);
            if (e <= s) return "";
            return decodeBytes(this, encoding, s, e);
        }

        toJSON() { return { type: "Buffer", data: Array.prototype.slice.call(this) }; }

        equals(other) { return this.compare(other) === 0; }

        compare(other) {
            var n = Math.min(this.length, other.length);
            for (var i = 0; i < n; i++) {
                if (this[i] !== other[i]) return this[i] < other[i] ? -1 : 1;
            }
            return this.length === other.length ? 0 : (this.length < other.length ? -1 : 1);
        }

        /* In Node, slice() shares the memory (like subarray) */
        slice(start, end) { return this.subarray(start, end); }

        write(string, offset, length, encoding) {
            if (typeof offset === "string") { encoding = offset; offset = 0; length = undefined; }
            else if (typeof length === "string") { encoding = length; length = undefined; }
            offset = offset === undefined ? 0 : offset >>> 0;
            var bytes = encodeString(string, encoding);
            var n = Math.min(bytes.length, this.length - offset, length === undefined ? Infinity : length >>> 0);
            if (n > 0) this.set(bytes.subarray(0, n), offset);
            return Math.max(0, n);
        }

        copy(target, targetStart, sourceStart, sourceEnd) {
            targetStart = targetStart === undefined ? 0 : targetStart >>> 0;
            sourceStart = sourceStart === undefined ? 0 : sourceStart >>> 0;
            sourceEnd = sourceEnd === undefined ? this.length : Math.min(this.length, sourceEnd >>> 0);
            var n = Math.min(sourceEnd - sourceStart, target.length - targetStart);
            if (n <= 0) return 0;
            target.set(this.subarray(sourceStart, sourceStart + n), targetStart);
            return n;
        }

        fill(value, offset, end, encoding) {
            if (typeof offset === "string") { encoding = offset; offset = 0; end = this.length; }
            offset = offset === undefined ? 0 : offset >>> 0;
            end = end === undefined ? this.length : Math.min(this.length, end >>> 0);
            if (typeof value === "string" || ArrayBuffer.isView(value)) {
                var bytes = typeof value === "string" ? encodeString(value, encoding) : value;
                if (bytes.length === 0) return Uint8Array.prototype.fill.call(this, 0, offset, end), this;
                for (var i = offset, k = 0; i < end; i++, k++) this[i] = bytes[k % bytes.length];
                return this;
            }
            Uint8Array.prototype.fill.call(this, (value | 0) & 255, offset, end);
            return this;
        }

        indexOf(value, byteOffset, encoding) {
            if (typeof value === "number") return Uint8Array.prototype.indexOf.call(this, value & 255, byteOffset);
            var needle = typeof value === "string" ? encodeString(value, encoding) : value;
            var from = byteOffset === undefined ? 0 : byteOffset | 0;
            if (from < 0) from = Math.max(0, this.length + from);
            if (needle.length === 0) return Math.min(from, this.length);
            outer:
            for (var i = from; i <= this.length - needle.length; i++) {
                for (var j = 0; j < needle.length; j++) {
                    if (this[i + j] !== needle[j]) continue outer;
                }
                return i;
            }
            return -1;
        }

        includes(value, byteOffset, encoding) { return this.indexOf(value, byteOffset, encoding) !== -1; }

        _dv() { return new DataView(this.buffer, this.byteOffset, this.byteLength); }
    }

    /* readUInt16LE, writeInt32BE, readFloatLE, ... through a DataView */
    var NUM_TYPES = [
        ["UInt8", "Uint8", 1], ["Int8", "Int8", 1], ["UInt16", "Uint16", 2], ["Int16", "Int16", 2],
        ["UInt32", "Uint32", 4], ["Int32", "Int32", 4], ["Float", "Float32", 4], ["Double", "Float64", 8]
    ];
    NUM_TYPES.forEach(function(t) {
        var name = t[0], dvName = t[1], size = t[2];
        var ends = size === 1 ? [""] : ["LE", "BE"];
        ends.forEach(function(endian) {
            var little = endian === "LE";
            var read = function(offset) { return this._dv()["get" + dvName](offset === undefined ? 0 : offset, little); };
            var write = function(value, offset) {
                offset = offset === undefined ? 0 : offset;
                this._dv()["set" + dvName](offset, value, little);
                return offset + size;
            };
            Buffer.prototype["read" + name + endian] = read;
            Buffer.prototype["write" + name + endian] = write;
            if (name.indexOf("UInt") === 0) { /* Node also has readUint8 etc. */
                Buffer.prototype["readUint" + name.substring(4) + endian] = read;
                Buffer.prototype["writeUint" + name.substring(4) + endian] = write;
            }
        });
    });

    Buffer.poolSize = 8192;
    var bufferModule = { Buffer: Buffer, kMaxLength: 0x7fffffff, constants: { MAX_LENGTH: 0x7fffffff } };

    if (typeof G.Buffer === "undefined") G.Buffer = Buffer;
    if (typeof G.global === "undefined") G.global = G;

    /* ------------------------------------------------------------------ TextEncoder / TextDecoder */

    if (typeof G.TextEncoder === "undefined") {
        G.TextEncoder = function TextEncoder() { this.encoding = "utf-8"; };
        G.TextEncoder.prototype.encode = function(str) { return utf8Encode(String(str === undefined ? "" : str)); };
    }
    if (typeof G.TextDecoder === "undefined") {
        G.TextDecoder = function TextDecoder(label) { this.encoding = normEncoding(label || "utf-8") === "utf16le" ? "utf-16le" : "utf-8"; };
        G.TextDecoder.prototype.decode = function(input) {
            if (input === undefined) return "";
            var bytes = input instanceof ArrayBuffer ? new Uint8Array(input)
                : new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
            return decodeBytes(bytes, this.encoding, 0, bytes.length);
        };
    }

    /* ------------------------------------------------------------------ helpers */

    var later = typeof G.queueMicrotask === "function"
        ? function(fn) { G.queueMicrotask(fn); }
        : function(fn) { Promise.resolve().then(fn); };

    if (typeof G.queueMicrotask === "undefined") G.queueMicrotask = later;
    if (typeof G.setImmediate === "undefined") {
        G.setImmediate = function(fn) {
            var args = Array.prototype.slice.call(arguments, 1);
            return setTimeout(function() { fn.apply(null, args); }, 0);
        };
        G.clearImmediate = function(id) { clearTimeout(id); };
    }

    function nodeError(code, syscall, path, message) {
        var e = new Error(code + ": " + (message || "no such file or directory") + ", " + syscall + " '" + path + "'");
        e.code = code;
        e.errno = code === "ENOENT" ? -2 : -1;
        e.syscall = syscall;
        e.path = path;
        return e;
    }

    function unavailable(what) {
        return function() { throw new Error(what + " is not available on the PS5"); };
    }

    /* A module whose functions all throw when called (requiring it still works). */
    function unavailableModule(name) {
        var target = {};
        if (typeof Proxy === "undefined") return target;
        return new Proxy(target, {
            get: function(t, prop) {
                if (prop === "__esModule" || prop === "then" || prop === "toJSON" || typeof prop === "symbol") return undefined;
                return unavailable(name + "." + String(prop));
            }
        });
    }

    var io = G.__native_io;
    var baseRequire = G.require;
    var fs = baseRequire("fs");
    var path = baseRequire("path");

    /* NW.js starts in the folder that holds package.json, which for desktop MV/MZ games is the folder ABOVE www; games
       read files like "www/Copyrights.txt" relative to it. Windows paths may also use backslashes. */
    var gameRootDir = (io && io.getGameRoot && io.getGameRoot()) || ".";
    var pkgRoot = (function() {
        /* the native file functions only reach inside the game folder, so package.json itself cannot be checked */
        return /[\/\\]www$/i.test(gameRootDir) ? path.dirname(gameRootDir) : null;
    })();

    function fixPath(p) {
        if (typeof p !== "string") return p;
        if (p.indexOf("\\") >= 0) p = p.replace(/\\/g, "/");
        if (pkgRoot && /^(\.\/)?www\//i.test(p)) return pkgRoot + "/" + p.replace(/^\.\//, "");
        return p;
    }

    /* Windows games write into %APPDATA%\\<name>\\... and expect those folders to exist. The folder lives inside the game
       folder here (see process.env.APPDATA below); create what is missing when something is written or listed there. */
    function appDataPrefix() { return gameRootDir + "/appdata"; }
    function ensureAppDir(dir) {
        var root = appDataPrefix();
        if (typeof dir !== "string" || (dir !== root && dir.indexOf(root + "/") !== 0)) return;
        var parts = dir.substring(root.length).split("/").filter(Boolean), cur = root;
        if (!io.existsSync(cur)) io.mkdirSync(cur);
        for (var i = 0; i < parts.length; i++) {
            cur += "/" + parts[i];
            if (!io.existsSync(cur)) io.mkdirSync(cur);
        }
    }

    function toArrayBuffer(view) {
        if (view instanceof ArrayBuffer) return view;
        if (view.byteOffset === 0 && view.byteLength === view.buffer.byteLength) return view.buffer;
        return view.buffer.slice(view.byteOffset, view.byteOffset + view.byteLength);
    }

    function encodingOf(options) {
        if (typeof options === "string") return options;
        if (options && typeof options === "object" && options.encoding) return options.encoding;
        return null;
    }

    /* ------------------------------------------------------------------ fs */

    var nativeReadText = fs.readFileSync;
    var nativeMkdir = fs.mkdirSync;

    function exists(p) { return io.existsSync(String(p)); }
    function isDir(p) { return io.isDirSync(String(p)); }

    fs.readFileSync = function(p, options) {
        if (typeof p === "number") return fdRead(p, options);
        p = String(p);
        if (!exists(p) || isDir(p)) throw nodeError(isDir(p) ? "EISDIR" : "ENOENT", "open", p,
                                                    isDir(p) ? "illegal operation on a directory" : undefined);
        var enc = encodingOf(options);
        if (enc && normEncoding(enc) === "utf8") return nativeReadText(p, options);
        var buf = Buffer.from(io.readFileBinary(p));
        return enc ? buf.toString(enc) : buf;
    };

    fs.writeFileSync = function(p, data, options) {
        if (typeof p === "number") return fdWrite(p, data);
        p = String(p);
        var enc = encodingOf(options);
        if (typeof data === "string") {
            if (!enc || normEncoding(enc) === "utf8") io.writeFileSync(p, data);
            else io.writeFileBinary(p, toArrayBuffer(encodeString(data, enc)));
        } else if (data instanceof ArrayBuffer || ArrayBuffer.isView(data)) {
            io.writeFileBinary(p, toArrayBuffer(data));
        } else {
            io.writeFileSync(p, String(data));
        }
    };

    fs.appendFileSync = function(p, data, options) {
        p = String(p);
        var add = typeof data === "string" ? encodeString(data, encodingOf(options)) : Buffer.from(data);
        var old = exists(p) ? Buffer.from(io.readFileBinary(p)) : new Buffer(0);
        io.writeFileBinary(p, toArrayBuffer(Buffer.concat([old, add])));
    };

    fs.existsSync = function(p) {
        try { return exists(p); } catch (e) { return false; }
    };

    fs.mkdirSync = function(p, options) {
        p = String(p);
        var recursive = options && typeof options === "object" && options.recursive;
        if (!recursive) {
            /* Node would throw EEXIST; games check existsSync first anyway, and the old shim did not throw */
            if (!exists(p)) nativeMkdir(p);
            return undefined;
        }
        var parts = p.replace(/\\/g, "/").split("/"), cur = "", first;
        for (var i = 0; i < parts.length; i++) {
            if (parts[i] === "" && i > 0) continue;
            cur = i === 0 ? parts[0] : cur + "/" + parts[i];
            if (cur && !exists(cur)) {
                nativeMkdir(cur);
                if (first === undefined) first = cur;
            }
        }
        return first;
    };

    var nativeReaddir = fs.readdirSync;
    fs.readdirSync = function(p, options) {
        p = String(p);
        if (!exists(p)) throw nodeError("ENOENT", "scandir", p);
        var names = nativeReaddir(p).filter(function(n) { return n !== "." && n !== ".."; });
        if (options && options.withFileTypes) {
            return names.map(function(n) {
                var full = p + "/" + n;
                var dir = isDir(full);
                return {
                    name: n,
                    isDirectory: function() { return dir; },
                    isFile: function() { return !dir; },
                    isSymbolicLink: function() { return false; }
                };
            });
        }
        return names;
    };

    function Stats(p) {
        var dir = isDir(p), size = null;
        this.isDirectory = function() { return dir; };
        this.isFile = function() { return !dir; };
        this.isSymbolicLink = function() { return false; };
        Object.defineProperty(this, "size", {
            enumerable: true,
            configurable: true,
            get: function() {
                if (size === null) size = dir ? 0 : io.readFileBinary(p).byteLength;
                return size;
            }
        });
        var t = new Date(0);
        this.mtime = this.ctime = this.atime = this.birthtime = t;
        this.mtimeMs = this.ctimeMs = this.atimeMs = this.birthtimeMs = 0;
        this.mode = dir ? 0o40755 : 0o100644;
    }

    fs.statSync = function(p, options) {
        p = String(p);
        if (!exists(p)) {
            if (options && options.throwIfNoEntry === false) return undefined;
            throw nodeError("ENOENT", "stat", p);
        }
        return new Stats(p);
    };
    fs.lstatSync = fs.statSync;
    fs.Stats = Stats;

    fs.accessSync = function(p) {
        if (!exists(p)) throw nodeError("ENOENT", "access", String(p));
    };
    fs.realpathSync = function(p) { return String(p); };
    fs.realpathSync.native = fs.realpathSync;

    fs.copyFileSync = function(src, dst) {
        src = String(src);
        if (!exists(src)) throw nodeError("ENOENT", "copyfile", src);
        io.writeFileBinary(String(dst), io.readFileBinary(src));
    };

    function removeTree(p) {
        if (isDir(p)) {
            nativeReaddir(p).forEach(function(n) {
                if (n !== "." && n !== "..") removeTree(p + "/" + n);
            });
        }
        io.unlinkSync(p);
    }
    fs.rmSync = function(p, options) {
        p = String(p);
        if (!exists(p)) {
            if (options && options.force) return;
            throw nodeError("ENOENT", "rm", p);
        }
        if (isDir(p) && !(options && options.recursive)) throw nodeError("EISDIR", "rm", p, "is a directory");
        removeTree(p);
    };
    fs.rmdirSync = function(p, options) {
        p = String(p);
        if (!exists(p)) throw nodeError("ENOENT", "rmdir", p);
        if (options && options.recursive) removeTree(p);
        else io.unlinkSync(p);
    };

    /* file descriptors: whole files are read at open and written at close */
    var fds = {}, nextFd = 10;
    fs.openSync = function(p, flags) {
        p = String(p);
        flags = flags === undefined ? "r" : String(flags);
        var entry = { path: p, flags: flags, pos: 0, data: null, dirty: false };
        if (flags.indexOf("r") === 0) {
            if (!exists(p)) throw nodeError("ENOENT", "open", p);
            entry.data = Buffer.from(io.readFileBinary(p));
        } else if (flags.indexOf("a") === 0) {
            entry.data = exists(p) ? Buffer.from(io.readFileBinary(p)) : new Buffer(0);
            entry.pos = entry.data.length;
            entry.dirty = true;
        } else { /* w, w+, wx */
            entry.data = new Buffer(0);
            entry.dirty = true;
        }
        var fd = nextFd++;
        fds[fd] = entry;
        return fd;
    };
    function fdEntry(fd) {
        var e = fds[fd];
        if (!e) throw nodeError("EBADF", "read", String(fd), "bad file descriptor");
        return e;
    }
    function fdRead(fd, options) {
        var e = fdEntry(fd);
        var enc = encodingOf(options);
        return enc ? e.data.toString(enc) : Buffer.from(e.data);
    }
    function fdWrite(fd, data) {
        var e = fdEntry(fd);
        e.data = Buffer.concat([e.data, typeof data === "string" ? Buffer.from(data) : Buffer.from(data)]);
        e.dirty = true;
    }
    fs.readSync = function(fd, buffer, offset, length, position) {
        var e = fdEntry(fd);
        if (offset && typeof offset === "object") { /* readSync(fd, buffer, { offset, length, position }) */
            position = offset.position; length = offset.length; offset = offset.offset;
        }
        offset = offset || 0;
        length = length === undefined ? buffer.length - offset : length;
        var from = position === null || position === undefined ? e.pos : position;
        var n = Math.max(0, Math.min(length, e.data.length - from));
        buffer.set(e.data.subarray(from, from + n), offset);
        if (position === null || position === undefined) e.pos = from + n;
        return n;
    };
    fs.writeSync = function(fd, data, offsetOrPosition, lengthOrEncoding, position) {
        var e = fdEntry(fd);
        var bytes;
        if (typeof data === "string") {
            bytes = encodeString(data, typeof lengthOrEncoding === "string" ? lengthOrEncoding : "utf8");
            position = offsetOrPosition;
        } else {
            var off = offsetOrPosition || 0;
            var len = lengthOrEncoding === undefined ? data.length - off : lengthOrEncoding;
            bytes = data.subarray(off, off + len);
        }
        var at = position === null || position === undefined ? e.pos : position;
        if (at + bytes.length > e.data.length) {
            var grown = new Buffer(at + bytes.length);
            grown.set(e.data);
            e.data = grown;
        }
        e.data.set(bytes, at);
        if (position === null || position === undefined) e.pos = at + bytes.length;
        e.dirty = true;
        return bytes.length;
    };
    fs.fstatSync = function(fd) {
        var e = fdEntry(fd);
        var st = exists(e.path) ? new Stats(e.path) : { isFile: function() { return true; }, isDirectory: function() { return false; } };
        Object.defineProperty(st, "size", { value: e.data.length });
        return st;
    };
    fs.closeSync = function(fd) {
        var e = fdEntry(fd);
        if (e.dirty) io.writeFileBinary(e.path, toArrayBuffer(e.data));
        delete fds[fd];
    };
    fs.fsyncSync = function() {};

    fs.constants = { F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1, O_RDONLY: 0, O_WRONLY: 1, O_RDWR: 2, COPYFILE_EXCL: 1 };
    fs.F_OK = 0; fs.R_OK = 4; fs.W_OK = 2; fs.X_OK = 1;
    fs.createReadStream = unavailable("fs.createReadStream");
    fs.createWriteStream = unavailable("fs.createWriteStream");
    fs.watch = unavailable("fs.watch");

    var WRITES = { writeFileSync: 1, appendFileSync: 1, openSync: 1 }, LISTS = { readdirSync: 1, statSync: 1, lstatSync: 1 };
    ["readFileSync", "writeFileSync", "appendFileSync", "existsSync", "mkdirSync", "readdirSync", "statSync",
     "lstatSync", "accessSync", "rmSync", "rmdirSync", "openSync", "unlinkSync", "realpathSync"].forEach(function(n) {
        var original = fs[n];
        if (typeof original !== "function") return;
        fs[n] = function(p) {
            p = arguments[0] = fixPath(p);
            if (typeof p === "string") {
                if (WRITES[n]) ensureAppDir(path.dirname(p));
                else if (LISTS[n] && !/\.[A-Za-z0-9]+$/.test(p)) ensureAppDir(p.replace(/\/+$/, ""));
            }
            return original.apply(fs, arguments);
        };
    });
    ["copyFileSync", "renameSync"].forEach(function(n) {
        var original = fs[n];
        if (typeof original !== "function") return;
        fs[n] = function(a, b) { return original.call(fs, fixPath(a), fixPath(b)); };
    });

    /* callback and promise versions of the synchronous functions */
    var ASYNC = ["readFile", "writeFile", "appendFile", "mkdir", "readdir", "stat", "lstat", "unlink", "rename",
                 "copyFile", "rm", "rmdir", "access", "realpath", "open", "close", "fstat"];
    fs.promises = {};
    ASYNC.forEach(function(name) {
        var sync = fs[name + "Sync"];
        if (typeof sync !== "function") return;
        fs[name] = function() {
            var args = Array.prototype.slice.call(arguments);
            var cb = typeof args[args.length - 1] === "function" ? args.pop() : null;
            var result, error = null;
            try { result = sync.apply(fs, args); } catch (e) { error = e; }
            if (cb) later(function() { error ? cb(error) : cb(null, result); });
        };
        fs.promises[name] = function() {
            var args = arguments;
            return new Promise(function(resolve, reject) {
                try { resolve(sync.apply(fs, args)); } catch (e) { reject(e); }
            });
        };
    });
    fs.exists = function(p, cb) {
        var r = fs.existsSync(p);
        if (typeof cb === "function") later(function() { cb(r); });
    };
    fs.read = function(fd, buffer, offset, length, position, cb) {
        try {
            var n = fs.readSync(fd, buffer, offset, length, position);
            later(function() { cb(null, n, buffer); });
        } catch (e) {
            later(function() { cb(e); });
        }
    };
    fs.write = function(fd, data) {
        var args = Array.prototype.slice.call(arguments);
        var cb = typeof args[args.length - 1] === "function" ? args.pop() : null;
        try {
            var n = fs.writeSync.apply(fs, args);
            if (cb) later(function() { cb(null, n, data); });
        } catch (e) {
            if (cb) later(function() { cb(e); });
        }
    };

    /* ------------------------------------------------------------------ path */

    function normalizeParts(p) {
        var abs = p.charAt(0) === "/";
        var out = [];
        p.split("/").forEach(function(seg) {
            if (seg === "" || seg === ".") return;
            if (seg === "..") {
                if (out.length && out[out.length - 1] !== "..") out.pop();
                else if (!abs) out.push("..");
                return;
            }
            out.push(seg);
        });
        return { abs: abs, parts: out };
    }

    path.normalize = function(p) {
        p = String(p).replace(/\\/g, "/");
        if (p === "") return ".";
        var trailing = p.charAt(p.length - 1) === "/";
        var n = normalizeParts(p);
        var s = (n.abs ? "/" : "") + n.parts.join("/");
        if (!s) s = n.abs ? "/" : ".";
        if (trailing && s !== "/") s += "/";
        return s;
    };
    path.isAbsolute = function(p) {
        p = String(p);
        return p.charAt(0) === "/" || p.charAt(0) === "\\" || /^[A-Za-z]:[\\/]/.test(p);
    };
    path.extname = function(p) {
        var base = path.basename(String(p));
        var dot = base.lastIndexOf(".");
        return dot <= 0 ? "" : base.substring(dot);
    };
    path.relative = function(from, to) {
        var a = normalizeParts(path.resolve(from)).parts, b = normalizeParts(path.resolve(to)).parts;
        var i = 0;
        while (i < a.length && i < b.length && a[i] === b[i]) i++;
        var up = [];
        for (var j = i; j < a.length; j++) up.push("..");
        return up.concat(b.slice(i)).join("/");
    };
    path.parse = function(p) {
        p = String(p);
        var dir = path.dirname(p), base = path.basename(p), ext = path.extname(p);
        return {
            root: p.charAt(0) === "/" ? "/" : "",
            dir: dir === "." && p.indexOf("/") < 0 ? "" : dir,
            base: base,
            ext: ext,
            name: ext ? base.substring(0, base.length - ext.length) : base
        };
    };
    path.format = function(o) {
        var base = o.base || ((o.name || "") + (o.ext || ""));
        var dir = o.dir || o.root || "";
        return dir ? (dir === o.root ? dir + base : dir + "/" + base) : base;
    };
    path.toNamespacedPath = function(p) { return p; };
    path.posix = path;
    path.win32 = path;

    /* ------------------------------------------------------------------ events */

    function EventEmitter() {
        this._events = this._events || {};
        this._maxListeners = this._maxListeners || 10;
    }
    EventEmitter.prototype.on = EventEmitter.prototype.addListener = function(name, fn) {
        if (!this._events) this._events = {};
        (this._events[name] = this._events[name] || []).push(fn);
        return this;
    };
    EventEmitter.prototype.prependListener = function(name, fn) {
        if (!this._events) this._events = {};
        (this._events[name] = this._events[name] || []).unshift(fn);
        return this;
    };
    EventEmitter.prototype.once = function(name, fn) {
        var self = this;
        function wrapper() {
            self.removeListener(name, wrapper);
            return fn.apply(this, arguments);
        }
        wrapper.listener = fn;
        return this.on(name, wrapper);
    };
    EventEmitter.prototype.off = EventEmitter.prototype.removeListener = function(name, fn) {
        var list = this._events && this._events[name];
        if (!list) return this;
        for (var i = list.length - 1; i >= 0; i--) {
            if (list[i] === fn || list[i].listener === fn) {
                list.splice(i, 1);
                break;
            }
        }
        return this;
    };
    EventEmitter.prototype.removeAllListeners = function(name) {
        if (!this._events) return this;
        if (name === undefined) this._events = {};
        else delete this._events[name];
        return this;
    };
    EventEmitter.prototype.emit = function(name) {
        var list = this._events && this._events[name];
        if (!list || !list.length) {
            if (name === "error" && arguments[1] instanceof Error) throw arguments[1];
            return false;
        }
        var args = Array.prototype.slice.call(arguments, 1);
        list.slice().forEach(function(fn) { fn.apply(this, args); }, this);
        return true;
    };
    EventEmitter.prototype.listeners = function(name) {
        return ((this._events && this._events[name]) || []).map(function(f) { return f.listener || f; });
    };
    EventEmitter.prototype.listenerCount = function(name) { return this.listeners(name).length; };
    EventEmitter.prototype.eventNames = function() { return Object.keys(this._events || {}); };
    EventEmitter.prototype.setMaxListeners = function(n) { this._maxListeners = n; return this; };
    EventEmitter.prototype.getMaxListeners = function() { return this._maxListeners; };
    EventEmitter.EventEmitter = EventEmitter;
    EventEmitter.defaultMaxListeners = 10;

    /* ------------------------------------------------------------------ util */

    function inspect(v) {
        if (typeof v === "string") return "'" + v + "'";
        try { return JSON.stringify(v); } catch (e) { return String(v); }
    }
    var util = {
        format: function(fmt) {
            var args = Array.prototype.slice.call(arguments, 1);
            if (typeof fmt !== "string") return [fmt].concat(args).map(inspect).join(" ");
            var s = fmt.replace(/%[sdifjoO%]/g, function(m) {
                if (m === "%%") return "%";
                if (!args.length) return m;
                var a = args.shift();
                switch (m) {
                case "%s": return String(a);
                case "%d": case "%i": return String(parseInt(a, 10));
                case "%f": return String(parseFloat(a));
                default: return inspect(a);
                }
            });
            return args.length ? s + " " + args.map(function(a) { return typeof a === "string" ? a : inspect(a); }).join(" ") : s;
        },
        inherits: function(ctor, superCtor) {
            ctor.super_ = superCtor;
            Object.setPrototypeOf(ctor.prototype, superCtor.prototype);
        },
        promisify: function(fn) {
            return function() {
                var args = Array.prototype.slice.call(arguments), self = this;
                return new Promise(function(resolve, reject) {
                    args.push(function(err, value) { err ? reject(err) : resolve(value); });
                    fn.apply(self, args);
                });
            };
        },
        deprecate: function(fn) { return fn; },
        inspect: inspect,
        isArray: Array.isArray,
        isBuffer: Buffer.isBuffer,
        isString: function(v) { return typeof v === "string"; },
        isFunction: function(v) { return typeof v === "function"; },
        isObject: function(v) { return v !== null && typeof v === "object"; },
        types: {
            isTypedArray: function(v) { return ArrayBuffer.isView(v) && !(v instanceof DataView); },
            isArrayBuffer: function(v) { return v instanceof ArrayBuffer; },
            isPromise: function(v) { return v instanceof Promise; }
        },
        TextEncoder: G.TextEncoder,
        TextDecoder: G.TextDecoder
    };

    function StringDecoder(encoding) { this.encoding = normEncoding(encoding || "utf8"); this._rest = new Buffer(0); }
    StringDecoder.prototype.write = function(buf) {
        var all = Buffer.concat([this._rest, Buffer.from(buf)]);
        var end = all.length;
        if (this.encoding === "utf8") { /* keep an incomplete character for the next call */
            var i = all.length - 1, k = 0;
            while (i >= 0 && k < 3 && (all[i] & 0xC0) === 0x80) { i--; k++; }
            if (i >= 0) {
                var c = all[i], need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
                if (need > all.length - i) end = i;
            }
        }
        this._rest = Buffer.from(all.subarray(end));
        return all.toString(this.encoding, 0, end);
    };
    StringDecoder.prototype.end = function(buf) {
        var s = buf ? this.write(buf) : "";
        s += this._rest.toString(this.encoding);
        this._rest = new Buffer(0);
        return s;
    };

    /* ------------------------------------------------------------------ os */

    var gameRoot = (io && io.getGameRoot && io.getGameRoot()) || ".";
    var os = {
        platform: function() { return "linux"; },
        type: function() { return "Linux"; },
        arch: function() { return "x64"; },
        release: function() { return "1.0.0"; },
        version: function() { return "PS5"; },
        hostname: function() { return "ps5"; },
        homedir: function() { return gameRoot; },
        tmpdir: function() { return gameRoot; },
        endianness: function() { return "LE"; },
        uptime: function() { return Math.floor(Date.now() / 1000) % 86400; },
        totalmem: function() { return 12 * 1024 * 1024 * 1024; },
        freemem: function() { return 4 * 1024 * 1024 * 1024; },
        cpus: function() {
            var cpu = { model: "PlayStation 5", speed: 3500, times: { user: 0, nice: 0, sys: 0, idle: 0, irq: 0 } };
            return [cpu, cpu, cpu, cpu, cpu, cpu, cpu, cpu];
        },
        loadavg: function() { return [0, 0, 0]; },
        networkInterfaces: function() { return {}; },
        userInfo: function() { return { username: "player", uid: 1000, gid: 1000, shell: null, homedir: gameRoot }; },
        EOL: "\n",
        constants: { signals: {}, errno: {} }
    };

    /* ------------------------------------------------------------------ zlib */

    /* Decompression is native (the engine's inflate). Compression writes valid but uncompressed ("stored") deflate
       data, or uses the game's own pako when it ships one. */

    var CRC_TABLE = null;
    function crc32(b) {
        if (!CRC_TABLE) {
            CRC_TABLE = new Uint32Array(256);
            for (var n = 0; n < 256; n++) {
                var c = n;
                for (var k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
                CRC_TABLE[n] = c >>> 0;
            }
        }
        var crc = 0xFFFFFFFF;
        for (var i = 0; i < b.length; i++) crc = CRC_TABLE[(crc ^ b[i]) & 255] ^ (crc >>> 8);
        return (crc ^ 0xFFFFFFFF) >>> 0;
    }

    function adler32(b) {
        var a = 1, s = 0;
        for (var i = 0; i < b.length; i++) {
            a = (a + b[i]) % 65521;
            s = (s + a) % 65521;
        }
        return ((s << 16) | a) >>> 0;
    }

    function storedBlocks(b) {
        var parts = [], pos = 0;
        do {
            var len = Math.min(65535, b.length - pos), last = pos + len >= b.length ? 1 : 0;
            var head = Buffer.from([last, len & 255, len >> 8, (~len) & 255, ((~len) >> 8) & 255]);
            parts.push(head, b.subarray(pos, pos + len));
            pos += len;
        } while (pos < b.length);
        return Buffer.concat(parts);
    }

    function u32(v, big) {
        var b = Buffer.alloc(4);
        if (big) b.writeUInt32BE(v >>> 0); else b.writeUInt32LE(v >>> 0);
        return b;
    }

    function zlibError(e) {
        var err = new Error(e && e.message ? e.message : "zlib error");
        err.code = "Z_DATA_ERROR";
        err.errno = -3;
        return err;
    }

    function toBytes(data) {
        if (typeof data === "string") return Buffer.from(data);
        if (data instanceof ArrayBuffer) return Buffer.from(data);
        return data;
    }

    function inflateWith(mode) {
        return function(data) {
            var bytes = toBytes(data);
            try {
                return Buffer.from(io.inflate(toArrayBuffer(bytes), mode));
            } catch (e) {
                throw zlibError(e);
            }
        };
    }

    function deflateWith(kind, pakoName) {
        return function(data, options) {
            if (G.pako && typeof G.pako[pakoName] === "function") return Buffer.from(G.pako[pakoName](toBytes(data), options));
            var b = toBytes(data), body = storedBlocks(b);
            if (kind === "raw") return body;
            if (kind === "zlib") return Buffer.concat([Buffer.from([0x78, 0x01]), body, u32(adler32(b), true)]);
            return Buffer.concat([Buffer.from([0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3]), body, u32(crc32(b), false), u32(b.length, false)]);
        };
    }

    var zlib = {
        inflateSync: inflateWith(0), inflateRawSync: inflateWith(1), gunzipSync: inflateWith(2), unzipSync: inflateWith(3),
        deflateSync: deflateWith("zlib", "deflate"), deflateRawSync: deflateWith("raw", "deflateRaw"),
        gzipSync: deflateWith("gzip", "gzip"),
        crc32: function(data) { return crc32(toBytes(data)); },
        constants: { Z_NO_COMPRESSION: 0, Z_BEST_SPEED: 1, Z_BEST_COMPRESSION: 9, Z_DEFAULT_COMPRESSION: -1,
                     Z_OK: 0, Z_DATA_ERROR: -3, Z_SYNC_FLUSH: 2, Z_FINISH: 4 }
    };
    ["inflate", "deflate", "inflateRaw", "deflateRaw", "gunzip", "gzip", "unzip"].forEach(function(n) {
        var sync = zlib[n + "Sync"];
        zlib[n] = function(data, options, cb) {
            if (typeof options === "function") { cb = options; options = undefined; }
            var r, err = null;
            try { r = sync(data, options); } catch (e) { err = e; }
            later(function() { err ? cb(err) : cb(null, r); });
        };
    });

    /* ------------------------------------------------------------------ crypto: hashes */

    /* MD5, SHA-1 and SHA-256 (standard algorithms), with Node's createHash / createHmac interface. */

    function bigCubeRoot(n) { /* floor(cbrt(n)) for a BigInt */
        var lo = 0n, hi = 1n << (BigInt(n.toString(2).length) / 3n + 2n);
        while (lo < hi) {
            var mid = (lo + hi + 1n) >> 1n;
            if (mid * mid * mid <= n) lo = mid; else hi = mid - 1n;
        }
        return lo;
    }
    function bigSqrt(n) {
        var lo = 0n, hi = 1n << (BigInt(n.toString(2).length) / 2n + 2n);
        while (lo < hi) {
            var mid = (lo + hi + 1n) >> 1n;
            if (mid * mid <= n) lo = mid; else hi = mid - 1n;
        }
        return lo;
    }

    var sha256Tables = null;
    function sha256Init() {
        if (sha256Tables) return sha256Tables;
        var primes = [], n = 2;
        while (primes.length < 64) {
            var isPrime = true;
            for (var i = 0; i < primes.length && primes[i] * primes[i] <= n; i++) if (n % primes[i] === 0) { isPrime = false; break; }
            if (isPrime) primes.push(n);
            n++;
        }
        var K = new Uint32Array(64), H = new Uint32Array(8), mask = 0xFFFFFFFFn;
        for (var j = 0; j < 64; j++) K[j] = Number(bigCubeRoot(BigInt(primes[j]) << 96n) & mask);
        for (var h = 0; h < 8; h++) H[h] = Number(bigSqrt(BigInt(primes[h]) << 64n) & mask);
        sha256Tables = { K: K, H: H };
        return sha256Tables;
    }

    function padMessage(b, bigEndian) {
        var len = b.length, total = (((len + 8) >> 6) + 1) * 64;
        var m = new Uint8Array(total);
        m.set(b);
        m[len] = 0x80;
        var dv = new DataView(m.buffer);
        var hi = Math.floor(len / 536870912), lo = (len * 8) >>> 0;
        if (bigEndian) { dv.setUint32(total - 8, hi); dv.setUint32(total - 4, lo); }
        else { dv.setUint32(total - 8, lo, true); dv.setUint32(total - 4, hi, true); }
        return m;
    }

    function md5(b) {
        var S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21];
        var K = new Int32Array(64);
        for (var i = 0; i < 64; i++) K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296) | 0;
        var m = padMessage(b, false), dv = new DataView(m.buffer);
        var a0 = 0x67452301 | 0, b0 = 0xefcdab89 | 0, c0 = 0x98badcfe | 0, d0 = 0x10325476 | 0;
        for (var off = 0; off < m.length; off += 64) {
            var A = a0, B = b0, C = c0, D = d0;
            for (var r = 0; r < 64; r++) {
                var F, g;
                if (r < 16) { F = (B & C) | (~B & D); g = r; }
                else if (r < 32) { F = (D & B) | (~D & C); g = (5 * r + 1) & 15; }
                else if (r < 48) { F = B ^ C ^ D; g = (3 * r + 5) & 15; }
                else { F = C ^ (B | ~D); g = (7 * r) & 15; }
                F = (F + A + K[r] + dv.getInt32(off + g * 4, true)) | 0;
                A = D; D = C; C = B;
                B = (B + ((F << S[r]) | (F >>> (32 - S[r])))) | 0;
            }
            a0 = (a0 + A) | 0; b0 = (b0 + B) | 0; c0 = (c0 + C) | 0; d0 = (d0 + D) | 0;
        }
        var out = Buffer.alloc(16), od = new DataView(out.buffer, out.byteOffset, 16);
        od.setInt32(0, a0, true); od.setInt32(4, b0, true); od.setInt32(8, c0, true); od.setInt32(12, d0, true);
        return out;
    }

    function sha1(b) {
        var m = padMessage(b, true), dv = new DataView(m.buffer), w = new Int32Array(80);
        var h0 = 0x67452301 | 0, h1 = 0xEFCDAB89 | 0, h2 = 0x98BADCFE | 0, h3 = 0x10325476 | 0, h4 = 0xC3D2E1F0 | 0;
        for (var off = 0; off < m.length; off += 64) {
            var t;
            for (t = 0; t < 16; t++) w[t] = dv.getInt32(off + t * 4);
            for (t = 16; t < 80; t++) { var x = w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16]; w[t] = (x << 1) | (x >>> 31); }
            var a = h0, bb = h1, c = h2, d = h3, e = h4;
            for (t = 0; t < 80; t++) {
                var f, k;
                if (t < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999; }
                else if (t < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1; }
                else if (t < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC | 0; }
                else { f = bb ^ c ^ d; k = 0xCA62C1D6 | 0; }
                var tmp = (((a << 5) | (a >>> 27)) + f + e + k + w[t]) | 0;
                e = d; d = c; c = (bb << 30) | (bb >>> 2); bb = a; a = tmp;
            }
            h0 = (h0 + a) | 0; h1 = (h1 + bb) | 0; h2 = (h2 + c) | 0; h3 = (h3 + d) | 0; h4 = (h4 + e) | 0;
        }
        var out = Buffer.alloc(20), od = new DataView(out.buffer, out.byteOffset, 20);
        [h0, h1, h2, h3, h4].forEach(function(v, i) { od.setInt32(i * 4, v); });
        return out;
    }

    function sha256(b) {
        var tb = sha256Init(), K = tb.K, H = new Int32Array(tb.H);
        var m = padMessage(b, true), dv = new DataView(m.buffer), w = new Int32Array(64);
        for (var off = 0; off < m.length; off += 64) {
            var t;
            for (t = 0; t < 16; t++) w[t] = dv.getInt32(off + t * 4);
            for (t = 16; t < 64; t++) {
                var x = w[t - 15], y = w[t - 2];
                var s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
                var s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
                w[t] = (w[t - 16] + s0 + w[t - 7] + s1) | 0;
            }
            var a = H[0], bb = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7];
            for (t = 0; t < 64; t++) {
                var S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
                var ch = (e & f) ^ (~e & g);
                var t1 = (h + S1 + ch + K[t] + w[t]) | 0;
                var S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
                var maj = (a & bb) ^ (a & c) ^ (bb & c);
                var t2 = (S0 + maj) | 0;
                h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = bb; bb = a; a = (t1 + t2) | 0;
            }
            H[0] = (H[0] + a) | 0; H[1] = (H[1] + bb) | 0; H[2] = (H[2] + c) | 0; H[3] = (H[3] + d) | 0;
            H[4] = (H[4] + e) | 0; H[5] = (H[5] + f) | 0; H[6] = (H[6] + g) | 0; H[7] = (H[7] + h) | 0;
        }
        var out = Buffer.alloc(32), od = new DataView(out.buffer, out.byteOffset, 32);
        for (var i = 0; i < 8; i++) od.setInt32(i * 4, H[i]);
        return out;
    }

    var HASHES = { md5: { fn: md5, block: 64 }, sha1: { fn: sha1, block: 64 }, sha256: { fn: sha256, block: 64 } };

    function hashSpec(name) {
        var key = String(name).toLowerCase().replace("-", "");
        if (!HASHES[key]) throw new Error("Digest method not supported");
        return HASHES[key];
    }

    function Hash(name) {
        this._spec = hashSpec(name);
        this._chunks = [];
    }
    Hash.prototype.update = function(data, enc) {
        this._chunks.push(typeof data === "string" ? Buffer.from(data, enc) : Buffer.from(data));
        return this;
    };
    Hash.prototype.copy = function() {
        var h = Object.create(Hash.prototype);
        h._spec = this._spec;
        h._chunks = this._chunks.slice();
        return h;
    };
    Hash.prototype.digest = function(enc) {
        var out = this._spec.fn(Buffer.concat(this._chunks));
        return enc && enc !== "buffer" ? out.toString(enc) : out;
    };

    function Hmac(name, key) {
        var spec = hashSpec(name);
        var k = typeof key === "string" ? Buffer.from(key) : Buffer.from(key);
        if (k.length > spec.block) k = spec.fn(k);
        var pad = Buffer.alloc(spec.block);
        pad.set(k);
        var ipad = Buffer.alloc(spec.block), opad = Buffer.alloc(spec.block);
        for (var i = 0; i < spec.block; i++) { ipad[i] = pad[i] ^ 0x36; opad[i] = pad[i] ^ 0x5c; }
        this._spec = spec;
        this._opad = opad;
        this._chunks = [ipad];
    }
    Hmac.prototype.update = Hash.prototype.update;
    Hmac.prototype.digest = function(enc) {
        var inner = this._spec.fn(Buffer.concat(this._chunks));
        var out = this._spec.fn(Buffer.concat([this._opad, inner]));
        return enc && enc !== "buffer" ? out.toString(enc) : out;
    };

    /* ------------------------------------------------------------------ crypto (random numbers, hashes) */

    var crypto = {
        randomBytes: function(n, cb) {
            var b = new Buffer(n >>> 0);
            for (var i = 0; i < b.length; i++) b[i] = (Math.random() * 256) | 0;
            if (typeof cb === "function") { later(function() { cb(null, b); }); return undefined; }
            return b;
        },
        randomInt: function(min, max) {
            if (max === undefined) { max = min; min = 0; }
            return min + Math.floor(Math.random() * (max - min));
        },
        randomUUID: function() {
            var h = crypto.randomBytes(16).toString("hex");
            return h.substr(0, 8) + "-" + h.substr(8, 4) + "-4" + h.substr(13, 3) + "-a" + h.substr(17, 3) + "-" + h.substr(20, 12);
        },
        getRandomValues: function(arr) {
            for (var i = 0; i < arr.length; i++) arr[i] = (Math.random() * 256) | 0;
            return arr;
        },
        createHash: function(name) { return new Hash(name); },
        createHmac: function(name, key) { return new Hmac(name, key); },
        getHashes: function() { return Object.keys(HASHES); },
        createCipheriv: unavailable("crypto.createCipheriv"),
        createDecipheriv: unavailable("crypto.createDecipheriv")
    };

    /* ------------------------------------------------------------------ process */

    var proc = G.process || (G.process = {});
    var started = Date.now();
    EventEmitter.call(proc);
    ["on", "addListener", "once", "off", "removeListener", "removeAllListeners", "emit", "listeners",
     "listenerCount", "prependListener", "setMaxListeners"].forEach(function(n) {
        if (typeof proc[n] !== "function") proc[n] = EventEmitter.prototype[n];
    });
    var defaults = {
        version: "v16.20.0",
        versions: { node: "16.20.0", nw: "0.72.0", "node-webkit": "0.72.0", chromium: "108.0.5359.95", v8: "10.8" },
        arch: "x64",
        pid: 1,
        ppid: 0,
        title: "Game",
        execPath: gameRoot + "/Game",
        argv0: "Game",
        execArgv: [],
        release: { name: "node" },
        features: {},
        config: { variables: {} },
        exitCode: 0
    };
    Object.keys(defaults).forEach(function(k) { if (proc[k] === undefined) proc[k] = defaults[k]; });
    if (!proc.argv) proc.argv = [];
    if (!proc.env) proc.env = {};
    if (proc.platform === undefined) proc.platform = "linux";
    proc.cwd = function() { return pkgRoot || gameRoot; };
    /* Windows variables games build file paths from (log folders, saves): a folder next to the game */
    var appData = gameRoot + "/appdata"; /* inside the game folder: the native file functions reach no further */
    ["APPDATA", "LOCALAPPDATA", "USERPROFILE", "HOME", "TEMP", "TMP"].forEach(function(k) {
        if (proc.env[k] === undefined) proc.env[k] = k === "TEMP" || k === "TMP" ? appData + "/tmp" : appData;
    });
    if (proc.env.USERNAME === undefined) proc.env.USERNAME = "Player";
    proc.chdir = proc.chdir || function() {};
    proc.umask = proc.umask || function() { return 18; };
    proc.emitWarning = proc.emitWarning || function(w) { console.warn("process warning: " + w); };
    proc.nextTick = proc.nextTick || function(fn) {
        var args = Array.prototype.slice.call(arguments, 1);
        later(function() { fn.apply(null, args); });
    };
    proc.uptime = proc.uptime || function() { return (Date.now() - started) / 1000; };
    proc.hrtime = proc.hrtime || (function() {
        function now() { return typeof performance !== "undefined" && performance.now ? performance.now() : Date.now(); }
        var hr = function(prev) {
            var t = now(), s = Math.floor(t / 1000), ns = Math.floor((t % 1000) * 1e6);
            if (prev) {
                s -= prev[0]; ns -= prev[1];
                if (ns < 0) { s--; ns += 1e9; }
            }
            return [s, ns];
        };
        if (typeof BigInt === "function") hr.bigint = function() { return BigInt(Math.floor(now() * 1e6)); };
        return hr;
    })();
    proc.memoryUsage = proc.memoryUsage || function() {
        return { rss: 0, heapTotal: 0, heapUsed: 0, external: 0, arrayBuffers: 0 };
    };
    if (!proc.stdout) proc.stdout = { write: function(s) { console.log(String(s).replace(/\n$/, "")); return true; }, isTTY: false };
    if (!proc.stderr) proc.stderr = { write: function(s) { console.warn(String(s).replace(/\n$/, "")); return true; }, isTTY: false };
    if (!proc.mainModule) proc.mainModule = { filename: gameRoot + "/index.html" };

    /* ------------------------------------------------------------------ nw (NW.js 0.13+ global) */

    if (typeof G.nw === "undefined") {
        var nwWindow = new EventEmitter();
        ["maximize", "minimize", "restore", "focus", "blur", "show", "hide", "close", "reload", "reloadIgnoringCache",
         "enterFullscreen", "leaveFullscreen", "toggleFullscreen", "enterKioskMode", "leaveKioskMode",
         "toggleKioskMode", "showDevTools", "closeDevTools", "setAlwaysOnTop", "setResizable", "setMinimumSize",
         "setMaximumSize", "resizeTo", "resizeBy", "moveTo", "moveBy", "setPosition", "requestAttention",
         "setProgressBar", "setBadgeLabel", "setShowInTaskbar", "capturePage"].forEach(function(n) {
            nwWindow[n] = function() {};
        });
        nwWindow.isFullscreen = false;
        nwWindow.isKioskMode = false;
        nwWindow.width = 1280;
        nwWindow.height = 720;
        nwWindow.x = 0;
        nwWindow.y = 0;
        nwWindow.title = "";
        nwWindow.window = G;
        G.nw = {
            Window: { get: function() { return nwWindow; }, open: function() { return nwWindow; } },
            App: { argv: [], fullArgv: [], dataPath: gameRoot, manifest: {}, quit: function() { proc.exit && proc.exit(); },
                   closeAllWindows: function() {}, clearCache: function() {}, on: function() {} },
            Shell: { openExternal: function() {}, openItem: function() {}, showItemInFolder: function() {} },
            Screen: { Init: function() {}, screens: [], on: function() {} },
            Clipboard: { get: function() { return { get: function() { return ""; }, set: function() {}, clear: function() {} }; } },
            Menu: function Menu() { this.items = []; this.append = function() {}; this.createMacBuiltin = function() {}; },
            MenuItem: function MenuItem() {},
            Tray: function Tray() {}
        };
    }

    /* ------------------------------------------------------------------ require() */

    var builtins = {
        fs: fs,
        "fs/promises": fs.promises,
        path: path,
        os: os,
        events: EventEmitter,
        util: util,
        buffer: bufferModule,
        string_decoder: { StringDecoder: StringDecoder },
        zlib: zlib,
        crypto: crypto,
        timers: { setTimeout: G.setTimeout, clearTimeout: G.clearTimeout, setInterval: G.setInterval,
                  clearInterval: G.clearInterval, setImmediate: G.setImmediate, clearImmediate: G.clearImmediate },
        process: proc,
        assert: (function() {
            var assert = function(v, msg) { if (!v) throw new Error(msg || "Assertion failed"); };
            assert.ok = assert;
            assert.equal = function(a, b, msg) { if (a != b) throw new Error(msg || (a + " == " + b)); };
            assert.strictEqual = function(a, b, msg) { if (a !== b) throw new Error(msg || (a + " === " + b)); };
            return assert;
        })(),
        url: {
            URL: G.URL,
            parse: function(u) { return { href: u, pathname: String(u).replace(/^[a-z]+:\/\/[^/]*/i, "").split("?")[0] }; },
            fileURLToPath: function(u) { return String(u).replace(/^file:\/\//, ""); },
            pathToFileURL: function(p) { return { href: "file://" + p }; }
        },
        querystring: {
            parse: function(s) {
                var o = {};
                String(s).replace(/^\?/, "").split("&").forEach(function(kv) {
                    if (!kv) return;
                    var i = kv.indexOf("=");
                    o[decodeURIComponent(i < 0 ? kv : kv.substring(0, i))] = i < 0 ? "" : decodeURIComponent(kv.substring(i + 1));
                });
                return o;
            },
            stringify: function(o) {
                return Object.keys(o || {}).map(function(k) { return encodeURIComponent(k) + "=" + encodeURIComponent(o[k]); }).join("&");
            }
        }
    };
    ["child_process", "http", "https", "http2", "net", "dgram", "tls", "dns", "cluster", "worker_threads",
     "readline", "repl", "vm", "v8", "inspector", "electron", "steamworks.js"].forEach(function(n) {
        builtins[n] = unavailableModule(n);
    });

    /* The game's own CommonJS files: require("./js/foo") or require("./data.json"), looked up from the game folder
       (www) and the folder above it, plus node_modules there. */
    var moduleCache = {};

    function readText(p) { return io.readFileSync(p); }

    function findFile(base) {
        var candidates = [base, base + ".js", base + ".json", base + "/index.js"];
        for (var i = 0; i < candidates.length; i++) {
            var c = candidates[i];
            if (exists(c) && !isDir(c)) return c;
        }
        if (exists(base) && isDir(base) && exists(base + "/package.json")) {
            try {
                var main = JSON.parse(readText(base + "/package.json")).main;
                if (main) return findFile(base + "/" + main.replace(/^\.\//, ""));
            } catch (e) {}
        }
        return null;
    }

    function resolveModule(name, fromDir) {
        var roots = [];
        if (fromDir) roots.push(fromDir);
        roots.push(gameRoot, path.dirname(gameRoot));
        if (name.charAt(0) === "/") return findFile(name);
        var relative = name.indexOf("./") === 0 || name.indexOf("../") === 0;
        for (var i = 0; i < roots.length; i++) {
            var f = relative ? findFile(path.normalize(roots[i] + "/" + name))
                             : findFile(path.normalize(roots[i] + "/node_modules/" + name));
            if (f) return f;
        }
        return null;
    }

    function loadModule(file) {
        if (moduleCache[file]) return moduleCache[file].exports;
        var module = { exports: {}, filename: file, id: file, loaded: false, children: [], paths: [] };
        moduleCache[file] = module;
        try {
            if (/\.json$/i.test(file)) {
                module.exports = JSON.parse(readText(file));
            } else {
                var dir = path.dirname(file);
                var localRequire = function(n) { return nodeRequire(n, dir); };
                localRequire.resolve = function(n) { return resolveModule(n, dir) || n; };
                localRequire.cache = moduleCache;
                var fn = new Function("exports", "require", "module", "__filename", "__dirname",
                                      readText(file) + "\n//# sourceURL=" + file);
                fn.call(module.exports, module.exports, localRequire, module, file, dir);
            }
        } catch (e) {
            delete moduleCache[file];
            throw e;
        }
        module.loaded = true;
        return module.exports;
    }

    function nodeRequire(name, fromDir) {
        name = String(name);
        var bare = name.indexOf("node:") === 0 ? name.substring(5) : name;
        if (Object.prototype.hasOwnProperty.call(builtins, bare)) return builtins[bare];
        /* nw.gui has its own stand-in further down the chain */
        if (bare !== "nw.gui") {
            var file = resolveModule(bare, fromDir);
            if (/greenworks/i.test(bare)) {
                /* A game may ship its own plain-JS greenworks module (for example a build without Steam); that one is
                   used. The native Windows module (.node) cannot be loaded here, so then the Steam stand-in answers. */
                if (file && /\.js$/i.test(file)) {
                    try { return loadModule(file); } catch (e) { /* fall back to the stand-in */ }
                }
            } else if (file) {
                return loadModule(file);
            }
        }
        return previousRequire(name);
    }

    var previousRequire = G.require;
    G.require = function(name) { return nodeRequire(name, null); };
    G.require.resolve = function(name) { return resolveModule(String(name), null) || String(name); };
    G.require.cache = moduleCache;
    G.require.main = proc.mainModule;
})();
