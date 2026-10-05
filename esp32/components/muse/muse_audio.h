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

#include "esp_err.h"

/*
 * The board's mic and speaker codecs share one duplex I2S bus, so both run at
 * the same format: 16 kHz, 16-bit, 2 slots. This module hides that and
 * exposes mono buffers. Captured audio is high-passed to remove DC and rumble.
 */

#define MUSE_AUDIO_RATE 16000
#define MUSE_AUDIO_CHUNK 320   /* 20 ms of mono frames */

esp_err_t muse_audio_init(int volume, int mic_gain_db);

/* Off closes the codecs: ADC, DAC, speaker amp and I2S stop. On reopens them
 * with the saved volume and mic gain. Reads and writes need it on. */
void muse_audio_power(bool on);

void muse_audio_set_volume(int volume);          /* 0..100 */
/* Bench: silent whatever the volume, until unmuted or a restart; not saved. */
void muse_audio_mute(bool muted);
void muse_audio_set_mic_gain(int db);            /* 0..MUSE_MIC_GAIN_MAX */

/* Measures the real capture/playback rates and per-mic levels; logs the result. */
void muse_audio_selftest(void);

/* Plays tones while recording, across mic gains; logs levels. For tuning. */
void muse_audio_loopback_test(int volume);

/* Blocking read of `frames` mono samples (mic pair mixed down). */
esp_err_t muse_audio_read(int16_t *mono, size_t frames);

/* Blocking write of `frames` mono samples to the speaker. */
esp_err_t muse_audio_write(const int16_t *mono, size_t frames);

/* RMS of a buffer in dBFS (-100 for silence). */
float muse_audio_dbfs(const int16_t *mono, size_t frames);

/* RMS of a buffer mapped to a 0..1 perceptual level. */
float muse_audio_level(const int16_t *mono, size_t frames);

/* Short UI chirp: rising for "go", falling for "done". */
void muse_audio_chirp(int rising);
