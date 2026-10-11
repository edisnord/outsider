/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "video/video_player.h"
#include "audio/audio_engine.h"
#include "io/file_io.h"
#include "rendering/renderer.h"
#include "rendering/sprite_batch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* pl_mpeg's declarations use FILE, so stdio.h must come first. */
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#ifdef RMMZ_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include "engine/error_handler.h"
#endif

#define MAX_VIDEOS 4

/* Decoding is driven by wall-clock time. A single huge step (window drag,
   debugger) would otherwise decode seconds of frames at once. */
#define MAX_STEP_SECONDS 0.25

/* Audio is decoded this far ahead of the video clock so the mixer's ring
   buffer never runs dry between frames. */
#define AUDIO_LEAD_SECONDS 0.15

#ifdef RMMZ_HAVE_FFMPEG
/* WebM (and other FFmpeg-readable) movies: the demuxer and decoders run on the thread that updates the videos. The
   clock is wall-clock time like the MPEG-1 path; a decoded picture waits in `pending` until its time has come. */
#define FF_QUEUE 8
typedef struct {
    AVFormatContext *fmt;
    AVCodecContext  *vctx, *actx;
    struct SwsContext *sws;
    struct SwrContext *swr;
    AVPacket        *pkt;
    AVFrame         *frame;          /* decoder output */
    AVFrame         *queue[FF_QUEUE];/* decoded pictures that are not due yet, oldest first */
    int              nq;
    int              vstream, astream;
    double           vtb, atb;       /* time bases (seconds per tick) */
    double           clock;          /* playback time in seconds */
    double           audio_time;     /* end of the sound pushed so far */
    double           duration;
    int              audio_rate;
    bool             eof;            /* the file is read to its end */
    bool             flushed;        /* decoders were told about the end */
    float           *pcm;            /* resampled sound, interleaved stereo float */
    int              pcm_cap;
} FFVideo;
#endif

typedef struct {
    bool              in_use;
    plm_t            *plm;
#ifdef RMMZ_HAVE_FFMPEG
    FFVideo          *ff;            /* set for movies played through FFmpeg (plm is then NULL) */
#endif
    int               width, height;
    uint8_t          *rgba;          /* latest decoded frame */
    bool              frame_dirty;   /* rgba changed since last upload */
    uint32_t          texture;
    int               frame_count;
    int               tex_frame;     /* frame_count at the last video_update_texture() */
    bool              playing;
    bool              paused;
    bool              visible;
    bool              loop;
    float             volume;
    AudioStreamHandle audio;
} Video;

static Video      s_videos[MAX_VIDEOS + 1];   /* index 0 unused */
static Renderer **s_renderer_slot = NULL;

static Video *get_video(VideoHandle h)
{
    if (h <= 0 || h > MAX_VIDEOS || !s_videos[h].in_use) return NULL;
    return &s_videos[h];
}

static void on_video_frame(plm_t *plm, plm_frame_t *frame, void *user)
{
    (void)plm;
    Video *v = user;
    plm_frame_to_rgba(frame, v->rgba, v->width * 4);
    v->frame_dirty = true;
    v->frame_count++;
}

static void on_audio_samples(plm_t *plm, plm_samples_t *samples, void *user)
{
    (void)plm;
    Video *v = user;
    if (v->audio != AUDIO_STREAM_INVALID) {
        audio_stream_push(v->audio, samples->interleaved, (int)samples->count);
    }
}


#ifdef RMMZ_HAVE_FFMPEG

static void ff_free(FFVideo *f)
{
    if (!f) return;
    if (f->swr) swr_free(&f->swr);
    if (f->sws) sws_freeContext(f->sws);
    for (int i = 0; i < f->nq; i++) av_frame_free(&f->queue[i]);
    if (f->frame) av_frame_free(&f->frame);
    if (f->pkt) av_packet_free(&f->pkt);
    if (f->vctx) avcodec_free_context(&f->vctx);
    if (f->actx) avcodec_free_context(&f->actx);
    if (f->fmt) avformat_close_input(&f->fmt);
    free(f->pcm);
    free(f);
}

static AVCodecContext *ff_open_decoder(AVStream *st, int threads)
{
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return NULL;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (!c) return NULL;
    if (avcodec_parameters_to_context(c, st->codecpar) < 0) { avcodec_free_context(&c); return NULL; }
    c->thread_count = threads;
    if (avcodec_open2(c, codec, NULL) < 0) { avcodec_free_context(&c); return NULL; }
    return c;
}

