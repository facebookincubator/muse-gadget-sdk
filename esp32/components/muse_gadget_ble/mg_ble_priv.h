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

/* Shared between mg_ble.c (the BLE side) and mg_voice.c (the voice task side). */

#include <stdbool.h>
#include <stdint.h>

#include "mg_proto.h"

/* mg_ble_init() succeeded; nothing else here may be used before. */
bool mg_active(void);

/* Serializes every use of the protocol state. Recursive. */
void mg_lock(void);
void mg_unlock(void);
mg_proto_t *mg_proto_get(void);

/* The offline queue, or NULL without the partition (or the option). */
mg_queue_t *mg_queue_get(void);
/* Hands a recorded clip to the worker to store; it frees `data`. */
bool mg_queue_submit(uint8_t codec, uint32_t start_ms, uint64_t start_unix_ms, uint8_t *data, uint32_t bytes);
/* The device's wall clock (set by clock sync) in ms since the epoch; 0 if never set. */
int64_t mg_unix_ms_now(void);

/* start_mic state from the client, under mg_lock. */
typedef struct {
    bool requested;          /* start_mic, not yet stopped */
    int64_t armed_us;        /* when it came (esp_timer, 64-bit): a capture not begun within MG_PRESS_WINDOW_MS of it
                              * never begins (mg_voice_capture_requested); the press it rode on is that old */
    mg_audio_format_t fmt;   /* what it asked for, as the device can do it */
} mg_capture_req_t;
mg_capture_req_t *mg_capture_req(void);

uint32_t mg_now_ms(void);

/* Logs an end of utterance the capture's outbox delivered (mg_capq_t) on
 * mg.voice, once each. Under mg_lock. */
void mg_capture_note(void);
/* The held live turn: a capture of this many bytes could be held whole now (PSRAM, or internal RAM within the hold's
 * cap and keep). For the held live turn's board check (MG_HELD_TURN_MIN_HOLD_MS). Any task. */
bool mg_ble_can_hold(size_t bytes);
/* Wakes the musegadgets worker for a round now. Any task. */
void mg_ble_kick(void);

/* Wakes the playback task (a stream started, stopped or was claimed). */
void mg_play_kick(void);

/* The activation buzz on the board's vibration motor; false without one. */
bool mg_vibrate(uint16_t freq_hz, uint16_t ms, uint8_t volume_pct);
/* Haptics: a cue of the device's own (src MGH_SRC_TALK or MGH_SRC_SETUP)
 * through the haptics core, under its rules; and cutting a client's pattern
 * short (an update starting: mg_haptic_reason_updating; a talk press:
 * mg_haptic_reason_in_use). No-ops without the core. */
bool mg_haptic_cue(uint8_t src, uint16_t ms);
void mg_haptic_interrupt(uint8_t reason);
