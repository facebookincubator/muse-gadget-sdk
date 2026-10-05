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

/* Bench: 16 kHz mono 16-bit PCM that stands in for the mic in the next
 * recording, so the whole voice path can be tested without a voice in the room.
 * Appends `bytes`; `done` arms it for the next press. False if it doesn't fit. */
bool muse_voice_clip_add(const uint8_t *pcm, size_t bytes, bool done);

/*
 * For Muse's agent (main/muse_hw_commands.c). Both run on the voice task once
 * it's idle, one of each at a time, and a talk press cuts them short.
 *
 * Plays 16 kHz mono PCM with Muse speaking it, then frees `pcm` and calls
 * done(played) on the voice task; played is false if a press cut it short.
 * False if a clip is already queued (the caller keeps `pcm`).
 */
typedef void (*muse_voice_play_cb)(bool played, void *user);
bool muse_voice_request_play(int16_t *pcm, size_t frames, muse_voice_play_cb done, void *user);
/* Listens ms long, then calls done with how long it listened, the average
 * and loudest-20-ms levels in dBFS, and the percent of time above -40 dBFS.
 * False if a listen is already queued. */
typedef void (*muse_voice_listen_cb)(int ms, float avg_db, float peak_db, int loud_pct, void *user);
bool muse_voice_request_listen(int ms, muse_voice_listen_cb done, void *user);

/* Records up to `frames` of 16 kHz mono into `pcm` (the caller's), then calls
 * done with how many it got; a talk press ends it early. False if a
 * recording is already queued. */
typedef void (*muse_voice_record_cb)(size_t frames, void *user);
bool muse_voice_request_record(int16_t *pcm, size_t frames, muse_voice_record_cb done, void *user);

/* Asleep with nothing to play: codecs off, Wi-Fi dozing. */
bool muse_voice_resting(void);

/* Voice notes recorded out of Hatch's reach wait to go, the oldest from the
 * last half hour: worth keeping Wi-Fi up for. */
bool muse_voice_notes_waiting(void);
