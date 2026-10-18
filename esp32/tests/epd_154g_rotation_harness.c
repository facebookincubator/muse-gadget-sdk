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

// Host harness for the 1.54G driver: frame turns, PWR taps and holds, and
// refresh windows.
// The runner extracts the production code into rotation.inc.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rotation.inc"

static unsigned ink_at(const uint8_t *frame, int x, int y) {
    return frame[y * EPD_ROW_BYTES + x / 4] >> (6 - 2 * (x % 4)) & 3;
}

static void put_ink(uint8_t *frame, int x, int y, unsigned ink) {
    int shift = 6 - 2 * (x % 4);
    uint8_t *byte = &frame[y * EPD_ROW_BYTES + x / 4];
    *byte = (uint8_t)((*byte & ~(3 << shift)) | (ink << shift));
}

static void check_turns(void) {
    static uint8_t src[EPD_FRAME_BYTES], dst[EPD_FRAME_BYTES], back[EPD_FRAME_BYTES];

    // A red pixel in the top-left corner goes clockwise round the corners.
    memset(src, 0x55, sizeof(src));  // all white
    put_ink(src, 0, 0, 3);
    const int corner[4][2] = {{0, 0}, {EPD_W - 1, 0}, {EPD_W - 1, EPD_H - 1}, {0, EPD_H - 1}};
    for (unsigned turns = 0; turns < 4; turns++) {
        rotate_frame(src, dst, turns);
        for (int y = 0; y < EPD_H; y++) {
            for (int x = 0; x < EPD_W; x++) {
                bool red = x == corner[turns][0] && y == corner[turns][1];
                assert(ink_at(dst, x, y) == (red ? 3u : 1u));
            }
        }
    }

    // A pixel off the diagonal: (10, 3) -> (196, 10) after one turn.
    memset(src, 0, sizeof(src));
    put_ink(src, 10, 3, 2);
    rotate_frame(src, dst, 1);
    assert(ink_at(dst, EPD_W - 1 - 3, 10) == 2);

    // Four quarter turns, and two half turns, give the frame back.
    srand(1);
    for (size_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)rand();
    memcpy(back, src, sizeof(src));
    for (int i = 0; i < 4; i++) {
        rotate_frame(back, dst, 1);
        memcpy(back, dst, sizeof(dst));
    }
    assert(memcmp(back, src, sizeof(src)) == 0);
    rotate_frame(src, dst, 2);
    rotate_frame(dst, back, 2);
    assert(memcmp(back, src, sizeof(src)) == 0);
    rotate_frame(src, dst, 0);
    assert(memcmp(dst, src, sizeof(src)) == 0);
}

#define MS(x) ((x) * 1000LL)

static void check_pwr(void) {
    pwr_key_t key = {0};

    // Held from power-on: neither a hold nor, on release, a tap.
    assert(pwr_key_update(&key, true, MS(0)) == PWR_NONE);
    assert(pwr_key_update(&key, true, MS(1500)) == PWR_NONE);
    assert(pwr_key_update(&key, false, MS(1600)) == PWR_NONE);

    // A tap fires on release; a bounce shorter than TAP_MIN_US does not.
    assert(pwr_key_update(&key, true, MS(2000)) == PWR_NONE);
    assert(pwr_key_update(&key, true, MS(2900)) == PWR_NONE);
    assert(pwr_key_update(&key, false, MS(2950)) == PWR_TAP);
    assert(pwr_key_update(&key, true, MS(3000)) == PWR_NONE);
    assert(pwr_key_update(&key, false, MS(3020)) == PWR_NONE);

    // A hold fires once, at one second, however long it lasts, and its
    // release is not a tap.
    assert(pwr_key_update(&key, true, MS(4000)) == PWR_NONE);
    assert(pwr_key_update(&key, true, MS(4999)) == PWR_NONE);
    assert(pwr_key_update(&key, true, MS(5000)) == PWR_HOLD);
    assert(pwr_key_update(&key, true, MS(7000)) == PWR_NONE);
    assert(pwr_key_update(&key, false, MS(7050)) == PWR_NONE);

    // The next hold fires again.
    assert(pwr_key_update(&key, true, MS(8000)) == PWR_NONE);
    assert(pwr_key_update(&key, true, MS(9000)) == PWR_HOLD);
}

static void check_window(void) {
    static uint8_t shown[EPD_FRAME_BYTES], next[EPD_FRAME_BYTES];
    memset(shown, 0x55, sizeof(shown));
    memcpy(next, shown, sizeof(next));

    // Nothing changed: no refresh. Nothing known: the whole panel.
    assert(!plan_window(shown, next).changed);
    epd_window_t w = plan_window(NULL, next);
    assert(w.changed && w.full && w.width == EPD_W && w.height == EPD_H);

    // One pixel: its byte, and a second row for R83H.
    put_ink(next, 101, 50, 3);
    w = plan_window(shown, next);
    assert(w.changed && !w.full && w.x == 100 && w.width == 4 && w.y == 50 && w.height == 2);
    // On the last row the extra row goes above.
    memcpy(next, shown, sizeof(next));
    put_ink(next, 0, EPD_H - 1, 0);
    w = plan_window(shown, next);
    assert(!w.full && w.y == EPD_H - 2 && w.height == 2);

    // A status line: rows 186 to 193 across the width.
    memcpy(next, shown, sizeof(next));
    for (int x = 10; x < 190; x++) put_ink(next, x, 186, 0), put_ink(next, x, 193, 0);
    w = plan_window(shown, next);
    assert(!w.full && w.x == 8 && w.width == 184 && w.y == 186 && w.height == 8);

    // Most of the panel: a full refresh.
    memcpy(next, shown, sizeof(next));
    put_ink(next, 0, 0, 0);
    put_ink(next, EPD_W - 1, EPD_H - 1, 0);
    w = plan_window(shown, next);
    assert(w.full);

    // R83H bytes: inclusive ends, the window flag last.
    uint8_t data[9];
    encode_window((epd_window_t){8, 186, 184, 8, true, false}, data);
    const uint8_t want[9] = {0, 8, 0, 191, 0, 186, 0, 193, 1};
    assert(memcmp(data, want, sizeof(want)) == 0);
    encode_window((epd_window_t){0, 0, EPD_W, EPD_H, true, true}, data);
    const uint8_t whole[9] = {0, 0, 0, 199, 0, 0, 0, 199, 0};
    assert(memcmp(data, whole, sizeof(whole)) == 0);
}

int main(void) {
    check_turns(); check_pwr(); check_window();
    puts("PASS epd 1.54G: turns, PWR taps and holds, refresh windows and their R83H bytes");
    return 0;
}
