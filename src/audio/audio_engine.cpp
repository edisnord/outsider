/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "audio/audio_engine.h"

#include "soloud.h"
#include "soloud_bus.h"
#include "soloud_wav.h"
#include "soloud_wavstream.h"
#include "soloud_thread.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_AUDIO_SOURCES 512

/* A loaded source is either a Wav (fully decoded) or a WavStream (streamed). */
typedef struct {
    bool           in_use;
    bool           is_stream;
    SoLoud::Wav       *wav;
    SoLoud::WavStream *stream;
} SourceEntry;

static SoLoud::Soloud  s_soloud;
static SoLoud::Bus     s_buses[AUDIO_BUS_COUNT];
static unsigned int     s_bus_handles[AUDIO_BUS_COUNT];
static SourceEntry      s_sources[MAX_AUDIO_SOURCES];
static bool             s_initialized = false;
static bool             s_disabled = false;

/* Free-list of source slots (slot 0 is reserved as AUDIO_SOURCE_INVALID). */
static int              s_free_head = -1;
static int              s_next_free[MAX_AUDIO_SOURCES];
static int              s_loaded_count = 0;

static int              s_peak_voices = 0;

/* PCM stream source: a ring buffer the main thread fills and the SoLoud
   mixer thread drains. SoLoud hands getAudio() a planar buffer (one run of
   aBufferSize floats per channel), so samples are de-interleaved on read. */

#define STREAM_RING_SECONDS 4
#define MAX_AUDIO_STREAMS   8

class PcmStream;

class PcmStreamInstance : public SoLoud::AudioSourceInstance {
public:
    explicit PcmStreamInstance(PcmStream *parent) : mParent(parent) {}
    virtual unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead,
                                  unsigned int aBufferSize);
    virtual bool hasEnded();
private:
    PcmStream *mParent;
};

class PcmStream : public SoLoud::AudioSource {
public:
    PcmStream(int sample_rate, int channels)
        : mRing(NULL), mCapacity(0), mHead(0), mCount(0), mEnded(false)
    {
        mChannels = (unsigned int)channels;
        mBaseSamplerate = (float)sample_rate;
        mCapacity = (size_t)sample_rate * STREAM_RING_SECONDS;
        mRing = (float *)malloc(mCapacity * (size_t)channels * sizeof(float));
        mMutex = SoLoud::Thread::createMutex();
    }
    virtual ~PcmStream()
    {
        stop();
        SoLoud::Thread::destroyMutex(mMutex);
        free(mRing);
    }
    virtual SoLoud::AudioSourceInstance *createInstance()
    {
        return new PcmStreamInstance(this);
    }

    void push(const float *interleaved, size_t frames)
    {
        if (!mRing) return;
        SoLoud::Thread::lockMutex(mMutex);
        if (frames > mCapacity) {
            interleaved += (frames - mCapacity) * mChannels;
            frames = mCapacity;
        }
        /* Drop the oldest data when full. */
        if (mCount + frames > mCapacity) {
            size_t drop = mCount + frames - mCapacity;
            mHead = (mHead + drop) % mCapacity;
            mCount -= drop;
        }
        size_t tail = (mHead + mCount) % mCapacity;
        for (size_t i = 0; i < frames; i++) {
            memcpy(mRing + tail * mChannels, interleaved + i * mChannels,
                   mChannels * sizeof(float));
            tail = (tail + 1) % mCapacity;
        }
        mCount += frames;
        SoLoud::Thread::unlockMutex(mMutex);
    }

    /* Called from the mixer thread. */
    unsigned int read(float *planar, unsigned int frames, unsigned int stride)
    {
        SoLoud::Thread::lockMutex(mMutex);
        unsigned int avail = (unsigned int)(mCount < frames ? mCount : frames);
        for (unsigned int i = 0; i < avail; i++) {
            const float *src = mRing + mHead * mChannels;
            for (unsigned int c = 0; c < mChannels; c++) {
                planar[c * stride + i] = src[c];
            }
            mHead = (mHead + 1) % mCapacity;
        }
        mCount -= avail;
        SoLoud::Thread::unlockMutex(mMutex);
        /* Underrun: pad with silence rather than ending the voice. */
        for (unsigned int i = avail; i < frames; i++) {
            for (unsigned int c = 0; c < mChannels; c++) planar[c * stride + i] = 0.0f;
        }
        return frames;
    }

    size_t queued()
    {
        SoLoud::Thread::lockMutex(mMutex);
        size_t n = mCount;
        SoLoud::Thread::unlockMutex(mMutex);
        return n;
    }

