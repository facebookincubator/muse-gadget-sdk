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

// Home Assistant Voice PE and Seeed reSpeaker Lite audio hardware.
// The XMOS XU316 runs the two-mic array (echo cancellation, noise suppression,
// gain) and is the I2S clock
// master for both directions; the ESP32-S3 follows. Speaker audio goes
// through the XMOS to an AIC3204 codec and an amplifier.

#define VOICE_MIC_RATE      16000
#define VOICE_SPEAKER_RATE  48000

// Reset the XMOS and set up the board's codec and both I2S channels.
esp_err_t voice_board_init(void);

// Microphone: 16 kHz mono PCM16 from the XMOS's processed channel.
esp_err_t voice_board_mic_start(void);
void voice_board_mic_stop(void);
// Read up to `frames` samples into `pcm`. Returns the number read and the
// peak absolute sample in `peak`.
size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak);

// Speaker: 48 kHz stereo, 32-bit samples, interleaved. Blocks until queued.
esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count);
void voice_board_amp(bool on);
// 0 to 100.
void voice_board_set_volume(int percent);

// Hardware mute state: true when the microphones are off or unreadable.
bool voice_board_muted(void);

// Dial detents since the last call: positive clockwise, zero without a dial.
int voice_board_dial_steps(void);