/* Opens `path` with FFmpeg. Returns NULL when it is not a movie FFmpeg can play. */
static FFVideo *ff_open(const char *path, int *w, int *h)
{
    FFVideo *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->vstream = f->astream = -1;
    if (avformat_open_input(&f->fmt, path, NULL, NULL) < 0) { f->fmt = NULL; ff_free(f); return NULL; }
    if (avformat_find_stream_info(f->fmt, NULL) < 0) { ff_free(f); return NULL; }
    f->vstream = av_find_best_stream(f->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (f->vstream < 0) { ff_free(f); return NULL; }
    AVStream *vs = f->fmt->streams[f->vstream];
    f->vctx = ff_open_decoder(vs, 1);
    if (!f->vctx) { ff_free(f); return NULL; }
    f->vtb = av_q2d(vs->time_base);
    f->astream = av_find_best_stream(f->fmt, AVMEDIA_TYPE_AUDIO, -1, f->vstream, NULL, 0);
    if (f->astream >= 0) {
        AVStream *as = f->fmt->streams[f->astream];
        f->actx = ff_open_decoder(as, 1);
        if (f->actx) {
            f->audio_rate = f->actx->sample_rate > 0 ? f->actx->sample_rate : 48000;
            AVChannelLayout out;
            av_channel_layout_default(&out, 2);
            if (swr_alloc_set_opts2(&f->swr, &out, AV_SAMPLE_FMT_FLT, f->audio_rate, &f->actx->ch_layout,
                                    f->actx->sample_fmt, f->actx->sample_rate, 0, NULL) < 0 || swr_init(f->swr) < 0) {
                swr_free(&f->swr);
                avcodec_free_context(&f->actx);
            }
        }
        if (!f->actx) f->astream = -1;
    }
    f->pkt = av_packet_alloc();
    f->frame = av_frame_alloc();
    if (!f->pkt || !f->frame) { ff_free(f); return NULL; }
    *w = f->vctx->width;
    *h = f->vctx->height;
    f->duration = f->fmt->duration > 0 ? (double)f->fmt->duration / AV_TIME_BASE : 0.0;
    return f;
}

static void ff_push_audio(Video *v, AVFrame *af)
{
    FFVideo *f = v->ff;
    if (!f->swr || v->audio == AUDIO_STREAM_INVALID) return;
    int max_out = swr_get_out_samples(f->swr, af ? af->nb_samples : 0) + 64;
    if (max_out > f->pcm_cap) {
        float *np = realloc(f->pcm, (size_t)max_out * 2 * sizeof(float));
        if (!np) return;
        f->pcm = np;
        f->pcm_cap = max_out;
    }
    uint8_t *out[1] = { (uint8_t *)f->pcm };
    int n = swr_convert(f->swr, out, f->pcm_cap, af ? (const uint8_t **)af->extended_data : NULL, af ? af->nb_samples : 0);
    if (n > 0) {
        audio_stream_push(v->audio, f->pcm, n);
        f->audio_time += (double)n / f->audio_rate;
    }
}

static void ff_drain_audio(Video *v)
{
    FFVideo *f = v->ff;
    while (avcodec_receive_frame(f->actx, f->frame) == 0) {
        ff_push_audio(v, f->frame);
        av_frame_unref(f->frame);
    }
}

static void ff_queue_drop_first(FFVideo *f)
{
    av_frame_free(&f->queue[0]);
    memmove(f->queue, f->queue + 1, sizeof(f->queue[0]) * (FF_QUEUE - 1));
    f->nq--;
}

static void ff_drain_video(Video *v)
{
    FFVideo *f = v->ff;
    while (avcodec_receive_frame(f->vctx, f->frame) == 0) {
        if (f->nq == FF_QUEUE) ff_queue_drop_first(f);   /* far behind: drop the oldest picture */
        AVFrame *copy = av_frame_alloc();
        if (copy) {
            av_frame_move_ref(copy, f->frame);
            f->queue[f->nq++] = copy;
        } else {
            av_frame_unref(f->frame);
        }
    }
}

/* Reads and decodes until a picture is waiting in the queue and the sound is ahead of the clock (or the file ends). */
static void ff_read_ahead(Video *v)
{
    FFVideo *f = v->ff;
    for (int guard = 0; guard < 600; guard++) {
        bool need_video = f->nq == 0 && !f->flushed;
        bool need_audio = f->astream >= 0 && v->audio != AUDIO_STREAM_INVALID && !f->eof &&
                          f->audio_time < f->clock + AUDIO_LEAD_SECONDS;
        if (!need_video && !need_audio) return;

        if (f->eof) {
            if (!f->flushed) {            /* the file is read: tell the decoders and collect what they still hold */
                avcodec_send_packet(f->vctx, NULL);
                ff_drain_video(v);
                if (f->actx) {
                    avcodec_send_packet(f->actx, NULL);
                    ff_drain_audio(v);
                }
                f->flushed = true;
            }
            return;
        }
        if (av_read_frame(f->fmt, f->pkt) < 0) { f->eof = true; continue; }
        if (f->pkt->stream_index == f->vstream) {
            avcodec_send_packet(f->vctx, f->pkt);
            ff_drain_video(v);
        } else if (f->pkt->stream_index == f->astream && f->actx) {
            avcodec_send_packet(f->actx, f->pkt);
            ff_drain_audio(v);
        }
        av_packet_unref(f->pkt);
    }
}

static double ff_pts(const FFVideo *f, const AVFrame *fr)
{
    return fr->best_effort_timestamp != AV_NOPTS_VALUE ? (double)fr->best_effort_timestamp * f->vtb : f->clock;
}

/* Converts the oldest queued picture into the RGBA buffer and removes it from the queue. */
static void ff_show_first(Video *v)
{
    FFVideo *f = v->ff;
    AVFrame *fr = f->queue[0];
    f->sws = sws_getCachedContext(f->sws, fr->width, fr->height, (enum AVPixelFormat)fr->format, v->width, v->height,
                                  AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    if (f->sws) {
        uint8_t *dst[4] = { v->rgba, NULL, NULL, NULL };
        int dst_stride[4] = { v->width * 4, 0, 0, 0 };
        sws_scale(f->sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, dst_stride);
        v->frame_dirty = true;
        v->frame_count++;
    }
    ff_queue_drop_first(f);
}

static void ff_update(Video *v, double dt)
{
    FFVideo *f = v->ff;
    f->clock += dt;
    ff_read_ahead(v);
    /* pictures whose time has come: skip all but the newest of them (no point converting what is never shown) */
    while (f->nq > 0 && ff_pts(f, f->queue[0]) <= f->clock) {
        if (f->nq > 1 && ff_pts(f, f->queue[1]) <= f->clock) {
            ff_queue_drop_first(f);
            continue;
        }
        ff_show_first(v);
        ff_read_ahead(v);
    }
}

static bool ff_ended(const Video *v)
{
    const FFVideo *f = v->ff;
    return f->flushed && f->nq == 0;
}

static void ff_rewind(Video *v)
{
    FFVideo *f = v->ff;
    av_seek_frame(f->fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(f->vctx);
    if (f->actx) avcodec_flush_buffers(f->actx);
    for (int i = 0; i < f->nq; i++) av_frame_free(&f->queue[i]);
    f->nq = 0;
    f->eof = f->flushed = false;
    f->clock = 0.0;
    f->audio_time = 0.0;
}

#endif /* RMMZ_HAVE_FFMPEG */

void video_player_init(Renderer **renderer_slot)
{
#ifdef RMMZ_HAVE_FFMPEG
    av_log_set_level(AV_LOG_ERROR);       /* the Opus decoder complains about harmless timestamp details */
#endif
    memset(s_videos, 0, sizeof(s_videos));
    s_renderer_slot = renderer_slot;
}

void video_player_shutdown(void)
{
    for (int i = 1; i <= MAX_VIDEOS; i++) {
        if (s_videos[i].in_use) video_close(i);
    }
    s_renderer_slot = NULL;
}

VideoHandle video_open(const char *path)
{
    if (!path) return VIDEO_HANDLE_INVALID;

    char resolved[1024];
    if (!file_io_resolve_path(path, resolved, sizeof(resolved))) return VIDEO_HANDLE_INVALID;

    int slot = 0;
    for (int i = 1; i <= MAX_VIDEOS; i++) {
        if (!s_videos[i].in_use) { slot = i; break; }
    }
    if (!slot) {
        compat_report("video", "too many open videos", "limit is %d", MAX_VIDEOS);
        return VIDEO_HANDLE_INVALID;
    }

    plm_t *plm = NULL;
    int w = 0, h = 0;
#ifdef RMMZ_HAVE_FFMPEG
    FFVideo *ff = NULL;
    size_t rlen = strlen(resolved);
    bool mpeg_file = rlen > 4 && (strcasecmp(resolved + rlen - 4, ".mpg") == 0 || strcasecmp(resolved + rlen - 5, ".mpeg") == 0);
    if (!mpeg_file) {
        ff = ff_open(resolved, &w, &h);
        if (!ff) return VIDEO_HANDLE_INVALID;
    } else
#endif
    {
        plm = plm_create_with_filename(resolved);
        if (!plm) return VIDEO_HANDLE_INVALID;
        w = plm_get_width(plm);
        h = plm_get_height(plm);
        if (w <= 0 || h <= 0) {
            plm_destroy(plm);
            return VIDEO_HANDLE_INVALID;
        }
    }

    Video *v = &s_videos[slot];
    memset(v, 0, sizeof(*v));
    v->in_use = true;
    v->plm = plm;
#ifdef RMMZ_HAVE_FFMPEG
    v->ff = ff;
#endif
    v->width = w;
    v->height = h;
    v->rgba = calloc((size_t)w * (size_t)h * 4 + 256, 1);   /* swscale's vector code may write a little past the last row */
    if (!v->rgba) {
        if (plm) plm_destroy(plm);
#ifdef RMMZ_HAVE_FFMPEG
        ff_free(ff);
#endif
        v->in_use = false;
        return VIDEO_HANDLE_INVALID;
    }
    /* plm_frame_to_rgba() never touches the alpha byte; the sprite shader
       premultiplies by it, so set every pixel opaque once. */
    for (size_t i = 3; i < (size_t)w * (size_t)h * 4; i += 4) v->rgba[i] = 0xFF;
    v->visible = true;
    v->volume = 1.0f;
    v->audio = AUDIO_STREAM_INVALID;

    if (plm) {
        plm_set_video_decode_callback(plm, on_video_frame, v);
        plm_set_audio_decode_callback(plm, on_audio_samples, v);
        plm_set_audio_lead_time(plm, AUDIO_LEAD_SECONDS);
        plm_set_loop(plm, 0);
    }
    return slot;
}

void video_close(VideoHandle h)
{
    Video *v = get_video(h);
    if (!v) return;
    if (v->audio != AUDIO_STREAM_INVALID) audio_stream_destroy(v->audio);
    if (v->texture) renderer_delete_texture(v->texture);
    if (v->plm) plm_destroy(v->plm);
#ifdef RMMZ_HAVE_FFMPEG
    ff_free(v->ff);
#endif
    free(v->rgba);
    memset(v, 0, sizeof(*v));
}

void video_play(VideoHandle h)
{
    Video *v = get_video(h);
    if (!v) return;
#ifdef RMMZ_HAVE_FFMPEG
    if (v->ff) {
        if (ff_ended(v)) return;
        if (v->audio == AUDIO_STREAM_INVALID && v->ff->astream >= 0) {
            v->audio = audio_stream_create(v->ff->audio_rate, 2);
            if (v->audio != AUDIO_STREAM_INVALID) audio_stream_set_volume(v->audio, v->volume);
        }
    } else
#endif
    {
        if (plm_has_ended(v->plm)) return;
        if (v->audio == AUDIO_STREAM_INVALID && plm_get_num_audio_streams(v->plm) > 0) {
            v->audio = audio_stream_create(plm_get_samplerate(v->plm), 2);
            if (v->audio == AUDIO_STREAM_INVALID) {
                /* No audio engine: decode video only. */
                plm_set_audio_enabled(v->plm, 0);
            } else {
                audio_stream_set_volume(v->audio, v->volume);
            }
        }
    }
    v->playing = true;
    v->paused = false;
    if (v->audio != AUDIO_STREAM_INVALID) audio_stream_set_paused(v->audio, false);
}

void video_pause(VideoHandle h)
{
    Video *v = get_video(h);
    if (!v) return;
    v->paused = true;
    if (v->audio != AUDIO_STREAM_INVALID) audio_stream_set_paused(v->audio, true);
}

void video_set_volume(VideoHandle h, float volume)
{
    Video *v = get_video(h);
    if (!v) return;
    v->volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    if (v->audio != AUDIO_STREAM_INVALID) audio_stream_set_volume(v->audio, v->volume);
}

void video_set_loop(VideoHandle h, bool loop)
{
    Video *v = get_video(h);
    if (!v) return;
    v->loop = loop;
    if (v->plm) plm_set_loop(v->plm, loop ? 1 : 0);
}

void video_set_visible(VideoHandle h, bool visible)
{
    Video *v = get_video(h);
    if (v) v->visible = visible;
}

bool video_get_state(VideoHandle h, VideoState *out)
{
    Video *v = get_video(h);
    if (!v || !out) return false;
    bool ended;
    double time, duration;
#ifdef RMMZ_HAVE_FFMPEG
    if (v->ff) {
        ended = ff_ended(v) && !v->loop;
        time = v->ff->clock;
        duration = v->ff->duration;
    } else
#endif
    {
        ended = plm_has_ended(v->plm) != 0;
        time = plm_get_time(v->plm);
        duration = plm_get_duration(v->plm);
    }
    out->playing = v->playing && !v->paused && !ended;
    out->paused = v->paused;
    out->ended = ended;
    out->time = time;
    out->duration = duration;
    out->width = v->width;
    out->height = v->height;
    return true;
}

bool video_update_texture(VideoHandle h, uint32_t texture)
{
    Video *v = get_video(h);
    if (!v || !texture || v->frame_count == 0 || v->tex_frame == v->frame_count) return false;
    renderer_update_texture(texture, v->width, v->height, v->rgba);
    v->tex_frame = v->frame_count;
    return true;
}

int video_get_frame_count(VideoHandle h)
{
    Video *v = get_video(h);
    return v ? v->frame_count : 0;
}

void video_player_update(double dt_seconds)
{
    if (dt_seconds <= 0.0) return;
    if (dt_seconds > MAX_STEP_SECONDS) dt_seconds = MAX_STEP_SECONDS;

    for (int i = 1; i <= MAX_VIDEOS; i++) {
        Video *v = &s_videos[i];
        if (!v->in_use || !v->playing || v->paused) continue;
#ifdef RMMZ_HAVE_FFMPEG
        if (v->ff) {
            if (ff_ended(v)) {
                if (v->loop) ff_rewind(v);
                else { v->playing = false; continue; }
            }
            ff_update(v, dt_seconds);
            continue;
        }
#endif
        if (plm_has_ended(v->plm)) {
            v->playing = false;
            continue;
        }
        plm_decode(v->plm, dt_seconds);
    }
}

void video_player_draw(void)
{
    Renderer *r = s_renderer_slot ? *s_renderer_slot : NULL;
    if (!r) return;

    for (int i = 1; i <= MAX_VIDEOS; i++) {
        Video *v = &s_videos[i];
        if (!v->in_use || !v->visible || !v->playing || v->frame_count == 0) continue;
        /* Keep showing the last frame while paused (e.g. mid-skip) but not
           once the movie has ended: the game shows itself again then. */
#ifdef RMMZ_HAVE_FFMPEG
        if (v->ff ? (ff_ended(v) && !v->loop) : (plm_has_ended(v->plm) && !v->loop)) continue;
#else
        if (plm_has_ended(v->plm) && !v->loop) continue;
#endif

        if (!v->texture) {
            v->texture = renderer_create_texture(v->width, v->height);
            if (!v->texture) continue;
            renderer_set_texture_filter(v->texture, true);
            v->frame_dirty = true;
        }
        if (v->frame_dirty) {
            renderer_update_texture(v->texture, v->width, v->height, v->rgba);
            v->frame_dirty = false;
        }

        /* Letterbox the movie inside the game area, like the browser's
           video element centred over the canvas. */
        int gw = 0, gh = 0;
        renderer_get_size(r, &gw, &gh);
        if (gw <= 0 || gh <= 0) continue;
        float scale_x = (float)gw / (float)v->width;
        float scale_y = (float)gh / (float)v->height;
        float scale = scale_x < scale_y ? scale_x : scale_y;
        float dw = (float)v->width * scale, dh = (float)v->height * scale;
        float dx = ((float)gw - dw) * 0.5f, dy = ((float)gh - dh) * 0.5f;

        renderer_unbind_fbo(r);
        SpriteBatch *sb = renderer_get_batch(r);
        sprite_batch_begin(sb);
        sprite_batch_set_blend_mode(sb, BLEND_MODE_NORMAL);
        sprite_batch_draw(sb, 0, 0.0f, 0.0f, (float)gw, (float)gh,
                          0.0f, 0.0f, 1.0f, 1.0f, 0xFF000000u, 1.0f);
        sprite_batch_draw(sb, v->texture, dx, dy, dw, dh,
                          0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu, 1.0f);
        sprite_batch_end(sb);
    }
}