    void markEnded() { mEnded = true; }
    bool ended() const { return mEnded; }

private:
    float  *mRing;
    size_t  mCapacity;   /* frames */
    size_t  mHead;       /* first unread frame */
    size_t  mCount;      /* unread frames */
    bool    mEnded;
    void   *mMutex;
};

unsigned int PcmStreamInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead,
                                         unsigned int aBufferSize)
{
    return mParent->read(aBuffer, aSamplesToRead, aBufferSize);
}

bool PcmStreamInstance::hasEnded()
{
    return mParent->ended();
}

typedef struct {
    PcmStream   *source;
    unsigned int voice;
} StreamEntry;

static StreamEntry s_streams[MAX_AUDIO_STREAMS + 1];   /* index 0 unused */

static PcmStream *get_stream(AudioStreamHandle h)
{
    if (h <= 0 || h > MAX_AUDIO_STREAMS) return NULL;
    return s_streams[h].source;
}

extern "C" AudioStreamHandle audio_stream_create(int sample_rate, int channels)
{
    if (!s_initialized || sample_rate <= 0 || channels < 1 || channels > 2) {
        return AUDIO_STREAM_INVALID;
    }
    for (int i = 1; i <= MAX_AUDIO_STREAMS; i++) {
        if (!s_streams[i].source) {
            PcmStream *src = new (std::nothrow) PcmStream(sample_rate, channels);
            if (!src) return AUDIO_STREAM_INVALID;
            s_streams[i].source = src;
            s_streams[i].voice = s_soloud.play(*src);
            return i;
        }
    }
    return AUDIO_STREAM_INVALID;
}

extern "C" void audio_stream_push(AudioStreamHandle stream, const float *interleaved, int frames)
{
    PcmStream *src = get_stream(stream);
    if (src && interleaved && frames > 0) src->push(interleaved, (size_t)frames);
}

extern "C" int audio_stream_queued_frames(AudioStreamHandle stream)
{
    PcmStream *src = get_stream(stream);
    return src ? (int)src->queued() : 0;
}

extern "C" void audio_stream_set_volume(AudioStreamHandle stream, float volume)
{
    if (get_stream(stream)) s_soloud.setVolume(s_streams[stream].voice, volume);
}

extern "C" void audio_stream_set_paused(AudioStreamHandle stream, bool paused)
{
    if (get_stream(stream)) s_soloud.setPause(s_streams[stream].voice, paused);
}

extern "C" void audio_stream_destroy(AudioStreamHandle stream)
{
    PcmStream *src = get_stream(stream);
    if (!src) return;
    src->markEnded();
    s_soloud.stop(s_streams[stream].voice);
    delete src;
    s_streams[stream].source = NULL;
    s_streams[stream].voice = 0;
}

static void destroy_all_streams(void)
{
    for (int i = 1; i <= MAX_AUDIO_STREAMS; i++) {
        if (s_streams[i].source) audio_stream_destroy(i);
    }
}

static void init_free_list(void)
{
    s_free_head = 1;
    for (int i = 1; i < MAX_AUDIO_SOURCES - 1; i++) {
        s_next_free[i] = i + 1;
    }
    s_next_free[MAX_AUDIO_SOURCES - 1] = -1;
    s_loaded_count = 0;
}

static AudioSourceHandle alloc_source(void)
{
    if (s_free_head < 0) {
        return AUDIO_SOURCE_INVALID;
    }
    int slot = s_free_head;
    s_free_head = s_next_free[slot];
    memset(&s_sources[slot], 0, sizeof(SourceEntry));
    s_sources[slot].in_use = true;
    s_loaded_count++;
    return (AudioSourceHandle)slot;
}

static void free_source_slot(int slot)
{
    if (slot < 1 || slot >= MAX_AUDIO_SOURCES) return;
    if (!s_sources[slot].in_use) return;
    s_sources[slot].in_use = false;
    s_next_free[slot] = s_free_head;
    s_free_head = slot;
    if (s_loaded_count > 0) s_loaded_count--;
}

static SourceEntry *get_source(AudioSourceHandle handle)
{
    if (handle == AUDIO_SOURCE_INVALID || handle >= MAX_AUDIO_SOURCES)
        return NULL;
    SourceEntry *e = &s_sources[handle];
    return e->in_use ? e : NULL;
}

static SoLoud::AudioSource *get_audio_source(AudioSourceHandle handle)
{
    SourceEntry *e = get_source(handle);
    if (!e) return NULL;
    if (e->is_stream) return e->stream;
    return e->wav;
}

