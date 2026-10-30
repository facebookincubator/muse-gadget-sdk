/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "muse_vstream.h"

/*
 * Full-screen video from a phone (CONFIG_MUSE_VIDEO): its camera, say, as
 * H.264, carried by a transport (Wi-Fi: muse_video_wifi.c) into the
 * transport's ring (muse_vstream.h). The menu's Phone camera item opens the
 * view and asks the transports' clients for a stream; any button closes it.
 * Frames are decoded on the first core (two bands: one per core) and drawn
 * on the UI core straight to the panel (LVGL paused), scaled to fit.
 * ../../../protocols/VIDEO.md has the design.
 */

/* What muse_video tells a transport after each frame, for its client. */
typedef struct {
    uint16_t shown, dropped;   /* since the stream started */
    uint8_t decode_ms;         /* average over the last stats period */
    uint16_t last_number;      /* the frame number of the last picture the panel finished drawing */
    uint16_t last_age_ms;      /* how long ago; 0xffff: none yet */
} muse_video_status_t;

/* A way for streams to arrive. Every callback may be called from the decoder
 * or the UI's tasks; none may block for long. */
typedef struct {
    const char *name;
    muse_vstream_t *ring;
    /* The decoder took a frame out of the ring: room for more (credits). */
    void (*freed)(const muse_video_status_t *st);
    /* The decoder can't go on until a keyframe. */
    void (*keyframe)(void);
    /* The gadget ends the stream (out of memory, or the view closed). */
    void (*stop)(void);
    /* The view opened (true) or closed: ask a client to start or stop. */
    void (*request)(bool want);
    /* A client that can stream is connected. */
    bool (*has_client)(void);
} muse_video_transport_t;

/* The board can show video (a panel driver with draw_frame). */
bool muse_video_supported(void);
/* The view is up (the menu picked it, or a client started a stream). */
bool muse_video_is_open(void);
/* From the menu: open the view and ask the clients for a stream. */
void muse_video_open(void);
/* Close the view and end the stream (any button while it's open). */
void muse_video_close(void);
/* From the UI's frame tick, LVGL locked: keeps the view's captions current.
 * True while the view is up (the face isn't drawn). */
bool muse_video_tick(void);
/* A client of some transport is connected (the menu's Ready / No phone). */
bool muse_video_has_client(void);

/* ---- for transports ---- */
/* Up to two; call once each, before any begin. */
void muse_video_add_transport(const muse_video_transport_t *t);
/* What this board offers; NULL if it can't show video. */
const muse_video_caps_t *muse_video_caps(void);
/* A client starts a stream into t's ring (which the transport has started).
 * False: busy (another transport streams, or the panel isn't up yet: the
 * client asks again). */
bool muse_video_begin(const muse_video_transport_t *t, const muse_video_params_t *vp);
/* The client (or its connection) ended t's stream. */
void muse_video_end(const muse_video_transport_t *t);
/* Bytes arrived in the ring: wakes the decoder (else it looks every 2 ms). */
void muse_video_wake(void);
/* The receive ring's size every transport should use (CONFIG_MUSE_VIDEO_BUFFER). */
uint32_t muse_video_buffer_size(void);
