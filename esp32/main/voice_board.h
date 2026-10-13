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

// Board-specific capture, playback, amplifier and controls. Voice PE and
// reSpeaker Lite use an XMOS audio processor; Waveshare Audio Board uses
// ES7210/ES8311 codecs with the ESP32 supplying the shared I2S clocks.

#define VOICE_MIC_RATE      16000
#define VOICE_SPEAKER_RATE  48000

// Initialize the board's audio hardware and both I2S channels.
esp_err_t voice_board_init(void);

// Microphone: 16 kHz mono PCM16 from the board's selected microphone channel.
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

// Mute state: true when muted or the board controls are unreadable.
bool voice_board_muted(void);

// Dial detents since the last call: positive clockwise, zero without a dial.
int voice_board_dial_steps(void);
