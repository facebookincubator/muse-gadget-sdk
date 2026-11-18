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
#include "sdkconfig.h"

/* PCM16 mono at MUSE_AUDIO_RATE, both ways. */
#define MUSE_MIRROR_AUDIO_CHUNK 640   /* bytes in one message to the viewer: 20 ms */

#define MUSE_MIRROR_AUDIO_MIC     1
#define MUSE_MIRROR_AUDIO_SPEAKER 2

#if CONFIG_MUSE_MIRROR
/* The audio path, on the samples the board just captured: the viewer's
 * microphone is mixed in or replaces them, and a listening viewer gets the result. */
void muse_mirror_audio_capture(int16_t *mono, size_t frames);
/* The audio path, on samples about to play: a listening viewer gets them.
 * True when the board's own speaker stays silent for them. */
bool muse_mirror_audio_playback(const int16_t *mono, size_t frames);
#else
static inline void muse_mirror_audio_capture(int16_t *mono, size_t frames) { (void)mono; (void)frames; }
static inline bool muse_mirror_audio_playback(const int16_t *mono, size_t frames) { (void)mono; (void)frames; return false; }
#endif

/* IMA ADPCM for a slow cable: four bits a sample. Each chunk starts with the
 * predictor (i16) and the step index (u8, then 0), so a lost chunk costs only itself. */
#define MUSE_MIRROR_ADPCM_CHUNK (4 + MUSE_MIRROR_AUDIO_CHUNK / 4)
typedef struct {
    int16_t predictor;
    uint8_t index;
} muse_mirror_adpcm_t;
void muse_mirror_adpcm(muse_mirror_adpcm_t *state, const uint8_t pcm[MUSE_MIRROR_AUDIO_CHUNK],
                       uint8_t out[MUSE_MIRROR_ADPCM_CHUNK]);

/* The mirror session's side. */
bool muse_mirror_audio_init(void);
void muse_mirror_audio_listen(uint8_t source);        /* 0 stops */
void muse_mirror_audio_speaker(bool viewer_only);
void muse_mirror_audio_mic(uint8_t mode);             /* 0 the board's, 1 both, 2 the viewer's */
void muse_mirror_audio_mic_put(const uint8_t *pcm, size_t samples);   /* little-endian, any alignment */
void muse_mirror_audio_mic_adpcm(const uint8_t chunk[MUSE_MIRROR_ADPCM_CHUNK]);   /* one caller at a time */
bool muse_mirror_audio_take(uint8_t chunk[MUSE_MIRROR_AUDIO_CHUNK], uint8_t *source);
void muse_mirror_audio_reset(void);
