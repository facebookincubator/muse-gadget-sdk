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

/* The accel device's events, from its readings (imu_events.h). */

#include "imu_events.h"

#include <math.h>
#include <string.h>

/* Counts are readings at IMU_EVENTS_HZ (10 ms each); levels are in g.
 * "Motion" is how far a reading is from gravity, "jerk" how far from the
 * reading before. */
#define GRAV_K 0.05f            /* gravity follows steady readings within about 0.2 s... */
#define GRAV_K_MOVING 0.005f    /* ...and barely jerky ones: a knock or a shake doesn't drag it */
#define STEADY_G 0.15f

#define FLAT_Z 0.85f            /* lying flat: gravity within about 30 degrees of z */
#define TILTED_Z 0.6f           /* clearly not flat any more */
#define FLAT_STILL_G 0.3f
#define FLAT_N 50               /* flat and still for 0.5 s */

#define QUIET_G 0.2f            /* still: this little motion and jerk */
#define QUIET_N 5               /* still for 50 ms before a tap */
#define TAP_JERK_G 0.6f         /* a tap begins with this much jerk... */
#define TAP_G 0.8f              /* ...or this much motion */
#define SPIKE_END_G 0.3f
#define SPIKE_MAX_N 4           /* and is over within 40 ms */
#define SETTLE_N 15             /* then still again within 150 ms... */
#define SETTLED_N 5             /* ...for 50 ms */
#define MOVED_G 0.5f            /* motion while settling: a movement, not a tap */
#define DOUBLE_N 40             /* a second tap starting within 0.4 s: a double tap */

#define SWING_G 1.0f            /* a swing: motion past this... */
#define SWING_PEAK_G 1.5f       /* ...peaking past this... */
#define SWING_MIN_N 4           /* ...for 40 ms at least, longer than a tap */
#define SHAKE_SWINGS 4          /* this many within a second: a shake */
#define SHAKE_WINDOW_N 100
#define AFTER_SHAKE_N 100       /* then no taps or shakes for a second */

static float norm3(const float v[3])
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void imu_events_init(imu_events_t *s)
{
    memset(s, 0, sizeof(*s));
    s->face = 2;
    s->settle_n = -1;
}

static unsigned face(imu_events_t *s, float motion)
{
    float z = s->grav[2];
    if (fabsf(z) < TILTED_Z) {
        s->face = 0;
        s->face_n = 0;
        return 0;
    }
    int cand = 0;
    if (motion < FLAT_STILL_G && fabsf(norm3(s->grav) - 1.0f) < 0.2f) {
        cand = z > FLAT_Z ? 1 : z < -FLAT_Z ? -1 : 0;
    }
    if (!cand || cand != s->face_cand) {
        s->face_cand = cand;
        s->face_n = 0;
        return 0;
    }
    if (++s->face_n < FLAT_N || s->face == cand) {
        return 0;
    }
    bool first = s->face == 2;
    s->face = cand;
    return first ? 0 : cand > 0 ? IMU_FACE_UP : IMU_FACE_DOWN;
}

static unsigned shake(imu_events_t *s, float motion)
{
    if (s->swings && ++s->window_n > SHAKE_WINDOW_N) {
        s->swings = 0;
    }
    if (motion > SWING_G) {
        s->swing_n++;
        s->swing_peak = motion > s->swing_peak ? motion : s->swing_peak;
        return 0;
    }
    bool swing = s->swing_n >= SWING_MIN_N && s->swing_peak >= SWING_PEAK_G;
    s->swing_n = 0;
    s->swing_peak = 0.0f;
    if (!swing) {
        return 0;
    }
    if (!s->swings++) {
        s->window_n = 0;
    }
    if (s->swings < SHAKE_SWINGS) {
        return 0;
    }
    s->swings = 0;
    return IMU_SHAKE;
}

static unsigned tap(imu_events_t *s, float motion, float jerk)
{
    unsigned ev = 0;
    /* A lone tap is told once a second couldn't start any more, and not
     * while one might be under way. */
    if (s->taps && ++s->since_tap > DOUBLE_N && !s->spike_n && s->settle_n < 0) {
        s->taps = 0;
        ev |= IMU_TAP;
    }
    bool still = motion < QUIET_G && jerk < QUIET_G;
    if (s->spike_n) {
        if (motion < SPIKE_END_G && jerk < SPIKE_END_G) {
            s->spike_n = 0;
            s->settle_n = 0;
            s->settled_n = 0;
        } else if (++s->spike_n > SPIKE_MAX_N) {
            s->spike_n = 0;   /* too long for a tap */
            s->quiet_n = 0;
        }
        return ev;
    }
    if (s->settle_n >= 0) {
        s->settle_n++;
        s->settled_n = still ? s->settled_n + 1 : 0;
        if (motion > MOVED_G || (s->settle_n > SETTLE_N && s->settled_n < SETTLED_N)) {
            s->settle_n = -1;
            s->quiet_n = 0;
        } else if (s->settled_n >= SETTLED_N) {
            s->settle_n = -1;
            s->quiet_n = s->settled_n;
            if (s->taps) {
                s->taps = 0;
                ev |= IMU_DOUBLE_TAP;
            } else {
                s->taps = 1;
                s->since_tap = 0;
            }
        }
        return ev;
    }
    if (s->quiet_n >= QUIET_N && (jerk > TAP_JERK_G || motion > TAP_G)) {
        s->spike_n = 1;
        s->quiet_n = 0;
        return ev;
    }
    s->quiet_n = still ? s->quiet_n + 1 : 0;
    return ev;
}

unsigned imu_events_feed(imu_events_t *s, const float a[3])
{
    if (!isfinite(a[0]) || !isfinite(a[1]) || !isfinite(a[2])) {
        return 0;
    }
    if (!s->primed) {
        memcpy(s->grav, a, sizeof(s->grav));
        memcpy(s->prev, a, sizeof(s->prev));
        s->primed = true;
        return 0;
    }
    float lin[3], d[3];
    for (int i = 0; i < 3; i++) {
        lin[i] = a[i] - s->grav[i];
        d[i] = a[i] - s->prev[i];
    }
    float motion = norm3(lin), jerk = norm3(d);
    float k = jerk < STEADY_G ? GRAV_K : GRAV_K_MOVING;
    for (int i = 0; i < 3; i++) {
        s->grav[i] += k * lin[i];
        s->prev[i] = a[i];
    }
    unsigned ev = face(s, motion);
    if (s->quiet_after > 0) {
        s->quiet_after--;
        s->quiet_n = 0;
        return ev;
    }
    unsigned shook = shake(s, motion);
    if (shook) {
        /* What looked like taps was the shake. */
        s->taps = s->spike_n = 0;
        s->settle_n = -1;
        s->quiet_n = 0;
        s->quiet_after = AFTER_SHAKE_N;
        return ev | shook;
    }
    return ev | tap(s, motion, jerk);
}
