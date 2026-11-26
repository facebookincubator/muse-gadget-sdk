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

#include "mg_play.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mgcommands.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}
#else
#define big_alloc malloc
#endif

#define LC3_FRAME_US 10000
#define LC3_MIN_BYTES 20
#define LC3_MAX_BYTES 400

/* 16000 first (a start_streaming without a rate takes it), then 24000, which
 * LC3 plays. */
static const uint32_t RATES[] = { 16000,
#if MG_LC3_24K
                                  24000,
#endif
                                  8000, 32000, 44100, 48000 };

void mg_play_init(mg_play_t *p, uint32_t buffer_bytes)
{
    memset(p, 0, sizeof(*p));
    p->size = buffer_bytes;
    p->out_us = MG_PLAY_DEPTH_UNKNOWN;
}

size_t mg_play_codecs(uint8_t *out, size_t cap)
{
    size_t n = 0;
    const uint8_t all[] = {
        mg_data_type_audio_sbc,
#if MG_WITH_LC3
        mg_data_type_audio_lc3,
#endif
        mg_data_type_audio_pcm,
    };
    for (size_t i = 0; i < sizeof(all) && n < cap; i++) {
        out[n++] = all[i];
    }
    return n;
}

size_t mg_play_rates(uint32_t *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < sizeof(RATES) / sizeof(RATES[0]) && n < cap; i++) {
        out[n++] = RATES[i];
    }
    return n;
}

static bool rate_ok(uint32_t rate)
{
    for (size_t i = 0; i < sizeof(RATES) / sizeof(RATES[0]); i++) {
        if (RATES[i] == rate) {
            return true;
        }
    }
    return false;
}

static enum sbc_freq sbc_freq_of(uint32_t rate)
{
    switch (rate) {
    case 16000: return SBC_FREQ_16K;
    case 32000: return SBC_FREQ_32K;
    case 44100: return SBC_FREQ_44K1;
    case 48000: return SBC_FREQ_48K;
    default: return (enum sbc_freq)-1;
    }
}

uint16_t mg_play_check(mg_play_params_t *pp)
{
    if (!pp->rate) {
        pp->rate = RATES[0];
    }
    if (!pp->channels) {
        pp->channels = 1;
    }
    if (!rate_ok(pp->rate) || pp->channels > MG_PLAY_MAX_CHANNELS
        || pp->bitrate_kbps > MG_PLAY_MAX_BITRATE_KBPS) {
        return mg_error_code_invalid_value;
    }
    switch (pp->codec) {
    case mg_data_type_audio_sbc:
        pp->frame_bytes = 0;   /* self-describing */
        return pp->rate == 8000 || pp->rate == 24000 ? mg_error_code_invalid_value : 0;   /* SBC has no 8 or 24 kHz */
    case mg_data_type_audio_pcm:
        pp->frame_bytes = 0;
        /* 16-bit mono: 8 and 16 kHz fit the maximum bitrate. */
        return pp->rate * 16 / 1000 <= MG_PLAY_MAX_BITRATE_KBPS ? 0 : mg_error_code_invalid_value;
#if MG_WITH_LC3
    case mg_data_type_audio_lc3:
        if (pp->rate != 16000 && !(MG_LC3_24K && pp->rate == 24000)) {
            return mg_error_code_invalid_value;   /* this build decodes 16 and 24 kHz, 10 ms */
        }
        if (!pp->frame_bytes) {
            pp->frame_bytes = pp->bitrate_kbps ? (uint16_t)(pp->bitrate_kbps * LC3_FRAME_US / 8000)
                                               : MG_LC3_DEFAULT_FRAME_BYTES;
        }
        return pp->frame_bytes >= LC3_MIN_BYTES && pp->frame_bytes <= LC3_MAX_BYTES ? 0 : mg_error_code_invalid_value;
#endif
    default:
        return mg_error_code_invalid_value;
    }
}

int32_t mg_play_frames_us(const mg_play_params_t *pp, const uint8_t *d, size_t len, uint32_t *frames)
{
    uint64_t us = 0;
    uint32_t n = 0;
    switch (pp->codec) {
    case mg_data_type_audio_sbc: {
        size_t off = 0;
        while (off < len) {
            struct sbc_frame f;
            if (len - off < SBC_PROBE_SIZE || sbc_probe(d + off, &f) != 0 || f.mode != SBC_MODE_MONO
                || (!f.msbc && f.freq != sbc_freq_of(pp->rate)) || (f.msbc && pp->rate != 16000)) {
                return -1;
            }
            size_t fs = sbc_get_frame_size(&f);
            if (!fs || fs > len - off) {
                return -1;
            }
            us += (uint64_t)f.nblocks * f.nsubbands * 1000000u / pp->rate;
            off += fs;
            n++;
        }
        break;
    }
    case mg_data_type_audio_lc3:
        if (!pp->frame_bytes || len % pp->frame_bytes) {
            return -1;
        }
        n = (uint32_t)(len / pp->frame_bytes);
        us = (uint64_t)n * LC3_FRAME_US;
        break;
    case mg_data_type_audio_pcm:
        if (len % 2) {
            return -1;
        }
        n = (uint32_t)(len / 2);
        us = (uint64_t)n * 1000000u / pp->rate;
        break;
    default:
        return -1;
    }
    if (frames) {
        *frames = n;
    }
    return (int32_t)us;
}

uint32_t mg_play_start_threshold_us(const mg_play_t *p)
{
    return p->start_us > MG_PLAY_PREBUFFER_US ? p->start_us : MG_PLAY_PREBUFFER_US;
}

/* Adaptive start: judges the stream that just ended (it played audio outside a call, on this link). */
static void adapt_start(mg_play_t *p)
{
    bool skip = p->start_skip;
    p->start_skip = false;
    if (skip || p->stream_call || !p->st.frames_popped) {
        return;
    }
    uint32_t cur = mg_play_start_threshold_us(p), next = cur;
    if (p->st.early_underruns) {
        uint32_t cap = mg_play_live_floor_us(p);
        next = cur + MG_PLAY_START_STEP_UP_US < cap ? cur + MG_PLAY_START_STEP_UP_US : cap;
        next = next > cur ? next : cur;
    } else if (!p->st.underruns && cur > MG_PLAY_PREBUFFER_US) {
        next = cur - MG_PLAY_START_STEP_DOWN_US > MG_PLAY_PREBUFFER_US ? cur - MG_PLAY_START_STEP_DOWN_US
                                                                       : MG_PLAY_PREBUFFER_US;
    }
    if (next != cur) {
        p->start_note = next > cur ? "early underrun last reply" : "clean reply";
    }
    p->start_us = next;
}

