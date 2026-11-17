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

#include "ww_frontend.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "frontend.h"
#include "frontend_util.h"

struct ww_frontend {
    struct FrontendState state;
};

ww_frontend_t *ww_frontend_create(void)
{
    struct FrontendConfig config;
    memset(&config, 0, sizeof(config));
    config.window.size_ms = 30;
    config.window.step_size_ms = 10;
    config.filterbank.num_channels = WW_MODEL_FEATURES;
    config.filterbank.lower_band_limit = 125.0f;
    config.filterbank.upper_band_limit = 7500.0f;
    config.noise_reduction.smoothing_bits = 10;
    config.noise_reduction.even_smoothing = 0.025f;
    config.noise_reduction.odd_smoothing = 0.06f;
    config.noise_reduction.min_signal_remaining = 0.05f;
    config.pcan_gain_control.enable_pcan = 1;
    config.pcan_gain_control.strength = 0.95f;
    config.pcan_gain_control.offset = 80.0f;
    config.pcan_gain_control.gain_bits = 21;
    config.log_scale.enable_log = 1;
    config.log_scale.scale_shift = 6;

    ww_frontend_t *frontend = calloc(1, sizeof(*frontend));
    if (frontend == NULL) {
        return NULL;
    }
    if (!FrontendPopulateState(&config, &frontend->state, 16000)) {
        /* It stops at the first failed allocation and keeps the earlier
         * ones; it zeroes the state first, so freeing all of it is safe. */
        FrontendFreeStateContents(&frontend->state);
        free(frontend);
        return NULL;
    }
    return frontend;
}

void ww_frontend_destroy(ww_frontend_t *frontend)
{
    if (frontend == NULL) {
        return;
    }
    FrontendFreeStateContents(&frontend->state);
    free(frontend);
}

void ww_frontend_reset(ww_frontend_t *frontend)
{
    if (frontend != NULL) {
        FrontendReset(&frontend->state);
    }
}

int8_t ww_frontend_quantize(uint16_t value)
{
    /* As microWakeWord's C runners do it: the feature in float, then
     * round to nearest. */
    const float feature = (float)value * 0.0390625f;
    const long q = lrintf(feature / WW_MODEL_INPUT_SCALE) + WW_MODEL_INPUT_ZERO_POINT;
    return (int8_t)(q < -128 ? -128 : q > 127 ? 127 : q);
}

size_t ww_frontend_push(ww_frontend_t *frontend, const int16_t *pcm, size_t frames,
                        ww_frontend_frame_fn callback, void *ctx)
{
    if (frontend == NULL || (pcm == NULL && frames > 0)) {
        return 0;
    }
    size_t emitted = 0;
    while (frames > 0) {
        size_t used = 0;
        struct FrontendOutput out = FrontendProcessSamples(&frontend->state, pcm, frames, &used);
        pcm += used;
        frames -= used;
        if (out.values != NULL && out.size == WW_MODEL_FEATURES) {
            int8_t features[WW_MODEL_FEATURES];
            for (size_t i = 0; i < WW_MODEL_FEATURES; i++) {
                features[i] = ww_frontend_quantize(out.values[i]);
            }
            emitted++;
            if (callback != NULL) {
                callback(ctx, features);
            }
        }
        if (used == 0) {
            break;
        }
    }
    return emitted;
}
