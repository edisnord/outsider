/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#ifndef RMMZ_AUDIO_ENGINE_H
#define RMMZ_AUDIO_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio bus indices matching RPG Maker MZ's 4 audio channels. */
typedef enum {
    AUDIO_BUS_BGM = 0,   /* Background music */
    AUDIO_BUS_BGS = 1,   /* Background sound (ambient) */
    AUDIO_BUS_ME  = 2,   /* Music effect (jingles) */
    AUDIO_BUS_SE  = 3,   /* Sound effect */
    AUDIO_BUS_COUNT = 4
} AudioBus;

/* Opaque handle to a loaded audio source (WAV/OGG file). */
typedef uint32_t AudioSourceHandle;
#define AUDIO_SOURCE_INVALID 0

/* Voice handle returned by audio_play(). Identifies a playing instance. */
typedef uint32_t AudioVoice;
#define AUDIO_VOICE_INVALID 0

/* What the mixer thread saw since the last call (which resets it). */
typedef struct {
    uint32_t callbacks;           /* device requests served */
    uint32_t empty;               /* requests that found nothing queued: silence was played */
    uint32_t late;                /* requests for more than two blocks: the device ran nearly dry */
    uint32_t max_mix_us;          /* longest callback, including waiting on SoLoud's lock */
    uint32_t max_gap_us;          /* longest time between one callback ending and the next starting */
    uint32_t max_request_frames;  /* largest single request */
    uint32_t block_frames;
} AudioBackendStats;

/* False when the SDL3 backend is not in use or nothing was requested. */
bool audio_backend_take_stats(AudioBackendStats *out);

/* Disable audio globally (call before init): init becomes a no-op and
   playback calls succeed silently. */
void audio_engine_set_disabled(bool disabled);

/* Initialize the audio engine, falling back to a silent null backend when no
   audio device is available. Returns true on success (including fallback). */
bool audio_engine_init(void);

/* Shut down the audio engine. Safe to call when uninitialized. */
void audio_engine_shutdown(void);

/* Returns true if initialized (including with the null backend). */
bool audio_engine_is_initialized(void);

/* Load an OGG/WAV file fully into memory (for SE/ME).
   Returns AUDIO_SOURCE_INVALID on failure. */
AudioSourceHandle audio_load(const char *path);

/* Open an OGG/WAV file for streaming playback (for BGM/BGS).
   Returns AUDIO_SOURCE_INVALID on failure. */
AudioSourceHandle audio_load_stream(const char *path);

/* Load audio from an in-memory buffer (the data is copied).
   Returns AUDIO_SOURCE_INVALID on failure. */
AudioSourceHandle audio_load_from_memory(const uint8_t *data, uint32_t size);

/* Free a loaded audio source, stopping any voices playing it. */
void audio_source_free(AudioSourceHandle handle);

/* Play a source on a bus. volume 0..1, pitch = rate multiplier, pan -1..1.
   Returns a voice handle, or AUDIO_VOICE_INVALID. */
AudioVoice audio_play(AudioSourceHandle source, AudioBus bus,
                      float volume, float pitch, float pan, bool loop);

/* Stop a specific voice. */
void audio_stop(AudioVoice voice);

/* Stop all voices on a specific bus. */
void audio_stop_bus(AudioBus bus);

/* Stop all audio on all buses. */
void audio_stop_all(void);

/* Pause a specific voice. */
void audio_pause(AudioVoice voice);

/* Resume a paused voice. */
void audio_resume(AudioVoice voice);

/* Pause all voices on a bus. */
void audio_pause_bus(AudioBus bus);

/* Resume all voices on a bus. */
void audio_resume_bus(AudioBus bus);

/* Set the volume of a playing voice (0.0 to 1.0). */
void audio_set_volume(AudioVoice voice, float volume);

/* Set the playback rate (pitch) of a playing voice. */
void audio_set_pitch(AudioVoice voice, float pitch);

/* Set the pan of a playing voice (-1.0 left, 0.0 center, 1.0 right). */
void audio_set_pan(AudioVoice voice, float pan);

/* Set the overall volume for a bus (0.0 to 1.0). */
void audio_set_bus_volume(AudioBus bus, float volume);

/* Fade a voice's volume to the target over duration seconds. */
void audio_fade_volume(AudioVoice voice, float target, float duration);

/* Fade a bus's volume to the target over duration seconds. */
void audio_fade_bus_volume(AudioBus bus, float target, float duration);

/* Query whether a voice is currently playing. */
bool audio_is_playing(AudioVoice voice);

/* Query whether a voice is currently paused. */
bool audio_is_paused(AudioVoice voice);

/* Get the current volume of a voice. */
float audio_get_volume(AudioVoice voice);

/* Get the current playback position in seconds. */
double audio_get_position(AudioVoice voice);

/* Seek to a position in seconds. */
void audio_seek(AudioVoice voice, double position);

/* Get a source's duration in seconds (0.0 if invalid or unknown). */
double audio_get_duration(AudioSourceHandle source);

/* Per-frame update (profiling counters). Called from the game loop. */
void audio_update(void);

/* Get the number of currently active voices. */
int audio_get_active_voice_count(void);

/* Get the number of loaded audio sources (for profiling). */
int audio_get_loaded_source_count(void);

/* Get the peak active voice count since last reset. */
int audio_get_peak_voice_count(void);

/* Reset peak voice counter. */
void audio_reset_peak_voice_count(void);

/* PCM streams: a voice fed with interleaved float samples from the main
   thread (video soundtracks). Underruns play silence. */

typedef int AudioStreamHandle;
#define AUDIO_STREAM_INVALID 0

/* Create and start a stream. Returns AUDIO_STREAM_INVALID when the engine
   is unavailable, in which case the other calls are no-ops. */
AudioStreamHandle audio_stream_create(int sample_rate, int channels);

/* Queue interleaved float frames. Drops the oldest data if the ring buffer
   (a few seconds) overflows. */
void audio_stream_push(AudioStreamHandle stream, const float *interleaved, int frames);

/* Frames queued and not yet consumed by the mixer. */
int audio_stream_queued_frames(AudioStreamHandle stream);

void audio_stream_set_volume(AudioStreamHandle stream, float volume);
void audio_stream_set_paused(AudioStreamHandle stream, bool paused);

/* Stop the voice and release the stream. */
void audio_stream_destroy(AudioStreamHandle stream);

#ifdef __cplusplus
}
#endif

#endif /* RMMZ_AUDIO_ENGINE_H */