bool mg_play_start(mg_play_t *p, const mg_play_params_t *params)
{
    if (!p->buf && !(p->buf = big_alloc(p->size))) {
        return false;
    }
    adapt_start(p);
    p->stream_call = false;
    p->play_at_us = 0;
    p->params = *params;
    p->head = p->len = p->buffered_us = p->pending_us = 0;
    p->in_bytes = p->in_us = 0;
    p->state = MG_PLAY_PREBUFFER;
    p->generation++;
    p->stream++;
    p->done_keep = false;
    p->starving = false;
    p->starve_us = 0;
    p->flushed = false;
    p->audio_clock_us = 0;
    p->write_clock_us = 0;
    p->wr_total = p->rd_total = 0;
    p->anchor_on = p->anchor_holding = false;
    p->pre_holding = false;
    p->pre_wait_us = 0;
    p->av_ok = false;   /* a new stream: no audio clock until its first anchor resolves */
    memset(&p->st, 0, sizeof(p->st));
    p->st.last_underrun_us = MG_PLAY_NO_UNDERRUN;
    /* A live depth holds max(asked, the floor); the allowance is the link's and stays. */
    if (params->live_depth_ms) {
        uint32_t asked = (uint32_t)params->live_depth_ms * 1000u, floor = mg_play_live_floor_us(p);
        p->live_us = asked > floor ? asked : floor;
    } else {
        p->live_us = 0;
    }
    p->live_clean_us = 0;
    return true;
}

uint32_t mg_play_live_floor_us(const mg_play_t *p)
{
    return MG_PLAY_PREBUFFER_US + MG_PLAY_CHUNK_US + (p->out_us == MG_PLAY_DEPTH_UNKNOWN ? 0 : p->out_us);
}

void mg_play_live_forget(mg_play_t *p)
{
    p->live_allow_us = 0;
    p->live_clean_us = 0;
    /* Adaptive start: a new connection starts at 30 ms, and the last link's stream isn't judged. */
    p->start_us = 0;
    p->start_note = NULL;
    p->start_skip = true;
}

void mg_play_release(mg_play_t *p)
{
    if (p->state == MG_PLAY_IDLE) {
        free(p->buf);
        p->buf = NULL;
    }
}

bool mg_play_active(const mg_play_t *p)
{
    return p->state != MG_PLAY_IDLE;
}

uint32_t mg_play_available(const mg_play_t *p)
{
    return p->state == MG_PLAY_PREBUFFER || p->state == MG_PLAY_PLAYING ? p->size - p->len : 0;
}

/* The stream's bytes per second, for the call's credit. */
static uint64_t stream_bytes_per_s(const mg_play_t *p)
{
    const mg_play_params_t *pp = &p->params;
    switch (pp->codec) {
    case mg_data_type_audio_lc3:
        return (uint64_t)pp->frame_bytes * (1000000u / LC3_FRAME_US);
    case mg_data_type_audio_pcm:
        return (uint64_t)pp->rate * 2;
    default:
        if (pp->bitrate_kbps) {
            return (uint64_t)pp->bitrate_kbps * 125;
        }
        if (p->in_us) {
            return p->in_bytes * 1000000u / p->in_us;
        }
        return (uint64_t)MG_PLAY_MAX_BITRATE_KBPS * 125;
    }
}

uint32_t mg_play_credit(const mg_play_t *p, uint32_t flight_us)
{
    uint32_t room = mg_play_available(p);
    if (!p->live_us || !room) {
        return room;
    }
    uint64_t bps = stream_bytes_per_s(p);
    uint64_t queued = (uint64_t)p->buffered_us + p->pending_us + (p->out_us == MG_PLAY_DEPTH_UNKNOWN ? 0 : p->out_us);
    /* What's in flight doesn't count against the depth: the link's measured share, plus the allowance.
     * While an anchor holds the reply's start, no more than the depth itself (mgcommands.h). */
    flight_us = flight_us < MG_PLAY_LIVE_FLIGHT_MAX_US ? flight_us : MG_PLAY_LIVE_FLIGHT_MAX_US;
    uint64_t window = (uint64_t)p->live_us + (p->anchor_holding ? 0 : flight_us + p->live_allow_us);
    if (queued >= window) {
        return 0;
    }
    uint64_t bytes = (window - queued) * bps / 1000000u;
    if (p->params.codec == mg_data_type_audio_lc3 && p->params.frame_bytes) {
        bytes -= bytes % p->params.frame_bytes;
    } else if (p->params.codec == mg_data_type_audio_pcm) {
        bytes &= ~(uint64_t)1;
    }
    return bytes < room ? (uint32_t)bytes : room;
}

uint32_t mg_play_received(const mg_play_t *p)
{
    return p->st.bytes_received;
}

void mg_play_drop_write(mg_play_t *p, size_t len)
{
    if (p->state != MG_PLAY_PREBUFFER && p->state != MG_PLAY_PLAYING) {
        return;
    }
    p->st.bytes_received += (uint32_t)len;
    p->st.bytes_dropped += (uint32_t)len;
    p->st.writes_dropped++;
}

bool mg_play_write(mg_play_t *p, const uint8_t *data, size_t len)
{
    if (p->state != MG_PLAY_PREBUFFER && p->state != MG_PLAY_PLAYING) {
        return false;   /* not streaming, or draining after stop: ignored */
    }
    p->st.bytes_received += (uint32_t)len;
    uint32_t frames = 0;
    int32_t us = mg_play_frames_us(&p->params, data, len, &frames);
    if (us < 0 || len > p->size - p->len) {
        /* Dropped whole, so the frames that follow still line up. */
        if (us < 0) {
            p->st.bad_frames++;
        }
        p->st.bytes_dropped += (uint32_t)len;
        p->st.writes_dropped++;
        return false;
    }
    uint32_t tail = (p->head + p->len) % p->size;
    size_t first = p->size - tail < len ? p->size - tail : len;
    memcpy(p->buf + tail, data, first);
    memcpy(p->buf, data + first, len - first);
    p->len += (uint32_t)len;
    p->buffered_us += (uint32_t)us;
    p->in_bytes += len;
    p->in_us += (uint32_t)us;
    p->st.frames_in += frames;
    p->write_clock_us = p->st.clock_us;
    p->wr_total += (uint32_t)len;
    return true;
}

