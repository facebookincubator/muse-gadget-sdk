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
#include <stddef.h>
#include <stdint.h>

#define MUSE_MIRROR_MAX_DIRTY 8
#define MUSE_MIRROR_RECT_HDR 14

typedef struct {
    int x1, y1, x2, y2;
} muse_mirror_rect_t;

/* Caller serializes access to this state and the framebuffer. */
typedef struct {
    int w, h;
    muse_mirror_rect_t dirty[MUSE_MIRROR_MAX_DIRTY];
    int ndirty;
    muse_mirror_rect_t tail;
    bool tail_on;
} muse_mirror_core_t;

bool muse_mirror_core_init(muse_mirror_core_t *c, int w, int h);
/* F: type:u8, state:u8, rects:u16, screen_w:u16, screen_h:u16.
 * Each rectangle: x,y,w,h:u16, coding:u8, reserved=0:u8, bytes:u32, payload.
 * Integers are little endian. Coding 0: RGB565; 1/2: row/column order pairs
 * (count:u16, pixel:u16), nonzero counts, runs may cross row/column boundaries.
 * Exactly w*h pixels and bytes consumed.
 */
/* Inclusive coordinates, clipped to the screen. */
void muse_mirror_core_dirty(muse_mirror_core_t *c, int x1, int y1, int x2, int y2);
bool muse_mirror_core_pending(const muse_mirror_core_t *c);
/* fb: w*h native RGB565 pixels. Returns an F payload (no length prefix).
 * Unsent pixels stay pending, with the previous tail served first.
 * Returns 0 without consuming dirt if capacity cannot hold one raw row. */
size_t muse_mirror_core_build(muse_mirror_core_t *c, const uint16_t *fb,
                              uint8_t *out, size_t capacity, uint8_t state);

/* Validate an insert/edit payload; rejected input leaves text empty. */
bool muse_mirror_core_text(const uint8_t *buf, size_t len, uint8_t *edit, char text[64]);
