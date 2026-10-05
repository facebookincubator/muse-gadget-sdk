/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

// RMNM: two 32-bit slots hold reference/mic1 and unused/mic2 PCM16 pairs.
static inline void waveshare_audio_extract_mic(const int32_t *raw, int16_t *pcm,
                                               size_t count, int *peak) {
    *peak = 0;
    for (size_t i = 0; i < count; i++) {
        pcm[i] = (int16_t)((uint32_t)raw[2 * i] >> 16);
        int magnitude = pcm[i] < 0 ? -(int)pcm[i] : pcm[i];
        if (magnitude > *peak) *peak = magnitude;
    }
}

// Shared player supplies 48 kHz stereo PCM32. Keep the bus at 16 kHz and
// average each three frames with a wide accumulator to avoid overflow.
static inline void waveshare_audio_downsample(const int32_t *in, int32_t *out,
                                              size_t count) {
    for (size_t i = 0; i < count; i++) {
        for (size_t c = 0; c < 2; c++) {
            int64_t sum = (int64_t)in[6*i+c] + in[6*i+2+c] + in[6*i+4+c];
            out[2*i+c] = (int32_t)(sum / 3);
        }
    }
}