/* A reply's first audio is next: the stream's very first, or the first after the gap between replies. */
static bool reply_start(const mg_play_t *p, uint32_t at)
{
    return p->st.frames_popped == 0 || (p->starving && !p->flushed && at - p->gap_at >= MG_PLAY_IDLE_US);
}

bool mg_play_anchor(mg_play_t *p, uint32_t frame)
{
    if (!p->shown32 || (p->state != MG_PLAY_PREBUFFER && p->state != MG_PLAY_PLAYING)) {
        return false;
    }
    p->anchor_on = true;
    p->anchor_holding = false;
    p->anchor_frame = frame;
    /* Its reply's audio already here, not yet played (it came just after it): it names that first sample. */
    bool head = p->pre_holding || (p->len && reply_start(p, p->st.clock_us));
    p->anchor_at = head ? p->rd_total : p->wr_total;
    p->anchor_wait_us = p->pre_holding ? p->pre_wait_us : 0;
    /* The audio clock's reference once it resolves: the anchored sample's place in the content written. */
    p->av_frame_next = frame;
    p->av_ref_next_us = head ? p->in_us - p->buffered_us : p->in_us;
    return true;
}

void mg_play_anchor_drop(mg_play_t *p)
{
    p->anchor_on = p->anchor_holding = false;
    p->pre_holding = false;
    p->av_ok = false;   /* the video stopped or restarted: its picture numbers no longer hold */
}

/* The anchored sample is up (rd_total reached anchor_at): resolve it. */
static void anchor_done(mg_play_t *p, bool held, int32_t offset)
{
    p->anchor_on = p->anchor_holding = false;
    p->st.anchors++;
    p->st.anchors_held += held;
    p->st.anchor_held_us = p->anchor_wait_us;
    p->st.anchor_offset = offset;
    p->st.anchor_measured = !held;
    p->av_frame = p->av_frame_next;
    p->av_ref_us = p->av_ref_next_us;
    p->av_ok = true;
}

bool mg_play_av_pos(const mg_play_t *p, uint32_t *frame, int32_t *pos_us)
{
    if (!p->av_ok || (p->state != MG_PLAY_PLAYING && p->state != MG_PLAY_DRAINING) || p->starving || p->flushed ||
        p->anchor_holding || p->pre_holding || p->out_us == MG_PLAY_DEPTH_UNKNOWN) {
        return false;
    }
    int64_t pos = (int64_t)(p->in_us - p->av_ref_us) - (int64_t)p->buffered_us - (int64_t)p->pending_us -
                  (int64_t)p->out_us;
    if (pos > INT32_MAX || pos < INT32_MIN) {
        return false;
    }
    *frame = p->av_frame;
    *pos_us = (int32_t)pos;
    return true;
}

void mg_play_stop(mg_play_t *p, bool keep)
{
    if (p->state == MG_PLAY_IDLE) {
        return;
    }
    p->generation++;
    if (keep && p->len) {
        p->state = MG_PLAY_DRAINING;
        return;
    }
    p->head = p->len = p->buffered_us = 0;
    if (!keep) {
        p->pending_us = 0;   /* dropped: the playback task throws its FIFO away */
    }
    p->state = keep ? MG_PLAY_DONE : MG_PLAY_IDLE;
    p->done_keep = keep;
}

static void peek(const mg_play_t *p, uint32_t off, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = p->buf[(p->head + off + i) % p->size];
    }
}

/* Bytes and duration of the frame at the ring's front. */
static bool front_frame(const mg_play_t *p, uint32_t off, size_t *bytes, uint32_t *us)
{
    const mg_play_params_t *pp = &p->params;
    if (off >= p->len) {
        return false;
    }
    switch (pp->codec) {
    case mg_data_type_audio_sbc: {
        uint8_t h[SBC_PROBE_SIZE];
        struct sbc_frame f;
        if (p->len - off < SBC_PROBE_SIZE) {
            return false;
        }
        peek(p, off, h, sizeof(h));
        if (sbc_probe(h, &f) != 0) {
            return false;
        }
        *bytes = sbc_get_frame_size(&f);
        *us = (uint32_t)((uint64_t)f.nblocks * f.nsubbands * 1000000u / pp->rate);
        break;
    }
    case mg_data_type_audio_lc3:
        *bytes = pp->frame_bytes;
        *us = LC3_FRAME_US;
        break;
    default: {
        /* PCM: 10 ms at a time. */
        size_t b = pp->rate / 100 * 2;
        *bytes = p->len - off < b ? (p->len - off) & ~1u : b;
        *us = (uint32_t)((uint64_t)(*bytes / 2) * 1000000u / pp->rate);
        break;
    }
    }
    return *bytes && *bytes <= p->len - off;
}

static uint32_t silence(mg_play_t *p, uint32_t chunk_us)
{
    p->st.silence_us += chunk_us;
    if (p->keep_open) {
        p->starve_us = 0;   /* a Live call's stream: open until stop_streaming, a disconnect or the call's end */
        return 0;
    }
    p->starve_us += chunk_us;
    if (p->starve_us >= MG_PLAY_STARVE_END_US) {
        /* The client stopped sending without stop_streaming: end it (drop). */
        p->head = p->len = p->buffered_us = 0;
        p->state = MG_PLAY_DONE;
        p->done_keep = false;
        p->generation++;
    }
    return 0;
}

