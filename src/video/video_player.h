/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#ifndef RMMZ_VIDEO_PLAYER_H
#define RMMZ_VIDEO_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

/* MPEG-1 video playback (pl_mpeg). RPG Maker MZ games ship WebM/MP4 movies;
   the packaging tools transcode them to MPEG-1 + MP2, which a single-header
   decoder can play without bundling libvpx or FFmpeg. Frames go to a GL
   texture drawn over the game after each frame; audio goes to a PCM stream
   on the audio engine. Only one video plays at a time in practice, but
   handles are independent. */

typedef struct Renderer Renderer;
typedef int VideoHandle;
#define VIDEO_HANDLE_INVALID 0

typedef struct {
    bool   playing;    /* play() called and not paused/ended */
    bool   paused;
    bool   ended;
    double time;       /* seconds */
    double duration;
    int    width;
    int    height;
} VideoState;

/* renderer_slot may be NULL (headless): frames are decoded but not drawn.
   The slot is dereferenced at draw time because the renderer is created
   lazily by the game. */
void video_player_init(Renderer **renderer_slot);
void video_player_shutdown(void);

/* Open a file resolved via file_io. Returns VIDEO_HANDLE_INVALID on failure. */
VideoHandle video_open(const char *path);
void video_close(VideoHandle h);

void video_play(VideoHandle h);
void video_pause(VideoHandle h);
void video_set_volume(VideoHandle h, float volume);
void video_set_loop(VideoHandle h, bool loop);
/* Whether the frame is drawn; playback continues regardless. */
void video_set_visible(VideoHandle h, bool visible);
bool video_get_state(VideoHandle h, VideoState *out);

/* Copies the newest decoded picture into `texture` (a texture of the movie's size made with renderer_create_texture)
   when there is a newer one than the last copy. Returns true when the texture changed. Used by plugins that show a
   movie as a picture instead of full screen. */
bool video_update_texture(VideoHandle h, uint32_t texture);

/* Advance every playing video by dt seconds (decodes frames and audio). */
void video_player_update(double dt_seconds);

/* Draw visible, playing videos letterboxed over the game area. Call after
   the game has rendered its frame and before the buffer swap. */
void video_player_draw(void);

/* Frames decoded so far (for tests). */
int video_get_frame_count(VideoHandle h);

#endif /* RMMZ_VIDEO_PLAYER_H */
