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

/* Data-only v1 pack; see protocols/PIXEL_AVATAR.md. No executable assets. */
enum { MUSE_PIXEL_PACK_MAX = 65536, MUSE_PIXEL_EDGE_MAX = 128,
       MUSE_PIXEL_PALETTE_MAX = 64, MUSE_PIXEL_CAPABILITIES = 15 };
typedef struct {
    const uint8_t *data, *source_key, *palette, *indices, *alpha;
    size_t size;
    uint16_t width, height, colors;
} muse_pixel_pack_t;
uint32_t muse_pixel_pack_crc(const void *data, size_t size);
bool muse_pixel_pack_parse(const uint8_t *data, size_t size, muse_pixel_pack_t *out);

/* Modes match Muse's BOOT/IDLE/LISTENING/THINKING/SPEAKING/ERROR/OFF.
 * reply_has_audio=false keeps a text reply from pretending to be speech.
 * Effects are anatomy-neutral: whole-sprite breathing/bob/tap and a subtle
 * state tint. There are no synthesized eyes, mouths, limbs or face rigs. */
typedef struct {
    uint32_t time_ms;
    uint16_t happy, level; /* 0..1000 */
    uint8_t mode;
    bool working, reply_has_audio;
} muse_pixel_pose_t;
typedef struct {
    int width, height, left, top;
    uint16_t tint;
    uint8_t tint_amount;
} muse_pixel_geometry_t;
bool muse_pixel_geometry(const muse_pixel_pack_t *pack, int canvas_px,
                         const muse_pixel_pose_t *pose, muse_pixel_geometry_t *out);
/* Pure strip rendering. Immutable packed input, no allocation, I/O or mutex.
 * Alpha is real A8, so black details never become transparent. */
bool muse_pixel_art_blit(const muse_pixel_pack_t *pack, int canvas_px,
                         const muse_pixel_pose_t *pose, uint16_t background,
                         uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);
