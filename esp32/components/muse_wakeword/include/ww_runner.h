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
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The interface contract with the wake word runner. This header
 * carries no ESP-IDF types so the host tests can link the same
 * code. The implementation linked here is ww_runner_bench.c
 * (serial/clap bench triggers; no speech detection);
 * muse_wakeword is written against this contract and nothing
 * else. Host tests script ww_event_t sequences directly in
 * tests/muse_wakeword_harness.c.
 *
 * Guarantees the integration relies on:
 * - All runner calls happen on the wake word task (create() runs at
 *   init, before that task starts), so implementations need no
 *   internal locking and must not call back into the integration.
 * - feed() accepts arbitrary frame counts (the firmware's 320-sample
 *   chunks included) and re-frames internally; it never allocates,
 *   blocks, or calls into ESP-IDF.
 * - WW_EVENT_WAKE means "fire now": smoothing, thresholding, and
 *   cooldown have already been applied inside the runner.
 * - SPEECH_START/END are advisory; a runner that never emits them
 *   still integrates (captures end on the hard cap).
 * - reset() is cheap and idempotent, and warm-up after create/reset
 *   is the runner's business, not the caller's.
 */

typedef enum {
    WW_EVENT_NONE = 0,
    WW_EVENT_WAKE,           /* wake word detected (detector already applied) */
    WW_EVENT_SPEECH_START,   /* endpointing only: speech present after wake */
    WW_EVENT_SPEECH_END,     /* endpointing only: trailing silence reached */
} ww_event_t;

typedef enum {
    WW_SENSITIVITY_LOW = 0,
    WW_SENSITIVITY_DEFAULT,
    WW_SENSITIVITY_HIGH,
} ww_sensitivity_t;

typedef enum {
    WW_VAD_ENERGY = 0,   /* SDK-side energy judgement; runner may stay silent */
    WW_VAD_SILERO,       /* runner emits SPEECH_START/END from the VAD model */
} ww_vad_mode_t;

typedef struct {
    const uint8_t *model;        /* .pte bytes (flash-resident), may be NULL (the bench runner ignores it) */
    size_t model_len;
    uint32_t sample_rate_hz;     /* must be 16000 for the current front end */
    ww_sensitivity_t sensitivity;
    ww_vad_mode_t vad;
} ww_config_t;

typedef struct ww_runner ww_runner_t;

ww_runner_t *ww_runner_create(const ww_config_t *cfg);   /* cfg may be NULL: defaults */
void ww_runner_destroy(ww_runner_t *r);
void ww_runner_reset(ww_runner_t *r);
ww_event_t ww_runner_feed(ww_runner_t *r, const int16_t *pcm, size_t frames);
void ww_runner_set_listening(ww_runner_t *r, bool on);
void ww_runner_set_suppressed(ww_runner_t *r, bool on);

#ifdef __cplusplus
}
#endif
