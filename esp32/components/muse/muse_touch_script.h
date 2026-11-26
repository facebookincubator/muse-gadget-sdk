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
 * A bench press on the device's own screen (muse_touch_press): down at
 * (x1, y1), moved in a straight line to (x2, y2) over ms, then lifted. The
 * touch read (muse_touch.c's read_cb, which LVGL and the video
 * poll both go through) reports it in place of the panel, so it lands where
 * a finger would: the home's Speaker, swipes, the
 * Sound page. After the lift it reports released for RELEASE_US, so every
 * reader sees the lift, then hands back to the panel.
 *
 * Pure (no IDF), for the host tests: the caller gives the clock.
 */

#include <stdbool.h>
#include <stdint.h>

#define MUSE_TOUCH_SCRIPT_RELEASE_US 120000   /* released this long after the lift: LVGL reads every 30 ms */
#define MUSE_TOUCH_SCRIPT_MAX_MS 5000
#define MUSE_TOUCH_SCRIPT_POINTS 6            /* a path's corners at most (muse_touch_script_arm_path) */

typedef struct {
    int x1, y1, x2, y2;
    int ms;
    int64_t t0_us;
    bool armed;
    /* A path (n > 2): its corners, walked at one speed (a finger's swipe
     * through keys, the passcode's tap=pass.swipe.). */
    int n;
    int px[MUSE_TOUCH_SCRIPT_POINTS], py[MUSE_TOUCH_SCRIPT_POINTS];
} muse_touch_script_t;

static inline void muse_touch_script_arm(muse_touch_script_t *s, int x1, int y1, int x2, int y2, int ms, int64_t now_us)
{
    s->x1 = x1;
    s->y1 = y1;
    s->x2 = x2;
    s->y2 = y2;
    s->ms = ms < 1 ? 1 : ms > MUSE_TOUCH_SCRIPT_MAX_MS ? MUSE_TOUCH_SCRIPT_MAX_MS : ms;
    s->t0_us = now_us;
    s->armed = true;
    s->n = 2;
}

/* Down at the first of n corners (2..MUSE_TOUCH_SCRIPT_POINTS), through the
 * others at one speed over ms, lifted at the last. */
static inline void muse_touch_script_arm_path(muse_touch_script_t *s, const int *xs, const int *ys, int n, int ms,
                                              int64_t now_us)
{
    n = n < 2 ? 2 : n > MUSE_TOUCH_SCRIPT_POINTS ? MUSE_TOUCH_SCRIPT_POINTS : n;
    muse_touch_script_arm(s, xs[0], ys[0], xs[n - 1], ys[n - 1], ms, now_us);
    for (int i = 0; i < n; i++) {
        s->px[i] = xs[i];
        s->py[i] = ys[i];
    }
    s->n = n;
}

/* Where a path is after `done` of `len` (n > 2): along its corners by length. */
static inline void muse_touch_script_path_at(const muse_touch_script_t *s, int64_t done, int64_t len, int *x, int *y)
{
    int64_t seg[MUSE_TOUCH_SCRIPT_POINTS], total = 0;
    for (int i = 1; i < s->n; i++) {
        int64_t dx = s->px[i] - s->px[i - 1], dy = s->py[i] - s->py[i - 1];
        int64_t d = 0;
        while ((d + 1) * (d + 1) <= dx * dx + dy * dy) d++;   /* integer length: no libm here */
        seg[i] = d;
        total += d;
    }
    int64_t at = total * done / len;
    for (int i = 1; i < s->n; i++) {
        if (at <= seg[i] || i == s->n - 1) {
            int64_t d = seg[i] ? seg[i] : 1;
            int64_t t = at < d ? at : d;
            *x = s->px[i - 1] + (int)((int64_t)(s->px[i] - s->px[i - 1]) * t / d);
            *y = s->py[i - 1] + (int)((int64_t)(s->py[i] - s->py[i - 1]) * t / d);
            return;
        }
        at -= seg[i];
    }
}

/* At now_us: 1 pressed at (*x, *y), 0 released (the lift), -1 not scripted
 * (read the panel). Disarms itself once the lift has been held. */
static inline int muse_touch_script_at(muse_touch_script_t *s, int64_t now_us, int *x, int *y)
{
    if (!s->armed) {
        return -1;
    }
    int64_t dt = now_us - s->t0_us;
    int64_t len = (int64_t)s->ms * 1000;
    if (dt < 0) {
        dt = 0;
    }
    if (dt <= len) {
        if (s->n > 2) {
            muse_touch_script_path_at(s, dt, len, x, y);
            return 1;
        }
        *x = s->x1 + (int)((int64_t)(s->x2 - s->x1) * dt / len);
        *y = s->y1 + (int)((int64_t)(s->y2 - s->y1) * dt / len);
        return 1;
    }
    *x = s->x2;
    *y = s->y2;
    if (dt > len + MUSE_TOUCH_SCRIPT_RELEASE_US) {
        s->armed = false;
        return -1;
    }
    return 0;
}
