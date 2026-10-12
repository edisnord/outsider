/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

/* SoLoud audio backend on top of SDL3.
 *
 * SoLoud ships backends for SDL1 and SDL2 only. Rather than patch its build,
 * this file provides the entry point SoLoud's `sdl2_static` backend would
 * have provided, so `Soloud::init(.., Soloud::AUTO, ..)` picks it up with no
 * changes to SoLoud itself. third_party/CMakeLists.txt compiles this instead
 * of the upstream soloud_sdl2_static.cpp.
 *
 * SDL3 replaced the pull-style audio callback with audio streams: the device
 * asks for however many bytes it wants, whenever it wants, and we push what
 * we have. SoLoud's mixer works in fixed-size blocks, so the callback loops,
 * mixing at most one block at a time.
 */

#include "soloud.h"
#include "audio_engine.h"

#include <SDL3/SDL.h>
#include <atomic>
#include <stdlib.h>

namespace SoLoud
{
    static SDL_AudioStream *gAudioStream = NULL;
    static float           *gScratch = NULL;
    static unsigned int     gChannels = 2;
    static unsigned int     gBufferFrames = 2048;

    /* Mixer-thread measurements, taken by the main thread through
       audio_backend_take_stats(). Plain atomics: the callback must never wait
       on anything the game thread holds. */
    static std::atomic<uint32_t> gStCallbacks(0), gStEmpty(0), gStLate(0);
    static std::atomic<uint32_t> gStMaxMixUs(0), gStMaxGapUs(0), gStMaxReqFrames(0);
    static uint64_t              gLastCallbackEnd = 0;

    static void stat_max(std::atomic<uint32_t> &a, uint32_t v)
    {
        uint32_t cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }

    /* Bytes one frame of interleaved float samples occupies. */
    static int frame_bytes(void)
    {
        return (int)(gChannels * sizeof(float));
    }

    static void SDLCALL soloud_sdl3_audiomixer(void *userdata, SDL_AudioStream *stream,
                                               int additional_amount, int total_amount)
    {
        (void)total_amount;
        Soloud *soloud = (Soloud *)userdata;
        if (!soloud || !gScratch) return;

        const uint64_t freq = SDL_GetPerformanceFrequency();
        const uint64_t t0 = SDL_GetPerformanceCounter();
        const int req_frames = additional_amount / frame_bytes();
        gStCallbacks.fetch_add(1, std::memory_order_relaxed);
        if (gLastCallbackEnd) stat_max(gStMaxGapUs, (uint32_t)((t0 - gLastCallbackEnd) * 1000000 / freq));
        stat_max(gStMaxReqFrames, (uint32_t)req_frames);
        /* The device asked for more than two blocks at once: it ran dry or
           nearly so while we were away. Nothing queued at all = it played silence. */
        if (req_frames > (int)gBufferFrames * 2) gStLate.fetch_add(1, std::memory_order_relaxed);
        if (SDL_GetAudioStreamQueued(stream) == 0) gStEmpty.fetch_add(1, std::memory_order_relaxed);

        while (additional_amount > 0) {
            int frames = additional_amount / frame_bytes();
            if (frames <= 0) break;
            /* Never mix more than the block size SoLoud sized its scratch
               buffers for. */
            if (frames > (int)gBufferFrames) frames = (int)gBufferFrames;

            soloud->mix(gScratch, (unsigned int)frames);

            int bytes = frames * frame_bytes();
            SDL_PutAudioStreamData(stream, gScratch, bytes);
            additional_amount -= bytes;
        }

        gLastCallbackEnd = SDL_GetPerformanceCounter();
        stat_max(gStMaxMixUs, (uint32_t)((gLastCallbackEnd - t0) * 1000000 / freq));
    }

    static void soloud_sdl3_deinit(Soloud *aSoloud)
    {
        (void)aSoloud;
        if (gAudioStream) {
            SDL_DestroyAudioStream(gAudioStream);
            gAudioStream = NULL;
        }
        free(gScratch);
        gScratch = NULL;
    }

    result sdl2static_init(Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate,
                           unsigned int aBuffer, unsigned int aChannels)
    {
        if (!SDL_WasInit(SDL_INIT_AUDIO)) {
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
                return UNKNOWN_ERROR;
        }

        if (aSamplerate == 0) aSamplerate = 44100;
        if (aBuffer == 0) aBuffer = 2048;
        if (aChannels == 0) aChannels = 2;

        /* Ask the device for buffers no larger than one mixing block, so the
           callback usually satisfies a request in a single mix. */
        char frames[16];
        SDL_snprintf(frames, sizeof(frames), "%u", aBuffer);
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);

        SDL_AudioSpec spec;
        SDL_zero(spec);
        spec.format = SDL_AUDIO_F32;
        spec.channels = (int)aChannels;
        spec.freq = (int)aSamplerate;

        gScratch = (float *)malloc((size_t)aBuffer * aChannels * sizeof(float));
        if (!gScratch) return OUT_OF_MEMORY;

        gChannels = aChannels;
        gBufferFrames = aBuffer;

        gAudioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                                 soloud_sdl3_audiomixer, (void *)aSoloud);
        if (!gAudioStream) {
            free(gScratch);
            gScratch = NULL;
            return UNKNOWN_ERROR;
        }

        aSoloud->postinit_internal(aSamplerate, aBuffer, aFlags, aChannels);
        aSoloud->mBackendCleanupFunc = soloud_sdl3_deinit;
        aSoloud->mBackendString = "SDL3";

        SDL_ResumeAudioStreamDevice(gAudioStream);
        return SO_NO_ERROR;
    }
}

extern "C" bool audio_backend_take_stats(AudioBackendStats *out)
{
    using namespace SoLoud;
    out->callbacks  = gStCallbacks.exchange(0);
    out->empty      = gStEmpty.exchange(0);
    out->late       = gStLate.exchange(0);
    out->max_mix_us = gStMaxMixUs.exchange(0);
    out->max_gap_us = gStMaxGapUs.exchange(0);
    out->max_request_frames = gStMaxReqFrames.exchange(0);
    out->block_frames = gBufferFrames;
    return out->callbacks != 0;
}
