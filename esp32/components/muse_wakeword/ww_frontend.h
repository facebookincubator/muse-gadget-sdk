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

#include "ww_frontend_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Streaming LFBE frontend frozen to the Hey Muse checkpoint config:
 * 16 kHz int16 PCM, 480-sample periodic Hann window centered in a
 * 512-point FFT, 320-sample hop, power spectrum, 40 Slaney mel filters
 * (Slaney area normalization), log(mel + 1e-6), then the checkpoint's
 * per-mel mean/std normalization from ww_frontend_config.h.
 *
 * ww_frontend_push accepts arbitrary sample counts and emits one
 * 40-float frame per hop through the callback. Create allocates and
 * precomputes tables; push and reset never allocate.
 */

typedef struct ww_frontend ww_frontend_t;

typedef void (*ww_frontend_frame_fn)(void *ctx, const float frame[WW_FRONTEND_N_MELS]);

ww_frontend_t *ww_frontend_create(void);
void ww_frontend_destroy(ww_frontend_t *frontend);
void ww_frontend_reset(ww_frontend_t *frontend);

/* Returns the number of feature frames emitted. callback may be NULL. */
size_t ww_frontend_push(ww_frontend_t *frontend, const int16_t *pcm, size_t frames,
                        ww_frontend_frame_fn callback, void *ctx);

#ifdef __cplusplus
}
#endif
