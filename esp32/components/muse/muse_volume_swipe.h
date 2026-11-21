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
#include <stdlib.h>

/* Coordinate-only recognizer, shared by every LVGL screen size. Horizontal
 * intent rejects the entire touch; a claimed vertical drag stays claimed. */
typedef struct {
    int x, y, origin, value, span, threshold;
    bool tracking, claimed;
} muse_volume_swipe_t;

static inline void muse_volume_swipe_begin(muse_volume_swipe_t *s, int x, int y,
                                           int height, int volume)
{
    *s = (muse_volume_swipe_t){
        .x = x, .y = y, .origin = volume, .value = volume,
        .span = height * 3 / 4 > 0 ? height * 3 / 4 : 1,
        .threshold = height / 40 > 8 ? height / 40 : 8,
        .tracking = true,
    };
}

static inline bool muse_volume_swipe_move(muse_volume_swipe_t *s, int x, int y)
{
    if (!s->tracking) return false;
    int dx = abs(x - s->x), dy = abs(y - s->y);
    if (!s->claimed) {
        if (dx >= s->threshold && dx >= dy) {
            s->tracking = false;
            return false;
        }
        if (dy < s->threshold || dy * 2 < dx * 3) return false;
        s->claimed = true;
    }
    int value = s->origin + (s->y - y) * 100 / s->span;
    s->value = value < 0 ? 0 : value > 100 ? 100 : value;
    /* Re-anchor at the limits so reversing direction responds immediately. */
    if (value <= 0 || value >= 100) {
        s->origin = s->value;
        s->y = y;
    }
    return true;
}
