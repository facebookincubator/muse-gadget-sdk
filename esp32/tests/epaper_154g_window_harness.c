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

// Partial-window planning, panel rotation and the PWR button gesture of the
// Waveshare 1.54G, from the headers alone.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "epaper_154g_rotation.h"
#include "epaper_154g_window.h"

// The status band of the 200x200 layout.
#define BAND_Y 158
#define BAND_H 36

static uint8_t shown[EPAPER_154G_WINDOW_FRAME_BYTES];
static uint8_t next[EPAPER_154G_WINDOW_FRAME_BYTES];

// Both frames all white (ink code 1).
static void reset(void) {
    memset(shown, 0x55, sizeof(shown));
    memcpy(next, shown, sizeof(next));
}

static epaper_154g_window_t plan_status(void) {
    return epaper_154g_window_plan(shown, next, true, false, BAND_Y, BAND_H, 80);
}

static epaper_154g_window_t plan_image(unsigned min_pixels) {
    return epaper_154g_window_plan(shown, next, true, false, 0, EPAPER_154G_WINDOW_HEIGHT, min_pixels);
}

static void check_full(epaper_154g_window_t w) {
    assert(w.changed && w.full && w.x == 0 && w.y == 0);
    assert(w.width == EPAPER_154G_WINDOW_WIDTH && w.height == EPAPER_154G_WINDOW_HEIGHT);
}

static void check_status_window(void) {
    reset();
    assert(!plan_status().changed);
    assert(!epaper_154g_window_plan(shown, next, true, true, BAND_Y, BAND_H, 80).changed);
    check_full(epaper_154g_window_plan(shown, next, false, false, BAND_Y, BAND_H, 80));

    // One pixel: a four-pixel column, two rows high (the controller wants end Y > start Y).
    epaper_154g_rotation_put(next, 17, 170, 0);
    epaper_154g_window_t w = plan_status();
    assert(w.changed && !w.full && w.changed_pixels == 1);
    assert(w.x == 16 && w.width == 4 && w.y == 170 && w.height == 2);

    // On the band's last row the second row comes from above.
    reset();
    epaper_154g_rotation_put(next, 199, BAND_Y + BAND_H - 1, 0);
    w = plan_status();
    assert(w.x == 196 && w.width == 4 && w.y == BAND_Y + BAND_H - 2 && w.height == 2);

    // Anything outside the band, or a forced refresh, is the whole screen.
    reset();
    epaper_154g_rotation_put(next, 0, BAND_Y - 1, 0);
    check_full(plan_status());
    reset();
    epaper_154g_rotation_put(next, 8, 160, 0);
    w = epaper_154g_window_plan(shown, next, true, true, BAND_Y, BAND_H, 80);
    check_full(w);
    assert(w.changed_pixels == 1);

    // Scattered changes merge into one byte-aligned rectangle.
    reset();
    epaper_154g_rotation_put(next, 20, 160, 0);
    epaper_154g_rotation_put(next, 101, 175, 3);
    w = plan_status();
    assert(w.x == 20 && w.width == 84 && w.y == 160 && w.height == 16 && w.changed_pixels == 2);
}

static void check_image_window(void) {
    // A 40-row region below the noise floor is left alone; raw images have a floor of one.
    reset();
    for (int x = 0; x < 79; x++) epaper_154g_rotation_put(next, x, 100, 0);
    assert(!plan_image(80).changed);
    epaper_154g_window_t w = plan_image(1);
    assert(w.changed && !w.full && w.x == 0 && w.width == 80 && w.y == 100 && w.height == 2);

    // At the floor the region counts; a quieter region does not widen the window.
    epaper_154g_rotation_put(next, 79, 100, 0);
    epaper_154g_rotation_put(next, 150, 10, 0);
    w = plan_image(80);
    assert(w.changed && !w.full && w.y == 100 && w.height == 2 && w.width == 80);
    assert(w.changed_pixels == 81);

    // Up to 40 percent of the panel is a window; more is a full refresh.
    reset();
    for (int y = 0; y < 80; y++) {
        for (int x = 0; x < EPAPER_154G_WINDOW_WIDTH; x++) epaper_154g_rotation_put(next, x, y, 0);
    }
    w = plan_image(80);
    assert(w.changed && !w.full && w.y == 0 && w.height == 80 && w.width == EPAPER_154G_WINDOW_WIDTH);
    for (int x = 0; x < EPAPER_154G_WINDOW_WIDTH; x++) epaper_154g_rotation_put(next, x, 80, 0);
    check_full(plan_image(80));
}

