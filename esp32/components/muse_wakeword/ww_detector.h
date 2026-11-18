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
 * Detector over the model's wake probabilities, one per 30 ms inference,
 * as microWakeWord's runners apply it: it fires when the mean of the last
 * five probabilities is above the sensitivity's cutoff, and the five must
 * all be real (no firing in the first 150 ms). After a fire it clears its
 * window and ignores 2 s of inferences. Pure state machine, no allocation
 * or platform calls, so host tests pin it directly.
 *
 * Cutoffs, from models/hey_muse.json:
 *   Low 0.26 (fewest false wakes), Default 0.22, High 0.13 (easiest to
 *   trigger).
 */

#define WW_DETECTOR_WINDOW 5
#define WW_DETECTOR_REFRACTORY_FRAMES 200   /* 2 s of 10 ms feature frames */
#define WW_DETECTOR_STRIDE 3                /* feature frames per inference */

typedef struct {
    float cutoff;
    float probs[WW_DETECTOR_WINDOW];
    int count;
    int head;
    int refractory;   /* feature frames left */
} ww_detector_t;

float ww_detector_cutoff(ww_sensitivity_t sensitivity);
void ww_detector_init(ww_detector_t *detector, ww_sensitivity_t sensitivity);
void ww_detector_reset(ww_detector_t *detector);

/* Feed one inference's probability (0..1); true exactly on the firing one. */
bool ww_detector_push(ww_detector_t *detector, float probability);

#ifdef __cplusplus
}
#endif
