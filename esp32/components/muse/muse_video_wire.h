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
 * The messages of video over Wi-Fi (../../../protocols/VIDEO.md, Wi-Fi), as
 * plain C over cJSON so the host tests build them. Each message on the TCP
 * connection is MUSE_VIDEO_WIRE_JSON, a uint16_t little-endian length, then
 * that many bytes of a JSON object with a "type"; the client's frames go
 * between them, each starting with MUSE_VIDEO_FRAME_MAGIC (muse_vstream.h).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "muse_vstream.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_VIDEO_WIRE_JSON 0x4A    /* 'J' */
#define MUSE_VIDEO_WIRE_VERSION 1
#define MUSE_VIDEO_WIRE_MAX_JSON 1024
#define MUSE_VIDEO_WIRE_SERVICE "_musevideo"
#define MUSE_VIDEO_WIRE_PROTO "_tcp"

typedef enum {
    MUSE_VIDEO_MSG_UNKNOWN = 0,   /* ignore it: a newer client's */
    MUSE_VIDEO_MSG_BAD,           /* not JSON, or no type */
    MUSE_VIDEO_MSG_START,
    MUSE_VIDEO_MSG_STOP,
    MUSE_VIDEO_MSG_STATUS,        /* asks for a buffer message now */
} muse_video_msg_t;

typedef enum {
    MUSE_VIDEO_START_OK = 0,
    MUSE_VIDEO_START_UNSUPPORTED,   /* a codec the board doesn't decode */
    MUSE_VIDEO_START_INVALID,       /* size, rate, bands or playout out of the caps */
} muse_video_start_check_t;

/* Parses one message from the client (json: n bytes, not terminated). For a
 * start, fills *p. */
muse_video_msg_t muse_video_wire_parse(const char *json, size_t n, muse_video_params_t *p);
/* Checks a start's parameters against what the board offers. */
muse_video_start_check_t muse_video_wire_check(const muse_video_params_t *p, const muse_video_caps_t *c);

/*
 * The gadget's messages, framed (the 'J', the length, the JSON) into out
 * (cap bytes). Each returns the framed length, or 0 if it didn't fit.
 */
size_t muse_video_wire_hello(uint8_t *out, size_t cap, const char *name, const muse_video_caps_t *c,
                             uint32_t buffer);
size_t muse_video_wire_buffer(uint8_t *out, size_t cap, uint32_t free_bytes, uint32_t received,
                              uint16_t shown, uint16_t dropped, uint8_t decode_ms, uint16_t last_frame,
                              uint16_t last_age_ms);
/* "keyframe", "stop", or "want" (with on). */
size_t muse_video_wire_simple(uint8_t *out, size_t cap, const char *type);
size_t muse_video_wire_want(uint8_t *out, size_t cap, bool on);
/* reason: "busy", "unsupported" or "invalid". */
size_t muse_video_wire_error(uint8_t *out, size_t cap, const char *for_type, const char *reason);

#ifdef __cplusplus
}
#endif
