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

#include "muse_wakeword.h"

#include <string.h>

#define CHUNK_MS 20   /* muse_audio's fixed chunk: 320 frames at 16 kHz */

uint32_t muse_wakeword_ms_to_chunks(uint32_t ms)
{
    uint64_t chunks = ((uint64_t)ms + CHUNK_MS - 1) / CHUNK_MS;
    return chunks > UINT32_MAX ? UINT32_MAX : (uint32_t)chunks;
}

muse_wakeword_config_t muse_wakeword_default_config(void)
{
    return (muse_wakeword_config_t){
        .start_timeout_chunks = muse_wakeword_ms_to_chunks(2500),
        .end_silence_chunks = muse_wakeword_ms_to_chunks(700),
        .max_capture_chunks = muse_wakeword_ms_to_chunks(15000),
        .energy_endpointing = true,
    };
}

void muse_wakeword_core_init(muse_wakeword_core_t *core, const muse_wakeword_config_t *cfg)
{
    if (!core) {
        return;
    }
    memset(core, 0, sizeof(*core));
    core->cfg = cfg ? *cfg : muse_wakeword_default_config();
    /* A zero count would make its timer fire on the first tick (a
     * voiced capture would submit immediately); clamp to one chunk. */
    if (core->cfg.start_timeout_chunks == 0) {
        core->cfg.start_timeout_chunks = 1;
    }
    if (core->cfg.end_silence_chunks == 0) {
        core->cfg.end_silence_chunks = 1;
    }
    if (core->cfg.max_capture_chunks == 0) {
        core->cfg.max_capture_chunks = 1;
    }
    core->state = MUSE_WW_IDLE;
}

void muse_wakeword_core_reset(muse_wakeword_core_t *core)
{
    if (!core) {
        return;
    }
    muse_wakeword_config_t cfg = core->cfg;
    muse_wakeword_core_init(core, &cfg);
}

muse_ww_state_t muse_wakeword_core_state(const muse_wakeword_core_t *core)
{
    return core ? core->state : MUSE_WW_IDLE;
}

muse_wakeword_stats_t muse_wakeword_core_stats(const muse_wakeword_core_t *core)
{
    return core ? core->stats : (muse_wakeword_stats_t){ 0 };
}

static void go_idle(muse_wakeword_core_t *core)
{
    core->state = MUSE_WW_IDLE;
    core->armed_chunks = 0;
    core->capture_chunks = 0;
    core->silence_chunks = 0;
}

static bool speech_now(const muse_wakeword_core_t *core, const muse_wakeword_input_t *in)
{
    return in->event == WW_EVENT_SPEECH_START || (core->cfg.energy_endpointing && in->voiced);
}

uint32_t muse_wakeword_core_tick(muse_wakeword_core_t *core, const muse_wakeword_input_t *in)
{
    if (!core || !in) {
        return MUSE_WW_ACTION_NONE;
    }

    /* Disable is immediate: close a synthetic hold so the turn it
     * opened ends now; a converted capture is the button's to end, but
     * the session is over either way. */
    if (!in->enabled) {
        uint32_t act = MUSE_WW_ACTION_NONE;
        if (core->state == MUSE_WW_CAPTURING) {
            act = MUSE_WW_ACTION_POST_PTT_UP | MUSE_WW_ACTION_SUBMITTED;
            core->stats.submits++;
        } else if (core->state == MUSE_WW_ARMED) {
            act = MUSE_WW_ACTION_CANCELLED;
            core->stats.cancels++;
        } else if (core->state == MUSE_WW_CAPTURING_PTT) {
            act = MUSE_WW_ACTION_SUBMITTED;
            core->stats.submits++;
        }
        go_idle(core);
        return act;
    }

    /* A real press wins over everything, including playback
     * suppression, and consumes the tick. The glue reports a physical
     * turn through the same gate as playback, so the press that cancels
     * an arming can arrive on a suppressed tick; swallowing it here
     * would leave a stale ARMED to resurrect after the button turn. */
    if (in->ptt_down) {
        if (core->state == MUSE_WW_ARMED) {
            go_idle(core);
            core->stats.cancels++;
            return MUSE_WW_ACTION_CANCELLED;
        }
        if (core->state == MUSE_WW_CAPTURING) {
            core->state = MUSE_WW_CAPTURING_PTT;
            core->stats.conversions++;
            return MUSE_WW_ACTION_CONVERTED_TO_PTT;
        }
        return MUSE_WW_ACTION_NONE;   /* IDLE or already the button's turn */
    }

    /* The converted capture is owned by the button: its release ends it
     * even while suppressed, or the capture outlives the physical turn.
     * The physical release ends the capture through the normal PTT
     * path; no synthetic up is posted. The cap is a safety net for
     * a release the core never sees. */
    if (core->state == MUSE_WW_CAPTURING_PTT) {
        core->capture_chunks++;
        if (in->ptt_up || core->capture_chunks >= core->cfg.max_capture_chunks) {
            go_idle(core);
            core->stats.submits++;
            return MUSE_WW_ACTION_SUBMITTED;
        }
        return MUSE_WW_ACTION_NONE;
    }

    /* The gadget must never trigger on its own voice. Timers pause
     * while playback lasts. */
    if (in->playback) {
        if (in->event == WW_EVENT_WAKE) {
            core->stats.suppressed_wakes++;
        }
        return MUSE_WW_ACTION_NONE;
    }

    switch (core->state) {
    case MUSE_WW_CAPTURING_PTT:
        /* Handled above, before playback suppression. */
        return MUSE_WW_ACTION_NONE;

    case MUSE_WW_IDLE:
        if (in->event == WW_EVENT_WAKE) {
            core->state = MUSE_WW_ARMED;
            core->armed_chunks = 0;
            core->stats.wakes++;
        }
        return MUSE_WW_ACTION_NONE;

    case MUSE_WW_ARMED:
        /* Only now, with speech actually present, may a turn open:
         * before this point nothing has been posted, so the
         * T_start cancel below uploads nothing. */
        if (speech_now(core, in)) {
            core->state = MUSE_WW_CAPTURING;
            core->capture_chunks = 0;
            core->silence_chunks = 0;
            return MUSE_WW_ACTION_POST_PTT_DOWN;
        }
        core->armed_chunks++;
        if (core->armed_chunks >= core->cfg.start_timeout_chunks) {
            go_idle(core);
            core->stats.cancels++;
            return MUSE_WW_ACTION_CANCELLED;
        }
        return MUSE_WW_ACTION_NONE;

    case MUSE_WW_CAPTURING:
        core->capture_chunks++;
        core->silence_chunks = speech_now(core, in) ? 0 : core->silence_chunks + 1;
        if (in->event == WW_EVENT_SPEECH_END ||
            (core->cfg.energy_endpointing && core->silence_chunks >= core->cfg.end_silence_chunks) ||
            core->capture_chunks >= core->cfg.max_capture_chunks) {
            go_idle(core);
            core->stats.submits++;
            return MUSE_WW_ACTION_POST_PTT_UP | MUSE_WW_ACTION_SUBMITTED;
        }
        return MUSE_WW_ACTION_NONE;
    }
    return MUSE_WW_ACTION_NONE;
}

