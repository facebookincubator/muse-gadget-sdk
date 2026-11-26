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
#include "sdkconfig.h"

/*
 * StopWatch with realtime enabled defaults to a held-mic call: hold yellow to
 * talk, release to listen in the same call, blue to end. Release hides the mic
 * meter immediately and keeps at most 120 ms of capture tail for codec lag.
 * Settings > Sound can select voice notes instead. Other boards retain their
 * existing note/tap flow.
 * Notes recorded out of reach are saved to PSRAM on boards with PSRAM.
 * Consumes muse_input_event_t from `queue` and drives muse_state for the UI.
 */
esp_err_t muse_voice_start(QueueHandle_t queue);

/* UI-loop state, including startup and teardown. PTT is pinned for the call. */
typedef enum {
    MUSE_VOICE_VIEW_NONE,
    MUSE_VOICE_VIEW_CONNECTING,
    MUSE_VOICE_VIEW_READY,
    MUSE_VOICE_VIEW_LISTENING,
    MUSE_VOICE_VIEW_THINKING,
    MUSE_VOICE_VIEW_WORKING,   /* nothing playing or said, Muse still has a caller turn open */
    MUSE_VOICE_VIEW_SPEAKING,
    MUSE_VOICE_VIEW_ENDING,
} muse_voice_view_t;
muse_voice_view_t muse_voice_call_view(void);
bool muse_voice_in_call(void);
/* A reply plays on the gadget now: a musegadgets client's stream_audio
 * reaching the speaker, or a Live call's subtitles band on screen
 * (it holds a lit panel until it ends; it never wakes a dark one). Any task. */
bool muse_voice_reply_holds_panel(void);
bool muse_voice_ptt_call(void);
bool muse_voice_ptt_enabled(void);   /* next press, not the current call */
const char *muse_voice_call_label(void);   /* NULL outside a PTT call */
const char *muse_voice_call_hint(void);    /* yellow button, NULL outside PTT */
/* Bounded best effort before software poweroff; suppresses new calls until
 * shutdown is cancelled (only when the board's power-off operation fails). */
void muse_voice_shutdown(int wait_ms);
void muse_voice_cancel_shutdown(void);

#if CONFIG_MUSE_WAKEWORD_ENABLE
/* Wake word: the next record() starts with this audio (copied,
 * whole 20 ms chunks, at most a second) instead of the push-to-talk
 * pre-roll ring, so a hands-free note includes the wake word itself. */
void muse_voice_seed_preroll(const int16_t *pcm, size_t frames);   /* NULL clears */
#endif

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

/*
 * A musegadgets client's assistant_state (mg_assistant_state_t), for the
 * face after a BLE turn; idle when it disconnects. Any task.
 */
void muse_voice_assistant_state(uint8_t state);

/* Asleep with nothing to play: codecs off, Wi-Fi dozing. */
bool muse_voice_resting(void);

/* Reply audio has drained: use a quiet avatar pose, retaining reply status,
 * captions and progress. False again as soon as more audio plays. */
bool muse_voice_reply_waiting(void);

/* Voice notes recorded out of Muse's reach wait to go, the oldest from the
 * last half hour: worth keeping Wi-Fi up for. */
bool muse_voice_notes_waiting(void);

/* The talk input was let go (input task): a musegadgets client gets
 * button_up only if the press passed the hold threshold and its button_down
 * went (a tap sends nothing). */
void muse_voice_talk_released(void);
/* The talk input went down (input task, at touch-down): a musegadgets
 * client's reply is muted at once, its audio kept, until the voice task
 * decides the press (a tap resumes it, a hold claims the turn): the
 * barge-in echo rule (mg_proto_barge_pause). */
void muse_voice_talk_pressed(void);
/* Where the voice task is (a static string) and for how long, *ms: the
 * input task's stuck-task report (muse_input.c) and the console's voice?. */
const char *muse_voice_where(uint32_t *ms);
/* A client's capture: the voice task has taken it up (it's capturing for the
 * client now), and whether that capture has read no mic chunk for stall_ms. */
bool muse_voice_in_client_capture(void);
bool muse_voice_capture_stalled(uint32_t stall_ms);
/* The voice task's FreeRTOS state: X running, R ready (waiting for its core),
 * B blocked, S suspended, D deleted, - not started. */
char muse_voice_task_state(void);
/* Since the voice task's last sign of life (a place in its loop, a mic read,
 * a speaker write, a delay or a rest slice), in ms; 0 before it starts. */
uint32_t muse_voice_quiet_ms(void);