size_t mg_play_pop(mg_play_t *p, uint8_t *out, size_t cap, uint32_t chunk_us)
{
    /* The stream's clock: this chunk starts at `at`, whatever it holds. */
    uint32_t at = p->st.clock_us;
    if (p->state == MG_PLAY_PREBUFFER || p->state == MG_PLAY_PLAYING || p->state == MG_PLAY_DRAINING) {
        p->st.clock_us += chunk_us;
    }
    switch (p->state) {
    case MG_PLAY_PREBUFFER:
        /* Only at the start: a gap later is concealed, never re-buffered. On a link that isn't active
         * the start is 150 ms, or the clip's end of data when it's shorter (no data for 100 ms). */
        if (p->len < p->size / 2) {
            bool slow = p->slow_link && !p->keep_open && !p->live_us;   /* a live stream paces live */
            /* Adaptive start: a reply outside a call starts at start_us; a call's at 30 ms. */
            uint32_t need = slow ? MG_PLAY_SLOW_START_US : p->keep_open ? MG_PLAY_PREBUFFER_US
                                                                        : mg_play_start_threshold_us(p);
            if (p->live_us) {
                /* A live stream's credit lets the buffer reach its depth less the output path, no more when
                 * nothing is in flight: a start that needs more would wait for bytes it never grants. */
                uint32_t out = p->out_us == MG_PLAY_DEPTH_UNKNOWN ? 0 : p->out_us;
                uint32_t room = p->live_us > out ? p->live_us - out : 0;
                need = need < room ? need : room;
            }
            bool ended = slow && p->len && at - p->write_clock_us >= MG_PLAY_SLOW_END_US;
            if (p->buffered_us < need && !ended) {
                return silence(p, chunk_us);
            }
        }
        p->state = MG_PLAY_PLAYING;
        p->play_at_us = at;   /* the reply's first audio: its first second counts from here */
        break;
    case MG_PLAY_PLAYING:
    case MG_PLAY_DRAINING:
        break;
    default:
        return 0;
    }
    bool anchored = false;   /* this chunk starts a reply an anchor held or released: its gap isn't an underrun */
    /* A reply's first audio (the stream's, or after the gap between replies) with call video showing and no
     * anchor yet: wait up to MG_PLAY_ANCHOR_WAIT_US for one. */
    if (!p->anchor_on && p->len && p->shown32 && (p->pre_holding || reply_start(p, at))) {
        uint32_t shown = 0;
        if (p->pre_wait_us < MG_PLAY_ANCHOR_WAIT_US && p->shown32(&shown)) {
            p->pre_holding = true;
            p->pre_wait_us += chunk_us;
            p->st.silence_us += chunk_us;
            return 0;   /* silence, not concealment (mg_play_concealing) */
        }
        anchored = p->pre_holding;   /* it waited: not an underrun */
        p->pre_holding = false;
    }
    if (p->anchor_on && p->rd_total == p->anchor_at && p->len) {
        /* The anchored sample is next. After a gap (the stream's start, or empty since its last audio) it
         * waits for its picture, playing silence; while audio plays it's only measured (below). */
        uint32_t shown = 0;
        bool showing = p->shown32 && p->shown32(&shown);
        int32_t off = showing ? (int32_t)(shown - p->anchor_frame) : 0;
        bool gap = p->starving || at == p->play_at_us || p->anchor_holding || p->pre_holding ||
                   p->st.frames_popped == 0;
        if (gap && showing && off < 0 && p->anchor_wait_us < MG_PLAY_ANCHOR_MAX_US) {
            p->anchor_holding = true;
            p->anchor_wait_us += chunk_us;
            p->st.silence_us += chunk_us;
            return 0;   /* silence (mg_play_concealing says no while holding) */
        }
        if (gap) {
            anchor_done(p, true, off);
            anchored = true;
            p->pre_holding = false;
        }
    }
    size_t n = 0;
    uint32_t got_us = 0;
    while (got_us < chunk_us) {
        size_t fb;
        uint32_t us;
        if (!front_frame(p, 0, &fb, &us) || n + fb > cap) {
            break;
        }
        if (p->anchor_on && p->rd_total == p->anchor_at) {
            /* Audio playing reached the anchored sample: measured, never paused. */
            uint32_t shown = 0;
            bool showing = p->shown32 && p->shown32(&shown);
            anchor_done(p, false, showing ? (int32_t)(shown - p->anchor_frame) : 0);
        }
        peek(p, 0, out + n, fb);
        p->rd_total += (uint32_t)fb;
        p->head = (p->head + (uint32_t)fb) % p->size;
        p->len -= (uint32_t)fb;
        p->buffered_us = p->buffered_us > us ? p->buffered_us - us : 0;
        n += fb;
        got_us += us;
        p->st.frames_popped++;
    }
    if (p->keep_open) {
        p->stream_call = true;   /* a Live call's stream: not judged by the adaptive start */
    }
    if (n) {
        /* Back from a gap: an underrun if it was short; one of MG_PLAY_IDLE_US or more was the space
         * between replies, and a flush's isn't one. */
        if (p->starving && !p->flushed && !anchored && at - p->gap_at < MG_PLAY_IDLE_US) {
            p->st.underruns++;
            p->st.last_underrun_us = p->gap_at;
            if (p->gap_at - p->play_at_us < MG_PLAY_EARLY_US) {
                p->st.early_underruns++;   /* in the reply's first second: the next one starts higher */
            }
            p->st.gap_us += at - p->gap_at;   /* concealed: the reply runs that late (the trim repays it) */
            if (p->live_us) {
                /* A live depth: the link held more in flight than the allowance covered. */
                uint32_t a = p->live_allow_us + MG_PLAY_LIVE_GROW_US;
                a = a < MG_PLAY_LIVE_ALLOW_MAX_US ? a : MG_PLAY_LIVE_ALLOW_MAX_US;
                p->st.live_grown += a != p->live_allow_us;
                p->live_allow_us = a;
                p->live_clean_us = 0;
            }
        } else if (p->starving && !p->flushed) {
            p->st.idle_gaps++;                /* a reply boundary */
        }
        if (p->live_us && p->live_allow_us && (p->live_clean_us += got_us) >= MG_PLAY_LIVE_CLEAN_US) {
            /* Clean for a while (audio played, between replies doesn't count): a little less. */
            p->live_allow_us = p->live_allow_us > MG_PLAY_LIVE_SHRINK_US ? p->live_allow_us - MG_PLAY_LIVE_SHRINK_US : 0;
            p->live_clean_us = 0;
            p->st.live_shrunk++;
        }
        p->starving = false;
        p->flushed = false;
        p->starve_us = 0;
        p->pre_wait_us = 0;   /* the next reply gets its own wait for an anchor */
        p->pending_us += got_us;
        p->audio_clock_us = at + chunk_us;
        return n;
    }
    if (p->state == MG_PLAY_DRAINING) {
        p->state = MG_PLAY_DONE;
        p->done_keep = true;
        p->head = p->len = p->buffered_us = 0;
        return 0;
    }
    /* Ran dry while playing: the playback task conceals this chunk
     * (mg_play_concealing), and the next frame that arrives plays at once
     * (counted as an underrun then, if the gap was short). */
    if (!p->starving) {
        p->starving = true;
        p->gap_at = at;
    }
    return silence(p, chunk_us);
}

bool mg_play_concealing(const mg_play_t *p)
{
    return p->state == MG_PLAY_PLAYING && p->starving && !p->flushed && !p->anchor_holding && !p->pre_holding;
}

bool mg_play_audible(const mg_play_t *p)
{
    if (p->state != MG_PLAY_PREBUFFER && p->state != MG_PLAY_PLAYING && p->state != MG_PLAY_DRAINING) {
        return false;
    }
    if (p->len || p->pending_us) {
        return true;
    }
    return !p->flushed && p->st.frames_popped && p->st.clock_us - p->audio_clock_us < MG_PLAY_AUDIBLE_TAIL_US;
}

