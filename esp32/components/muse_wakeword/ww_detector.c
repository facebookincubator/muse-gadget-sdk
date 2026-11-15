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

#include "ww_detector.h"

#include <string.h>

float ww_detector_cutoff(ww_sensitivity_t sensitivity)
{
    switch (sensitivity) {
    case WW_SENSITIVITY_LOW:
        return 0.26f;
    case WW_SENSITIVITY_HIGH:
        return 0.13f;
    case WW_SENSITIVITY_DEFAULT:
    default:
        return 0.22f;
    }
}

void ww_detector_init(ww_detector_t *detector, ww_sensitivity_t sensitivity)
{
    if (detector == NULL) {
        return;
    }
    memset(detector, 0, sizeof(*detector));
    detector->cutoff = ww_detector_cutoff(sensitivity);
}

void ww_detector_reset(ww_detector_t *detector)
{
    if (detector == NULL) {
        return;
    }
    const float cutoff = detector->cutoff;
    memset(detector, 0, sizeof(*detector));
    detector->cutoff = cutoff;
}

bool ww_detector_push(ww_detector_t *detector, float probability)
{
    if (detector == NULL) {
        return false;
    }
    detector->probs[detector->head] = probability;
    detector->head = (detector->head + 1) % WW_DETECTOR_WINDOW;
    if (detector->count < WW_DETECTOR_WINDOW) {
        detector->count++;
    }
    float mean = 0.0f;
    for (int i = 0; i < detector->count; i++) {
        mean += detector->probs[i];
    }
    mean = detector->count == WW_DETECTOR_WINDOW ? mean / WW_DETECTOR_WINDOW : 0.0f;
    if (detector->refractory > 0) {
        detector->refractory -= WW_DETECTOR_STRIDE;
        return false;
    }
    if (mean > detector->cutoff) {
        detector->refractory = WW_DETECTOR_REFRACTORY_FRAMES;
        detector->count = 0;
        detector->head = 0;
        return true;
    }
    return false;
}
