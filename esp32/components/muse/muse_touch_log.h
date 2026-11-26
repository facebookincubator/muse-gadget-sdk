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
 * The touch that started or ended a call, for its log lines (so a call
 * that started or ended by itself, or by a stray touch, is told apart from the
 * gadget alone): where it was, how long it was held, and how many touch-downs
 * the panel saw in the last 5 s (muse_touch.c counts them,
 * muse_touch_downs_within()). Pure, host-tested (test_mg_ble).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define MUSE_TOUCH_RING 16              /* touch-downs remembered */
#define MUSE_TOUCH_DOWNS_WINDOW_MS 5000 /* "touch-downs in the last 5 s" */

typedef struct {
    int64_t at_us[MUSE_TOUCH_RING];
    uint32_t n;                         /* touch-downs since boot */
} muse_touch_ring_t;

static inline void muse_touch_ring_add(muse_touch_ring_t *r, int64_t now_us)
{
    r->at_us[r->n++ % MUSE_TOUCH_RING] = now_us ? now_us : 1;
}

/* Touch-downs within window_ms of now (MUSE_TOUCH_RING at most). */
static inline int muse_touch_ring_count(const muse_touch_ring_t *r, int64_t now_us, int window_ms)
{
    int c = 0;
    for (int i = 0; i < MUSE_TOUCH_RING; i++) {
        int64_t t = r->at_us[i];
        c += t && now_us - t <= (int64_t)window_ms * 1000 && t <= now_us;
    }
    return c;
}

/* Both go out on mg.call and mg.lct, which the Nordic UART log mirror shows to
 * any central with no pairing: no coordinates (x and y stay in the signature
 * for the cable console's lines). */

/* "touch(hold)" (hold in ms): the call timeline's by= / end= detail. */
static inline int muse_touch_tag(char *out, size_t cap, int x, int y, uint32_t hold_ms)
{
    (void)x;
    (void)y;
    return snprintf(out, cap, "touch(%u)", (unsigned)hold_ms);
}

/* "held N ms, M touch-downs in the last 5 s": the mg.call line's detail. */
static inline int muse_touch_detail(char *out, size_t cap, int x, int y, uint32_t hold_ms, int downs)
{
    (void)x;
    (void)y;
    return snprintf(out, cap, "held %u ms, %d touch-down%s in the last 5 s", (unsigned)hold_ms, downs,
                    downs == 1 ? "" : "s");
}
