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

// Plays 16 kHz mono PCM16 on the speaker. Writers fill a buffer in PSRAM and
// a separate task drains it to I2S, so a slow network never stalls the audio
// and a stalled speaker never blocks the network.

#define VOICE_PLAYER_RATE 16000

esp_err_t voice_player_init(void);

// Start a new reply, stopping any playback still going. Playback starts once
// enough is buffered, or at voice_player_end() if the reply is shorter.
void voice_player_begin(void);
// Blocks while the buffer is full. Fails once the reply has been stopped.
esp_err_t voice_player_write(const int16_t *pcm, size_t samples);
// No more audio for this reply; what is buffered still plays.
void voice_player_end(void);
// Wait for the reply to finish playing. False on timeout.
bool voice_player_wait(int timeout_ms);
// Drop the rest of the reply now.
void voice_player_stop(void);
// True once this reply has made a sound.
bool voice_player_started(void);
