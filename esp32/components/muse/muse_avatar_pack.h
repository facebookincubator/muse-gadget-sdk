/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { MUSE_AVATAR_PACK_MAX = 1024 * 1024, MUSE_AVATAR_FRAME_MAX = 32768,
       MUSE_AVATAR_CLIPS = 6, MUSE_AVATAR_PACK_HEADER = 64 };
typedef enum { MUSE_CLIP_IDLE, MUSE_CLIP_WAKING, MUSE_CLIP_WORKING,
    MUSE_CLIP_MAKING, MUSE_CLIP_WAITING, MUSE_CLIP_CELEBRATE } muse_avatar_clip_kind_t;
typedef struct { uint32_t offset, bytes, frames; bool loop; } muse_avatar_clip_t;
typedef struct {
    const uint8_t *data;
    size_t size;
    uint16_t width, height, fps;
    uint32_t coverage;
    muse_avatar_clip_t clip[MUSE_AVATAR_CLIPS];
} muse_avatar_pack_t;
/* The caller owns immutable data. A complete bounded validation, including
 * every frame CRC and exact EOF, precedes publication or decoder input. */
bool muse_avatar_pack_parse(const uint8_t *data, size_t size, const uint8_t source[32],
                            muse_avatar_pack_t *out);
uint32_t muse_avatar_pack_crc(const void *data, size_t size);
bool muse_avatar_pack_frame(const muse_avatar_pack_t *pack, unsigned kind, uint32_t *cursor,
                            const uint8_t **data, uint32_t *size);
