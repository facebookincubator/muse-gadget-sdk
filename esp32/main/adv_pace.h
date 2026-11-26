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
 * The musegadgets advertising interval: fast, 30-50 ms,
 * for 30 s after boot, after a disconnect (from the link's loss) and after a
 * talk press with no app connected; then slow, Apple's recommended 417.5 ms
 * (318.75 and 546.25 ms are the other choices). For the companion's payload
 * once the gadget is set up; Link setup's own advertising before setup keeps
 * its fast default. Pure, for the host tests: ble_server.c keeps one, arms a
 * one-shot callout for its end and restarts advertising with the slow set.
 */

#include <stdbool.h>
#include <stdint.h>

#define ADV_PACE_FAST_US (30LL * 1000 * 1000)
#define ADV_PACE_FAST_ITVL_MIN 48   /* 0.625 ms units: 30 ms */
#define ADV_PACE_FAST_ITVL_MAX 80   /* 50 ms */
#define ADV_PACE_SLOW_ITVL 668      /* 417.5 ms */

typedef struct {
    int64_t fast_until_us;   /* the fast window's end (0: never started) */
    const char *why;         /* its trigger, for the log */
} adv_pace_t;

/* A trigger at now_us (boot, a disconnect, a talk press with no app): fast
 * for 30 s from now, a window already open re-armed. */
static inline void adv_pace_trigger(adv_pace_t *a, int64_t now_us, const char *why)
{
    a->fast_until_us = now_us + ADV_PACE_FAST_US;
    a->why = why;
}

static inline bool adv_pace_fast(const adv_pace_t *a, int64_t now_us)
{
    return a->fast_until_us && now_us < a->fast_until_us;
}

/* The interval to advertise at now_us, 0.625 ms units. */
static inline void adv_pace_itvl(const adv_pace_t *a, int64_t now_us, uint16_t *min, uint16_t *max)
{
    bool fast = adv_pace_fast(a, now_us);
    *min = fast ? ADV_PACE_FAST_ITVL_MIN : ADV_PACE_SLOW_ITVL;
    *max = fast ? ADV_PACE_FAST_ITVL_MAX : ADV_PACE_SLOW_ITVL;
}