extern "C" void audio_engine_set_disabled(bool disabled)
{
    s_disabled = disabled;
}

extern "C" bool audio_engine_init(void)
{
    if (s_disabled) return true;
    if (s_initialized) return true;

    SoLoud::result res = s_soloud.init(
        SoLoud::Soloud::CLIP_ROUNDOFF,
        SoLoud::Soloud::AUTO,
        48000,                 /* the PS5 device runs at 48 kHz; SoLoud hands a backend 44100 when asked for 0, which made SDL resample every block */
        0,                     /* default buffer size */
        2                      /* stereo */
    );

    if (res != SoLoud::SO_NO_ERROR) {
        /* No audio device (headless): null backend keeps the API functional. */
        res = s_soloud.init(
            SoLoud::Soloud::CLIP_ROUNDOFF,
            SoLoud::Soloud::NULLDRIVER,
            44100,
            1024,
            2
        );
        if (res != SoLoud::SO_NO_ERROR) {
            fprintf(stderr, "audio_engine_init: failed to initialize SoLoud (%d)\n", res);
            return false;
        }
        printf("Audio engine initialized (null backend — no audio output).\n");
    } else {
        printf("Audio engine initialized.\n");
    }

    /* Each bus is a permanent voice on the main mixer. */
    for (int i = 0; i < AUDIO_BUS_COUNT; i++) {
        s_bus_handles[i] = s_soloud.play(s_buses[i]);
    }

    memset(s_sources, 0, sizeof(s_sources));
    init_free_list();
    s_peak_voices = 0;
    s_initialized = true;
    return true;
}

extern "C" void audio_engine_shutdown(void)
{
    if (!s_initialized) return;

    destroy_all_streams();
    s_soloud.stopAll();

    for (int i = 1; i < MAX_AUDIO_SOURCES; i++) {
        if (s_sources[i].in_use) {
            delete s_sources[i].wav;
            delete s_sources[i].stream;
            memset(&s_sources[i], 0, sizeof(SourceEntry));
        }
    }
    init_free_list();

    s_soloud.deinit();
    s_initialized = false;
    printf("Audio engine shut down.\n");
}

extern "C" bool audio_engine_is_initialized(void)
{
    return s_initialized;
}

extern "C" AudioSourceHandle audio_load(const char *path)
{
    if (s_disabled) return 1; /* dummy handle: audio is disabled */
    if (!s_initialized || !path) return AUDIO_SOURCE_INVALID;

    AudioSourceHandle h = alloc_source();
    if (h == AUDIO_SOURCE_INVALID) {
        fprintf(stderr, "audio_load: no free source slots\n");
        return AUDIO_SOURCE_INVALID;
    }

    SourceEntry *e = &s_sources[h];
    e->wav = new (std::nothrow) SoLoud::Wav();
    if (!e->wav) {
        free_source_slot((int)h);
        return AUDIO_SOURCE_INVALID;
    }

    SoLoud::result res = e->wav->load(path);
    if (res != SoLoud::SO_NO_ERROR) {
        fprintf(stderr, "audio_load: failed to load '%s' (error %d)\n", path, res);
        delete e->wav;
        e->wav = NULL;
        free_source_slot((int)h);
        return AUDIO_SOURCE_INVALID;
    }

    e->is_stream = false;
    return h;
}

extern "C" AudioSourceHandle audio_load_stream(const char *path)
{
    if (s_disabled) return 1;
    if (!s_initialized || !path) return AUDIO_SOURCE_INVALID;

    AudioSourceHandle h = alloc_source();
    if (h == AUDIO_SOURCE_INVALID) {
        fprintf(stderr, "audio_load_stream: no free source slots\n");
        return AUDIO_SOURCE_INVALID;
    }

    SourceEntry *e = &s_sources[h];
    e->stream = new (std::nothrow) SoLoud::WavStream();
    if (!e->stream) {
        free_source_slot((int)h);
        return AUDIO_SOURCE_INVALID;
    }

    SoLoud::result res = e->stream->load(path);
    if (res != SoLoud::SO_NO_ERROR) {
        fprintf(stderr, "audio_load_stream: failed to load '%s' (error %d)\n", path, res);
        delete e->stream;
        e->stream = NULL;
        free_source_slot((int)h);
        return AUDIO_SOURCE_INVALID;
    }

    e->is_stream = true;
    return h;
}

/* stb_vorbis is compiled into libsoloud; only this one function is needed here. */
extern "C" int stb_vorbis_decode_memory(const unsigned char *mem, int len, int *channels, int *sample_rate, short **output);

