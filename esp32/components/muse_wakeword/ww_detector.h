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
#include <stdint.h>

#include "ww_runner.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Posterior detector for the streaming wake word model: a 5-frame
 * moving average of the wakeword posterior against a sensitivity
 * threshold, 30-frame warm-up suppression (the model receptive field
 * is 30 frames; the first 29 outputs are meaningless), and a 1 s
 * (50-frame) refractory period after a fire. Pure state machine, no
 * allocation or platform calls, so host tests pin it directly.
 */

#define WW_DETECTOR_SMOOTH_FRAMES 5
#define WW_DETECTOR_WARMUP_FRAMES 29
#define WW_DETECTOR_COOLDOWN_FRAMES 50

typedef struct {
    float threshold;
    float ring[WW_DETECTOR_SMOOTH_FRAMES];
    float sum;
    uint8_t ring_count;
    uint8_t ring_head;
    uint32_t frames_seen;
    uint32_t cooldown_left;
} ww_detector_t;

void ww_detector_init(ww_detector_t *detector, ww_sensitivity_t sensitivity);
void ww_detector_reset(ww_detector_t *detector);

/* Feed one frame's wakeword posterior; true exactly on the firing frame. */
bool ww_detector_push(ww_detector_t *detector, float posterior);

#ifdef __cplusplus
}
#endif
