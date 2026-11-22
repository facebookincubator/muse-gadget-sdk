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

typedef struct muse_aec muse_aec_t;

/* Software echo cancellation for a 16 kHz mono microphone and speaker.
 * A failed start leaves the caller free to use its half-duplex fallback. */
muse_aec_t *muse_aec_start(void);
/* One 20 ms mic read paired with the audio just written to the speaker.
 * Returns 512 cleaned samples when a frame completes, otherwise zero. */
size_t muse_aec_feed(muse_aec_t *aec, const int16_t *mic,
                     const int16_t *played, size_t played_frames);
/* Valid until the next feed or stop. */
int16_t *muse_aec_output(muse_aec_t *aec);
void muse_aec_stop(muse_aec_t *aec);
