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

#include "ww_runner.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wake word ("Hey Muse") session orchestration. This header is the
 * host-testable core: the session state machine, the audio rings, and
 * the energy fallback, all pure C with no ESP-IDF types so
 * tests/test_muse_wakeword.py can drive them on the host. The
 * firmware glue (tap in muse_voice.c, wake word task, NVS, gadget
 * commands) lives in components/muse; see this component's README.md.
 *
 * The core is ticked once per audio chunk (320 frames / 20 ms at
 * 16 kHz). All timers are chunk counts owned here; the runner
 * only emits events. Actions tell the caller which synthetic PTT
 * events to post to the muse_input queue: a turn is opened
 * (POST_PTT_DOWN) only once speech actually starts, so a T_start
 * cancel never opens a Muse turn and uploads nothing.
 */

typedef enum {
    MUSE_WW_IDLE = 0,
    MUSE_WW_ARMED,           /* heard the wake word; waiting for speech */
    MUSE_WW_CAPTURING,       /* synthetic hold open; speech being captured */
    MUSE_WW_CAPTURING_PTT,   /* the real button took over the open capture */
} muse_ww_state_t;

enum {
    MUSE_WW_ACTION_NONE = 0,
    MUSE_WW_ACTION_POST_PTT_DOWN = 1u << 0,
    MUSE_WW_ACTION_POST_PTT_UP = 1u << 1,
    MUSE_WW_ACTION_CANCELLED = 1u << 2,        /* a wake ended with nothing posted */
    MUSE_WW_ACTION_SUBMITTED = 1u << 3,        /* a capture ended and goes to Muse */
    MUSE_WW_ACTION_CONVERTED_TO_PTT = 1u << 4,
};

/* Timer counts in 20 ms chunks. A zero count is clamped to 1 chunk by
 * muse_wakeword_core_init, so degenerate configs cannot make a timer
 * fire on the first tick. */
typedef struct {
    uint32_t start_timeout_chunks;   /* T_start: no speech after wake -> cancel */
    uint32_t end_silence_chunks;     /* trailing silence -> end of capture (energy tier) */
    uint32_t max_capture_chunks;     /* hard cap, matches muse_voice's 15 s */
    bool energy_endpointing;         /* judge speech from `voiced`, not just runner events */
} muse_wakeword_config_t;

/* Defaults: 2.5 s / 700 ms / 15 s at 20 ms chunks, rounded up. */
muse_wakeword_config_t muse_wakeword_default_config(void);

/* Milliseconds to whole 20 ms chunks, rounded up (0 stays 0). */
uint32_t muse_wakeword_ms_to_chunks(uint32_t ms);

typedef struct {
    ww_event_t event;
    bool voiced;      /* energy judgement of this chunk (muse_wakeword_energy_voiced) */
    bool ptt_down;    /* a real talk-button press arrived this chunk */
    bool ptt_up;      /* a real talk-button release arrived this chunk */
    bool playback;    /* reply audio playing: wakes are discarded */
    bool enabled;     /* runtime listening opt-in */
} muse_wakeword_input_t;

typedef struct {
    uint32_t wakes;
    uint32_t cancels;
    uint32_t submits;
    uint32_t conversions;
    uint32_t suppressed_wakes;   /* wakes discarded during playback */
} muse_wakeword_stats_t;

typedef struct {
    muse_wakeword_config_t cfg;
    muse_ww_state_t state;
    uint32_t armed_chunks;
    uint32_t capture_chunks;
    uint32_t silence_chunks;
    muse_wakeword_stats_t stats;
} muse_wakeword_core_t;

void muse_wakeword_core_init(muse_wakeword_core_t *core, const muse_wakeword_config_t *cfg);
void muse_wakeword_core_reset(muse_wakeword_core_t *core);
/* One chunk through the state machine; returns MUSE_WW_ACTION_* flags. */
uint32_t muse_wakeword_core_tick(muse_wakeword_core_t *core, const muse_wakeword_input_t *in);
/* The turn a POST_PTT_DOWN asked for could not be opened (the input queue
 * was full): back to idle, counted as a cancel, with nothing left to post. */
