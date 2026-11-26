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

/*
 * The stream parameters and capabilities of mg_command_stream_video, as the
 * protocol carries them. The receive ring and the frame format are the ones
 * every video transport shares (muse_vstream.h); mg_glue.c hands the stream
 * to muse_video.
 */

#include <stdbool.h>
#include <stdint.h>

#include "muse_vstream.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t codec;
    uint16_t width, height;   /* the picture, after cropping */
    uint8_t fps;
    uint16_t kbps;
    uint16_t playout_ms;      /* 0: show each frame once decoded; else at fps, this long after the first */
    uint8_t tiles;            /* horizontal bands, each its own stream of the codec (mg_stream_video_parameter_tiles); 0/1: one */
    uint8_t content;          /* mg_video_content_t: video, or a UI picture (mg_stream_video_parameter_content) */
} mg_video_params_t;

/* What a display board offers for mg_command_stream_video. */
typedef struct {
    uint16_t display_w, display_h;
    bool round;
    uint16_t max_w, max_h;       /* largest picture it decodes */
    uint8_t max_fps;
    uint16_t sug_w, sug_h;       /* the stream it suggests */
    uint8_t sug_fps;
    uint16_t sug_kbps;
    uint16_t l2cap_psm;          /* the stream also on this L2CAP CoC PSM, 0: Data only */
    uint16_t max_playout_ms;     /* longest playout delay it paces, 0: none */
    uint8_t max_tiles;           /* most bands it decodes in parallel (mg_stream_video_capability_tiles), 0/1: one */
    uint8_t tile_split_pct;      /* two bands: the top band's share of the height, % (capability_tile_split); 0: equal */
    bool vp8;                    /* decodes mg_data_type_video_vp8 as well as H.264 */
    bool no_h264;                /* doesn't decode H.264 (a board without PSRAM): only the codecs flagged here */
    /* mg_stream_video_capability_ui: UI streams of up to ui_w x ui_h (0: not
     * offered), at up to ui_fps, frames of up to ui_max_frame bytes, in up
     * to ui_tiles bands. */
    uint16_t ui_w, ui_h;
    uint8_t ui_fps;
    uint32_t ui_max_frame;
    uint8_t ui_tiles;
    uint8_t ui_still;            /* still patch formats (1 << mg_video_still_format_t); 0: none */
} mg_video_caps_t;

#ifdef __cplusplus
}
#endif
