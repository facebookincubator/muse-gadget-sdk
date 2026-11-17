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

#include <stddef.h>
#include <stdint.h>

#include "ww_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The model's front end: TensorFlow Lite Micro's microfrontend (the
 * esphome/esp-micro-speech-features component, plain C) with the settings
 * microWakeWord trains with: 16 kHz int16 PCM, 30 ms window, 10 ms step,
 * 40 channels from 125 to 7500 Hz, noise reduction, PCAN gain control and
 * log scale. Each 10 ms step's 40 features are quantized to the model's
 * int8 input (ww_model.h) and handed to the callback.
 *
 * ww_frontend_push accepts any number of samples. Create allocates (about
 * 11 KB); push and reset never allocate.
 */

typedef struct ww_frontend ww_frontend_t;

typedef void (*ww_frontend_frame_fn)(void *ctx, const int8_t features[WW_MODEL_FEATURES]);

ww_frontend_t *ww_frontend_create(void);
void ww_frontend_destroy(ww_frontend_t *frontend);
void ww_frontend_reset(ww_frontend_t *frontend);

/* Returns the number of feature frames emitted. callback may be NULL. */
size_t ww_frontend_push(ww_frontend_t *frontend, const int16_t *pcm, size_t frames,
                        ww_frontend_frame_fn callback, void *ctx);

/* One microfrontend value to the model's int8 input. */
int8_t ww_frontend_quantize(uint16_t value);

#ifdef __cplusplus
}
#endif
