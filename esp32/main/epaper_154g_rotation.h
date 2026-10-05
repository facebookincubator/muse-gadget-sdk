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

enum { EPAPER_154G_POWER_BUTTON_GPIO = 18 };

void epaper_154g_power_button_poll(bool boot_pressed);

// Logical square canvas -> clockwise physical panel coordinates.
static inline void epaper_154g_rotation_xy(unsigned rotation, int x, int y, int *px, int *py) {
    switch (rotation & 3) {
    case 1: *px = 199 - y; *py = x; break;
    case 2: *px = 199 - x; *py = 199 - y; break;
    case 3: *px = y; *py = 199 - x; break;
    default: *px = x; *py = y; break;
    }
}

static inline unsigned epaper_154g_rotation_pixel(const uint8_t *frame, int x, int y) {
    return frame[y * 50 + x / 4] >> (6 - 2 * (x % 4)) & 3;
}

static inline void epaper_154g_rotation_put(uint8_t *frame, int x, int y, unsigned ink) {
    int offset = y * 50 + x / 4, shift = 6 - 2 * (x % 4);
    frame[offset] = (frame[offset] & ~(3 << shift)) | (ink << shift);
}

static inline void epaper_154g_rotation_frame(const uint8_t *logical, uint8_t *physical, unsigned rotation) {
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 200; x++) {
            int px, py;
            epaper_154g_rotation_xy(rotation, x, y, &px, &py);
            epaper_154g_rotation_put(physical, px, py, epaper_154g_rotation_pixel(logical, x, y));
        }
    }
}

// Any BOOT overlap cancels the remaining PWR gesture.
typedef enum {
    EPAPER_154G_POWER_NONE,
    EPAPER_154G_POWER_PAGE,
    EPAPER_154G_POWER_ROTATE,
} epaper_154g_power_action_t;

typedef struct {
    bool pressed, combo, long_fired;
    int64_t started_us;
} epaper_154g_power_button_t;

static inline epaper_154g_power_action_t epaper_154g_power_button_update(epaper_154g_power_button_t *button,
                                                              bool pressed, bool boot,
                                                              int64_t now_us) {
    if (pressed && !button->pressed) {
        button->started_us = now_us;
        button->combo = boot;
        button->long_fired = false;
    }
    if (boot && (pressed || button->pressed)) button->combo = true;
    int64_t held_us = now_us - button->started_us;
    epaper_154g_power_action_t action = EPAPER_154G_POWER_NONE;
    if (pressed && !button->combo && !button->long_fired && held_us >= 1000000) {
        button->long_fired = true;
        action = EPAPER_154G_POWER_ROTATE;
    } else if (!pressed && button->pressed && !button->combo && !button->long_fired
               && held_us >= 50000 && held_us < 1000000) {
        action = EPAPER_154G_POWER_PAGE;
    }
    button->pressed = pressed;
    return action;
}
