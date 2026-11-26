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
 * A swipe that starts on a control is a swipe (a slow drag that begins on a
 * home button must not press it): once a touch has moved past the slop from where it went down, it
 * belongs to the swipe, and the control under its start gets neither its tap
 * nor its hold, however long the finger stays down. Once a talk turn has
 * begun (held 250 ms within the slop), movement doesn't end it and
 * swipes are off until release.
 *
 * Plain C, for the host tests: muse_ui.c feeds it the pointer's samples (its
 * touch-down and lift, a timer at LVGL's read period while it's down, and
 * every ask); a video's own touch path (LVGL paused)
 * uses muse_swipe_past_slop.
 */

#include <stdbool.h>
#include <stdint.h>

/* The touch hold slop (<= 12 px); the page swipe takes over from
 * LVGL's scroll limit (10 px) too, whichever comes first (muse_ui_touch_swiped). */
#define MUSE_SWIPE_SLOP_PX 12

typedef struct {
    bool down;      /* a finger is down */
    bool swiped;    /* this touch has moved past the slop from its start: a swipe */
    int16_t x0, y0; /* where it went down */
    int16_t max;    /* the farthest it got from there, on either axis (px) */
} muse_swipe_t;

/* Either axis past the slop: a swipe (the slop is per axis, "<= 12 px"). */
static inline bool muse_swipe_past_slop(int dx, int dy)
{
    return dx > MUSE_SWIPE_SLOP_PX || dx < -MUSE_SWIPE_SLOP_PX || dy > MUSE_SWIPE_SLOP_PX || dy < -MUSE_SWIPE_SLOP_PX;
}

/* One sample of the panel: pressed, at x, y. True on the sample a touch
 * becomes a swipe. */
static inline bool muse_swipe_sample(muse_swipe_t *s, bool pressed, int x, int y)
{
    if (!pressed) {
        s->down = false;   /* swiped stays readable until the next touch-down */
        return false;
    }
    if (!s->down) {
        s->down = true;
        s->swiped = false;
        s->x0 = (int16_t)x;
        s->y0 = (int16_t)y;
        s->max = 0;
        return false;
    }
    int dx = x - s->x0, dy = y - s->y0;
    int m = dx < 0 ? -dx : dx, my = dy < 0 ? -dy : dy;
    m = my > m ? my : m;
    s->max = (int16_t)(m > s->max ? m : s->max);
    if (!s->swiped && muse_swipe_past_slop(dx, dy)) {
        s->swiped = true;
        return true;
    }
    return false;
}
