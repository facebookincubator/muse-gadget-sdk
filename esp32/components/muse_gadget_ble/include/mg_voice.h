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

#include "mg_route.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The voice transport layer between a gadget's push-to-talk loop and where
 * the audio goes. Each utterance asks mg_voice_route() once, at the press:
 *
 *   MG_ROUTE_WIFI   the loop's existing path to Muse, untouched
 *   MG_ROUTE_BLE    mg_voice_begin/audio/end stream it to the mg client
 *   MG_ROUTE_QUEUE  mg_voice_begin/audio/end record it to the offline queue
 *   MG_ROUTE_NONE   refuse the press
 *
 * The policy is CONFIG_MUSE_GADGET_BLE_ROUTE_*. PCM is 16 kHz mono in any
 * chunk size. Call these from the voice task only (the one that reads the
 * mic); mg_voice_gesture from wherever presses are seen.
 */

/* wifi_ready: the existing path could start a turn now. */
mg_route_t mg_voice_route(bool wifi_ready);

/* Every press and release, whatever the route: an mg client that's ready
 * gets [gesture]. False if it didn't go. */
bool mg_voice_gesture(bool down);

/*
 * The bench guard: ptt.minrise=N on the consoles
 * (automation; 1-40 dB, 0 off; RAM only, off at every boot, never stored).
 * mg_voice_hold, just before a BLE push-to-talk turn's button_down: with N
 * set, the turn is held on the gadget (mg_proto_turn_hold: nothing of it
 * reaches the client) until its capture rises N dB over its own floor, then
 * released whole and in order; one that never rises is dropped at its end,
 * unsent, logged "ptt: nothing heard (rise X dB < N): turn not sent" and
 * counted (ptt_nothing on the status line); mg_voice_turn_dropped says so
 * after mg_voice_end. False: not held (N is 0, or no ready client).
 */
/*
 * The held live turn (route MG_ROUTE_HELD; mg_proto_held_turn_begin):
 * mg_voice_held_begin, just before the press's button_down, holds the turn
 * (its gestures, SBC 16 kHz audio and end) until the client is proven and
 * ready, then it goes live with everything held; not within MG_HELD_TURN_MS
 * of the press (or the link dropped), the held audio becomes an offline clip
 * and the capture goes on into it. mg_voice_turn_queued says, after
 * mg_voice_end, that this turn went to the clip queue by its end (one still
 * held then goes live, or to the queue at its deadline, by the worker).
 * mg_voice_held_begin false: not held (no link, or a held turn already open).
 */
bool mg_voice_held_begin(void);
bool mg_voice_turn_queued(void);

void mg_voice_set_minrise(uint8_t db);
uint8_t mg_voice_minrise(void);
uint32_t mg_voice_nothing_heard(void);
bool mg_voice_hold(void);
bool mg_voice_turn_dropped(void);

/*
 * Opens an utterance on `route` (BLE or QUEUE). For push-to-talk
 * (from_client false) on BLE, plays the activation buzz on the board's
 * vibration motor first, if it has one and the client turned haptics on.
 * False if the route can't take it any more.
 */
bool mg_voice_begin(mg_route_t route, bool from_client);
/* False once the utterance should end (client gone, stop_mic, or the queue's full). */
bool mg_voice_audio(const int16_t *pcm, size_t frames);

/* The talk-threshold buzz through the haptics core (its rules,
 * and it takes the motor from a client's pattern). False without the core:
 * the caller buzzes the board directly as before. */
bool mg_ble_haptic_talk_cue(uint16_t ms);
/* Closes it: a live one always ends with [stop_mic]. keep: the utterance
 * counts (a queued clip is stored, a short tap's isn't). */
void mg_voice_end(bool keep);

/* The client asked for capture with start_mic and still wants it. */
bool mg_voice_capture_requested(void);

/*
 * The voice loop takes the speaker and mic (a push-to-talk turn, a chirp):
 * a client's playback ends with stop_streaming drop, and new playback is
 * refused until it's released.
 */
void mg_voice_claim(bool claim);
/* A push-to-talk turn holds the speaker (mg_proto_claim_turn): a client's
 * reply that starts meanwhile waits and plays once this goes off. */
void mg_voice_claim_turn(bool on);
/* A talk press's touch-down mutes the reply, its audio kept; off (a tap)
 * resumes it where it paused; the turn's claim ends it too
 * (mg_proto_barge_pause: the barge-in echo rule). */
void mg_voice_barge_pause(bool on);
/* A client's stream is open on the speaker (playing, buffering, or empty
 * between a Live call's replies): the codecs stay open for it. */
bool mg_voice_playing(void);
/* A client's audio is playing (mg_play_audible): held, or played within the
 * last 300 ms; not an open stream's silence between replies. The speaking
 * face, the panel hold and "playback over" follow this. */
bool mg_voice_audible(void);

/* The connected client has sent assistant_state; older clients never do. */
bool mg_voice_client_reports_state(void);

/* A client's BLE link is up, whether or not its session is ready or
 * push-to-talk is on. */
bool mg_voice_client_linked(void);

/*
 * Logs a face (or LED ring) change of a musegadgets turn as
 * "mg.face: <what>", which the Nordic UART mirror carries to the client.
 */
void mg_voice_face(const char *what);

/* The bench pairing auto-confirm (the Nordic UART's !PAIR AUTO or the
 * USB console's >pair.auto): armed now (the confirm screen says so); take = press it, once. */
bool mg_ble_pair_auto_armed(void);
bool mg_ble_pair_auto_take(void);   /* both in mg_ble.c */
/* Link modes (mg_ble.h, in mg_ble.c), for the input task: the talk
 * press's activity, and a user's wake or touch (sleep -> ready). */
void mg_ble_link_activity(void);
/* Control's queue counts for the USB status line (mg_ble.h, in mg_ble.c). */
int mg_ble_reply_stats(bool json, char *out, size_t n);
void mg_ble_link_turn(void);
void mg_ble_link_user(const char *why);

#ifdef __cplusplus
}
#endif