static void check_rotated_band(void) {
    // The band turns with the panel: a column at 90 and 270 degrees, the top at 180.
    for (unsigned r = 0; r < 4; r++) {
        int px, py;
        reset();
        epaper_154g_rotation_xy(r, 17, 170, &px, &py);
        epaper_154g_rotation_put(next, px, py, 0);
        epaper_154g_window_t w = epaper_154g_window_plan_rotated(shown, next, true, false, BAND_Y, BAND_H, 80, r);
        assert(w.changed && !w.full);
        assert(w.x <= px && px < w.x + w.width && w.y <= py && py < w.y + w.height);
        assert(w.x % 4 == 0 && w.width % 4 == 0 && w.height >= 2);

        reset();
        epaper_154g_rotation_xy(r, 17, BAND_Y - 1, &px, &py);
        epaper_154g_rotation_put(next, px, py, 0);
        check_full(epaper_154g_window_plan_rotated(shown, next, true, false, BAND_Y, BAND_H, 80, r));
    }
}

static void check_encode_and_commit(void) {
    // Inclusive coordinates, high byte first; the last byte picks the partial mode.
    epaper_154g_window_t w = {16, 170, 4, 2, true, false, 1};
    uint8_t data[9];
    epaper_154g_window_encode(w, data);
    const uint8_t expected[9] = {0, 16, 0, 19, 0, 170, 0, 171, 1};
    assert(!memcmp(data, expected, sizeof(data)));
    epaper_154g_window_t full = {0, 0, 200, 200, true, true, 0};
    epaper_154g_window_encode(full, data);
    const uint8_t whole[9] = {0, 0, 0, 199, 0, 0, 0, 199, 0};
    assert(!memcmp(data, whole, sizeof(data)));

    // Only the window's bytes reach the shadow of the glass.
    reset();
    memset(next, 0xff, sizeof(next));
    epaper_154g_window_commit(shown, next, w);
    for (int i = 0; i < EPAPER_154G_WINDOW_FRAME_BYTES; i++) {
        bool inside = i == 170 * EPAPER_154G_WINDOW_ROW_BYTES + 4 || i == 171 * EPAPER_154G_WINDOW_ROW_BYTES + 4;
        assert(shown[i] == (inside ? 0xff : 0x55));
    }
}

static void check_rotation(void) {
    static uint8_t logical[EPAPER_154G_WINDOW_FRAME_BYTES];
    static uint8_t physical[EPAPER_154G_WINDOW_FRAME_BYTES];
    static uint8_t back[EPAPER_154G_WINDOW_FRAME_BYTES];
    int px, py;

    // Clockwise: the top-left corner goes right, then down, then left.
    epaper_154g_rotation_xy(1, 0, 0, &px, &py);
    assert(px == 199 && py == 0);
    epaper_154g_rotation_xy(2, 0, 0, &px, &py);
    assert(px == 199 && py == 199);
    epaper_154g_rotation_xy(3, 0, 0, &px, &py);
    assert(px == 0 && py == 199);
    epaper_154g_rotation_xy(1, 199, 0, &px, &py);
    assert(px == 199 && py == 199);

    for (int i = 0; i < EPAPER_154G_WINDOW_FRAME_BYTES; i++) logical[i] = (uint8_t)(i * 73 + i / 50);
    for (unsigned r = 0; r < 4; r++) {
        memset(physical, 0, sizeof(physical));
        epaper_154g_rotation_frame(logical, physical, r);
        for (int y = 0; y < EPAPER_154G_WINDOW_HEIGHT; y += 7) {
            for (int x = 0; x < EPAPER_154G_WINDOW_WIDTH; x += 3) {
                epaper_154g_rotation_xy(r, x, y, &px, &py);
                assert(epaper_154g_rotation_pixel(physical, px, py) == epaper_154g_rotation_pixel(logical, x, y));
            }
        }
        epaper_154g_rotation_frame(physical, back, (4 - r) & 3);
        assert(!memcmp(back, logical, sizeof(back)));
    }

    // Four pixels to a byte, the leftmost in the top bits.
    memset(physical, 0, EPAPER_154G_WINDOW_ROW_BYTES);
    epaper_154g_rotation_put(physical, 0, 0, 3);
    epaper_154g_rotation_put(physical, 3, 0, 2);
    assert(physical[0] == 0xc2 && epaper_154g_rotation_pixel(physical, 3, 0) == 2);
}