void muse_wakeword_core_abort(muse_wakeword_core_t *core);
muse_ww_state_t muse_wakeword_core_state(const muse_wakeword_core_t *core);
muse_wakeword_stats_t muse_wakeword_core_stats(const muse_wakeword_core_t *core);

/*
 * Energy fallback: voiced when the chunk's mean square reaches
 * rms_threshold^2, in integer arithmetic so host and device agree
 * bit for bit. An empty chunk is never voiced.
 */
#define MUSE_WAKEWORD_ENERGY_RMS_DEFAULT 512   /* about -36 dBFS */
bool muse_wakeword_energy_voiced(const int16_t *pcm, size_t frames, int32_t rms_threshold);

/*
 * Caller-owned frame ring for the upload pre-roll. Storage is the
 * caller's (allocated once at init from MUSE_BIG_CAPS in firmware);
 * nothing here allocates. Push never blocks: on overflow the oldest
 * frames are dropped and counted. Not thread safe: one task owns a
 * ring (the cross-task tap is a FreeRTOS stream buffer in the glue).
 */
typedef struct {
    int16_t *storage;
    size_t capacity_frames;
    size_t head;        /* index of the oldest frame */
    size_t fill_frames;
    uint32_t dropped_frames;
} muse_ww_ring_t;

void muse_ww_ring_init(muse_ww_ring_t *ring, int16_t *storage, size_t capacity_frames);
void muse_ww_ring_clear(muse_ww_ring_t *ring);
void muse_ww_ring_push(muse_ww_ring_t *ring, const int16_t *pcm, size_t frames);
/* Removes up to `frames` oldest frames into out; returns how many. */
size_t muse_ww_ring_pop(muse_ww_ring_t *ring, int16_t *out, size_t frames);
/* Copies the held frames oldest-first without removing them (pre-roll). */
size_t muse_ww_ring_snapshot(const muse_ww_ring_t *ring, int16_t *out, size_t out_frames);
size_t muse_ww_ring_fill(const muse_ww_ring_t *ring);
uint32_t muse_ww_ring_drops(const muse_ww_ring_t *ring);

/*
 * Numbered audio chunks for the hand-off from the wake task to the
 * recorder. The glue numbers every chunk it taps and keeps the latest
 * ones here (the voice task's own copy); the wake task learns each
 * chunk's number with it. When the wake task seeds a hands-free turn
 * with the pre-roll ending at chunk M, the recorder appends the chunks
 * numbered M+1 onward that it had already tapped, so the note has no gap
 * however far the wake task trails. Caller-owned storage, one task.
 */
#define MUSE_WW_CHUNK_FRAMES 320   /* muse_audio's 20 ms chunk at 16 kHz */

typedef struct {
    uint32_t seq;
    int16_t pcm[MUSE_WW_CHUNK_FRAMES];
} muse_ww_chunk_t;

typedef struct {
    muse_ww_chunk_t *slots;
    size_t capacity;
    uint32_t next_seq;   /* the number the next pushed chunk gets */
} muse_ww_history_t;

void muse_ww_history_init(muse_ww_history_t *h, muse_ww_chunk_t *slots, size_t capacity);
/* Copies one chunk in, numbers it, and returns its slot (overwriting the oldest). */
const muse_ww_chunk_t *muse_ww_history_push(muse_ww_history_t *h, const int16_t *pcm);
/* The held chunks numbered `from` or later, oldest first, at most max:
 * pointers into the slots, valid until the next push. Chunks no longer
 * held are skipped; a `from` past the newest gives none. */
size_t muse_ww_history_since(const muse_ww_history_t *h, uint32_t from, const int16_t *out[], size_t max);

#ifdef __cplusplus
}
#endif