bool muse_wakeword_energy_voiced(const int16_t *pcm, size_t frames, int32_t rms_threshold)
{
    if (!pcm || !frames) {
        return false;
    }
    int64_t sum_sq = 0;
    for (size_t i = 0; i < frames; i++) {
        sum_sq += (int64_t)pcm[i] * pcm[i];
    }
    int64_t need = (int64_t)rms_threshold * rms_threshold * (int64_t)frames;
    return sum_sq >= need;
}

void muse_ww_ring_init(muse_ww_ring_t *ring, int16_t *storage, size_t capacity_frames)
{
    if (!ring) {
        return;
    }
    *ring = (muse_ww_ring_t){
        .storage = storage,
        .capacity_frames = storage ? capacity_frames : 0,
    };
}

void muse_ww_ring_clear(muse_ww_ring_t *ring)
{
    if (!ring) {
        return;
    }
    ring->head = 0;
    ring->fill_frames = 0;
}

void muse_ww_ring_push(muse_ww_ring_t *ring, const int16_t *pcm, size_t frames)
{
    if (!ring || !pcm) {
        return;
    }
    if (!ring->capacity_frames) {
        ring->dropped_frames += (uint32_t)frames;
        return;
    }
    if (frames > ring->capacity_frames) {
        /* Only the newest capacity's worth can survive; all currently
         * held frames and the head of this push are dropped. */
        ring->dropped_frames += (uint32_t)(ring->fill_frames + frames - ring->capacity_frames);
        pcm += frames - ring->capacity_frames;
        frames = ring->capacity_frames;
        ring->head = 0;
        ring->fill_frames = 0;
    } else if (ring->fill_frames + frames > ring->capacity_frames) {
        size_t overflow = ring->fill_frames + frames - ring->capacity_frames;
        ring->head = (ring->head + overflow) % ring->capacity_frames;
        ring->fill_frames -= overflow;
        ring->dropped_frames += (uint32_t)overflow;
    }
    size_t tail = (ring->head + ring->fill_frames) % ring->capacity_frames;
    for (size_t i = 0; i < frames; i++) {
        ring->storage[tail] = pcm[i];
        tail = (tail + 1) % ring->capacity_frames;
    }
    ring->fill_frames += frames;
}

size_t muse_ww_ring_pop(muse_ww_ring_t *ring, int16_t *out, size_t frames)
{
    if (!ring || !out || !ring->capacity_frames) {
        return 0;
    }
    size_t n = frames < ring->fill_frames ? frames : ring->fill_frames;
    for (size_t i = 0; i < n; i++) {
        out[i] = ring->storage[ring->head];
        ring->head = (ring->head + 1) % ring->capacity_frames;
    }
    ring->fill_frames -= n;
    return n;
}

size_t muse_ww_ring_snapshot(const muse_ww_ring_t *ring, int16_t *out, size_t out_frames)
{
    if (!ring || !out || !ring->capacity_frames) {
        return 0;
    }
    /* When out is too small for everything held, the newest frames
     * win: the tail holds the wake word and speech start. */
    size_t skip = ring->fill_frames > out_frames ? ring->fill_frames - out_frames : 0;
    size_t n = ring->fill_frames - skip;
    size_t at = (ring->head + skip) % ring->capacity_frames;
    for (size_t i = 0; i < n; i++) {
        out[i] = ring->storage[at];
        at = (at + 1) % ring->capacity_frames;
    }
    return n;
}

size_t muse_ww_ring_fill(const muse_ww_ring_t *ring)
{
    return ring ? ring->fill_frames : 0;
}

uint32_t muse_ww_ring_drops(const muse_ww_ring_t *ring)
{
    return ring ? ring->dropped_frames : 0;
}
