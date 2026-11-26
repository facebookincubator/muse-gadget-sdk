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

/*
 * Build options of the musegadgets BLE component, from Kconfig on the device.
 * The host tests define the MG_* macros themselves.
 */

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

/* A BLE-only gadget: no Wi-Fi path to route to. */
#ifndef MG_STANDALONE
#ifdef CONFIG_MUSE_GADGET_BLE_STANDALONE
#define MG_STANDALONE 1
#else
#define MG_STANDALONE 0
#endif
#endif

#ifndef MG_WITH_LC3
#ifdef CONFIG_MUSE_GADGET_BLE_LC3
#define MG_WITH_LC3 1
#else
#define MG_WITH_LC3 0
#endif
#endif

/* The 24 kHz gate: LC3 at 24 kHz listed (playback, and capture where the bus
 * takes it) only once the board's check has passed (encode/decode time,
 * the mic filter on real audio): new apps ask for 24 kHz whenever listed. */
#ifndef MG_LC3_24K
#ifdef CONFIG_MUSE_GADGET_BLE_LC3_24K
#define MG_LC3_24K MG_WITH_LC3
#else
#define MG_LC3_24K 0
#endif
#endif

#ifndef MG_SBC_BITPOOL
#ifdef CONFIG_MUSE_GADGET_BLE_SBC_BITPOOL
#define MG_SBC_BITPOOL CONFIG_MUSE_GADGET_BLE_SBC_BITPOOL
#else
#define MG_SBC_BITPOOL 26
#endif
#endif

/* Longest utterance, from the press or start_mic. */
#ifndef MG_MAX_UTTERANCE_S
#define MG_MAX_UTTERANCE_S 30
#endif

/* Capture audio held for a slow link (mg_capq_t): this many seconds of the
 * capture's format, where the platform has the memory (PSRAM). */
#ifndef MG_CAPTURE_HOLD_S
#ifdef CONFIG_MUSE_GADGET_BLE_CAPTURE_HOLD_S
#define MG_CAPTURE_HOLD_S CONFIG_MUSE_GADGET_BLE_CAPTURE_HOLD_S
#else
#define MG_CAPTURE_HOLD_S 10
#endif
#endif

/*
 * The held live turn: a
 * talk press while a client's link is up but the client isn't proven (token
 * proof) or ready (subscribed, push-to-talk enabled) yet captures as a live
 * turn held on the gadget (mg_proto_held_turn_begin). Each choice is one
 * switch here, so a correction is a one-line flip and its host case.
 */
/* The held turn's staleness bound: output held for the proof
 * and not released within this long of the capture's end is discarded (logged,
 * counted), so a proof minutes later never sends speech the user gave up on. A
 * held live turn the clip queue can take goes there instead (the queue wins). */
#ifndef MG_OUTPUT_STALE_MS
#define MG_OUTPUT_STALE_MS 3000
#endif
/* Held live turns only on boards that can hold them: an offline clip
 * queue, or a hold of at least this long; otherwise the connect burst's press
 * stays gesture-only with the brief "Connecting" cue. */
#ifndef MG_HELD_TURN_MIN_HOLD_MS
#define MG_HELD_TURN_MIN_HOLD_MS 2000
#endif
/* Proven and ready within this long of the press: the turn goes live with all
 * it held; else the held audio goes to the offline clip queue. */
#ifndef MG_HELD_TURN_MS
#define MG_HELD_TURN_MS 3000
#endif
/* A proof that fails inside the window: 0 discards the held turn,
 * 1 would queue it. */
#ifndef MG_HELD_TURN_FAIL_QUEUES
#define MG_HELD_TURN_FAIL_QUEUES 0
#endif
/* The link drops inside the window: 1 queues the held turn (never lost; a clip
 * goes only to a proven central), 0 would discard it. */
#ifndef MG_HELD_TURN_DROP_QUEUES
#define MG_HELD_TURN_DROP_QUEUES 1
#endif
/* No queue at the deadline (the setting off, no partition, no room): 1 keeps
 * holding (sent once proven and ready, the capture
 * stopped when the hold fills, discarded on a failed proof, a drop, or the
 * staleness bound 3 s after its capture's end), 0 would
 * discard it at the deadline. */
#ifndef MG_HELD_TURN_NO_QUEUE_HOLDS
#define MG_HELD_TURN_NO_QUEUE_HOLDS 1
#endif
/* A held turn records SBC at 16 kHz, the clip format, whatever
 * audio_codec says, so it can always become a clip; at go-live its own
 * change_data_type announces SBC before its first Data (both
 * apps decode mic Data by the announced type). 0
 * would record the turn's own format (settings 7, 13, 14) and keep holding
 * when that can't be a clip. */
#ifndef MG_HELD_TURN_SBC
#define MG_HELD_TURN_SBC 1
#endif

/* A turn held by the bench guard (ptt.minrise, mg_proto_turn_hold) keeps all
 * of itself until it rises or ends (and one held for the token proof,
 * until the proof matches): the longest push-to-talk turn (muse_voice's
 * 15 s cap, MAX_SECS, post-roll inside it) plus its pre-roll (at most 1 s). */
#ifndef MG_CAPTURE_GUARD_S
#define MG_CAPTURE_GUARD_S 16
#endif