bool mg_play_flush(mg_play_t *p)
{
    if (p->state != MG_PLAY_PREBUFFER && p->state != MG_PLAY_PLAYING && p->state != MG_PLAY_DRAINING) {
        return false;
    }
    p->flush_us = p->buffered_us + p->pending_us;
    p->st.flushes++;
    p->st.flushed_us += p->flush_us;
    p->head = p->len = p->buffered_us = p->pending_us = 0;
    p->rd_total = p->wr_total;   /* the ring is empty: what was written counts as taken */
    p->av_ok = false;   /* and the audio clock: what it counted from is gone */
    p->anchor_on = p->anchor_holding = false;   /* a flush drops a pending anchor */
    p->flush_seq++;
    p->flushed = true;
    p->starving = true;          /* empty now; the next audio plays at once */
    p->gap_at = p->st.clock_us;
    p->audio_clock_us = 0;
    return true;
}

void mg_play_set_pending(mg_play_t *p, uint32_t us)
{
    p->pending_us = us;
}

uint16_t mg_play_queued_ms(const mg_play_t *p)
{
    if (p->out_us == MG_PLAY_DEPTH_UNKNOWN) {
        return MG_PLAY_QUEUED_UNKNOWN;
    }
    if (p->state != MG_PLAY_PREBUFFER && p->state != MG_PLAY_PLAYING && p->state != MG_PLAY_DRAINING) {
        return 0;
    }
    if (p->flushed && !p->len && !p->pending_us) {
        return 0;   /* flushed (mgcommands.h: the buffer_update shows it emptied) */
    }
    uint64_t us = (uint64_t)p->buffered_us + p->pending_us + p->out_us;
    uint64_t ms = (us + 500) / 1000;
    return ms >= MG_PLAY_QUEUED_UNKNOWN ? MG_PLAY_QUEUED_UNKNOWN - 1 : (uint16_t)ms;
}

/* ---- decoding ---------------------------------------------------------- */

bool mg_dec_open(mg_dec_t *d, const mg_play_params_t *params)
{
    memset(d, 0, sizeof(*d));
    d->params = *params;
    sbc_reset(&d->sbc);
#if MG_WITH_LC3
    if (params->codec == mg_data_type_audio_lc3) {
        /* The library's own size: memory sized under other LC3 layout definitions would be overrun. */
        d->lc3 = lc3_decoder_size(LC3_FRAME_US, (int)params->rate) <= sizeof(d->lc3_mem)
                     ? lc3_setup_decoder(LC3_FRAME_US, (int)params->rate, 0, &d->lc3_mem)
                     : NULL;
        if (!d->lc3) {
            return false;
        }
    }
#endif
    return mg_resample_open(&d->rs, params->rate, MG_PLAY_OUT_RATE);
}

void mg_dec_close(mg_dec_t *d)
{
    mg_resample_close(&d->rs);
}

bool mg_dec_flush(mg_dec_t *d)
{
    /* The decoders start over (LC3's concealment state, SBC) and the resampler from silence, keeping its tables:
     * a flush allocates nothing, so it can't leave the decoder without a resampler. */
    mg_play_params_t pp = d->params;
    mg_resample_t rs = d->rs;
    uint32_t frames = d->frames, bad = d->bad, samples_out = d->samples_out, concealed = d->concealed;
    int32_t peak = d->peak;
    uint64_t sum_sq = d->sum_sq;
    memset(d, 0, sizeof(*d));
    d->params = pp;
    d->rs = rs;
    mg_resample_reset(&d->rs);
    sbc_reset(&d->sbc);
    bool ok = true;
#if MG_WITH_LC3
    if (pp.codec == mg_data_type_audio_lc3) {
        d->lc3 = lc3_decoder_size(LC3_FRAME_US, (int)pp.rate) <= sizeof(d->lc3_mem)
                     ? lc3_setup_decoder(LC3_FRAME_US, (int)pp.rate, 0, &d->lc3_mem)
                     : NULL;
        ok = d->lc3 != NULL;
    }
#endif
    d->frames = frames;
    d->bad = bad;
    d->samples_out = samples_out;
    d->concealed = concealed;
    d->peak = peak;
    d->sum_sq = sum_sq;
    return ok;
}

/* The last 10 ms that reached the output (SBC and PCM's faded repeat). */
static void keep_last(mg_dec_t *d, const int16_t *pcm, size_t n)
{
    const size_t keep = sizeof(d->last) / sizeof(d->last[0]);
    if (n >= keep) {
        memcpy(d->last, pcm + n - keep, sizeof(d->last));
        d->nlast = (uint32_t)keep;
        return;
    }
    size_t have = d->nlast;
    size_t drop = have + n > keep ? have + n - keep : 0;
    memmove(d->last, d->last + drop, (have - drop) * sizeof(int16_t));
    memcpy(d->last + have - drop, pcm, n * sizeof(int16_t));
    d->nlast = (uint32_t)(have - drop + n);
}

static size_t emit(mg_dec_t *d, const int16_t *pcm, size_t n, int16_t *out, size_t cap)
{
    size_t o = mg_resample_run(&d->rs, pcm, n, out, cap);
    for (size_t i = 0; i < o; i++) {
        int32_t v = out[i] < 0 ? -out[i] : out[i];
        d->peak = v > d->peak ? v : d->peak;
        d->sum_sq += (uint64_t)((int32_t)out[i] * out[i]);
    }
    d->samples_out += (uint32_t)o;
    return o;
}