extern "C" AudioSourceHandle audio_load_from_memory(const uint8_t *data, uint32_t size)
{
    if (s_disabled) return 1;
    if (!s_initialized || !data || size == 0) return AUDIO_SOURCE_INVALID;

    AudioSourceHandle h = alloc_source();
    if (h == AUDIO_SOURCE_INVALID) return AUDIO_SOURCE_INVALID;

    SourceEntry *e = &s_sources[h];
    e->wav = new (std::nothrow) SoLoud::Wav();
    if (!e->wav) {
        free_source_slot((int)h);
        return AUDIO_SOURCE_INVALID;
    }

    SoLoud::result res = SoLoud::UNKNOWN_ERROR;
    if (size > 4 && memcmp(data, "OggS", 4) == 0) {
        /* SoLoud sizes its buffer from the length the Ogg file claims and then writes whatever the decoder returns;
           files whose claimed length is too short (some encoders, or files cut by a game's own tools) overflowed
           the heap and took the whole program down. Decode the file completely here and hand over the result. */
        int channels = 0, rate = 0;
        short *pcm = NULL;
        int frames = stb_vorbis_decode_memory(data, (int)size, &channels, &rate, &pcm);
        if (frames > 0 && channels > 0 && pcm) {
            int use = channels > 8 ? 8 : channels;
            float *planar = new (std::nothrow) float[(size_t)frames * use];
            if (planar) {
                for (int c = 0; c < use; c++)
                    for (int i = 0; i < frames; i++)
                        planar[(size_t)c * frames + i] = pcm[(size_t)i * channels + c] * (1.0f / 32768.0f);
                res = e->wav->loadRawWave(planar, (unsigned int)((size_t)frames * use), (float)rate, (unsigned int)use,
                                          false, true);
                if (res != SoLoud::SO_NO_ERROR) delete[] planar;
            }
        }
        free(pcm);
    } else {
        res = e->wav->loadMem(
            const_cast<unsigned char *>(data), size,
            true,  /* copy */
            true   /* take ownership of the copy */
        );
    }
    if (res != SoLoud::SO_NO_ERROR) {
        fprintf(stderr, "audio_load_from_memory: failed (error %d)\n", res);
        delete e->wav;
        e->wav = NULL;
        free_source_slot((int)h);
        return AUDIO_SOURCE_INVALID;
    }

    e->is_stream = false;
    return h;
}

extern "C" void audio_source_free(AudioSourceHandle handle)
{
    if (s_disabled) return;
    SourceEntry *e = get_source(handle);
    if (!e) return;

    /* Stop voices first so the audio thread never touches freed memory. */
    if (e->wav) s_soloud.stopAudioSource(*e->wav);
    if (e->stream) s_soloud.stopAudioSource(*e->stream);

    delete e->wav;
    delete e->stream;
    e->wav = NULL;
    e->stream = NULL;
    free_source_slot((int)handle);
}

extern "C" AudioVoice audio_play(AudioSourceHandle source, AudioBus bus,
                                  float volume, float pitch, float pan, bool loop)
{
    if (s_disabled) return 1; /* dummy voice */
    if (!s_initialized) return AUDIO_VOICE_INVALID;
    SoLoud::AudioSource *src = get_audio_source(source);
    if (!src) return AUDIO_VOICE_INVALID;
    if (bus < 0 || bus >= AUDIO_BUS_COUNT) return AUDIO_VOICE_INVALID;

    src->setLooping(loop);

    unsigned int voice = s_buses[bus].play(*src, volume, pan);
    if (voice == 0) return AUDIO_VOICE_INVALID;

    if (pitch != 1.0f) {
        s_soloud.setRelativePlaySpeed(voice, pitch);
    }

    return (AudioVoice)voice;
}

extern "C" void audio_stop(AudioVoice voice)
{
    if (!s_initialized || voice == AUDIO_VOICE_INVALID) return;
    s_soloud.stop(voice);
}

extern "C" void audio_stop_bus(AudioBus bus)
{
    if (!s_initialized || bus < 0 || bus >= AUDIO_BUS_COUNT) return;
    /* Stopping the bus voice drops every voice routed through it; restart it. */
    s_soloud.stop(s_bus_handles[bus]);
    s_bus_handles[bus] = s_soloud.play(s_buses[bus]);
}

extern "C" void audio_stop_all(void)
{
    if (!s_initialized) return;
    for (int i = 0; i < AUDIO_BUS_COUNT; i++) {
        audio_stop_bus((AudioBus)i);
    }
}

