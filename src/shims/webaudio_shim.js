/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

(function() {
    "use strict";

    /* Web Audio API shim: AudioContext and node classes backed by the native
       SoLoud engine through __native_audio. */

    var _native = globalThis.__native_audio;
    var _initialized = false;
    var _startTime = 0; /* monotonic time base for currentTime */
    var _contextInstance = null;

    function _now() {
        if (typeof performance !== "undefined" && performance.now) {
            return performance.now() / 1000.0;
        }
        return Date.now() / 1000.0;
    }

    /* ---- AudioParam ---- */

    function AudioParam(defaultValue) {
        this.value = defaultValue !== undefined ? defaultValue : 0;
        this.defaultValue = this.value;
        this.minValue = -3.4028235e38;
        this.maxValue = 3.4028235e38;
        this._scheduled = [];
        this._owner = null;
        this._paramName = "";
    }

    AudioParam.prototype.setValueAtTime = function(value, startTime) {
        this.value = value;
        this._applyToNative();
        return this;
    };

    /* RPG Maker uses this for fade-in/out; the owning node maps it to a native fade. */
    AudioParam.prototype.linearRampToValueAtTime = function(value, endTime) {
        var duration = endTime - (_contextInstance ? _contextInstance.currentTime : 0);
        if (duration < 0) duration = 0;

        if (this._owner && this._owner._applyFade) {
            this._owner._applyFade(this._paramName, value, duration);
        } else {
            this.value = value;
            this._applyToNative();
        }
        return this;
    };

    AudioParam.prototype.exponentialRampToValueAtTime = function(value, endTime) {
        this.value = value;
        this._applyToNative();
        return this;
    };

    AudioParam.prototype.setTargetAtTime = function(target, startTime, timeConstant) {
        this.value = target;
        this._applyToNative();
        return this;
    };

    AudioParam.prototype.cancelScheduledValues = function(startTime) {
        this._scheduled = [];
        return this;
    };

    AudioParam.prototype._applyToNative = function() {
        if (this._owner && this._owner._applyParam) {
            this._owner._applyParam(this._paramName, this.value);
        }
    };

    /* ---- AudioNode ---- */

    function AudioNode() {
        this.context = null;
        this.numberOfInputs = 1;
        this.numberOfOutputs = 1;
        this._connectedTo = null;
    }

    AudioNode.prototype.connect = function(destination) {
        this._connectedTo = destination;
        return destination;
    };

    AudioNode.prototype.disconnect = function() {
        this._connectedTo = null;
    };

    /* ---- AudioDestinationNode ---- */

    function AudioDestinationNode(ctx) {
        AudioNode.call(this);
        this.context = ctx;
        this.maxChannelCount = 2;
        this.numberOfInputs = 1;
        this.numberOfOutputs = 0;
    }
    AudioDestinationNode.prototype = Object.create(AudioNode.prototype);
    AudioDestinationNode.prototype.constructor = AudioDestinationNode;

    /* ---- GainNode ---- */

    function GainNode(ctx) {
        AudioNode.call(this);
        this.context = ctx;
        this.gain = new AudioParam(1.0);
        this.gain._owner = this;
        this.gain._paramName = "gain";
        this._voiceHandle = 0;
    }
    GainNode.prototype = Object.create(AudioNode.prototype);
    GainNode.prototype.constructor = GainNode;

    GainNode.prototype._applyParam = function(name, value) {
        if (name === "gain" && this._voiceHandle && _native) {
            _native.setVolume(this._voiceHandle, value);
        }
    };

    GainNode.prototype._applyFade = function(name, targetValue, duration) {
        if (name === "gain" && this._voiceHandle && _native) {
            _native.fadeVolume(this._voiceHandle, targetValue, duration);
        }
        this.gain.value = targetValue;
    };

    /* ---- PannerNode ---- */

    function PannerNode(ctx) {
        AudioNode.call(this);
        this.context = ctx;
        this.panningModel = "equalpower";
        this.distanceModel = "inverse";
        this._pan = 0;
        this._voiceHandle = 0;
    }
    PannerNode.prototype = Object.create(AudioNode.prototype);
    PannerNode.prototype.constructor = PannerNode;

    /* RPG Maker calls setPosition(pan, 0, 1-abs(pan)); x is the pan amount. */
    PannerNode.prototype.setPosition = function(x, y, z) {
        this._pan = x;
        if (this._voiceHandle && _native) {
            _native.setPan(this._voiceHandle, x);
        }
    };

    PannerNode.prototype.setOrientation = function(x, y, z) {
    };

    /* ---- AudioBuffer ---- */

    /* Decoded audio is never freed explicitly (browsers GC it), so release the
       backing native source when the JS wrapper is collected. */
    var _audioBufferRegistry = (typeof FinalizationRegistry === "function")
        ? new FinalizationRegistry(function(handle) {
            if (handle && _native && _native.freeSource) {
                try { _native.freeSource(handle); } catch (e) { /* ignore */ }
            }
        })
        : null;

    function AudioBuffer(options) {
        this.sampleRate = (options && options.sampleRate) || 44100;
        this.length = (options && options.length) || 0;
        this.duration = this.length / this.sampleRate;
        this.numberOfChannels = (options && options.numberOfChannels) || 1;
        this._nativeHandle = 0;
    }

    /* A sound that does not play leaves no trace otherwise; each cause is reported once to the compat log. */
    function _audioNote(key, detail) {
        if (typeof globalThis.__compat_report === "function") globalThis.__compat_report("audio", key, detail);
    }

    var _SHARE_MAX_BYTES = 1 << 20;     /* files up to this size are shared (sound effects, short jingles) */
    var _SHARE_MAX_ENTRIES = 96;
    var _sharedBuffers = new Map();     /* content key -> the AudioBuffer that owns the native source */

    function _attachNativeHandle(buffer, handle) {
        buffer._nativeHandle = handle;
        if (handle && _audioBufferRegistry) {
            _audioBufferRegistry.register(buffer, handle);
        }
    }

    /* Raw samples are not exposed; RPG Maker never inspects them. */
    AudioBuffer.prototype.getChannelData = function(channel) {
        return new Float32Array(this.length);
    };

    /* ---- AudioBufferSourceNode ---- */

    function AudioBufferSourceNode(ctx) {
        AudioNode.call(this);
        this.context = ctx;
        this.buffer = null;
        this.loop = false;
        this.loopStart = 0;
        this.loopEnd = 0;
        this.playbackRate = new AudioParam(1.0);
        this.playbackRate._owner = this;
        this.playbackRate._paramName = "playbackRate";
        this.onended = null;
        this._voiceHandle = 0;
        this._started = false;
        this._stopped = false;
        this._endedFired = false;
    }
    AudioBufferSourceNode.prototype = Object.create(AudioNode.prototype);
    AudioBufferSourceNode.prototype.constructor = AudioBufferSourceNode;

    /* Fires onended at most once (on stop() or natural end). */
    function _fireEnded(node) {
        if (node._endedFired) return;
        node._endedFired = true;
        if (typeof node.onended === "function") {
            var cb = node.onended;
            Promise.resolve().then(function() {
                try { cb.call(node); } catch (e) { /* ignore */ }
            });
        }
    }

    /* The native backend has no completion callback, so non-looping voices are
       polled once per frame to fire onended (RPG Maker resumes BGM after an ME
       from that handler). */
    var _activeSources = [];
    var _pinnedLoops = [];      /* looping nodes kept reachable while playing; not polled */
    var _pollScheduled = false;
    function _pollActiveSources() {
        _pollScheduled = false;
        for (var i = _activeSources.length - 1; i >= 0; i--) {
            var n = _activeSources[i];
            var finished = n._stopped || n._endedFired || !n._voiceHandle ||
                           !_native || !_native.isPlaying(n._voiceHandle);
            if (finished) {
                _activeSources.splice(i, 1);
                if (!n._stopped) _fireEnded(n);
            }
        }
        _schedulePoll();
    }
    function _schedulePoll() {
        if (_pollScheduled || _activeSources.length === 0) return;
        _pollScheduled = true;
        if (typeof requestAnimationFrame === "function") {
            requestAnimationFrame(_pollActiveSources);
        } else if (typeof setTimeout === "function") {
            setTimeout(_pollActiveSources, 16);
        }
    }

    AudioBufferSourceNode.prototype.start = function(when, offset, duration) {
        if (this._started) return;
        this._started = true;

        if (!this.buffer || !this.buffer._nativeHandle || !_native) {
            _audioNote("start without a decoded buffer", !this.buffer ? "no buffer set" :
                       (!this.buffer._nativeHandle ? "buffer has no native source (duration " + this.buffer.duration + ")" : "no native audio"));
            return;
        }

        var gainNode = null;
        var pannerNode = null;
        var node = this._connectedTo;
        while (node) {
            if (node instanceof GainNode) gainNode = node;
            if (node instanceof PannerNode) pannerNode = node;
            node = node._connectedTo;
        }

        var volume = gainNode ? gainNode.gain.value : 1.0;
        var pitch = this.playbackRate.value;
        var pan = pannerNode ? pannerNode._pan : 0.0;
        var looping = this.loop;

        /* Web Audio has no bus concept; the AudioManager routes externally. */
        var bus = _native.BUS_SE;

        var voice = _native.play(this.buffer._nativeHandle, bus, volume, pitch, pan, looping);
        this._voiceHandle = voice;
        if (!voice) _audioNote("play returned no voice", "volume " + volume + ", pitch " + pitch + ", pan " + pan +
                               ", loop " + looping + ", duration " + this.buffer.duration);

        /* Let gain/panner nodes apply live parameter changes to this voice. */
        if (gainNode) gainNode._voiceHandle = voice;
        if (pannerNode) pannerNode._voiceHandle = voice;

        if (offset && offset > 0 && voice) {
            _native.seek(voice, offset);
        }

        /* Looping voices are not polled (isPlaying() takes SoLoud's audio mutex);
           they are only pinned so the FinalizationRegistry cannot free the
           buffer mid-play. stop() unpins. */
        if (voice && !looping) {
            _activeSources.push(this);
            _schedulePoll();
        } else if (voice) {
            _pinnedLoops.push(this);
        }
    };

    AudioBufferSourceNode.prototype.stop = function(when) {
        if (this._stopped) return;
        this._stopped = true;
        var idx = _activeSources.indexOf(this);
        if (idx >= 0) _activeSources.splice(idx, 1);
        var pidx = _pinnedLoops.indexOf(this);
        if (pidx >= 0) _pinnedLoops.splice(pidx, 1);
        if (this._voiceHandle && _native) {
            _native.stop(this._voiceHandle);
        }
        _fireEnded(this);
    };

    AudioBufferSourceNode.prototype._applyParam = function(name, value) {
        if (name === "playbackRate" && this._voiceHandle && _native) {
            _native.setPitch(this._voiceHandle, value);
        }
    };

    AudioBufferSourceNode.prototype._applyFade = function(name, targetValue, duration) {
        if (name === "playbackRate") {
            /* Pitch changes are instant in SoLoud. */
            if (this._voiceHandle && _native) {
                _native.setPitch(this._voiceHandle, targetValue);
            }
            this.playbackRate.value = targetValue;
        }
    };

    /* ---- AudioContext ---- */

    function AudioContext() {
        if (!_initialized && _native) {
            _native.init();
            _initialized = true;
            _startTime = _now();
        }
        this.destination = new AudioDestinationNode(this);
        this.sampleRate = 44100;
        this.state = "running";
        this._resumeResolve = null;
        _contextInstance = this;
    }

    Object.defineProperty(AudioContext.prototype, "currentTime", {
        get: function() {
            return _now() - _startTime;
        },
        configurable: true
    });

    AudioContext.prototype.resume = function() {
        this.state = "running";
        var self = this;
        return new Promise(function(resolve) {
            resolve();
        });
    };

    AudioContext.prototype.suspend = function() {
        this.state = "suspended";
        return new Promise(function(resolve) { resolve(); });
    };

    AudioContext.prototype.close = function() {
        this.state = "closed";
        return new Promise(function(resolve) { resolve(); });
    };

    AudioContext.prototype.createGain = function() {
        return new GainNode(this);
    };

    AudioContext.prototype.createPanner = function() {
        return new PannerNode(this);
    };

    AudioContext.prototype.createBufferSource = function() {
        return new AudioBufferSourceNode(this);
    };

    /* ---- Pass-through effect nodes ----
       Audio plugins insert compressors, filters and analysers between the
       source and the destination. The native mixer has no such stages, so
       these nodes keep their parameters and simply forward the connection;
       the chain walk in AudioBufferSourceNode.start skips them. */
    function passThroughNode(ctx, params, extra) {
        var node = new AudioNode();
        node.context = ctx;
        for (var name in params) {
            node[name] = new AudioParam(params[name]);
            node[name]._owner = node;
            node[name]._paramName = name;
        }
        for (var key in extra) node[key] = extra[key];
        return node;
    }

    AudioContext.prototype.createDynamicsCompressor = function() {
        return passThroughNode(this, { threshold: -24, knee: 30, ratio: 12, attack: 0.003, release: 0.25 },
                               { reduction: 0 });
    };

    AudioContext.prototype.createBiquadFilter = function() {
        return passThroughNode(this, { frequency: 350, detune: 0, Q: 1, gain: 0 }, {
            type: "lowpass",
            getFrequencyResponse: function(freqs, mag, phase) {
                for (var i = 0; i < freqs.length; i++) { mag[i] = 1; phase[i] = 0; }
            }
        });
    };

    AudioContext.prototype.createDelay = function(maxDelayTime) {
        return passThroughNode(this, { delayTime: 0 }, { maxDelayTime: maxDelayTime || 1 });
    };

    AudioContext.prototype.createConvolver = function() {
        return passThroughNode(this, {}, { buffer: null, normalize: true });
    };

    AudioContext.prototype.createWaveShaper = function() {
        return passThroughNode(this, {}, { curve: null, oversample: "none" });
    };

    AudioContext.prototype.createChannelSplitter = function(count) {
        var node = passThroughNode(this, {}, {});
        node.numberOfOutputs = count || 6;
        return node;
    };

    AudioContext.prototype.createChannelMerger = function(count) {
        var node = passThroughNode(this, {}, {});
        node.numberOfInputs = count || 6;
        return node;
    };

    AudioContext.prototype.createAnalyser = function() {
        var node = passThroughNode(this, {}, {
            fftSize: 2048, frequencyBinCount: 1024, minDecibels: -100, maxDecibels: -30,
            smoothingTimeConstant: 0.8
        });
        var zero = function(array) { for (var i = 0; i < array.length; i++) array[i] = 0; };
        var mid = function(array) { for (var i = 0; i < array.length; i++) array[i] = 128; };
        node.getByteFrequencyData = zero;
        node.getFloatFrequencyData = function(array) { for (var i = 0; i < array.length; i++) array[i] = -100; };
        node.getByteTimeDomainData = mid;
        node.getFloatTimeDomainData = zero;
        return node;
    };

    /* StereoPannerNode: a PannerNode driven by a `pan` AudioParam, so the
       source's chain walk picks up the pan like the positional panner. */
    AudioContext.prototype.createStereoPanner = function() {
        var node = new PannerNode(this);
        node.pan = new AudioParam(0);
        node.pan._owner = node;
        node.pan._paramName = "pan";
        node._applyParam = function(name, value) {
            if (name === "pan") this.setPosition(value, 0, 1 - Math.abs(value));
        };
        return node;
    };

    AudioContext.prototype.decodeAudioData = function(arrayBuffer, successCallback, errorCallback) {
        var self = this;
        var promise = new Promise(function(resolve, reject) {
            if (!_native) {
                var err = new Error("Native audio not available");
                reject(err);
                return;
            }

            try {
                var data = arrayBuffer;
                if (arrayBuffer instanceof ArrayBuffer) {
                    data = arrayBuffer;
                }

                /* A sound effect is decoded again for every play, and a decoded source is only released when the
                   collector finalizes its wrapper, which in a long run comes far too late for the 512 source slots:
                   once they were all taken every effect failed to decode (840 failures in one console run). Short
                   files are decoded once and shared by content. */
                var cacheKey = null;
                if (data && data.byteLength > 0 && data.byteLength <= _SHARE_MAX_BYTES) {
                    var bytes = new Uint8Array(data), h1 = 0x811c9dc5, h2 = 0x12345678;
                    for (var bi = 0; bi < bytes.length; bi++) {
                        h1 = Math.imul(h1 ^ bytes[bi], 16777619);
                        h2 = Math.imul(h2 + bytes[bi], 2246822519) ^ (h2 >>> 13);
                    }
                    cacheKey = bytes.length + ":" + (h1 >>> 0) + ":" + (h2 >>> 0);
                    var shared = _sharedBuffers.get(cacheKey);
                    if (shared) {
                        _sharedBuffers.delete(cacheKey);      /* most recently used goes last */
                        _sharedBuffers.set(cacheKey, shared);
                        var again = new AudioBuffer({ sampleRate: shared.sampleRate, length: shared.length,
                                                      numberOfChannels: shared.numberOfChannels });
                        again.duration = shared.duration;
                        again._nativeHandle = shared._nativeHandle;
                        again._owner = shared;                /* keeps the source alive while this wrapper is */
                        resolve(again);
                        return;
                    }
                }

                var handle = _native.loadFromMemory(data);
                if (!handle) {
                    _audioNote("decodeAudioData failed", "the native decoder rejected " +
                               (data && data.byteLength) + " bytes");
                    reject(new Error("Failed to decode audio data"));
                    return;
                }

                var duration = _native.getDuration(handle);
                var sampleRate = 44100; /* SoLoud default */
                var length = Math.floor(duration * sampleRate);

                var buffer = new AudioBuffer({
                    sampleRate: sampleRate,
                    length: length,
                    numberOfChannels: 2
                });
                buffer.duration = duration;
                _attachNativeHandle(buffer, handle);
                if (cacheKey) {
                    _sharedBuffers.set(cacheKey, buffer);
                    if (_sharedBuffers.size > _SHARE_MAX_ENTRIES)
                        _sharedBuffers.delete(_sharedBuffers.keys().next().value);
                }

                resolve(buffer);
            } catch (e) {
                reject(e);
            }
        });

        if (typeof successCallback === "function") {
            promise.then(successCallback, errorCallback || function() {});
        }

        return promise;
    };

    AudioContext.prototype.createBuffer = function(numberOfChannels, length, sampleRate) {
        return new AudioBuffer({
            numberOfChannels: numberOfChannels,
            length: length,
            sampleRate: sampleRate
        });
    };

    /* ---- Html5Audio stub (referenced by RPG Maker MZ, never used here) ---- */

    var Html5Audio = {
        _initialized: false,
        _unlocked: false,
        _audioElement: null,
        _volume: 1,
        _loadListeners: [],
        _hasError: false,

        setup: function(url) {
        },

        initialize: function() {
            this._initialized = true;
            return true;
        },

        setStaticSe: function(se) {
        },

        clear: function() {
        },

        play: function(loop, offset) {
        },

        stop: function() {
        },

        fadeIn: function(duration) {
        },

        fadeOut: function(duration) {
        },

        seek: function() {
            return 0;
        },

        addLoadListener: function(listener) {
        },

        isReady: function() {
            return false;
        },

        isError: function() {
            return false;
        },

        isPlaying: function() {
            return false;
        }
    };

    Object.defineProperty(Html5Audio, "url", {
        get: function() { return ""; },
        configurable: true
    });

    Object.defineProperty(Html5Audio, "volume", {
        get: function() { return this._volume; },
        set: function(v) { this._volume = v; },
        configurable: true
    });

    /* ---- Exports ---- */

    globalThis.AudioContext = AudioContext;
    globalThis.webkitAudioContext = AudioContext;
    globalThis.AudioBuffer = AudioBuffer;
    globalThis.AudioBufferSourceNode = AudioBufferSourceNode;
    globalThis.GainNode = GainNode;
    globalThis.PannerNode = PannerNode;
    globalThis.AudioParam = AudioParam;
    globalThis.AudioNode = AudioNode;
    globalThis.AudioDestinationNode = AudioDestinationNode;
    globalThis.Html5Audio = Html5Audio;

    if (typeof globalThis.window !== "undefined") {
        globalThis.window.AudioContext = AudioContext;
        globalThis.window.webkitAudioContext = AudioContext;
    }

})();