size_t mg_dec_run(mg_dec_t *d, const uint8_t *data, size_t len, int16_t *out, size_t cap)
{
    size_t off = 0, o = 0;
    const mg_play_params_t *pp = &d->params;
    while (off < len) {
        size_t n = 0, used = 0;
        switch (pp->codec) {
        case mg_data_type_audio_sbc: {
            struct sbc_frame f;
            if (len - off < SBC_PROBE_SIZE || sbc_probe(data + off, &f) != 0) {
                d->bad++;
                return o;
            }
            used = sbc_get_frame_size(&f);
            n = (size_t)f.nblocks * f.nsubbands;
            if (!used || used > len - off || n > MG_RESAMPLE_MAX_IN
                || sbc_decode(&d->sbc, data + off, (unsigned)used, &f, d->pcm, 1, NULL, 0) != 0) {
                d->bad++;
                return o;
            }
            break;
        }
#if MG_WITH_LC3
        case mg_data_type_audio_lc3:
            used = pp->frame_bytes;
            n = pp->rate / 100;   /* 10 ms: 160 or 240 samples */
            if (used > len - off || lc3_decode(d->lc3, data + off, (int)used, LC3_PCM_FORMAT_S16, d->pcm, 1) < 0) {
                d->bad++;
                return o;
            }
            break;
#endif
        case mg_data_type_audio_pcm:
            used = len - off;
            if (used > sizeof(d->pcm)) {
                used = sizeof(d->pcm);
            }
            used &= ~(size_t)1;
            n = used / 2;
            for (size_t i = 0; i < n; i++) {
                d->pcm[i] = (int16_t)(data[off + 2 * i] | data[off + 2 * i + 1] << 8);
            }
            break;
        default:
            return o;
        }
        off += used;
        d->frames++;
        size_t e = emit(d, d->pcm, n, out + o, cap - o);
        keep_last(d, out + o, e);
        o += e;
        d->conceal_us = 0;   /* a frame decoded: the gap (if any) is over */
    }
    return o;
}

void mg_play_ramp_in(int16_t *pcm, size_t n, size_t ramp)
{
    if (ramp > n) {
        ramp = n;
    }
    for (size_t i = 0; i < ramp; i++) {
        pcm[i] = (int16_t)((int32_t)pcm[i] * (int32_t)(i + 1) / (int32_t)(ramp + 1));
    }
}

size_t mg_dec_conceal(mg_dec_t *d, int16_t *out, size_t cap, uint32_t us)
{
    size_t want = (size_t)((uint64_t)us * MG_PLAY_OUT_RATE / 1000000u);
    want = want < cap ? want : cap;
    size_t o = 0;
    uint32_t at = d->conceal_us;
    d->conceal_us += us;
    d->concealed++;
#if MG_WITH_LC3
    if (d->params.codec == mg_data_type_audio_lc3 && d->lc3) {
        /* The decoder's own concealment, 10 ms frames at the stream's rate,
         * resampled like real ones, for the gap's first MG_PLAY_CONCEAL_MAX_US. */
        while (o < want && at < MG_PLAY_CONCEAL_MAX_US) {
            size_t n = d->params.rate / 100;
            if (n > MG_RESAMPLE_MAX_IN || lc3_decode(d->lc3, NULL, 0, LC3_PCM_FORMAT_S16, d->pcm, 1) < 0) {
                break;
            }
            o += emit(d, d->pcm, n, out + o, cap - o);
            at += LC3_FRAME_US;
        }
        if (o > want) {
            o = want;   /* the resampler's odd sample: the speaker takes whole chunks */
        }
    } else
#endif
    if (at == 0 && d->nlast) {
        /* SBC and PCM: the last 10 ms once more, faded to nothing. */
        size_t n = d->nlast < want ? d->nlast : want;
        for (size_t i = 0; i < n; i++) {
            out[i] = (int16_t)((int32_t)d->last[i] * (int32_t)(n - i) / (int32_t)n);
        }
        o = n;
    }
    memset(out + o, 0, (want - o) * sizeof(int16_t));   /* then silence until data */
    return want;
}

void mg_level_add(mg_level_t *l, const int16_t *pcm, size_t n, bool muted)
{
    l->samples += (uint32_t)n;
    if (muted) {
        l->muted += (uint32_t)n;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        int32_t v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        l->peak = v > l->peak ? v : l->peak;
        l->sum_sq += (uint64_t)((int32_t)pcm[i] * pcm[i]);
    }
}

void mg_boost_set(mg_boost_t *b, int db10)
{
    if (db10 == b->db10 && (db10 == 0 || b->gain_q12)) {
        return;
    }
    b->db10 = db10;
    b->gain_q12 = db10 > 0 ? (int32_t)lroundf(4096.0f * powf(10.0f, (float)db10 / 200.0f)) : 4096;
}

void mg_boost_run(mg_boost_t *b, int16_t *pcm, size_t n)
{
    if (b->db10 <= 0) {
        return;
    }
    int32_t env = b->env;
    for (size_t i = 0; i < n; i++) {
        int32_t y = (pcm[i] * b->gain_q12 + 2048) >> 12;   /* up to ~2x full scale at +6 dB */
        int32_t a = y < 0 ? -y : y;
        env -= env >> 10;                                   /* release: 1024 samples, 64 ms at 16 kHz */
        if (a > env) {
            env = a;                                        /* attack: at once, so nothing gets past */
        }
        if (env > MG_BOOST_CEILING) {
            y = (int32_t)((int64_t)y * MG_BOOST_CEILING / env);
            b->limited++;
        }
        pcm[i] = (int16_t)y;
    }
    b->env = env;
}

int mg_boost_for_volume(int max_db10, int volume)
{
    if (max_db10 <= 0 || volume <= 50) {
        return 0;
    }
    if (volume > 100) {
        volume = 100;
    }
    return (max_db10 * (volume - 50) + 25) / 50;
}

static double db(double amp)
{
    double v = 20 * log10(amp / 32768.0 + 1e-9);
    return v < -120 ? -120 : v;
}

int mg_play_format_stats(char *out, size_t cap, const char *how, const mg_play_params_t *pp, const mg_play_stats_t *st,
                         const mg_dec_t *d, const mg_level_t *spk)
{
    const char *codec = pp->codec == mg_data_type_audio_sbc ? "sbc" : pp->codec == mg_data_type_audio_lc3 ? "lc3" : "pcm";
    double dec_rms = d->samples_out ? sqrt((double)d->sum_sq / d->samples_out) : 0;
    mg_level_t same = { .sum_sq = d->sum_sq, .peak = d->peak, .samples = d->samples_out };
    if (!spk) {
        spk = &same;
    }
    double spk_rms = spk->samples ? sqrt((double)spk->sum_sq / spk->samples) : 0;
    char off[40] = "";
    if (spk->muted && spk->muted >= spk->samples) {
        snprintf(off, sizeof(off), ", speaker off");
    } else if (spk->muted) {
        snprintf(off, sizeof(off), ", speaker off %lu ms", (unsigned long)(spk->muted / (MG_PLAY_OUT_RATE / 1000)));
    }
    /* Three lines: the Nordic UART mirror takes lines of up to 160 characters. */
    return snprintf(out, cap,
                    "stream end (%s): %s %lu Hz, %lu frames in, %lu decoded (%lu bad), %lu underruns, %lu ms silence\n"
                    "stream bytes: %lu received (%lu dropped), %lu ms played\n"
                    "stream level: decoded rms %.1f peak %.1f dBFS, speaker rms %.1f peak %.1f dBFS%s",
                    how, codec, (unsigned long)pp->rate, (unsigned long)st->frames_in, (unsigned long)d->frames,
                    (unsigned long)d->bad, (unsigned long)st->underruns, (unsigned long)(st->silence_us / 1000),
                    (unsigned long)st->bytes_received, (unsigned long)st->bytes_dropped,
                    (unsigned long)(d->samples_out / (MG_PLAY_OUT_RATE / 1000)), db(dec_rms), db(d->peak), db(spk_rms),
                    db(spk->peak), off);
}