extern "C" void audio_pause(AudioVoice voice)
{
    if (!s_initialized || voice == AUDIO_VOICE_INVALID) return;
    s_soloud.setPause(voice, true);
}

extern "C" void audio_resume(AudioVoice voice)
{
    if (!s_initialized || voice == AUDIO_VOICE_INVALID) return;
    s_soloud.setPause(voice, false);
}

extern "C" void audio_pause_bus(AudioBus bus)
{
    if (!s_initialized || bus < 0 || bus >= AUDIO_BUS_COUNT) return;
    s_soloud.setPause(s_bus_handles[bus], true);
}

extern "C" void audio_resume_bus(AudioBus bus)
{
    if (!s_initialized || bus < 0 || bus >= AUDIO_BUS_COUNT) return;
    s_soloud.setPause(s_bus_handles[bus], false);
}

extern "C" void audio_set_volume(AudioVoice voice, float volume)
{
    if (!s_initialized || voice == AUDIO_VOICE_INVALID) return;
    s_soloud.setVolume(voice, volume);
}

extern "C" void audio_set_pitch(AudioVoice voice, float pitch)
{
    if (!s_initialized || voice == AUDIO_VOICE_INVALID) return;
    s_soloud.setRelativePlaySpeed(voice, pitch);
}

extern "C" void audio_set_pan(AudioVoice voice, float pan)
{
    if (!s_initialized || voice == AUDIO_VOICE_INVALID) return;
    s_soloud.setPan(voice, pan);
}

extern "C" void audio_set_bus_volume(AudioBus bus, float volume)
{
    if (!s_initialized || bus < 0 || bus >= AUDIO_BUS_COUNT) return;
    s_soloud.setVolume(s_bus_handles[bus], volume);
}

extern "C" void audio_fade_volume(AudioVoice voice, float target, float duration)
{
    if (!s_initialized || voice == AUDIO_VOICE_INVALID) return;
    s_soloud.fadeVolume(voice, target, (double)duration);
}

extern "C" void audio_fade_bus_volume(AudioBus bus, float target, float duration)
{
    if (!s_initialized || bus < 0 || bus >= AUDIO_BUS_COUNT) return;
    s_soloud.fadeVolume(s_bus_handles[bus], target, (double)duration);
}

extern "C" bool audio_is_playing(AudioVoice voice)
{
    if (!s_initialized || voice == AUDIO_VOICE_INVALID) return false;
    return s_soloud.isValidVoiceHandle(voice) && !s_soloud.getPause(voice);
}

extern "C" bool audio_is_paused(AudioVoice voice)
{
    if (!s_initialized || voice == AUDIO_VOICE_INVALID) return false;
    return s_soloud.getPause(voice);
}

extern "C" float audio_get_volume(AudioVoice voice)
{
    if (!s_initialized || voice == AUDIO_VOICE_INVALID) return 0.0f;
    return s_soloud.getVolume(voice);
}

extern "C" double audio_get_position(AudioVoice voice)
{
    if (!s_initialized || voice == AUDIO_VOICE_INVALID) return 0.0;
    return s_soloud.getStreamPosition(voice);
}

extern "C" void audio_seek(AudioVoice voice, double position)
{
    if (!s_initialized || voice == AUDIO_VOICE_INVALID) return;
    s_soloud.seek(voice, position);
}

extern "C" double audio_get_duration(AudioSourceHandle source)
{
    if (!s_initialized) return 0.0;
    SourceEntry *e = get_source(source);
    if (!e) return 0.0;
    if (e->wav) return e->wav->getLength();
    if (e->stream) return e->stream->getLength();
    return 0.0;
}

extern "C" void audio_update(void)
{
    if (!s_initialized) return;

    /* Exclude the permanent bus voices from the peak count. */
    int active = (int)s_soloud.getActiveVoiceCount() - AUDIO_BUS_COUNT;
    if (active < 0) active = 0;
    if (active > s_peak_voices) {
        s_peak_voices = active;
    }
}

extern "C" int audio_get_active_voice_count(void)
{
    if (!s_initialized) return 0;
    int active = (int)s_soloud.getActiveVoiceCount() - AUDIO_BUS_COUNT;
    return active > 0 ? active : 0;
}

extern "C" int audio_get_loaded_source_count(void)
{
    return s_loaded_count;
}

extern "C" int audio_get_peak_voice_count(void)
{
    return s_peak_voices;
}

extern "C" void audio_reset_peak_voice_count(void)
{
    s_peak_voices = 0;
}
