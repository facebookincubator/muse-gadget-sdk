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
 * The SDK-owned wake word runner contract.
 * This header carries no ESP-IDF types: the same runner, ww_runner_espnn.cpp
 * (microfrontend + models/hey_muse.pte through ExecuTorch's ESP-NN delegate
 * + ww_detector.c), builds into the firmware and, against the same registry
 * components, on a host: tests/test_muse_wakeword.py builds the real runner
 * that way (WakeWordRunner), and scripts ww_event_t sequences through
 * tests/muse_wakeword_harness.c for the session core.
 *
 * Guarantees the integration relies on:
 * - All runner calls happen on one task (the firmware's wake word task;
 *   create() runs at init, before that task starts), so the runner takes
 *   no locks and must not call back into the integration.
 * - feed() accepts arbitrary frame counts (the firmware's 320-sample
 *   chunks included) and re-frames internally. Neither feed() nor reset()
 *   allocates: everything is allocated at create.
 * - WW_EVENT_WAKE means "fire now": smoothing, thresholding, and the 2 s
 *   refractory period have already been applied inside the runner.
 * - SPEECH_START/END are advisory; a runner that never emits them
 *   still integrates (captures end on energy silence or the hard cap).
 *   This runner never emits them: endpointing is the SDK-side energy
 *   tier, and there is no VAD model.
 * - reset() is idempotent and clears the model's streaming state; after a
 *   failed inference it also reloads the model.
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
    const uint8_t *model;        /* .pte bytes (kept by the caller); NULL uses the embedded model */
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
/* False while the runner cannot detect anything (its model failed to
 * load or run); reset() retries. Lets "listening" mean listening. */
bool ww_runner_ok(ww_runner_t *r);
/* Tests and tuning only: fn(ctx, probability) after every inference (30 ms),
 * before the detector sees it. NULL removes it. */
void ww_runner_set_score_hook(ww_runner_t *r, void (*fn)(void *ctx, float probability), void *ctx);

#ifdef __cplusplus
}
#endif