int mg_play_format_tick(char *out, size_t cap, bool end, const mg_play_stats_t *st, uint32_t prev_underruns,
                        uint32_t span_ms, uint32_t buffered_us, uint16_t queued_ms, uint32_t internal_free,
                        uint32_t internal_low)
{
    long last = st->last_underrun_us == MG_PLAY_NO_UNDERRUN ? -1L : (long)(st->last_underrun_us / 1000);
    long queued = queued_ms == MG_PLAY_QUEUED_UNKNOWN ? -1L : (long)queued_ms;
    return snprintf(out, cap,
                    "av play%s: t=%lu span=%lu underruns=%lu new=%lu last=%ld buf=%lu queued=%ld int=%lu low=%lu",
                    end ? " end" : "", (unsigned long)(st->clock_us / 1000), (unsigned long)span_ms,
                    (unsigned long)st->underruns, (unsigned long)(st->underruns - prev_underruns), last,
                    (unsigned long)(buffered_us / 1000), queued, (unsigned long)internal_free,
                    (unsigned long)internal_low);
}

int mg_play_format_trimmed(char *out, size_t cap, uint32_t dropped, uint32_t repeated, uint32_t repaid)
{
    return snprintf(out, cap, " dropped=%lu repeated=%lu repaid=%lu", (unsigned long)dropped, (unsigned long)repeated,
                    (unsigned long)repaid);
}

/* ---- drift trim -------------------------------------------------------- */

void mg_trim_reset(mg_trim_t *t)
{
    memset(t, 0, sizeof(*t));
    t->every = MG_PLAY_TRIM_EVERY;
}

static int32_t iabs32(int32_t v)
{
    return v < 0 ? -v : v;
}

/* The band below the target: a third of the troughs' headroom at most (the floor: they reach empty first),
 * and the zone a repeat at 0.1 % counts as back, half of that at most. */
static int32_t trim_below(const mg_trim_t *t)
{
    return t->floor_us / 3 < MG_PLAY_TRIM_BAND_US ? t->floor_us / 3 : MG_PLAY_TRIM_BAND_US;
}

static int32_t trim_done_below(const mg_trim_t *t)
{
    int32_t half = trim_below(t) / 2;
    return half < MG_PLAY_TRIM_DONE_US ? half : MG_PLAY_TRIM_DONE_US;
}

/* One look at the smoothed depth, off the target by `off`, at now; `depth`: this window's own (a starved
 * window, 0, never starts a repeat). */
static mg_trim_event_t trim_judge(mg_trim_t *t, int64_t win_start, int64_t now, int32_t depth, int32_t off)
{
    int32_t below = trim_below(t);
    if (t->dir == 0) {
        int want = off > MG_PLAY_TRIM_BAND_US ? -1 : off < -below && depth > 0 ? +1 : 0;
        if (want == 0) {
            t->past_dir = 0;
            return MG_TRIM_NONE;
        }
        if (want != t->past_dir) {
            t->past_dir = want;
            t->past_since = win_start;   /* the whole window was past it */
        }
        if (now - t->past_since < MG_PLAY_TRIM_HOLD_US) {
            return MG_TRIM_NONE;
        }
        t->dir = want;
        t->every = MG_PLAY_TRIM_EVERY;
        t->phase = 0;
        t->past_dir = 0;
        t->step_at = now;
        t->step_off = iabs32(off);
        t->rate_at = now - MG_PLAY_TRIM_HOLD_US;   /* a start may stop as soon as it's back */
        return MG_TRIM_START;
    }
    if (now - t->step_at < MG_PLAY_TRIM_STEP_US) {
        return MG_TRIM_NONE;
    }
    mg_trim_event_t ev = MG_TRIM_NONE;
    bool done = t->dir < 0 ? off <= MG_PLAY_TRIM_DONE_US : off >= -trim_done_below(t);
    bool reached = t->dir < 0 ? off <= 0 : off >= 0;
    bool past = t->dir < 0 ? off > MG_PLAY_TRIM_BAND_US : off < -below;
    bool gained = t->step_off - iabs32(off) > 250;
    bool settled = now - t->rate_at >= MG_PLAY_TRIM_HOLD_US;   /* 2 s at this rate since it last slowed */
    if (t->every >= MG_PLAY_TRIM_EVERY && done && settled) {
        /* 0.1 %, back within 10 ms of the target: off. */
        t->dir = 0;
        t->past_dir = 0;
        ev = MG_TRIM_STOP;
    } else if (t->every < MG_PLAY_TRIM_EVERY && reached && settled) {
        /* A faster rate brought it to the target: half as fast (the clock may still be off by more than
         * 0.1 %; stopping would let it drift past the band again). */
        t->every *= 2;
        t->rate_at = now;
        ev = MG_TRIM_SLOWER;
    } else if (past && !gained && t->every / 2 >= MG_PLAY_TRIM_EVERY_MIN) {
        /* Still past the band and not gaining: the clock is further off than this rate corrects. */
        t->every /= 2;
        ev = MG_TRIM_FASTER;
    }
    t->step_at = now;
    t->step_off = iabs32(off);
    return ev;
}

