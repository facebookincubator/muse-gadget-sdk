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
 * The well-known accel device's events (components/muse_apps/sdk/api/api.json: tap, double_tap,
 * shake, face_up, face_down), told from its readings in software for an IMU
 * whose own feature engine doesn't have them. Plain C with no IDF, so the
 * host tests run it (tests/test_apps_dev.py).
 *
 * Feed it every new sample, about IMU_EVENTS_HZ a second, in g, in the
 * screen's frame (x right, y up, z out of the screen). The thresholds are
 * estimates for a watch-sized gadget, not yet tuned on hardware.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMU_EVENTS_HZ 100

/* Bits of what imu_events_feed() returns: 1 << each event's index, in
 * the well-known accel device's order. */
#define IMU_TAP (1u << 0)
#define IMU_DOUBLE_TAP (1u << 1)
#define IMU_SHAKE (1u << 2)
#define IMU_FACE_UP (1u << 3)
#define IMU_FACE_DOWN (1u << 4)

typedef struct {
    bool primed;
    float grav[3];      /* gravity: the readings, low-passed */
    float prev[3];      /* the last reading */
    /* Lying flat: the last orientation found (2 none yet, 1 up, -1 down, 0
     * neither), and one being confirmed. The first is taken without an event. */
    int face, face_cand, face_n;
    /* Taps: a short, sharp spike out of stillness that settles again. */
    int quiet_n;        /* still readings in a row */
    int spike_n;        /* readings into a spike; 0 none */
    int settle_n;       /* readings since it ended; -1 not settling */
    int settled_n;      /* still readings since it ended */
    int taps;           /* a tap waiting to see whether a second follows */
    int since_tap;      /* readings since that tap */
    /* Shakes: strong swings, each lasting a while, several in a second. */
    int swing_n;        /* readings in the current swing */
    float swing_peak;
    int swings;         /* swings since window_n began */
    int window_n;
    int quiet_after;    /* readings before another shake (or tap) counts */
} imu_events_t;

void imu_events_init(imu_events_t *s);

/* One reading; returns the events it completes (IMU_* bits). A single
 * tap is told once no second one followed within 0.4 s. */
unsigned imu_events_feed(imu_events_t *s, const float a[3]);

#ifdef __cplusplus
}
#endif