static void check_power_button(void) {
    epaper_154g_power_button_t b = {0};
    const int64_t ms = 1000;

    // A short press pages on release; under 50 ms is contact noise.
    assert(epaper_154g_power_button_update(&b, true, false, 0) == EPAPER_154G_POWER_NONE);
    assert(epaper_154g_power_button_update(&b, true, false, 400 * ms) == EPAPER_154G_POWER_NONE);
    assert(epaper_154g_power_button_update(&b, false, false, 500 * ms) == EPAPER_154G_POWER_PAGE);
    assert(epaper_154g_power_button_update(&b, true, false, 2000 * ms) == EPAPER_154G_POWER_NONE);
    assert(epaper_154g_power_button_update(&b, false, false, 2030 * ms) == EPAPER_154G_POWER_NONE);

    // A second's hold rotates once while still held; the release then does nothing.
    assert(epaper_154g_power_button_update(&b, true, false, 3000 * ms) == EPAPER_154G_POWER_NONE);
    assert(epaper_154g_power_button_update(&b, true, false, 3999 * ms) == EPAPER_154G_POWER_NONE);
    assert(epaper_154g_power_button_update(&b, true, false, 4000 * ms) == EPAPER_154G_POWER_ROTATE);
    assert(epaper_154g_power_button_update(&b, true, false, 4050 * ms) == EPAPER_154G_POWER_NONE);
    assert(epaper_154g_power_button_update(&b, false, false, 5000 * ms) == EPAPER_154G_POWER_NONE);

    // BOOT at any point makes it a setup combo, whichever key is released first.
    assert(epaper_154g_power_button_update(&b, true, true, 6000 * ms) == EPAPER_154G_POWER_NONE);
    assert(epaper_154g_power_button_update(&b, true, false, 6100 * ms) == EPAPER_154G_POWER_NONE);
    assert(epaper_154g_power_button_update(&b, false, false, 6300 * ms) == EPAPER_154G_POWER_NONE);
    assert(epaper_154g_power_button_update(&b, true, false, 7000 * ms) == EPAPER_154G_POWER_NONE);
    assert(epaper_154g_power_button_update(&b, true, true, 7100 * ms) == EPAPER_154G_POWER_NONE);
    assert(epaper_154g_power_button_update(&b, true, false, 8500 * ms) == EPAPER_154G_POWER_NONE);
    assert(epaper_154g_power_button_update(&b, false, false, 8600 * ms) == EPAPER_154G_POWER_NONE);

    // The next gesture starts clean.
    assert(epaper_154g_power_button_update(&b, true, false, 9000 * ms) == EPAPER_154G_POWER_NONE);
    assert(epaper_154g_power_button_update(&b, false, false, 9100 * ms) == EPAPER_154G_POWER_PAGE);
}

int main(void) {
    check_status_window();
    check_image_window();
    check_rotated_band();
    check_encode_and_commit();
    check_rotation();
    check_power_button();
    puts("PASS epaper window: band windows, one-row fix, image floor and 40% cap, "
         "rotated bands, R83H encoding, rotation, PWR page/rotate/combo");
    return 0;
}
