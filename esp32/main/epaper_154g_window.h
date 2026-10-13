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
#include <string.h>

enum {
    EPAPER_154G_WINDOW_WIDTH = 200,
    EPAPER_154G_WINDOW_HEIGHT = 200,
    EPAPER_154G_WINDOW_ROW_BYTES = 50,
    EPAPER_154G_WINDOW_FRAME_BYTES = 10000,
};

typedef struct {
    uint16_t x, y, width, height;
    bool changed, full;
    uint32_t changed_pixels;
} epaper_154g_window_t;

static inline epaper_154g_window_t epaper_154g_window_plan_area(const uint8_t *shown, const uint8_t *next,
                                                bool valid, bool force_full,
                                                int band_x, int band_y, int band_width, int band_height) {
    epaper_154g_window_t full = {0, 0, EPAPER_154G_WINDOW_WIDTH, EPAPER_154G_WINDOW_HEIGHT, true, true, 0};
    if (!valid) return full;
    const bool image = band_x == 0 && band_y == 0 && band_width == EPAPER_154G_WINDOW_WIDTH
                       && band_height == EPAPER_154G_WINDOW_HEIGHT;
    int left = EPAPER_154G_WINDOW_ROW_BYTES, right = -1;
    int top = EPAPER_154G_WINDOW_HEIGHT, bottom = -1;
    bool outside = false;
    for (int y = 0; y < EPAPER_154G_WINDOW_HEIGHT; y++) {
        for (int byte = 0; byte < EPAPER_154G_WINDOW_ROW_BYTES; byte++) {
            int offset = y * EPAPER_154G_WINDOW_ROW_BYTES + byte;
            uint8_t delta = shown[offset] ^ next[offset];
            if (!delta) continue;
            for (int shift = 0; shift < 8; shift += 2) {
                if ((delta >> shift) & 3) {
                    full.changed_pixels++;
                    int x = byte * 4 + (6 - shift) / 2;
                    if (x < band_x || x >= band_x + band_width
                        || y < band_y || y >= band_y + band_height) outside = true;
                }
            }
            if (byte < left) left = byte;
            if (byte > right) right = byte;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    if (right < 0) return (epaper_154g_window_t){0};
    if (force_full || outside) return full;
    // R83H requires end Y greater than start Y, even for a one-row change.
    if (top == bottom) {
        if (bottom + 1 < band_y + band_height) bottom++;
        else top--;
    }
    if (image && (right - left + 1) * 4 * (bottom - top + 1) * 100
        > EPAPER_154G_WINDOW_WIDTH * EPAPER_154G_WINDOW_HEIGHT * 75) return full;
    return (epaper_154g_window_t){left * 4, top, (right - left + 1) * 4, bottom - top + 1,
                             true, false, full.changed_pixels};
}

// Existing callers describe a horizontal logical status band.
static inline epaper_154g_window_t epaper_154g_window_plan(const uint8_t *shown, const uint8_t *next,
                                                bool valid, bool force_full,
                                                int band_y, int band_height) {
    return epaper_154g_window_plan_area(shown, next, valid, force_full, 0, band_y,
                                   EPAPER_154G_WINDOW_WIDTH, band_height);
}

static inline epaper_154g_window_t epaper_154g_window_plan_rotated(const uint8_t *shown, const uint8_t *next,
                                                        bool valid, bool force_full, int band_y,
                                                        int band_height, unsigned rotation) {
    int x = 0, y = band_y, width = EPAPER_154G_WINDOW_WIDTH, height = band_height;
    switch (rotation & 3) {
    case 1: x = EPAPER_154G_WINDOW_WIDTH - band_y - band_height; y = 0;
            width = band_height; height = EPAPER_154G_WINDOW_HEIGHT; break;
    case 2: y = EPAPER_154G_WINDOW_HEIGHT - band_y - band_height; break;
    case 3: x = band_y; y = 0; width = band_height; height = EPAPER_154G_WINDOW_HEIGHT; break;
    }
    return epaper_154g_window_plan_area(shown, next, valid, force_full, x, y, width, height);
}

// R83H uses inclusive coordinates; each byte covers four 2-bit pixels.
static inline void epaper_154g_window_encode(epaper_154g_window_t window, uint8_t data[9]) {
    uint16_t end_x = window.x + window.width - 1;
    uint16_t end_y = window.y + window.height - 1;
    data[0] = window.x >> 8;
    data[1] = window.x & 0xff;
    data[2] = end_x >> 8;
    data[3] = end_x & 0xff;
    data[4] = window.y >> 8;
    data[5] = window.y & 0xff;
    data[6] = end_y >> 8;
    data[7] = end_y & 0xff;
    data[8] = window.full ? 0 : 1;
}

static inline void epaper_154g_window_commit(uint8_t *shown, const uint8_t *next, epaper_154g_window_t window) {
    for (int y = window.y; y < window.y + window.height; y++) {
        int offset = y * EPAPER_154G_WINDOW_ROW_BYTES + window.x / 4;
        memcpy(shown + offset, next + offset, window.width / 4);
    }
}
