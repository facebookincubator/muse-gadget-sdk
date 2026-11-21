/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 */
#include "muse_motion.h"
#include "muse_motion_core.h"
#include "muse_board.h"
#include "muse_settings.h"
#include "esp_log.h"
#include <stdatomic.h>
#include <stdio.h>

static muse_motion_core_t s_core;
static bool s_requested, s_polled;
static atomic_bool s_active, s_quiet;
static atomic_int s_pose;
static atomic_int s_x, s_y, s_z;
static uint32_t s_last_poll, s_last_sample;
static bool s_was_asleep;
static atomic_uint s_wakes;
static atomic_uint s_detection_ms;

bool muse_motion_active(void) { return atomic_load(&s_active); }
bool muse_motion_quiet(void) { return atomic_load(&s_quiet); }
uint32_t muse_motion_poll_ms(void)
{
    return muse_settings_lift_wake() ? 50 : muse_state_asleep() ? 250 : 100;
}

static void clear_state(void)
{
    muse_motion_reset(&s_core);
    atomic_store(&s_quiet, false);
    atomic_store(&s_pose, MUSE_POSE_UNKNOWN);
    atomic_store(&s_x, 0);
    atomic_store(&s_y, 0);
    atomic_store(&s_z, 0);
}

static void failed(esp_err_t err)
{
    atomic_store(&s_active, false);
    clear_state();
    (void)muse_board->motion_enable(false);
    ESP_LOGW("muse_motion", "motion disabled (%s); toggle gestures off/on to retry", esp_err_to_name(err));
}

bool muse_motion_tick(uint32_t now)
{
    bool asleep = muse_state_asleep();
    if (asleep && !s_was_asleep) {
        /* A movement made before pressing Sleep must not immediately undo
         * that explicit action. Start the next pickup from the current pose. */
        s_core.moving = s_core.settling = false;
        s_core.rising = s_core.rise_pending = s_core.rise_woke = false;
        s_core.woke = false;
        if (s_core.have_reference) s_core.rebase = true;
    }
    s_was_asleep = asleep;
    bool wanted = muse_settings_lift_wake() || muse_settings_face_down_quiet();
    if (wanted != s_requested) {
        s_requested = wanted;
        clear_state();
        s_polled = false;
        if (!muse_board->motion_enable || !muse_board->motion_read) return false;
        esp_err_t err = muse_board->motion_enable(wanted);
        atomic_store(&s_active, wanted && err == ESP_OK);
        s_last_sample = now;
        if (err != ESP_OK) failed(err);
    }
    if (!muse_motion_active()) return false;
    /* Lift wake follows the 21 Hz sensor at 20 Hz; quiet-only keeps its
     * slower cadence. The input wait uses this same interval. */
    uint32_t interval = muse_motion_poll_ms();
    if (s_polled && (uint32_t)(now - s_last_poll) < interval) return false;
    s_polled = true;
    s_last_poll = now;
    muse_motion_sample_t a;
    esp_err_t err = muse_board->motion_read(&a);
    if (err == ESP_ERR_NOT_FINISHED && (uint32_t)(now - s_last_sample) < 1000) return false;
    if (err != ESP_OK) {
        failed(err);
        return false;
    }
    s_last_sample = now;
    atomic_store(&s_x, a.x);
    atomic_store(&s_y, a.y);
    atomic_store(&s_z, a.z);
    bool wake = muse_motion_update(&s_core, a, now);
    atomic_store(&s_pose, s_core.pose);
    atomic_store(&s_quiet, muse_settings_face_down_quiet() && s_core.pose == MUSE_POSE_DOWN);
    wake = wake && muse_settings_lift_wake();
    if (wake && asleep) {
        atomic_fetch_add(&s_wakes, 1);
        atomic_store(&s_detection_ms, (uint32_t)(now - s_core.moved_ms));
    }
    return wake;
}

void muse_motion_status(void)
{
    static const char *const poses[] = { "unknown", "face_up", "face_down", "upright" };
    /* Diagnostic fields are individual snapshots, not a synchronized frame. */
    printf("@motion {\"active\":%s,\"lift_wake\":%s,\"face_down_quiet\":%s,\"quiet\":%s,"
           "\"pose\":\"%s\",\"accel_mg\":[%d,%d,%d],\"asleep\":%s,\"wakes\":%u,\"detect_ms\":%u}\n",
           muse_motion_active() ? "true" : "false", muse_settings_lift_wake() ? "true" : "false",
           muse_settings_face_down_quiet() ? "true" : "false", muse_motion_quiet() ? "true" : "false",
           poses[atomic_load(&s_pose)], atomic_load(&s_x), atomic_load(&s_y), atomic_load(&s_z),
           muse_state_asleep() ? "true" : "false", atomic_load(&s_wakes), atomic_load(&s_detection_ms));
    fflush(stdout);
}
