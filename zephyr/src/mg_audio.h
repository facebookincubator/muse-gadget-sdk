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

#ifndef MG_AUDIO_H_
#define MG_AUDIO_H_

#include <stdbool.h>
#include <stdint.h>

#include "mg_codec.h"

enum mg_audio_activity {
	MG_AUDIO_IDLE,
	MG_AUDIO_LIVE,
	MG_AUDIO_RECORDING,
	MG_AUDIO_CLIP,
};

/*
 * The audio thread produces everything on Data: live capture, offline
 * recording into the queue, and clip downloads. It sends change_data_type
 * before a stream's first Data chunk and stop_mic after a live stream's
 * last, so Control and Data stay in order. Starting anything stops
 * what was running first. mg_core_audio_idle() runs when it goes idle.
 */

/* rate: a capture rate the codec lists (mg_codec_rates()); kbps: clamped
 * into the codec's range (mg_codec_format_cfg). The caller picks them
 * (16 kHz and 32 kb/s unless the client asked). Offline clips are 16 kHz in
 * the clip format the core picks; clip_info carries it. */
int mg_audio_start_live(uint8_t codec, uint16_t rate, uint16_t kbps, bool ptt);
int mg_audio_start_recording(uint8_t codec, uint16_t kbps);
/*
 * The hold contract's pre-roll: capture (what =
 * MG_AUDIO_LIVE or MG_AUDIO_RECORDING) starts now, the mic indicator on at
 * once, but nothing leaves the device: encoded frames wait in a backlog
 * holding the last second. mg_audio_commit() (the press passed the hold
 * threshold, button_down already queued) sends change_data_type, the
 * backlog as a burst, then live audio (live), or opens the clip with the
 * backlog (recording). mg_audio_discard() (a tap), or a stop before the
 * commit, ends it with nothing sent, no stop_mic and no clip.
 */
int mg_audio_start_held(enum mg_audio_activity what, uint8_t codec, uint16_t rate, uint16_t kbps);
void mg_audio_commit(void);

/*
 * Mic output goes only to a
 * central that passed this connection's token proof (or the bench switch).
 * A gated capture's output (its change_data_type, Data, the gestures the
 * core passes through mg_audio_out_control() and its stop_mic) is held
 * whole, in order, in a hold taken from the heap at its start
 * (MG_AUDIO_HOLD_BYTES), and goes once mg_core_gate_open(gate) says so;
 * nothing of it is visible meanwhile (request_status's data type, the
 * console's activity, link activity). A failed proof (mg_audio_gate_fail())
 * discards it; held output not released within 3 s of the capture's end is
 * discarded; a hold that fills stops the capture and keeps what it holds.
 */
enum mg_audio_gate {
	MG_AUDIO_GATE_NONE,
	MG_AUDIO_GATE_PROOF, /* a client's start_mic while a proof challenge is out */
	MG_AUDIO_GATE_TURN,  /* a held live turn (the connect burst): proven and ready */
	MG_AUDIO_GATE_VOICE, /* a talk turn in a test/automation build: voice heard (mg_voice.h) */
};
#define MG_AUDIO_HOLD_BYTES (24 * 1024) /* 3.2 s of 60 kb/s SBC */
#define MG_AUDIO_HELD_TURN_MS 3000      /* then the clip queue */
/* A client's start_mic before the proof (its output held). */
int mg_audio_start_live_gated(uint8_t codec, uint16_t rate, uint16_t kbps);
/* A talk press in the connect burst: a live turn in the
 * clip format (SBC 16 kHz), pre-roll as mg_audio_start_held, its output held
 * until the client is proven and ready; not within MG_AUDIO_HELD_TURN_MS of
 * now, or the link drops (mg_audio_gate_drop()), the held audio starts an
 * offline clip and the turn goes on into it. -ENOMEM: no hold; -EBUSY: an
 * earlier capture's output still held. */
int mg_audio_start_turn_held(void);
/* A capture's Control message from the core (its gestures): held with its
 * audio while the capture's output is held, dropped once a held turn became
 * a clip, else sent. */
void mg_audio_out_control(const uint8_t *buf, size_t len);
void mg_audio_gate_fail(void); /* the proof failed: held output discarded unsent */
void mg_audio_gate_drop(void); /* the link ended: a held turn to the clip queue, else discarded */
bool mg_audio_output_held(void); /* a capture's output is held */
struct mg_audio_gate_stats {
	uint32_t held, releases, discards, stale, full, turns, queued;
	uint32_t silent; /* talk turns dropped unsent: nothing above room noise (test builds) */
};
void mg_audio_gate_stats(struct mg_audio_gate_stats *st);
void mg_audio_discard(void);
/* The last talk turn was dropped unsent: nothing above room noise (test and
 * automation builds, mg_voice.h); cleared when the next capture starts. */
bool mg_audio_last_turn_silent(void);
int mg_audio_read_clip(uint16_t index);
/* Ends capture (flushing the last chunk) or abandons a clip download. */
void mg_audio_stop(void);

enum mg_audio_activity mg_audio_activity(void);
/* The type most recently announced with change_data_type. */
void mg_audio_current_format(struct mg_audio_format *fmt);
void mg_audio_set_gain(uint8_t gain);
/* The last turns' capture levels, newest first (at most max). */
struct mg_cap_record;
size_t mg_audio_levels(struct mg_cap_record *out, size_t max);
void mg_audio_init(void);

/* Audio source (mg_audio_dmic.c, mg_audio_synth.c, or the board's own code
 * with CONFIG_MG_AUDIO_SRC_BOARD): 10 ms blocks of mono
 * 16-bit samples at the rate it was started with (16000, or 24000 with
 * CONFIG_MG_LC3_24K): rate / 100 samples. */
#define MG_AUDIO_BLOCK_SAMPLES 160
#if defined(CONFIG_MG_LC3_24K)
#define MG_AUDIO_MAX_BLOCK_SAMPLES 240
#else
#define MG_AUDIO_MAX_BLOCK_SAMPLES MG_AUDIO_BLOCK_SAMPLES
#endif
int mg_audio_src_start(uint16_t rate);
/* The mic source is there and ready (0), checked once at boot for the
 * board-ready mark; weak default 0 for sources without a probe. */
int mg_audio_src_probe(void);
/* Blocks up to timeout_ms for the next block; returns its sample count
 * (rate / 100) or <0. */
int mg_audio_src_read(int16_t *pcm, int timeout_ms);
void mg_audio_src_stop(void);

#endif
