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

/*
 * Thresholds are the checkpoint bundle's operating points, re-derived
 * from its calibration corpus with this 5-frame smoothing applied
 * (see models/hey_muse.json for the bundle identity).
 */
static float ww_detector_threshold(ww_sensitivity_t sensitivity)
{
    switch (sensitivity) {
    case WW_SENSITIVITY_LOW:
        return 0.99f;
    case WW_SENSITIVITY_HIGH:
        return 0.90f;
    case WW_SENSITIVITY_DEFAULT:
    default:
        return 0.95f;
    }
}

void ww_detector_init(ww_detector_t *detector, ww_sensitivity_t sensitivity)
{
    if (detector == NULL) {
        return;
    }
    memset(detector, 0, sizeof(*detector));
    detector->threshold = ww_detector_threshold(sensitivity);
}

void ww_detector_reset(ww_detector_t *detector)
{
    if (detector == NULL) {
        return;
    }
    const float threshold = detector->threshold;
    memset(detector, 0, sizeof(*detector));
    detector->threshold = threshold;
}

bool ww_detector_push(ww_detector_t *detector, float posterior)
{
    if (detector == NULL) {
        return false;
    }

    detector->frames_seen++;
    if (detector->frames_seen <= WW_DETECTOR_WARMUP_FRAMES) {
        return false;
    }

    if (detector->ring_count == WW_DETECTOR_SMOOTH_FRAMES) {
        detector->sum -= detector->ring[detector->ring_head];
    } else {
        detector->ring_count++;
    }
    detector->ring[detector->ring_head] = posterior;
    detector->sum += posterior;
    detector->ring_head = (uint8_t)((detector->ring_head + 1) % WW_DETECTOR_SMOOTH_FRAMES);

    if (detector->cooldown_left > 0) {
        detector->cooldown_left--;
        return false;
    }

    const float smoothed = detector->sum / (float)detector->ring_count;
    if (smoothed >= detector->threshold) {
        detector->cooldown_left = WW_DETECTOR_COOLDOWN_FRAMES;
        return true;
    }
    return false;
}
