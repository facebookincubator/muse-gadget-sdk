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

#include "ww_runner_bench.h"

#include <stdlib.h>

#include "muse_wakeword.h"   /* muse_wakeword_energy_voiced */

/*
 * The runner contract with test triggers behind it instead of a model.
 * This is the runner the component links; a build that logs
 * "runner=bench" has no wake word detection whatsoever; see
 * ww_runner_bench.h for what can make it fire.
 */
#define PENDING_MAX 4

struct ww_runner {
    ww_config_t cfg;
    ww_event_t pending[PENDING_MAX];
    size_t pending_n;
    uint32_t loud_run;
    uint32_t quiet_run;
    uint32_t cooldown;
    bool burst_too_long;
    bool listening;
    bool suppressed;
};

ww_runner_t *ww_runner_create(const ww_config_t *cfg)
{
    ww_runner_t *r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    if (cfg) {
        r->cfg = *cfg;
    } else {
        r->cfg.sample_rate_hz = 16000;
        r->cfg.sensitivity = WW_SENSITIVITY_DEFAULT;
        r->cfg.vad = WW_VAD_ENERGY;
    }
    r->listening = true;
    return r;
}

void ww_runner_destroy(ww_runner_t *r)
{
    free(r);
}

static void forget(ww_runner_t *r)
{
    r->pending_n = 0;
    r->loud_run = 0;
    r->quiet_run = 0;
    r->cooldown = 0;
    r->burst_too_long = false;
}

void ww_runner_reset(ww_runner_t *r)
{
    if (r) {
        forget(r);
    }
}

static void push(ww_runner_t *r, ww_event_t ev)
{
    if (r && r->pending_n < PENDING_MAX) {
        r->pending[r->pending_n++] = ev;
    }
}

void ww_runner_bench_trigger(ww_runner_t *r)
{
    push(r, WW_EVENT_WAKE);
}

void ww_runner_bench_trigger_test(ww_runner_t *r)
{
    push(r, WW_EVENT_WAKE);
    push(r, WW_EVENT_SPEECH_START);
}

void ww_runner_set_listening(ww_runner_t *r, bool on)
{
    if (!r) {
        return;
    }
    r->listening = on;
    if (!on) {
        forget(r);
    }
}

void ww_runner_set_suppressed(ww_runner_t *r, bool on)
{
    if (r) {
        r->suppressed = on;
    }
}

/* A clap concentrates its energy in the first few milliseconds, so a
 * whole-20-ms RMS threshold misses it (seen on the BOX-3: three such
 * chunks in a row never happened). Call a chunk loud when it is either
 * sustained-loud, or has a real transient peak with one loud 5 ms
 * slice. The peak gate keeps ordinary loud-ish room sound out. */
static bool loud_chunk(const int16_t *pcm, size_t frames)
{
    if (!pcm || !frames) {
        return false;
    }
    if (muse_wakeword_energy_voiced(pcm, frames, WW_BENCH_LOUD_RMS)) {
        return true;
    }
    int32_t peak = 0;
    for (size_t i = 0; i < frames; i++) {
        int32_t v = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
        peak = v > peak ? v : peak;
    }
    if (peak < WW_BENCH_PEAK_MIN) {
        return false;
    }
    if (frames <= WW_BENCH_SUBFRAME_FRAMES) {
        return muse_wakeword_energy_voiced(pcm, frames, WW_BENCH_SUBFRAME_RMS);
    }
    for (size_t off = 0; off + WW_BENCH_SUBFRAME_FRAMES <= frames; off += WW_BENCH_SUBFRAME_FRAMES) {
        if (muse_wakeword_energy_voiced(pcm + off, WW_BENCH_SUBFRAME_FRAMES, WW_BENCH_SUBFRAME_RMS)) {
            return true;
        }
    }
    return false;
}

ww_event_t ww_runner_feed(ww_runner_t *r, const int16_t *pcm, size_t frames)
{
    if (!r || !r->listening) {
        return WW_EVENT_NONE;
    }
    if (r->suppressed) {
        forget(r);   /* a trigger during playback is discarded */
        return WW_EVENT_NONE;
    }
    if (r->pending_n) {
        ww_event_t ev = r->pending[0];
        for (size_t i = 1; i < r->pending_n; i++) {
            r->pending[i - 1] = r->pending[i];
        }
        r->pending_n--;
        return ev;
    }
    if (r->cooldown) {
        r->cooldown--;
        r->loud_run = r->quiet_run = 0;
        return WW_EVENT_NONE;
    }
    if (loud_chunk(pcm, frames)) {
        r->loud_run++;
        if (r->loud_run > WW_BENCH_BURST_MAX_CHUNKS) {
            r->burst_too_long = true;
        }
        r->quiet_run = 0;
        return WW_EVENT_NONE;
    }
    if (r->loud_run > 0) {
        r->quiet_run++;
        if (r->quiet_run >= WW_BENCH_QUIET_CHUNKS) {
            bool fire = !r->burst_too_long;
            r->loud_run = r->quiet_run = 0;
            r->burst_too_long = false;
            if (fire) {
                r->cooldown = WW_BENCH_COOLDOWN_CHUNKS;
                return WW_EVENT_WAKE;
            }
        }
        return WW_EVENT_NONE;
    }
    r->quiet_run = 0;
    return WW_EVENT_NONE;
}