mg_trim_event_t mg_trim_update(mg_trim_t *t, int64_t now_us, uint32_t held_us, bool playing)
{
    if (!playing) {
        /* A gap, the prebuffer or the drain: no window. A gap of a window or longer (not a concealed
         * chunk or two) starts the count toward a trim over. */
        if (t->win_open || !t->gap_from) {
            t->gap_from = now_us ? now_us : 1;
        }
        t->win_open = false;
        if (now_us - t->gap_from >= MG_PLAY_TRIM_WINDOW_US) {
            t->past_dir = 0;
        }
        return MG_TRIM_NONE;
    }
    t->gap_from = 0;
    if (!t->win_open) {
        t->win_open = true;
        t->win_start = now_us;
        t->win_sum = 0;
        t->win_n = 0;
        t->win_min = held_us;
        if (!t->playing_seen) {
            t->playing_seen = true;
            t->play_from = now_us;
        }
    }
    t->win_sum += held_us;
    t->win_n++;
    t->win_min = held_us < t->win_min ? held_us : t->win_min;
    if (now_us - t->win_start < MG_PLAY_TRIM_WINDOW_US) {
        return MG_TRIM_NONE;
    }
    t->win_open = false;
    int32_t mean = (int32_t)(t->win_sum / t->win_n);
    if (!t->target_us) {
        /* The steady depth: the windows after the first second's settling. */
        t->mean_us = mean;
        if (t->win_start - t->play_from < MG_PLAY_TRIM_SETTLE_US) {
            return MG_TRIM_NONE;
        }
        t->learn_sum += (uint32_t)mean;
        if (!t->learn_n || t->win_min < t->learn_min) {
            t->learn_min = t->win_min;
        }
        t->learn_n++;
        if ((uint64_t)t->learn_n * MG_PLAY_TRIM_WINDOW_US < MG_PLAY_TRIM_LEARN_US) {
            return MG_TRIM_NONE;
        }
        int32_t target = (int32_t)(t->learn_sum / t->learn_n);
        t->target_us = target > 0 ? target : 1;
        t->allow_at_target = t->live_allow_us;
        t->floor_us = (int32_t)t->learn_min;
        t->mean_us = t->target_us;
        return MG_TRIM_TARGET;
    }
    /* Smoothed over about 400 ms: the packets' sawtooth and the link's jitter move one window's mean. */
    int32_t step = (mean - t->mean_us) / MG_PLAY_TRIM_SMOOTH;
    t->mean_us = step ? t->mean_us + step : mean;   /* the last few us at once: integer steps would stall short */
    return trim_judge(t, t->win_start, now_us, mean, t->mean_us - mg_trim_target(t));
}

int32_t mg_trim_target(const mg_trim_t *t)
{
    if (!t->target_us) {
        return 0;
    }
    int32_t target = t->target_us + (t->live_allow_us - t->allow_at_target);
    if (t->live_cap_us > 0 && target > t->live_cap_us) {
        target = t->live_cap_us;
    }
    return target > 0 ? target : 1;
}

/* in[0..lin) resampled to out[0..lout) by linear interpolation, ends kept. */
static void warp(const int16_t *in, size_t lin, int16_t *out, size_t lout)
{
    for (size_t j = 0; j < lout; j++) {
        uint64_t pos = ((uint64_t)j * (lin - 1) << 16) / (lout - 1);
        size_t k = (size_t)(pos >> 16);
        int64_t f = (int64_t)(pos & 0xffff);
        int32_t a = in[k], b = k + 1 < lin ? in[k + 1] : a;
        out[j] = (int16_t)(a + (((int64_t)(b - a) * f) >> 16));
    }
}

void mg_trim_owe(mg_trim_t *t, uint32_t gap_us)
{
    t->debt += (uint32_t)((uint64_t)gap_us * MG_PLAY_OUT_RATE / 1000000u);
}

void mg_trim_forgive(mg_trim_t *t)
{
    t->debt = 0;
}

size_t mg_trim_run(mg_trim_t *t, int16_t *pcm, size_t n, size_t cap)
{
    bool repay = t->debt && t->repay_room;
    int dir = repay ? -1 : t->dir;
    uint32_t every = repay ? MG_PLAY_REPAY_EVERY : t->every;
    if (dir == 0) {
        t->phase = 0;
        return n;
    }
    const size_t span = MG_PLAY_TRIM_SPAN;
    int16_t in[MG_PLAY_TRIM_SPAN + 1], out[MG_PLAY_TRIM_SPAN + 1];
    size_t i = 0;
    while (i < n) {
        if (repay && !t->debt) {
            /* Made up: the drift trim's own rate, from the next block. */
            break;
        }
        size_t need = t->phase < every ? every - t->phase : 0;
        size_t p = i + need;
        size_t lin = dir < 0 ? span + 1 : span;
        if (p + lin > n || (dir > 0 && n + 1 > cap)) {
            /* Not due in this block, or due without room for the trim's span: the next block (from its start). */
            t->phase += (uint32_t)(n - i);
            break;
        }
        memcpy(in, pcm + p, lin * sizeof(int16_t));
        if (dir < 0) {
            warp(in, span + 1, out, span);
            memcpy(pcm + p, out, span * sizeof(int16_t));
            memmove(pcm + p + span, pcm + p + span + 1, (n - p - span - 1) * sizeof(int16_t));
            n--;
            if (repay) {
                t->repaid++;
                if (!--t->debt) {
                    t->repaid_now = true;
                }
            } else {
                t->dropped++;
            }
        } else {
            warp(in, span, out, span + 1);
            memmove(pcm + p + span + 1, pcm + p + span, (n - p - span) * sizeof(int16_t));
            memcpy(pcm + p, out, (span + 1) * sizeof(int16_t));
            n++;
            t->repeated++;
        }
        size_t lout = dir < 0 ? span : span + 1;
        i = p + lout;
        t->phase = (uint32_t)lout;
    }
    return n;
}

int mg_trim_format_repay(char *out, size_t cap, const mg_trim_t *t, bool done)
{
    if (done) {
        return snprintf(out, cap, "repaid");
    }
    return snprintf(out, cap, "repay %lu ms", (unsigned long)(t->debt * 1000u / MG_PLAY_OUT_RATE));
}

int mg_trim_format(char *out, size_t cap, mg_trim_event_t ev, const mg_trim_t *t, uint32_t out_us)
{
    int64_t dma = out_us != MG_PLAY_DEPTH_UNKNOWN ? out_us : 0;
    long depth_ms = (long)((t->mean_us + dma + 500) / 1000);
    if (ev == MG_TRIM_TARGET) {
        return snprintf(out, cap, "trim target %ld ms", (long)((mg_trim_target(t) + dma + 500) / 1000));
    }
    if (ev == MG_TRIM_STOP) {
        return snprintf(out, cap, "trim off (depth %ld ms)", depth_ms);
    }
    /* A start, or a running trim's new rate. */
    unsigned tenths = 1000u / (t->every ? t->every : MG_PLAY_TRIM_EVERY);   /* 1000: 0.1 %, 500: 0.2 %, 250: 0.4 % */
    return snprintf(out, cap, "trim %c%u.%u%% (depth %ld ms)", t->dir < 0 ? '-' : '+', tenths / 10, tenths % 10,
                    depth_ms);
}
