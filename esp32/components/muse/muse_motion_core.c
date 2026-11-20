/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 */
#include "muse_motion_core.h"
#include <math.h>
#include <string.h>

void muse_motion_reset(muse_motion_core_t *s)
{
    memset(s, 0, sizeof(*s));
}

static int64_t distance_sq(muse_motion_sample_t a, muse_motion_sample_t b)
{
    int32_t x = (int32_t)a.x - b.x, y = (int32_t)a.y - b.y, z = (int32_t)a.z - b.z;
    return (int64_t)x*x + (int64_t)y*y + (int64_t)z*z;
}

bool muse_motion_update(muse_motion_core_t *s, muse_motion_sample_t raw, uint32_t now)
{
    if (s->have_sample && (uint32_t)(now - s->last_ms) > 500) muse_motion_reset(s);
    s->have_sample = true;
    s->last_ms = now;
    if (s->woke && (uint32_t)(now - s->wake_ms) > 1500) s->woke = false;
    int64_t mag = (int64_t)raw.x*raw.x + (int64_t)raw.y*raw.y + (int64_t)raw.z*raw.z;
    if (s->rising && (uint32_t)(now - s->rise_ms) > 1500) {
        s->rising = s->rise_pending = s->rise_woke = false;
    }
    /* The first impulse distinguishes lift/braking from lower/landing.
     * A confirmed upward start may subsequently have low apparent gravity
     * while the hand brakes; that is not a new lowering episode. */
    if (!s->lowering && !s->rising && s->have_reference) {
        if (mag > 1150LL*1150) {
            if (!s->rise_pending) s->rise_ms = now;
            s->rise_pending = true;
            if ((uint32_t)(now - s->rise_ms) >= 40) {
                s->rising = true;
                s->rise_pending = false;
            }
        } else {
            s->rise_pending = false;
        }
    }
    /* Lowering reduces apparent gravity. Normalizing that smaller vector
     * can otherwise turn translation into a fake viewing tilt. Block wake
     * through the downward impulse and landing, then adopt the new rest. */
    if (mag < 900LL*900 && !s->rising) {
        s->lowering = true;
        s->moving = s->settling = false;
    }
    bool inhibit = s->lowering;
    if (s->lowering) {
        /* A fixed delay can expire in mid-air. Require a full stable window
         * relative to one anchor, not merely tiny changes between samples. */
        if (mag < 900LL*900 || mag > 1100LL*1100 ||
            distance_sq(raw, s->landing_reference) > 60LL*60) {
            s->lowering_ms = now;
            s->landing_reference = raw;
        } else if ((uint32_t)(now - s->lowering_ms) >= 600) {
            s->lowering = false;
            s->rebase = true;
            inhibit = false;
        }
    }
    if (s->rising && !s->rise_woke && s->have_reference && s->reference.z >= -250) {
        s->rise_woke = true;
        s->moved_ms = s->rise_ms;
        s->moving = s->settling = false;
        if (!s->woke) {
            s->woke = true;
            s->wake_ms = now;
            return true;
        }
    }
    if (mag < 750LL*750 || mag > 1250LL*1250) {
        /* Brief acceleration bursts are normal during a pickup. Do not
         * discard its resting reference or interpret raw Z as a reversal.
         * Require fresh gravity samples before waking. */
        if (!s->bad) s->bad_ms = now;
        s->bad = true;
        s->settling = false;
        s->candidate = MUSE_POSE_UNKNOWN;
        s->since_ms = now;
        if ((uint32_t)(now - s->bad_ms) > 500) {
            s->have_reference = s->filter_valid = s->moving = false;
            s->pose = MUSE_POSE_UNKNOWN;
        }
        return false;
    }
    s->bad = false;
    /* Direction comes from normalized, low-pass-filtered gravity rather
     * than acceleration magnitude. Translation and landing impulses alone
     * do not establish a lift direction. */
    float scale = 1000.0f / sqrtf((float)mag);
    muse_motion_sample_t a = { (int16_t)(raw.x*scale), (int16_t)(raw.y*scale), (int16_t)(raw.z*scale) };
    muse_motion_sample_t unfiltered = a;
    if (s->rebase) {
        s->rebase = false;
        s->reference = a;
        s->have_reference = true;
        s->filter_valid = false;
        s->moving = s->settling = false;
    }
    if (s->filter_valid) {
        a.x = (s->filtered.x + 2*a.x) / 3;
        a.y = (s->filtered.y + 2*a.y) / 3;
        a.z = (s->filtered.z + 2*a.z) / 3;
        float norm = 1000.0f / sqrtf((float)((int32_t)a.x*a.x + (int32_t)a.y*a.y + (int32_t)a.z*a.z));
        a.x = (int16_t)(a.x*norm);
        a.y = (int16_t)(a.y*norm);
        a.z = (int16_t)(a.z*norm);
    }
    bool stable = s->filter_valid && distance_sq(a, s->filtered) <= 80LL*80;
    s->filter_valid = true;
    s->filtered = s->previous = a;
    bool wake = false;
    if (s->have_reference && !inhibit) {
        int32_t progress = (int32_t)sqrtf((float)distance_sq(a, s->reference));
        int32_t raw_progress = (int32_t)sqrtf((float)distance_sq(unfiltered, s->reference));
        if (!s->moving && progress >= 180) {
            bool tilt = (s->reference.z >= 300 && a.z <= s->reference.z - 15 && a.z >= -250) ||
                        (s->reference.z <= -650 && a.z >= -250);
            if (tilt) {
                s->moving = true;
                s->settling = false;
                s->moved_ms = now;
                s->direction = s->reference.z < 0 ? 1 : -1;
                s->best_progress = raw_progress;
            } else if (stable) {
                s->reference = a;   /* adopt a completed put-down without waking */
            }
        }
        if (s->moving) {
            bool reversed = raw_progress + 90 < s->best_progress ||
                (s->direction < 0 && a.z >= s->reference.z - 5);
            if (reversed || (uint32_t)(now - s->moved_ms) > 2500) {
                s->moving = s->settling = false;
                s->reference = a;
            } else if (a.z >= -250) {
                if (raw_progress > s->best_progress) s->best_progress = raw_progress;
                if (!s->settling) s->settled_ms = now;
                s->settling = true;
                /* Two fresh 21 Hz samples: no long stillness requirement. */
                if ((uint32_t)(now - s->settled_ms) >= 40) {
                    wake = !s->woke;
                    if (wake) {
                        s->woke = true;
                        s->wake_ms = now;
                    }
                    s->moving = s->settling = false;
                    s->reference = a;
                }
            } else {
                s->settling = false;
            }
        }
    }
    muse_motion_pose_t p = MUSE_POSE_UNKNOWN;
    if (a.z >= (s->pose == MUSE_POSE_UP ? 650 : 800)) p = MUSE_POSE_UP;
    else if (a.z <= (s->pose == MUSE_POSE_DOWN ? -650 : -800)) p = MUSE_POSE_DOWN;
    else if (a.z > -550 && a.z < 550) p = MUSE_POSE_UPRIGHT;
    if (p == MUSE_POSE_UNKNOWN || p != s->candidate) {
        s->candidate = p;
        s->since_ms = now;
        return wake;
    }
    uint32_t dwell = p == MUSE_POSE_UP ? 300 : 600;
    if ((uint32_t)(now - s->since_ms) >= dwell) {
        s->pose = p;
        if (!s->have_reference) {
            s->reference = a;
            s->have_reference = true;
        }
    }
    return wake;
}
