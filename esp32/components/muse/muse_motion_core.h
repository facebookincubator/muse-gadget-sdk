/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Acceleration in sensor coordinates, in milli-g. +Z points out of the face
 * on the 1.75C. No gyroscope or integration is needed for these gestures. */
typedef struct { int16_t x, y, z; } muse_motion_sample_t;
typedef enum {
    MUSE_POSE_UNKNOWN, MUSE_POSE_UP, MUSE_POSE_DOWN, MUSE_POSE_UPRIGHT,
} muse_motion_pose_t;

typedef struct {
    muse_motion_pose_t pose, candidate;
    uint32_t since_ms, last_ms;
    uint32_t moved_ms, settled_ms;
    uint32_t bad_ms;
    uint32_t lowering_ms;
    uint32_t rise_ms;
    uint32_t wake_ms;
    muse_motion_sample_t reference, previous, filtered, landing_reference;
    int32_t best_progress;
    int8_t direction;
    bool have_sample, have_reference, moving, settling, filter_valid, bad, rebase, lowering;
    bool rise_pending, rising, rise_woke, woke;
} muse_motion_core_t;

/* Returns true once after a pickup/tilt followed by a settled viewing pose.
 * Initial placement, stationary noise and sampling gaps never wake. */
bool muse_motion_update(muse_motion_core_t *s, muse_motion_sample_t a, uint32_t now_ms);
void muse_motion_reset(muse_motion_core_t *s);
