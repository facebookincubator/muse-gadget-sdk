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

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * Push-to-talk turn loop: hold -> stream speech to Hatch, release -> think -> speak.
 * Out of Hatch's reach (Wi-Fi down, say) a note is saved to PSRAM and goes once
 * it's back, on boards with PSRAM. Consumes muse_input_event_t from `queue` and
 * drives muse_state for the UI.
 */
esp_err_t muse_voice_start(QueueHandle_t queue);

/* While on (settings' Sound page), idle mic audio feeds muse_voice_monitor_db(). */
void muse_voice_set_monitor(bool on);
/* Smoothed mic level in dBFS (fast attack, slow release). */
float muse_voice_monitor_db(void);

/* Plays a short chirp at the current volume (when idle). */
void muse_voice_request_chirp(void);

/* Runs muse_audio_loopback_test() at the current volume (when idle); results go to the log. */
void muse_voice_request_loopback(void);

/* Bench test: decodes and plays a built-in MP3 reply. */
void muse_voice_request_mp3test(void);

/* Asleep with nothing to play: codecs off, Wi-Fi dozing. */
bool muse_voice_resting(void);

/* Voice notes recorded out of Hatch's reach wait to go, the oldest from the
 * last half hour: worth keeping Wi-Fi up for. */
bool muse_voice_notes_waiting(void);

/* 录音时是否同时把 PCM 丢到串口（本地 ASR 用；默认开） */
void muse_voice_set_mic_dump(bool on);
bool muse_voice_mic_dump(void);
