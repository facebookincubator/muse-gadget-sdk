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

#include "mg_proto.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "mg_codec.h"
#include "mg_device_session.h"
#include "mg_token_proof.h"
#include "mgcommands.h"


#define DEFAULT_BUZZ_FREQ_HZ 210
#define DEFAULT_BUZZ_MS 100
#define DEFAULT_BUZZ_VOLUME 80

void mg_settings_defaults(mg_settings_t *s)
{
    *s = (mg_settings_t){
        .buzz_freq_hz = DEFAULT_BUZZ_FREQ_HZ,
        .buzz_ms = DEFAULT_BUZZ_MS,
        .buzz_volume = DEFAULT_BUZZ_VOLUME,
        .codec = mg_data_type_audio_sbc,
        .queue_enabled = false,
        .capture_rate = MG_CODEC_RATE,
        .capture_kbps = MG_CAPTURE_DEFAULT_BITRATE_KBPS,
    };
}

static uint16_t le16(const uint8_t *b)
{
    return (uint16_t)(b[0] | b[1] << 8);
}

static void put16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *b, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
}

static void stream_abort(mg_proto_t *p, bool notify);
static size_t setting_width(const mg_proto_t *p, uint8_t param);
static bool setting_offered(const mg_proto_t *p, uint8_t param);
static void proof_reset(mg_proto_t *p);

static bool haptic_offered(const mg_proto_t *p)
{
    return p->cfg.haptics && p->cfg.haptic_actuator && p->ops.haptic;
}

static bool proof_supported(const mg_proto_t *p)
{
    return p->ops.proof_key && p->ops.proof_clear && p->ops.random;
}

/* The proof gate (mg_proto.h): mic output to the connected central may go now. */
static bool out_ok(const mg_proto_t *p)
{
    return !p->cfg.output_proof || p->out_bench || p->out_bench_conn || p->proof.state == MG_PROOF_MATCHED;
}

/* The gadget holds a proof key (it's been set up): a proof can match. */
static bool have_key(const mg_proto_t *p)
{
    if (!proof_supported(p)) {
        return false;
    }
    uint8_t k[MG_TOKEN_PROOF_KEY_LEN];
    bool ok = p->ops.proof_key(p->ops.ctx, k);
    mg_token_proof_wipe(k, sizeof(k));
    return ok;
}

/* A destructive or storing Control command (power off, the offline queue's erase, a stored setting): once the
 * gadget is set up, only from a central whose token proof matched on this connection; the bench output switch
 * doesn't count. Set up with no key (a setup cut short, a pair from firmware before the key) has no owner:
 * refused (fail closed). An unset gadget is open (the recovery design). */
static bool owner_ok(const mg_proto_t *p)
{
    if (have_key(p)) {
        return p->proof.state == MG_PROOF_MATCHED;
    }
    return !(proof_supported(p) && p->ops.set_up && p->ops.set_up(p->ops.ctx));
}

/* A release build's set-up gadget takes these only from a connection whose proof matched (bench and automation
 * builds: always). owner_ok is that: true unproven only on a gadget that isn't set up. */
static bool proof_input_ok(const mg_proto_t *p)
{
    return !p->cfg.proof_inputs || owner_ok(p);
}

/* Audio and video streams to the gadget (mgcommands.h, Token proof, start_streaming): once it's set up, only from a
 * central whose token proof matched on this connection, or on an automation build with the bench override on. */
static bool stream_ok(const mg_proto_t *p)
{
    return owner_ok(p) || (p->cfg.bench_streams && (p->out_bench_conn || p->out_bench));
}

static void held_turn_keep(mg_proto_t *p);
static void held_turn_release(mg_proto_t *p);
static bool output_stale(mg_proto_t *p);

/* The held live turn may go live: the client proven (or the bench switch) and ready. */
static bool held_turn_ready(const mg_proto_t *p)
{
    return out_ok(p) && mg_proto_ready(p) && p->ptt_enabled;
}

/* The device session needs the token proof: hello waits for a match. */
static bool session_supported(const mg_proto_t *p)
{
    return p->cfg.session && p->ops.session_credentials && p->ops.session_state && p->ops.session_message
           && proof_supported(p);
}

/* The credentials-only device session (Wi-Fi gadgets, cfg.creds_session). */
static bool creds_supported(const mg_proto_t *p)
{
    return !p->cfg.session && p->creds && p->ops.session_credentials && p->ops.session_store_tokens
           && proof_supported(p);
}

/* mg_session_cap_sealed_clear declared: the clear only as the sealed mg_session_clear. The BLE-only
 * session's config says so; the credentials-only one's build (MGDS_CAP_SEALED_CLEAR, its MGDS_CAPS too). */
static bool sealed_clear_declared(const mg_proto_t *p)
{
    return (session_supported(p) && p->cfg.session->sealed_clear) || (creds_supported(p) && MGDS_CAP_SEALED_CLEAR);
}

/* ---- sending ------------------------------------------------------------ */

/* What one notification carries on this link: min(ATT_MTU - 3,
 * MG_MAX_ATT_VALUE) (mgcommands.h). NimBLE cuts a longer one short without a
 * word (ble_att_truncate_to_mtu), so nothing longer is ever handed to it. */
static size_t link_max(const mg_proto_t *p)
{
    size_t m = p->mtu > 3 ? (size_t)p->mtu - 3 : 0;
    return m < MG_MAX_ATT_VALUE ? m : MG_MAX_ATT_VALUE;
}

static bool raw(mg_proto_t *p, mg_ch_t ch, const uint8_t *d, size_t n)
{
    if (!p->connected || !p->sub[ch] || !p->ops.notify) {
        return false;
    }
    if (n > link_max(p)) {
        p->link_refused++;   /* refused whole, never cut short (callers that hold check first) */
        return false;
    }
    return p->ops.notify(p->ops.ctx, ch, d, n);
}

static uint32_t now_ms(const mg_proto_t *p);
static bool proof_flush(mg_proto_t *p);
static bool speaker_due(mg_proto_t *p, int *v);

/* Sent now or not at all: what a refusal makes stale and the next tick sends
 * afresh (stream credit, touch events), and what has its own queue (the
 * capture's outbox, the session's and the credentials' ports). The token
 * proof answer still goes first. */
static bool send_control_now(mg_proto_t *p, const uint8_t *d, size_t n)
{
    if (p->proof_out.len && !proof_flush(p)) {
        return false;   /* a token proof answer goes ahead of everything else on Control */
    }
    bool ok = raw(p, MG_CH_CONTROL, d, n);
    p->ctl_sent += ok;
    return ok;
}

/* The Control reply outbox (one more request_status line once overran a
 * gadget's 5-reply reserve, and a gadget's reply slot silently
 * dropped an over-long capabilities reply): a one-shot reply the stack
 * refuses (request_status's lines, settings, errors) waits
 * here whole, in order, behind the token proof answer, and goes as the stack
 * takes it (every pump). One longer than the link's ATT payload waits for the
 * MTU exchange (and the replies after it, in order). One that finds no room
 * is counted (ctlq.dropped, on the console's status line). */
static void ctlq_reset(mg_ctlq_t *q)
{
    q->used = 0;
    q->n = 0;
}

static size_t ctlq_head_len(const mg_ctlq_t *q)
{
    return (size_t)q->buf[0] | (size_t)q->buf[1] << 8;
}

static bool ctlq_flush(mg_proto_t *p)
{
    mg_ctlq_t *q = &p->ctlq;
    if (!q->n) {
        return true;
    }
    if (!p->connected) {
        ctlq_reset(q);
        return true;
    }
    if (!proof_flush(p)) {
        return false;
    }
    while (q->n) {
        size_t len = ctlq_head_len(q);
        if (len > link_max(p)) {
            return false;   /* waits whole for the MTU exchange */
        }
        if (!raw(p, MG_CH_CONTROL, q->buf + 2, len)) {
            return false;
        }
        p->ctl_sent++;
        q->used = (uint16_t)(q->used - (2 + len));
        memmove(q->buf, q->buf + 2 + len, q->used);
        q->n--;
    }
    return true;
}

static bool send_control(mg_proto_t *p, const uint8_t *d, size_t n)
{
    mg_ctlq_t *q = &p->ctlq;
    if (!p->connected) {
        return false;
    }
    bool fits = n <= link_max(p);
    if (fits && ctlq_flush(p) && send_control_now(p, d, n)) {
        return true;
    }
    if (q->n >= MG_CTLQ_MAX || (size_t)q->used + 2 + n > sizeof(q->buf)) {
        q->dropped++;   /* on the console's status line (never expected) */
        return false;
    }
    if (!fits) {
        q->held_mtu++;
    }
    q->buf[q->used] = (uint8_t)n;
    q->buf[q->used + 1] = (uint8_t)(n >> 8);
    memcpy(q->buf + q->used + 2, d, n);
    q->used = (uint16_t)(q->used + 2 + n);
    q->n++;
    if (q->n > q->most) {
        q->most = q->n;
    }
    if (q->used > q->most_bytes) {
        q->most_bytes = q->used;
    }
    return true;   /* it goes, whole and in order, as the stack (and the link) takes it */
}

void mg_proto_reply_stats(const mg_proto_t *p, mg_reply_stats_t *out)
{
    *out = (mg_reply_stats_t){
        .dropped = p->ctlq.dropped,
        .held_mtu = p->ctlq.held_mtu,
        .link_refused = p->link_refused,
        .cmd_direct = p->cmdq.direct,
        .capture_lost = p->capq.ctl_lost,
        .session_dropped = p->session.ctl_dropped,
        .touch_dropped = p->touch_dropped,
        .waiting = p->ctlq.n,
        .most = p->ctlq.most,
    };
}

int mg_reply_stats_format(const mg_reply_stats_t *s, bool json, char *out, size_t cap)
{
    const char *f = json ? "{\"dropped\":%lu,\"held_mtu\":%lu,\"link_refused\":%lu,\"cmd_direct\":%lu,"
                           "\"capture_lost\":%lu,\"session_dropped\":%lu,\"touch_dropped\":%lu,\"nus_dropped\":%lu,"
                           "\"host_closed\":%lu,"
                           "\"waiting\":%u,\"most\":%u}"
                         : "dropped=%lu held_mtu=%lu link_refused=%lu cmd_direct=%lu capture_lost=%lu "
                           "session_dropped=%lu touch_dropped=%lu nus_dropped=%lu host_closed=%lu waiting=%u most=%u";
    return snprintf(out, cap, f, (unsigned long)s->dropped, (unsigned long)s->held_mtu,
                    (unsigned long)s->link_refused, (unsigned long)s->cmd_direct, (unsigned long)s->capture_lost,
                    (unsigned long)s->session_dropped, (unsigned long)s->touch_dropped, (unsigned long)s->nus_dropped,
                    (unsigned long)s->host_closed, (unsigned)s->waiting, (unsigned)s->most);
}

void mg_proto_control_sent(mg_proto_t *p)
{
    if (p->ctl_done != p->ctl_sent) {
        p->ctl_done++;
    }
}

static bool send_data(mg_proto_t *p, const uint8_t *d, size_t n)
{
    return raw(p, MG_CH_DATA, d, n);
}

static void send_error(mg_proto_t *p, uint8_t cmd, uint16_t code)
{
    uint8_t m[5] = { mg_command_error, cmd, mg_error_data_code, (uint8_t)code, (uint8_t)(code >> 8) };
    _Static_assert(sizeof(m) <= 20, "an error fits even before the MTU exchange (ATT_MTU 23)");
    send_control(p, m, sizeof(m));
}

/* A codec captures at a rate here (24 kHz: LC3, where the mic delivers it). */
static bool capture_rate_listed(const mg_proto_t *p, uint8_t codec, uint16_t rate)
{
    uint16_t lo, hi;
    if (!mg_codec_kbps_range(codec, rate, &lo, &hi)) {
        return false;
    }
    return rate == MG_CODEC_RATE || p->cfg.capture_24k;
}

static void send_data_type(mg_proto_t *p)
{
    uint8_t m[10];   /* mg_codec_label: 9 */
    _Static_assert(sizeof(m) <= MG_CTL_REPLY_MAX && sizeof(m) <= MG_CAPQ_CTL_BYTES, "change_data_type fits");
    /* An audio capture's label is its own format (rate, frame bytes, duration). A held turn's
     * isn't announced yet (mg_proto_turn_hold, or a held live turn): the type from before it. */
    bool held = p->capq.gate || p->capq.held_turn;
    /* ...nor one that began unproven, until the proof (not even its format). */
    bool pre = !held && p->pre_set && !out_ok(p);
    uint8_t type = held ? p->gate_data_type : pre ? p->pre_data_type : p->data_type;
    const mg_audio_format_t *fmt = held ? &p->gate_data_fmt : pre ? &p->pre_data_fmt : &p->data_fmt;
    size_t n = fmt->type && fmt->type == type ? mg_codec_label(fmt, m) : mg_codec_change_data_type(type, m);
    send_control(p, m, n);
}

/* ---- a capture's level (mg_capture_meter_t, mg_proto.h) ------------------ */

void mg_capture_meter_set_ring(mg_capture_meter_t *m, uint32_t *ring)
{
    m->loud.e = ring;
}

void mg_capture_meter_reset(mg_capture_meter_t *m)
{
    uint32_t *ring = m->loud.e;
    memset(m, 0, sizeof(*m));
    m->floor_acc = UINT64_MAX;
    mg_loud1s_reset(&m->loud, ring);
}

void mg_capture_meter_add(mg_capture_meter_t *m, const int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int32_t v = pcm[i];
        int32_t a = v < 0 ? -v : v;
        uint64_t sq = (uint64_t)((int64_t)v * v);
        m->peak = a > m->peak ? a : m->peak;
        m->clipped += a >= MG_METER_CLIP;
        m->acc += sq;
        m->win_acc += sq;
        /* The floor's 20 ms window is two of the loudest second's 10 ms
         * frames: each frame's energy from the sums already kept. */
        if (++m->win_n == MG_METER_FRAME) {
            m->win_half = m->win_acc;
            mg_loud1s_push(&m->loud, (uint32_t)(m->win_half / MG_METER_FRAME));
        } else if (m->win_n == MG_METER_WINDOW) {
            mg_loud1s_push(&m->loud, (uint32_t)((m->win_acc - m->win_half) / MG_METER_FRAME));
            m->floor_acc = m->win_acc < m->floor_acc ? m->win_acc : m->floor_acc;
            m->win_acc = 0;
            m->win_n = 0;
        }
    }
    m->frames += (uint32_t)n;
}

static float meter_db(double mean_square)
{
    float db = (float)(10.0 * log10(mean_square / (32768.0 * 32768.0) + 1e-10));
    return db < -100.0f ? -100.0f : db;
}

void mg_capture_meter_level(const mg_capture_meter_t *m, uint32_t rate_hz, mg_capture_level_t *out)
{
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->ms = rate_hz ? (uint32_t)((uint64_t)m->frames * 1000u / rate_hz) : 0;
    out->rms_db = m->frames ? meter_db((double)m->acc / m->frames) : -100.0f;
    out->peak_db = (float)(20.0 * log10((m->peak + 1) / 32768.0));
    out->floor_db = m->floor_acc != UINT64_MAX ? meter_db((double)m->floor_acc / MG_METER_WINDOW) : out->rms_db;
    out->loud_valid = mg_loud1s_db(&m->loud, &out->loud1s_db);
    out->clipped = m->clipped;
}

/* ---- client commands held while replies wait (mg_cmdq_t, mg_proto.h) ------ */

static void command(mg_proto_t *p, const uint8_t *d, size_t n);

/* Commands whose answers don't go through the reply outbox: never held. */
static bool cmd_never_held(uint8_t cmd)
{
    return cmd == mg_command_token_proof || cmd == mg_command_session;
}

static void cmdq_reset(mg_proto_t *p)
{
    p->cmdq.used = 0;
    p->cmdq.n = 0;
}

/* Runs the commands that waited, in order, while the reply outbox is empty. */
static void cmdq_run(mg_proto_t *p)
{
    mg_cmdq_t *q = &p->cmdq;
    if (q->running) {
        return;
    }
    q->running = true;
    while (q->n && !p->ctlq.n && p->connected) {
        uint8_t m[MG_CMDQ_BYTES];
        size_t len = q->buf[0];
        memcpy(m, q->buf + 1, len);
        q->used = (uint16_t)(q->used - (1 + len));
        memmove(q->buf, q->buf + 1 + len, q->used);
        q->n--;
        command(p, m, len);
    }
    q->running = false;
}

/* A client's Control command: now, or after the replies already waiting. */
static void command_in_order(mg_proto_t *p, const uint8_t *d, size_t n)
{
    mg_cmdq_t *q = &p->cmdq;
    if (!n || cmd_never_held(d[0]) || (!q->n && !p->ctlq.n)) {
        command(p, d, n);
        return;
    }
    if (n > 0xFF || q->used + 1 + n > sizeof(q->buf)) {
        q->direct++;
        command(p, d, n);   /* best effort: never expected */
        return;
    }
    q->buf[q->used] = (uint8_t)n;
    memcpy(q->buf + q->used + 1, d, n);
    q->used = (uint16_t)(q->used + 1 + n);
    q->n++;
    if (q->n > q->most) {
        q->most = q->n;
    }
}

/* ---- the capture's outbox (mg_capq_t, mg_proto.h) ------------------------ */

#define MG_DATA_PAYLOAD_MAX 256   /* the largest Data notification (mg_proto_data_payload) */

static uint32_t now_ms(const mg_proto_t *p);

bool mg_proto_capture_pending(const mg_proto_t *p)
{
    return p->capq.nctl || p->capq.wseq != p->capq.rseq;
}

uint32_t mg_proto_capture_held(const mg_proto_t *p)
{
    return p->capq.wseq - p->capq.rseq;
}

/* Gives the buffer back once nothing is held and no capture is open. */
static void capq_release(mg_proto_t *p, bool force)
{
    mg_capq_t *q = &p->capq;
    if (!force && (p->live || mg_proto_capture_pending(p))) {
        return;
    }
    if (q->owned && q->buf && p->ops.capq_free) {
        p->ops.capq_free(p->ops.ctx, q->buf);
    }
    q->buf = NULL;
    q->cap = 0;
    q->owned = false;
    q->rpos = 0;
    q->rseq = q->wseq;
}

/* The link went: whatever was held goes with it (nobody to send it to). */
static void capq_reset(mg_proto_t *p)
{
    mg_capq_t *q = &p->capq;
    q->nctl = 0;
    q->nlevels = 0;
    memset(&q->next_level, 0, sizeof(q->next_level));
    q->utt_sent = q->utt_dropped = q->utt_held_max = 0;
    q->gate = false;   /* a held turn goes with the link, unsent (mg_proto_turn_drop then finds none) */
    q->proof_full = false;   /* ...and a capture held for the proof: discarded, never sent */
    q->held_turn = q->ht_spill = q->ht_voice = false;   /* ...and a held live turn */
    q->stale_armed = false;
    capq_release(p, true);
}

/* A buffer for a capture at fmt: CONFIG_MUSE_GADGET_BLE_CAPTURE_HOLD_S of it,
 * or what the platform gives, else the built-in one. */
/* A held turn's buffer (mg_proto_turn_hold) must hold the whole turn: one
 * smaller (an earlier utterance's, still draining) is replaced by a bigger one
 * with what it holds copied over, in order; kept if none is given. */
static void capq_grow(mg_proto_t *p, size_t want)
{
    mg_capq_t *q = &p->capq;
    size_t got = 0;
    size_t held = q->wseq - q->rseq;
    void *b = p->ops.capq_alloc ? p->ops.capq_alloc(p->ops.ctx, want, &got) : NULL;
    if (!b || got <= q->cap || got < held) {
        if (b && p->ops.capq_free) {
            p->ops.capq_free(p->ops.ctx, b);
        }
        return;
    }
    size_t first = held < q->cap - q->rpos ? held : q->cap - q->rpos;
    memcpy(b, q->buf + q->rpos, first);
    memcpy((uint8_t *)b + first, q->buf, held - first);
    if (q->owned && p->ops.capq_free) {
        p->ops.capq_free(p->ops.ctx, q->buf);
    }
    q->buf = b;
    q->cap = got;
    q->owned = true;
    q->rpos = 0;
}

static void capq_ensure(mg_proto_t *p, const mg_audio_format_t *fmt)
{
    mg_capq_t *q = &p->capq;
    size_t per_s = fmt->frame_us ? (size_t)fmt->frame_bytes * (1000000u / fmt->frame_us) : 0;
    /* A turn held whole (the bench guard's, or one held for the token proof) takes the guard's size. */
    bool whole = mg_proto_capture_whole(p);
    size_t want = per_s * (whole && MG_CAPTURE_GUARD_S > MG_CAPTURE_HOLD_S ? MG_CAPTURE_GUARD_S : MG_CAPTURE_HOLD_S);
    if (q->buf) {
        if (whole && q->cap < want) {
            capq_grow(p, want);
        }
        return;
    }
    size_t got = 0;
    void *b = want && p->ops.capq_alloc ? p->ops.capq_alloc(p->ops.ctx, want, &got) : NULL;
    if (b && got >= fmt->frame_bytes) {
        q->buf = b;
        q->cap = got;
        q->owned = true;
    } else {
        if (b && p->ops.capq_free) {
            p->ops.capq_free(p->ops.ctx, b);
        }
        q->buf = q->fallback;
        q->cap = sizeof(q->fallback);
        q->owned = false;
    }
    q->rpos = 0;
    q->rseq = q->wseq;
}

/* Control messages at or before the read position whose frame size the
 * dropped audio has now reached. */
static void capq_pass(mg_capq_t *q)
{
    for (uint8_t i = 0; i < q->nctl; i++) {
        mg_capq_ctl_t *c = &q->ctl[i];
        if ((int32_t)(c->at - q->rseq) > 0) {
            break;
        }
        if (!c->passed) {
            c->passed = true;
            if (c->fb) {
                q->rfb = c->fb;
            }
        }
    }
}

/* Drops the oldest whole frame held (overflow). */
static bool capq_drop_oldest(mg_capq_t *q)
{
    uint32_t held = q->wseq - q->rseq;
    if (!held || !q->cap) {
        return false;
    }
    capq_pass(q);
    uint32_t f = q->rfb ? q->rfb : 1;
    if (f > held) {
        f = held;
    }
    q->rseq += f;
    q->rpos = (q->rpos + f) % q->cap;
    q->utt_dropped++;
    capq_pass(q);
    return true;
}

static bool capq_push(mg_proto_t *p, const uint8_t *d, size_t n)
{
    mg_capq_t *q = &p->capq;
    if (!q->buf || !n) {
        return true;
    }
    if (!out_ok(p) || q->held_turn) {
        /* Held for the token proof (or a held live turn), every frame of it: what doesn't fit is
         * refused (the capture stops) and what's held is kept, never the oldest dropped. */
        if (q->proof_full || n > q->cap - (q->wseq - q->rseq)) {
            if (!q->proof_full) {
                q->proof_full = true;
                q->proof_fills++;
            }
            return false;
        }
    }
    if (n > q->cap) {
        /* Never more than the buffer: keep the newest whole frames. */
        size_t fb = q->wfb ? q->wfb : 1;
        size_t keep = q->cap / fb * fb;
        q->utt_dropped += (uint32_t)((n - keep) / fb);
        d += n - keep;
        n = keep;
    }
    while (q->cap - (q->wseq - q->rseq) < n && capq_drop_oldest(q)) {
    }
    size_t w = (q->rpos + (q->wseq - q->rseq)) % q->cap;
    size_t first = n < q->cap - w ? n : q->cap - w;
    memcpy(q->buf + w, d, first);
    memcpy(q->buf, d + first, n - first);
    q->wseq += (uint32_t)n;
    return true;
}

/* A chunk that wraps the ring's end, through a copy (the voice task's stack
 * is small: the copy's buffer only when it wraps, once a lap). */
static __attribute__((noinline)) bool capq_send_wrapped(mg_proto_t *p, size_t n)
{
    mg_capq_t *q = &p->capq;
    uint8_t tmp[MG_DATA_PAYLOAD_MAX];
    size_t first = q->cap - q->rpos;
    memcpy(tmp, q->buf + q->rpos, first);
    memcpy(tmp + first, q->buf, n - first);
    return send_data(p, tmp, n);
}

void mg_proto_capture_level(mg_proto_t *p, const mg_capture_level_t *lv)
{
    p->capq.next_level = *lv;
}

int mg_proto_format_utterance(const mg_proto_t *p, char *out, size_t cap)
{
    const mg_capq_t *q = &p->capq;
    uint32_t held_ms = q->end_fb && q->end_frame_us
                           ? (uint32_t)((uint64_t)q->end_held_max / q->end_fb * q->end_frame_us / 1000) : 0;
    const mg_capture_level_t *lv = &q->end_level;
    int n = snprintf(out, cap, "utterance over BLE: %.1f s, %u bytes, %u frames dropped", (double)lv->ms / 1000.0,
                     (unsigned)q->end_sent_bytes, (unsigned)q->end_dropped);
    if (n >= 0 && (size_t)n < cap && lv->valid) {
        n += snprintf(out + n, cap - (size_t)n, ", rms %.1f dBFS, peak %.1f dBFS, floor %.1f dBFS",
                      (double)lv->rms_db, (double)lv->peak_db, (double)lv->floor_db);
        if (n >= 0 && (size_t)n < cap && lv->loud_valid) {
            n += snprintf(out + n, cap - (size_t)n, ", loud1s %.1f dBFS", (double)lv->loud1s_db);
        }
        if (n >= 0 && (size_t)n < cap) {
            n += snprintf(out + n, cap - (size_t)n, ", %u clipped", (unsigned)lv->clipped);
        }
    }
    if (n >= 0 && (size_t)n < cap) {
        n += snprintf(out + n, cap - (size_t)n, "; end sent %u ms after the capture ended, up to %u ms held for the link",
                      (unsigned)q->end_wait_ms, (unsigned)held_ms);
    }
    return n;
}

/* worker: the worker task's own pump (mg_proto_capture_pump), the only one that runs the client commands that
 * waited (mg_cmdq_t). Every other caller (the voice task's audio slices, a command's own answer) only sends:
 * a waiting command can write flash (set_settings, a time zone), which a task with its stack in PSRAM must
 * never do, and the voice task's is. */
static bool capture_pump(mg_proto_t *p, bool worker)
{
    mg_capq_t *q = &p->capq;
    if (!p->connected) {
        return false;
    }
    if (!proof_flush(p)) {
        return true;   /* the token proof answer first; the capture waits behind it */
    }
    bool replies = !ctlq_flush(p);   /* replies the stack refused: in order, as it takes them */
    if (!replies) {
        if (worker) {
            cmdq_run(p);             /* the commands that waited for them */
        }
        replies = p->ctlq.n || p->cmdq.n;   /* (left for the worker: it keeps looking) */
    }
    if (!replies) {
        mg_proto_speaker_pump(p);    /* an owed speaker_volume report, after the replies */
        int v;
        replies = speaker_due(p, &v);   /* still owed: the worker keeps looking */
    }
    /* Nothing of a capture goes to a central that hasn't passed this connection's token proof
     * (held whole until it matches, discarded if it fails or the link goes). The worker isn't kept
     * looking for it: the match pumps it. */
    output_stale(p);   /* held output past its staleness bound goes, unsent, before anything releases it */
    if (q->held_turn && held_turn_ready(p)) {
        held_turn_release(p);   /* proven and ready: the held live turn goes live */
    }
    bool hold = !out_ok(p);
    while (!hold) {
        if (q->nctl && (int32_t)(q->ctl[0].at - q->rseq) <= 0) {
            mg_capq_ctl_t *c = &q->ctl[0];
            if (c->held) {
                break;   /* a held turn's (mg_proto_turn_hold): nothing of it goes until it's released */
            }
            if (!send_control_now(p, c->msg, c->len)) {
                break;   /* the stack is full: the next pump tries again */
            }
            if (c->fb && !c->passed) {
                q->rfb = c->fb;
            }
            if (c->end) {
                q->ends_sent++;
                q->end_wait_ms = now_ms(p) - c->queued_ms;
                q->end_sent_bytes = q->utt_sent;
                q->end_dropped = q->utt_dropped;
                q->end_held_max = q->utt_held_max;
                q->end_fb = q->rfb;
                q->end_frame_us = p->data_fmt.frame_us;
                if (q->nlevels) {
                    q->end_level = q->levels[0];
                    q->nlevels--;
                    memmove(&q->levels[0], &q->levels[1], q->nlevels * sizeof(q->levels[0]));
                } else {
                    memset(&q->end_level, 0, sizeof(q->end_level));
                }
                q->utt_sent = q->utt_dropped = 0;
                q->utt_held_max = q->wseq - q->rseq;
            }
            q->nctl--;
            memmove(&q->ctl[0], &q->ctl[1], q->nctl * sizeof(q->ctl[0]));
            continue;
        }
        uint32_t avail = q->wseq - q->rseq;
        if (q->nctl && q->ctl[0].at - q->rseq < avail) {
            avail = q->ctl[0].at - q->rseq;
        }
        if (q->gate || q->held_turn) {
            /* A held turn's audio waits too, whatever its control messages did. */
            int32_t before = (int32_t)((q->gate ? q->gate_at : q->ht_at) - q->rseq);
            avail = before <= 0 ? 0 : (uint32_t)before < avail ? (uint32_t)before : avail;
        }
        if (!avail || !q->cap) {
            break;
        }
        size_t fb = q->rfb ? q->rfb : 1;
        size_t payload = mg_proto_data_payload(p);
        size_t chunk = payload / fb * fb;
        if (!chunk) {
            /* MTU below MG_MIN_ATT_MTU: no frame fits a notification. */
            while (q->wseq != q->rseq && (!q->nctl || q->ctl[0].at != q->rseq) && capq_drop_oldest(q)) {
            }
            continue;
        }
        if (p->ops.data_room && !p->ops.data_room(p->ops.ctx)) {
            break;   /* keep the stack's last buffers for control messages */
        }
        size_t n = avail < chunk ? avail : chunk / fb * fb;   /* whole frames, or all that's left */
        bool sent = q->cap - q->rpos >= n ? send_data(p, q->buf + q->rpos, n) : capq_send_wrapped(p, n);
        if (!sent) {
            break;
        }
        q->rseq += (uint32_t)n;
        q->rpos = (q->rpos + n) % q->cap;
        q->utt_sent += (uint32_t)n;
    }
    uint32_t held = q->wseq - q->rseq;   /* what the link couldn't take yet */
    if (held > q->utt_held_max) {
        q->utt_held_max = held;
    }
    capq_release(p, false);
    /* A held live turn waiting for its client isn't the worker's 5 ms rounds (it looks every 20 ms). */
    return replies || (!hold && !q->held_turn && mg_proto_capture_pending(p));
}

bool mg_proto_capture_pump(mg_proto_t *p)
{
    return capture_pump(p, true);
}

/* Queues a control message at the audio's current end, then sends what the
 * link takes. fb: the frame size of the audio after it (0: unchanged). */
static bool capq_control(mg_proto_t *p, const uint8_t *m, size_t n, uint16_t fb, bool end)
{
    mg_capq_t *q = &p->capq;
    _Static_assert(MG_CAPQ_CTL_BYTES >= 9, "change_data_type's label (9), a gesture (2) and stop_mic (1) fit a slot");
    if (n > MG_CAPQ_CTL_BYTES) {
        bool ok = send_control_now(p, m, n);   /* never: every capture control fits a slot */
        q->ctl_lost += !ok;
        return ok;
    }
    if (q->nctl == MG_CAPQ_CTL_MAX) {
        capture_pump(p, false);
        if (q->nctl == MG_CAPQ_CTL_MAX) {
            q->ctl_lost++;
            /* best effort: never expected (twelve waiting); a held turn's never goes ahead of its release */
            return !q->gate && !q->held_turn && out_ok(p) && send_control_now(p, m, n);   /* nor one held for the proof */
        }
    }
    mg_capq_ctl_t *c = &q->ctl[q->nctl++];
    memset(c, 0, sizeof(*c));
    c->held = q->gate || q->held_turn;
    c->at = q->wseq;
    c->queued_ms = now_ms(p);
    c->fb = fb;
    c->len = (uint8_t)n;
    c->end = end;
    memcpy(c->msg, m, n);
    if (end) {
        /* Its level, in order with the ends before it (three turns at most wait). */
        if (q->nlevels == sizeof(q->levels) / sizeof(q->levels[0])) {
            memmove(&q->levels[0], &q->levels[1], (q->nlevels - 1) * sizeof(q->levels[0]));
            q->nlevels--;
        }
        q->levels[q->nlevels++] = q->next_level;
        memset(&q->next_level, 0, sizeof(q->next_level));
    }
    if (fb) {
        q->wfb = fb;
    }
    capture_pump(p, false);
    return true;
}

/* power_off is offered where the board can power off now. */
static bool power_off_offered(const mg_proto_t *p)
{
    return p->ops.power_off && (!p->ops.power_off_ready || p->ops.power_off_ready(p->ops.ctx));
}

static void send_features(mg_proto_t *p)
{
    /* Every list here is built into its own maximum's room (mg_proto.h,
     * MG_REPLY_*_MAX), each at most one notification at the MTU floor. */
    _Static_assert(MG_REPLY_FEATURES_MAX <= MG_CTL_REPLY_MAX && MG_REPLY_START_MIC_MAX <= MG_CTL_REPLY_MAX
                       && MG_REPLY_FORMATS_MAX <= MG_CTL_REPLY_MAX && MG_REPLY_SETTINGS_MAX <= MG_CTL_REPLY_MAX
                       && MG_REPLY_ACTIONS_MAX <= MG_CTL_REPLY_MAX && MG_REPLY_UI_INPUT_MAX <= MG_CTL_REPLY_MAX,
                   "request_status' lines fit one notification at MG_MIN_ATT_MTU");
    _Static_assert(MG_REPLY_FEATURES_MAX <= 20, "the command list fits even before the MTU exchange");
    uint8_t m[MG_REPLY_FEATURES_MAX];
    size_t n = 0;
    m[n++] = mg_command_supported_features;
    /* Commands the device accepts; ones it only notifies (change_data_type, gesture, error) aren't listed. */
    static const uint8_t base[] = {
        mg_command_start_mic, mg_command_stop_mic, mg_command_set_mic_gain, mg_command_request_status,
        mg_command_get_settings, mg_command_set_settings,
    };
    _Static_assert(1 + sizeof(base) + 8 <= sizeof(m), "the command list: six always, eight optional");
    memcpy(m + n, base, sizeof(base));
    n += sizeof(base);
    if (p->cfg.play) {
        m[n++] = mg_command_stream_audio;
    }
    if (p->cfg.queue) {
        m[n++] = mg_command_audio_queue;
    }
    if (p->cfg.assistant) {
        m[n++] = mg_command_assistant_state;
    }
    if (proof_supported(p)) {
        m[n++] = mg_command_token_proof;
    }
    m[n++] = mg_command_device_action;
    if (p->cfg.video && p->cfg.vstream) {
        m[n++] = mg_command_stream_video;
    }
    if (p->cfg.ui_input) {
        m[n++] = mg_command_ui_input;
    }
    if (session_supported(p) || creds_supported(p)) {
        m[n++] = mg_command_session;
    }
    send_control(p, m, n);

    /* What the device session can do, right after the command list: sealed
     * credentials (no BLE bond needed), and client_refreshes when this
     * gadget holds no Wi-Fi to refresh its own pair. */
    if (session_supported(p) || creds_supported(p)) {
        /* client_refreshes whenever the gadget holds no Wi-Fi credentials (it
         * can't refresh its own pair), asked now: a Wi-Fi gadget set up
         * token-only has none until Wi-Fi is added. */
        bool refreshes = p->ops.client_refreshes ? p->ops.client_refreshes(p->ops.ctx)
                                                 : session_supported(p) && p->cfg.session->client_refreshes;
        /* credentials_only: the Wi-Fi gadgets' credentials-only session
         * (portable layer 2, MGDS_CAPS) has no VM session over BLE, so the apps
         * never open one or send state; the BLE-only session carries one. */
        bool creds_only = creds_supported(p) && !session_supported(p);
        uint8_t caps[4] = { mg_command_supported_features, mg_command_sub_feature, mg_command_session,
                            (uint8_t)(mg_session_cap_sealed | (refreshes ? mg_session_cap_client_refreshes : 0)
                                      | (creds_only ? mg_session_cap_credentials_only : 0)
                                      | (sealed_clear_declared(p) ? mg_session_cap_sealed_clear : 0)) };
        send_control(p, caps, sizeof(caps));
    }

    {
        uint8_t c[MG_REPLY_START_MIC_MAX];
        size_t k = 0;
        c[k++] = mg_command_supported_features;
        c[k++] = mg_command_sub_feature;
        c[k++] = mg_command_start_mic;
        k += mg_codec_list(c + k, sizeof(c) - k);
        send_control(p, c, k);
    }

    /* The formats each codec captures, a typed
     * list owned by change_data_type: (codec, count, count x (rate, min kb/s,
     * max kb/s)), 16000 first. */
    {
        uint8_t c[MG_REPLY_FORMATS_MAX];
        size_t k = 0;
        c[k++] = mg_command_supported_features;
        c[k++] = mg_command_sub_feature;
        c[k++] = mg_command_change_data_type;
        uint8_t codecs[MG_CAPTURE_CODECS_MAX];
        size_t nc = mg_codec_list(codecs, sizeof(codecs));
        static const uint16_t RATES[] = { MG_CODEC_RATE, MG_CODEC_RATE_HIGH };
        _Static_assert(sizeof(RATES) / sizeof(RATES[0]) == MG_CAPTURE_RATES_MAX, "MG_CAPTURE_RATES_MAX");
        for (size_t i = 0; i < nc; i++) {
            size_t at = k;
            c[k++] = codecs[i];
            c[k++] = 0;
            for (size_t r = 0; r < MG_CAPTURE_RATES_MAX; r++) {
                uint16_t lo, hi;
                if (!capture_rate_listed(p, codecs[i], RATES[r]) || !mg_codec_kbps_range(codecs[i], RATES[r], &lo, &hi)) {
                    continue;
                }
                put16(c + k, RATES[r]);
                put16(c + k + 2, lo);
                put16(c + k + 4, hi);
                k += 6;
                c[at + 1]++;
            }
        }
        send_control(p, c, k);
    }

    /* Every setting get_settings and set_settings accept (none past
     * MG_SETTING_PARAM_LAST: test_reply_sizes checks, so the list has room
     * for all of them). */
    {
        uint8_t c[MG_REPLY_SETTINGS_MAX];
        size_t k = 0;
        c[k++] = mg_command_supported_features;
        c[k++] = mg_command_sub_feature;
        c[k++] = mg_command_set_settings;
        for (unsigned s = 0; s <= MG_SETTING_PARAM_LAST; s++) {
            if (setting_offered(p, (uint8_t)s)) {
                c[k++] = (uint8_t)s;
            }
        }
        send_control(p, c, k);
    }

    /* Device actions: every one the device implements, the
     * device-to-client answers with their requests. */
    _Static_assert(MG_REPLY_ACTIONS_MAX <= sizeof(m) && MG_REPLY_UI_INPUT_MAX <= sizeof(m), "room in m");
    n = 0;
    m[n++] = mg_command_supported_features;
    m[n++] = mg_command_sub_feature;
    m[n++] = mg_command_device_action;
    if (p->ops.battery_status) {
        m[n++] = mg_device_action_request_battery_status;
        m[n++] = mg_device_action_report_battery_status;
    }
    if (power_off_offered(p)) {
        m[n++] = mg_device_action_power_off;
    }
    m[n++] = mg_device_action_throughput_test;
    if (p->ops.clock_set && p->ops.clock_get) {
        m[n++] = mg_device_action_set_unix_time;
        m[n++] = mg_device_action_get_unix_time;
        m[n++] = mg_device_action_report_unix_time;
    }
    if (haptic_offered(p)) {
        m[n++] = mg_device_action_haptic_play;
        m[n++] = mg_device_action_haptic_stop;
    }
    send_control(p, m, n);

    /* UI input: what of it the device offers (touch events, the HID modes). */
    if (p->cfg.ui_input) {
        n = 0;
        m[n++] = mg_command_supported_features;
        m[n++] = mg_command_sub_feature;
        m[n++] = mg_command_ui_input;
        for (unsigned f = 1; f < 8; f++) {   /* cfg.ui_input's bits 1..7: mg_ui_input_feature_t */
            if (p->cfg.ui_input & (1u << f)) {
                m[n++] = (uint8_t)f;
            }
        }
        send_control(p, m, n);
    }
}

/* ---- device session: mg_session's ops ------------------------------------ */

static bool session_notify(void *ctx, const uint8_t *d, size_t n)
{
    return send_control_now(ctx, d, n);
}

static bool session_credentials(void *ctx, mg_session_creds_t *c)
{
    mg_proto_t *p = ctx;
    return p->ops.session_credentials(p->ops.ctx, c);
}

static bool session_store_tokens(void *ctx, const char *access_token, const char *refresh_token, uint64_t expires)
{
    mg_proto_t *p = ctx;
    return p->ops.session_store_tokens(p->ops.ctx, access_token, refresh_token, expires);
}

static void session_state(void *ctx, uint8_t state, uint16_t epoch)
{
    mg_proto_t *p = ctx;
    p->ops.session_state(p->ops.ctx, state, epoch);
}

static void session_message(void *ctx)
{
    mg_proto_t *p = ctx;
    p->ops.session_message(p->ops.ctx);
}

static void session_data_refused(void *ctx, const char *why, uint32_t len)
{
    mg_proto_t *p = ctx;
    if (p->ops.session_data_refused) {
        p->ops.session_data_refused(p->ops.ctx, why, len);
    }
}

typedef enum { CLEAR_DONE, CLEAR_PENDING, CLEAR_DROPPED } clear_run_t;

static clear_run_t clear_erase(mg_proto_t *p);

static int session_clear(void *ctx)
{
    /* A board whose erase runs elsewhere answers from its result (mg_proto_clear_done()). */
    clear_run_t r = clear_erase(ctx);
    return r == CLEAR_PENDING ? MG_SESSION_CLEAR_PENDING : r == CLEAR_DONE;   /* proof_clear has no failure to report */
}

static bool session_busy(void *ctx)
{
    mg_proto_t *p = ctx;
    return p->ops.session_busy && p->ops.session_busy(p->ops.ctx);
}

static bool session_take_nonces(void *ctx, uint8_t client_nonce[MG_TOKEN_PROOF_NONCE_LEN],
                                uint8_t device_nonce[MG_TOKEN_PROOF_NONCE_LEN])
{
    return mg_proto_proof_take_nonces(ctx, client_nonce, device_nonce);
}

static bool session_proof_key(void *ctx, uint8_t k[MG_TOKEN_PROOF_KEY_LEN])
{
    mg_proto_t *p = ctx;
    return p->ops.proof_key(p->ops.ctx, k);
}

/* ---- the credentials-only device session (portable layer 2's port) ------- */

/* The credential while layer 2 reads it (a hello's tag, then each
 * fragment): read once from Link's config, wiped and freed once nothing is
 * owed. */
struct mg_creds_cache {
    mg_session_creds_t c;
    uint16_t len[2];   /* access, refresh */
};

static void creds_drop_cache(mg_proto_t *p)
{
    if (p->creds_cache) {
        mg_token_proof_wipe(p->creds_cache, sizeof(*p->creds_cache));
        free(p->creds_cache);
        p->creds_cache = NULL;
    }
}

static mg_creds_cache_t *creds_load(mg_proto_t *p)
{
    if (!p->creds_cache) {
        mg_creds_cache_t *c = calloc(1, sizeof(*c));
        if (!c) {
            return NULL;
        }
        size_t a = 0, r = 0;
        bool ok = p->ops.session_credentials(p->ops.ctx, &c->c) && (a = strlen(c->c.access_token)) > 0
                  && (r = strlen(c->c.refresh_token)) > 0 && a <= UINT16_MAX && r <= UINT16_MAX;
        if (!ok) {
            mg_token_proof_wipe(c, sizeof(*c));
            free(c);
            return NULL;
        }
        c->len[0] = (uint16_t)a;
        c->len[1] = (uint16_t)r;
        p->creds_cache = c;
    }
    return p->creds_cache;
}

static int creds_send(void *ctx, const uint8_t *frame, size_t len)
{
    mg_proto_t *p = ctx;
    if (!p->connected) {
        return -1;
    }
    return send_control_now(p, frame, len) ? MGDS_SENT : MGDS_AGAIN;
}

static uint16_t creds_mtu(void *ctx)
{
    return ((mg_proto_t *)ctx)->mtu;
}

static bool creds_recovery(void *ctx)
{
    return ((mg_proto_t *)ctx)->cfg.recovery_mode != 0;
}

static bool creds_proven(void *ctx)
{
    return mg_proto_proof_matched(ctx);
}

static bool creds_take_nonces(void *ctx, uint8_t client_nonce[16], uint8_t device_nonce[16])
{
    return mg_proto_proof_take_nonces(ctx, client_nonce, device_nonce);
}

static int creds_get_k(void *ctx, uint8_t k[32])
{
    mg_proto_t *p = ctx;
    mg_creds_cache_t *c = creds_load(p);
    if (!c) {
        return MGDS_NONE;   /* no pair */
    }
    if (c->c.proof_k_set) {
        memcpy(k, c->c.proof_k, 32);   /* read with the pair and node id: one setup's */
        return MGDS_OK;
    }
    return p->ops.proof_key(p->ops.ctx, k) ? MGDS_OK : MGDS_NONE;
}

static int creds_node_id(void *ctx, char node[MGDS_NODE_MAX], uint8_t *len)
{
    mg_creds_cache_t *c = creds_load(ctx);
    size_t n = c ? strlen(c->c.node_id) : 0;
    if (!n || n > MGDS_NODE_MAX) {
        return MGDS_FAIL;
    }
    memcpy(node, c->c.node_id, n);
    *len = (uint8_t)n;
    return MGDS_OK;
}

static int creds_token_len(void *ctx, int which, uint16_t *len)
{
    mg_creds_cache_t *c = creds_load(ctx);
    if (!c || which < 0 || which > 1) {
        return MGDS_FAIL;
    }
    *len = c->len[which];
    return MGDS_OK;
}

static int creds_token_read(void *ctx, int which, uint16_t off, uint8_t *buf, size_t n)
{
    mg_creds_cache_t *c = creds_load(ctx);
    if (!c || which < 0 || which > 1 || (size_t)off + n > c->len[which]) {
        return MGDS_FAIL;
    }
    memcpy(buf, (which ? c->c.refresh_token : c->c.access_token) + off, n);
    return MGDS_OK;
}

/* Layer 2 holds a hello (and an update's commit) while this is true, up to
 * MGDS_DEFER_MS: the gadget is being set up again. */
static bool creds_busy(void *ctx)
{
    mg_proto_t *p = ctx;
    return p->ops.session_busy && p->ops.session_busy(p->ops.ctx);
}

static uint8_t *creds_borrow(void *ctx, size_t need)
{
    mg_proto_t *p = ctx;
    if (p->creds_in || need > MGDS_SEALED_MAX) {
        return NULL;
    }
    p->creds_in = malloc(MGDS_SEALED_MAX);
    return p->creds_in;
}

static void creds_release(void *ctx)
{
    mg_proto_t *p = ctx;
    if (p->creds_in) {
        mg_token_proof_wipe(p->creds_in, MGDS_SEALED_MAX);
        free(p->creds_in);
        p->creds_in = NULL;
    }
}

/* The stored access_expires_at, handed back in the credentials (layer 2
 * puts it last): what the gadget last stored with its pair, the client's
 * write-back or its own refresh's. False (none): left out. */
static bool creds_expires(void *ctx, uint64_t *at)
{
    mg_creds_cache_t *c = creds_load(ctx);
    if (!c || !c->c.access_expires_at) {
        return false;
    }
    *at = c->c.access_expires_at;
    return true;
}

/* The refreshed pair and the update's access_expires_at (0: none, so none is
 * kept) in one commit (K and node_id unchanged): the board's
 * session_store_tokens, the same journalled replace the VM session uses. */
static int creds_commit_expires(void *ctx, const char *access, uint16_t access_len, const char *refresh,
                                uint16_t refresh_len, uint64_t expires)
{
    mg_proto_t *p = ctx;
    char *a = malloc((size_t)access_len + 1), *r = malloc((size_t)refresh_len + 1);
    bool ok = false;
    if (a && r) {
        memcpy(a, access, access_len);
        a[access_len] = '\0';
        memcpy(r, refresh, refresh_len);
        r[refresh_len] = '\0';
        ok = p->ops.session_store_tokens(p->ops.ctx, a, r, expires);
    }
    if (a) {
        mg_token_proof_wipe(a, (size_t)access_len + 1);
    }
    if (r) {
        mg_token_proof_wipe(r, (size_t)refresh_len + 1);
    }
    free(a);
    free(r);
    creds_drop_cache(p);   /* the next hello reads the new pair */
    return ok ? MGDS_OK : MGDS_FAIL;
}

static int creds_commit(void *ctx, const char *access, uint16_t access_len, const char *refresh, uint16_t refresh_len)
{
    return creds_commit_expires(ctx, access, access_len, refresh, refresh_len, 0);
}

/* The session ends (a connection, a new challenge, a setup change, a clear). */
static void creds_reset(mg_proto_t *p, bool link_up)
{
    if (p->creds) {
        mgds_reset(p->creds, link_up && p->connected);
        creds_drop_cache(p);
    }
}

static void output_discard(mg_proto_t *p);
static void output_drop_held(mg_proto_t *p);
static void streams_end_unproven(mg_proto_t *p, bool force);

/* The sealed clear's erase (mg_session_clear, both session kinds): the token proof's clear, without the
 * session's end (the session answers, then ends itself). PENDING: the board's erase runs elsewhere
 * (ops.proof_clear_async), and it calls mg_proto_clear_done() with its result. DROPPED: a new setup committed
 * since the clear was taken (the session resets for it): the clear was the old setup's, so nothing is erased. */
static clear_run_t clear_erase(mg_proto_t *p)
{
    if (p->setup_changing) {
        return CLEAR_DROPPED;
    }
    streams_end_unproven(p, true);   /* the token goes: streams it admitted end */
    proof_reset(p);
    bool pending = p->ops.proof_clear_async && p->ops.proof_clear_async(p->ops.ctx);
    if (!pending) {
        p->ops.proof_clear(p->ops.ctx);
    }
    output_discard(p);   /* the key is gone: a capture still to go never goes */
    return pending ? CLEAR_PENDING : CLEAR_DONE;
}

static int creds_clear(void *ctx)
{
    clear_run_t r = clear_erase(ctx);   /* pending: mg_proto_clear_done() answers */
    return r == CLEAR_PENDING ? MGDS_CLEAR_PENDING : r == CLEAR_DONE ? MGDS_OK : MGDS_FAIL;
}

static void creds_init(mg_proto_t *p)
{
    struct block {
        mgds_t t;
        mgds_port_t port;
    } *b = calloc(1, sizeof(*b));
    if (!b) {
        return;
    }
    b->port = (mgds_port_t){
        .ctx = p,
        /* The device takes no data (drops and credits it back): the
         * smallest hello the protocol allows. */
        .max_message = MG_SESSION_MIN_MESSAGE,
        .window = MG_SESSION_MIN_MESSAGE,
        .send = creds_send,
        .mtu = creds_mtu,
        .recovery = creds_recovery,
        .proven = creds_proven,
        .take_nonces = creds_take_nonces,
        .get_k = creds_get_k,
        .node_id = creds_node_id,
        .token_len = creds_token_len,
        .token_read = creds_token_read,
        .busy = creds_busy,
        .borrow = creds_borrow,
        .release = creds_release,
        .commit = creds_commit,
        .expires = creds_expires,
        .commit_expires = creds_commit_expires,
        .clear = creds_clear,
    };
    mgds_init(&b->t, &b->port);
    p->creds = &b->t;
    p->creds_port = &b->port;
}

/* ---- lifecycle ---------------------------------------------------------- */

static void reset_connection(mg_proto_t *p)
{
    /* A held live turn outlives the link, for the clip queue (asked before its format goes). */
    bool keep_held = p->capq.held_turn && MG_HELD_TURN_DROP_QUEUES && mg_proto_held_turn_queueable(p);
    p->tp.mode = 0;   /* a throughput test ends with the connection, unreported */
    p->tp.armed = false;
    p->stops_owed = 0;
    proof_reset(p);
    mg_token_proof_wipe(&p->proof_out, sizeof(p->proof_out));
    ctlq_reset(&p->ctlq);           /* replies for the last connection go with it */
    cmdq_reset(p);                   /* ...and the commands that waited for them */
    if (session_supported(p)) {
        mg_session_reset(&p->session);   /* the VM session lives on the connection */
    }
    creds_reset(p, false);
    memset(p->sub, 0, sizeof(p->sub));
    p->mtu = 23;
    p->ptt_enabled = false;
    p->ptt_held = false;
    p->ptt_asked = false;
    p->ptt_cleared_note = false;
    /* Who is connected (client_app, 19) goes with the connection. */
    if (p->client_app[0] || p->client_app[1 + p->client_app[0]]) {
        p->client_gen++;
    }
    memset(p->client_app, 0, sizeof(p->client_app));
    /* set_mic_gain is per connection (mgcommands.h): a level a client set
     * ends with it, and the next one starts at the device's own gain. */
    if (p->mic_gain) {
        p->mic_gain = 0;
        if (p->ops.mic_gain) {
            p->ops.mic_gain(p->ops.ctx, 0);
        }
    }
    p->haptics = false;
    p->start_mic_codec = 0;
    p->data_type = p->settings.codec;
    p->data_fmt.type = 0;   /* the codec's default label until a capture says otherwise */
    p->pre_set = false;
    p->live = false;
    if (keep_held) {
        held_turn_keep(p);   /* its audio alone, until mg_proto_held_turn_take */
    } else {
        capq_reset(p);   /* what a capture held goes with the link */
    }
    p->reports_state = false;
    p->transfer = false;
    p->rx_type = 0;
    p->flush_update_owed = false;
    p->framed = false;   /* framed Data ends at disconnect */
    p->touch_enabled = false;
    memset(p->ui_ctl, 0, sizeof(p->ui_ctl));
    if (p->video.active) {
        p->video.active = false;
        if (p->cfg.vstream) {
            muse_vstream_stop(p->cfg.vstream);
        }
        if (p->ops.video_end) {
            p->ops.video_end(p->ops.ctx);
        }
    }
}

static bool capture_rate_ok(const mg_proto_t *p, uint16_t rate);
static bool capture_kbps_ok(const mg_proto_t *p, uint16_t kbps);

void mg_proto_init(mg_proto_t *p, const mg_proto_config_t *cfg, const mg_proto_ops_t *ops,
                   const mg_settings_t *settings)
{
    memset(p, 0, sizeof(*p));
    p->cfg = *cfg;
    p->ops = *ops;
    p->spk_seen = -1;   /* no client has seen a level yet */
    if (settings) {
        p->settings = *settings;
    } else {
        mg_settings_defaults(&p->settings);
    }
    if (!mg_codec_supported(p->settings.codec)) {
        p->settings.codec = mg_data_type_audio_sbc;
    }
    if (!p->cfg.queue) {
        p->settings.queue_enabled = false;
    }
    /* A kept rate or bitrate this build doesn't offer (24 kHz with the 24 kHz
     * gate off, a smaller range) falls back to the default. */
    if (!capture_rate_ok(p, p->settings.capture_rate)) {
        p->settings.capture_rate = MG_CODEC_RATE;
    }
    if (!capture_kbps_ok(p, p->settings.capture_kbps)) {
        p->settings.capture_kbps = MG_CAPTURE_DEFAULT_BITRATE_KBPS;
    }
    if (session_supported(p)) {
        mg_session_ops_t sops = {
            .ctx = p,
            .notify = session_notify,
            .credentials = session_credentials,
            .store_tokens = p->ops.session_store_tokens ? session_store_tokens : NULL,
            .state = session_state,
            .message = session_message,
            .take_nonces = session_take_nonces,
            .proof_key = session_proof_key,
            .busy = session_busy,
            .data_refused = session_data_refused,
            .clear = session_clear,
        };
        mg_session_init(&p->session, p->cfg.session, &sops);
    } else if (p->cfg.creds_session && p->ops.session_credentials && p->ops.session_store_tokens
               && proof_supported(p)) {
        creds_init(p);
    }
    reset_connection(p);
}

void mg_proto_drop_play(mg_proto_t *p)
{
    p->cfg.play = NULL;   /* every reader checks it: no speaker from here on */
}

const mg_settings_t *mg_proto_settings(const mg_proto_t *p)
{
    return &p->settings;
}

void mg_proto_connect(mg_proto_t *p)
{
    stream_abort(p, false);
    /* Settings 7, 13 and 14 are the device's own, kept across connections and
     * reboots: SBC / 16000 / 32 on a fresh device. */
    reset_connection(p);
    p->ctl_sent = p->ctl_done = p->update_seq = p->video.update_seq = 0;
    if (p->cfg.play) {
        mg_play_live_forget(p->cfg.play);   /* a new link: what it holds in flight is learned again */
    }
    p->flight_us = 0;
    p->framed_selects = p->framed_wr_video = p->framed_wr_audio = p->framed_disc = p->rx_switches = 0;
    p->rx_hold = false;
    p->rx_hold_bytes = 0;
    p->out_bench_conn = false;   /* the Nordic UART's bench switch: never carried into a new connection */
    p->connected = true;
}

void mg_proto_disconnect(mg_proto_t *p)
{
    bool was = p->connected;
    stream_abort(p, false);   /* nobody left to tell */
    reset_connection(p);
    p->connected = false;
    p->out_bench_conn = false;   /* the Nordic UART's bench switch ends with its connection */
    /* A turn left thinking or responding ends with its client. */
    if (was && p->cfg.assistant && p->ops.assistant_state) {
        p->ops.assistant_state(p->ops.ctx, mg_assistant_state_idle);
    }
    /* Ends a capture the client started, or one it was about to get. */
    if (was && p->ops.capture) {
        p->ops.capture(p->ops.ctx, false, 0);
    }
}

void mg_proto_set_mtu(mg_proto_t *p, uint16_t mtu)
{
    /* Every write and notification is at most min(ATT_MTU - 3,
     * MG_MAX_ATT_VALUE) bytes (mgcommands.h): ATT caps an attribute value at
     * 512, so at MTU 517 a 514-byte notification is invalid. Keeping the MTU
     * seen by every payload computation (data, the device session
     * and its layer 2) at 512 + 3 bounds them all. */
    if (mtu > MG_MAX_ATT_VALUE + 3) {
        mtu = MG_MAX_ATT_VALUE + 3;
    }
    bool changed = mtu != p->mtu;
    p->mtu = mtu;
    /* What waited for the exchange (longer than MTU - 3: the token proof
     * answer, replies) goes now, whole and in order; the commands that
     * waited behind it run from the next pump (the worker's, not the
     * stack's event). */
    if (changed && p->connected && proof_flush(p)) {
        ctlq_flush(p);
    }
    if (session_supported(p)) {
        mg_session_set_mtu(&p->session, mtu);
    }
}

void mg_proto_subscribe(mg_proto_t *p, mg_ch_t ch, bool on)
{
    if (ch < MG_CH_COUNT) {
        p->sub[ch] = on;
    }
}

/* ---- settings ----------------------------------------------------------- */

/* Link modes (settings 16 and 17): the device implements them. */
static bool link_modes_offered(const mg_proto_t *p)
{
    return p->ops.link_mode && p->ops.set_link_mode && p->ops.link_params;
}

/* Byte width of a parameter's value, 0 if unknown here. */
static size_t setting_width(const mg_proto_t *p, uint8_t param)
{
    switch (param) {
    case mg_setting_parameter_haptics_actuator:
        return haptic_offered(p) ? 4 : 0;   /* read-only */
    case mg_setting_parameter_spec_version:
    case mg_setting_parameter_push_to_talk_enabled:
    case mg_setting_parameter_audio_codec:
        return 1;
    /* The activation buzz needs a vibration motor. */
    case mg_setting_parameter_haptics_enabled:
    case mg_setting_parameter_ptt_buzz_volume_percent:
        return p->cfg.haptics ? 1 : 0;
    case mg_setting_parameter_ptt_buzz_freq_hz:
    case mg_setting_parameter_ptt_buzz_duration_ms:
        return p->cfg.haptics ? 2 : 0;
    case mg_setting_parameter_audio_queue_enabled:
        return p->cfg.queue ? 1 : 0;
    case mg_setting_parameter_speaker_volume:
        return p->ops.speaker_volume && p->ops.set_speaker_volume ? 1 : 0;
    /* Listed only where there's a choice (more than one rate; LC3's bitrates). */
    case mg_setting_parameter_capture_sample_rate:
        return p->cfg.capture_24k && MG_WITH_LC3 ? 2 : 0;
    case mg_setting_parameter_capture_bitrate_kbps:
        return MG_WITH_LC3 ? 2 : 0;
    /* Link modes: per connection; link_params is read-only. */
    case mg_setting_parameter_link_mode:
        return link_modes_offered(p) ? 1 : 0;
    case mg_setting_parameter_link_params:
        return link_modes_offered(p) ? 6 : 0;
    default:
        return 0;
    }
}

/* Setting 19, client_app: variable length, so outside setting_width.
 * client_name (18) is reserved (the apps don't
 * send a device name; the gadget reads the central's GAP name itself): not
 * listed, so a write or read of it is unsupported like any unlisted one. */
static bool client_setting(uint8_t param)
{
    return param == mg_setting_parameter_client_app;
}

static bool setting_offered(const mg_proto_t *p, uint8_t param)
{
    return setting_width(p, param) || client_setting(param);
}

bool mg_proto_utf8_text_ok(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        uint32_t cp;
        size_t len;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7F) {
                return false;   /* C0 controls (NUL included) and DEL */
            }
            i++;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;   /* a continuation byte first, 0xC0/0xC1 (overlong) or past U+10FFFF */
        }
        if (n - i < len) {
            return false;
        }
        for (size_t k = 1; k < len; k++) {
            if ((s[i + k] & 0xC0) != 0x80) {
                return false;
            }
            cp = cp << 6 | (s[i + k] & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0x80 && cp <= 0x9F)) {
            return false;   /* overlong, out of range, a surrogate, or a C1 control */
        }
        i += len;
    }
    return true;
}

/* client_app's value at a (n bytes available): its length on the wire, 0 if
 * it's cut short (invalid_length), or -1 if a field is over its limit or isn't
 * text (invalid_value). */
static int client_value_len(const uint8_t *a, size_t n)
{
    const size_t max = MG_CLIENT_APP_FIELD_MAX;
    size_t o = 0;
    for (size_t f = 0; f < 2; f++) {
        if (o >= n) {
            return 0;
        }
        size_t len = a[o];
        if (len > max) {
            return -1;
        }
        if (n - o - 1 < len) {
            return 0;
        }
        if (!mg_proto_utf8_text_ok(a + o + 1, len)) {
            return -1;
        }
        o += 1 + len;
    }
    return (int)o;
}

/* client_app's stored value and its length on the wire. */
static const uint8_t *client_value(const mg_proto_t *p, size_t *len)
{
    *len = 2u + p->client_app[0] + p->client_app[1 + p->client_app[0]];
    return p->client_app;
}

static void copy_text(char *out, size_t n, const uint8_t *s, size_t len)
{
    if (!n) {
        return;
    }
    if (len >= n) {
        len = n - 1;
        while (len && (s[len] & 0xC0) == 0x80) {
            len--;   /* never half a character */
        }
    }
    memcpy(out, s, len);
    out[len] = 0;
}

void mg_proto_client_app(const mg_proto_t *p, char *name, size_t name_n, char *version, size_t version_n)
{
    const uint8_t *a = p->client_app;
    if (name) {
        copy_text(name, name_n, a + 1, a[0]);
    }
    if (version) {
        copy_text(version, version_n, a + 2 + a[0], a[1 + a[0]]);
    }
}

/* A rate some codec captures at; a kb/s inside some codec's range. */
static bool capture_rate_ok(const mg_proto_t *p, uint16_t rate)
{
    uint8_t codecs[4];
    size_t n = mg_codec_list(codecs, sizeof(codecs));
    for (size_t i = 0; i < n; i++) {
        if (capture_rate_listed(p, codecs[i], rate)) return true;
    }
    return false;
}

static bool capture_kbps_ok(const mg_proto_t *p, uint16_t kbps)
{
    uint8_t codecs[4];
    size_t n = mg_codec_list(codecs, sizeof(codecs));
    static const uint16_t RATES[] = { MG_CODEC_RATE, MG_CODEC_RATE_HIGH };
    for (size_t i = 0; i < n; i++) {
        for (size_t r = 0; r < 2; r++) {
            uint16_t lo, hi;
            if (capture_rate_listed(p, codecs[i], RATES[r]) && mg_codec_kbps_range(codecs[i], RATES[r], &lo, &hi) &&
                kbps >= lo && kbps <= hi) {
                return true;
            }
        }
    }
    return false;
}

/* An LC3 capture whose frame wouldn't fit one Data write at this link's MTU (every frame goes whole, so none would
 * go): its bitrate lowered to the largest whose frame fits ("a device clamps to what the agreed MTU takes",
 * mgcommands.h, audio_lc3). */
static void fit_link(const mg_proto_t *p, mg_audio_format_t *f)
{
    size_t room = mg_proto_data_payload(p);
    if (f->type != mg_data_type_audio_lc3 || f->frame_bytes <= room || !room || !f->frame_us) {
        return;
    }
    mg_audio_format_t fit;
    uint16_t kbps = (uint16_t)(room * 8000u / f->frame_us);
    if (mg_codec_format_at(f->type, f->rate_hz, kbps, &fit) && fit.frame_bytes <= room) {
        *f = fit;
    }
}

void mg_proto_mic_format(const mg_proto_t *p, uint8_t codec, uint16_t rate, uint16_t kbps, mg_audio_format_t *out)
{
    if (!codec || !mg_codec_supported(codec)) {
        codec = p->settings.codec;
    }
    if (!rate || !capture_rate_listed(p, codec, rate)) {
        rate = MG_CODEC_RATE;
    }
    if (!kbps) {
        kbps = MG_CAPTURE_DEFAULT_BITRATE_KBPS;
    }
    if (!mg_codec_format_at(codec, rate, kbps, out)) {
        mg_codec_format(mg_data_type_audio_sbc, out);   /* not reached: SBC at 16 kHz is always there */
    }
    fit_link(p, out);
}

static uint16_t setting_get(const mg_proto_t *p, uint8_t param)
{
    switch (param) {
    case mg_setting_parameter_spec_version: return MG_SPEC_VERSION;
    case mg_setting_parameter_push_to_talk_enabled: return p->ptt_enabled;
    case mg_setting_parameter_haptics_enabled: return p->haptics;
    case mg_setting_parameter_ptt_buzz_freq_hz: return p->settings.buzz_freq_hz;
    case mg_setting_parameter_ptt_buzz_duration_ms: return p->settings.buzz_ms;
    case mg_setting_parameter_ptt_buzz_volume_percent: return p->settings.buzz_volume;
    case mg_setting_parameter_audio_codec: return p->settings.codec;
    case mg_setting_parameter_audio_queue_enabled: return p->settings.queue_enabled;
    case mg_setting_parameter_speaker_volume: return p->ops.speaker_volume(p->ops.ctx);
    case mg_setting_parameter_capture_sample_rate: return p->settings.capture_rate;
    case mg_setting_parameter_capture_bitrate_kbps: return p->settings.capture_kbps;
    case mg_setting_parameter_link_mode: return p->ops.link_mode(p->ops.ctx);
    default: return 0;
    }
}

/* Applies one setting; false if the value is out of range. *persist: it's a stored one. */
static bool setting_set(mg_proto_t *p, uint8_t param, uint16_t v, bool *persist)
{
    switch (param) {
    case mg_setting_parameter_push_to_talk_enabled:
        if (v > 1) return false;
        if (p->ptt_held) {
            p->ptt_asked = v;   /* on once the new proof is valid */
            return true;
        }
        p->ptt_enabled = v;
        return true;
    case mg_setting_parameter_haptics_enabled:
        if (v > 1) return false;
        p->haptics = v;
        return true;
    case mg_setting_parameter_ptt_buzz_freq_hz:
        if (v < 20 || v > 20000) return false;
        p->settings.buzz_freq_hz = v;
        break;
    case mg_setting_parameter_ptt_buzz_duration_ms:
        if (v < 1 || v > 5000) return false;
        p->settings.buzz_ms = v;
        break;
    case mg_setting_parameter_ptt_buzz_volume_percent:
        if (v > 100) return false;
        p->settings.buzz_volume = (uint8_t)v;
        break;
    case mg_setting_parameter_audio_codec:
        if (!mg_codec_supported((uint8_t)v)) return false;
        p->settings.codec = (uint8_t)v;
        if (!p->live && !p->transfer) {
            p->data_type = p->settings.codec;
            p->data_fmt.type = 0;
        }
        break;
    case mg_setting_parameter_audio_queue_enabled:
        if (v > 1) return false;
        p->settings.queue_enabled = v;
        break;
    case mg_setting_parameter_speaker_volume:
        /* The board keeps this one (it's also the Muse speaker setting). */
        if (v > 100) return false;
        p->ops.set_speaker_volume(p->ops.ctx, (uint8_t)v);
        /* The client knows the level it set: never reported back (a device
         * change landing meanwhile was reported inside the set, by the glue). */
        p->spk_seen = (int16_t)p->ops.speaker_volume(p->ops.ctx);
        p->spk_owed = false;
        return true;
    /* Kept like setting 7. */
    case mg_setting_parameter_capture_sample_rate:
        if (!capture_rate_ok(p, v)) return false;
        p->settings.capture_rate = v;
        break;
    case mg_setting_parameter_capture_bitrate_kbps:
        if (!capture_kbps_ok(p, v)) return false;
        p->settings.capture_kbps = v;
        break;
    case mg_setting_parameter_link_mode:
        /* The app's mode for this connection (not stored): the device
         * reports the effective mode when it changes. */
        return p->ops.set_link_mode(p->ops.ctx, (uint8_t)v);
    default:
        return false;   /* spec_version, haptics_actuator and link_params are read-only */
    }
    *persist = true;
    return true;
}

/* A get_settings answer's entry for param: its bytes (1 + value), 0 if the
 * device doesn't take param. */
static size_t get_settings_entry(const mg_proto_t *p, uint8_t param)
{
    if (client_setting(param)) {
        size_t len;
        client_value(p, &len);
        return 1 + len;
    }
    size_t w = setting_width(p, param);
    return w ? 1 + w : 0;
}

/* The answer: each piece at most min(MTU - 3, MG_CTL_REPLY_MAX)
 * bytes, a get_settings answer of its own, so none outgrows the link or the
 * reply buffer; checked whole before anything goes (an unknown parameter is
 * unsupported, an entry no piece can carry or an answer past
 * MG_GET_SETTINGS_PIECES pieces is invalid_length). client_app (19) is as
 * written (empty while unset). */
#define MG_GET_SETTINGS_PIECES 8
static void cmd_get_settings(mg_proto_t *p, const uint8_t *a, size_t n)
{
    /* client_name and client_app are the owner's content: a set-up release gadget's
     * unproven central gets the answer without them, then proof_required once (asked alone: proof_required only).
     * The filter is in the two loops below, so it holds at any parameter count. */
    bool skip = !proof_input_ok(p);
    size_t shown = 0;
    bool owner_skipped = false;
    for (size_t i = 0; i < n; i++) {
        if (skip && client_setting(a[i])) {
            owner_skipped = true;
        } else {
            shown++;
        }
    }
    if (owner_skipped && !shown) {
        send_error(p, mg_command_get_settings, mg_error_code_proof_required);
        return;
    }
    size_t link = link_max(p);
    size_t cap = link < MG_CTL_REPLY_MAX ? link : MG_CTL_REPLY_MAX;
    size_t pieces = 1, o = 1;
    for (size_t i = 0; i < n; i++) {
        if (skip && client_setting(a[i])) {
            continue;   /* not in the answer */
        }
        size_t e = get_settings_entry(p, a[i]);
        if (!e) {
            send_error(p, mg_command_get_settings, mg_error_code_unsupported);
            return;
        }
        if (1 + e > cap) {
            send_error(p, mg_command_get_settings, mg_error_code_invalid_length);
            return;
        }
        if (o + e > cap) {
            pieces++;
            o = 1;
        }
        o += e;
    }
    if (pieces > MG_GET_SETTINGS_PIECES) {
        send_error(p, mg_command_get_settings, mg_error_code_invalid_length);
        return;
    }
    uint8_t m[MG_CTL_REPLY_MAX];
    _Static_assert(sizeof(m) >= 1 + 1 + 2 * (1 + MG_CLIENT_APP_FIELD_MAX), "the longest client_app entry fits a piece");
    _Static_assert(MG_GET_SETTINGS_PIECES * (2 + MG_CTL_REPLY_MAX) <= MG_CTLQ_RING, "every piece can wait whole");
    o = 0;
    m[o++] = mg_command_get_settings;
    for (size_t i = 0; i < n; i++) {
        if (skip && client_setting(a[i])) {
            continue;   /* not in the answer */
        }
        if (o + get_settings_entry(p, a[i]) > cap) {
            send_control(p, m, o);
            o = 1;
        }
        m[o++] = a[i];
        if (client_setting(a[i])) {
            size_t len;
            const uint8_t *v = client_value(p, &len);
            memcpy(m + o, v, len);
            o += len;
            continue;
        }
        size_t w = setting_width(p, a[i]);
        if (w == 4) {
            memcpy(m + o, p->cfg.haptic_actuator, 4);   /* haptics_actuator: kind, flags, min_on_ms, steps */
            o += 4;
            continue;
        }
        if (w == 6) {
            p->ops.link_params(p->ops.ctx, m + o);   /* link_params: interval, latency, timeout */
            o += 6;
            continue;
        }
        uint16_t v = setting_get(p, a[i]);
        m[o++] = (uint8_t)v;
        if (w == 2) {
            m[o++] = (uint8_t)(v >> 8);
        }
    }
    send_control(p, m, o);
    if (owner_skipped) {
        send_error(p, mg_command_get_settings, mg_error_code_proof_required);   /* once, after the answer */
    }
}

/* speaker_volume's report, owed until it leaves on Control (the
 * old report was dropped without a
 * word when no client was ready, a full outbox dropped it, and a
 * disconnect's ctlq_reset discarded a queued one). It never waits in the
 * outbox: once the replies queued ahead of it have gone, it's sent now or
 * stays owed, so the level it carries is always the level at the send. */
static bool speaker_due(mg_proto_t *p, int *v)
{
    if (!p->ops.speaker_volume || !mg_proto_ready(p)) {
        return false;
    }
    *v = p->ops.speaker_volume(p->ops.ctx);
    if (p->spk_seen < 0 && !p->spk_owed) {
        p->spk_seen = (int16_t)*v;   /* the first client since boot: the level it reads, nothing owed */
        return false;
    }
    if (p->spk_seen == *v) {
        p->spk_owed = false;   /* back where the client last saw it: nothing to say */
        return false;
    }
    return true;
}

void mg_proto_speaker_pump(mg_proto_t *p)
{
    int v;
    if (!speaker_due(p, &v) || !ctlq_flush(p)) {
        return;   /* replies queued ahead of it go first, in order */
    }
    uint8_t m[3] = { mg_command_get_settings, mg_setting_parameter_speaker_volume, (uint8_t)v };
    if (send_control_now(p, m, sizeof(m))) {
        p->spk_seen = (int16_t)v;
        p->spk_owed = false;
        p->spk_sent++;
    }
}

void mg_proto_report_setting(mg_proto_t *p, uint8_t param)
{
    if (param == mg_setting_parameter_speaker_volume) {
        if (p->ops.speaker_volume) {
            p->spk_owed = true;
            mg_proto_speaker_pump(p);
        }
        return;
    }
    size_t w = setting_width(p, param);
    if (!w || w > 2 || !mg_proto_ready(p)) {
        return;   /* haptics_actuator (4 bytes) is read-only and never changes */
    }
    uint16_t v = setting_get(p, param);
    uint8_t m[4] = { mg_command_get_settings, param, (uint8_t)v, (uint8_t)(v >> 8) };
    send_control(p, m, 2 + w);
}

/* A setting the device keeps across restarts (NVS): mg's own (the buzz, codec, offline queue, capture rate and
 * bitrate) and the board's speaker volume. push_to_talk, haptics, link_mode and client_app are this connection's. */
static bool stored_setting(uint8_t param)
{
    switch (param) {
    case mg_setting_parameter_ptt_buzz_freq_hz:
    case mg_setting_parameter_ptt_buzz_duration_ms:
    case mg_setting_parameter_ptt_buzz_volume_percent:
    case mg_setting_parameter_audio_codec:
    case mg_setting_parameter_audio_queue_enabled:
    case mg_setting_parameter_speaker_volume:
    case mg_setting_parameter_capture_sample_rate:
    case mg_setting_parameter_capture_bitrate_kbps:
        return true;
    default:
        return false;
    }
}

/* Whether a set_settings names a stored setting the device takes (up to its first malformed or unsupported
 * entry, where the apply loop stops too). */
static void cmd_set_settings(mg_proto_t *p, const uint8_t *a, size_t n)
{
    /* On a set-up gadget a stored setting needs this connection's token proof (mgcommands.h, set_settings): without
     * it, that pair is skipped and changes nothing, the write's other pairs apply, and proof_required is notified
     * once. Per-connection settings stay open. */
    int owner = -1;   /* owner_ok, asked at the first stored setting */
    bool skipped = false;
    bool persist = false;
    size_t i = 0;
    while (i < n) {
        uint8_t param = a[i++];
        if (client_setting(param)) {
            /* client_app: this connection's, never stored; an
             * over-long or malformed value is refused whole. */
            int len = client_value_len(a + i, n - i);
            if (len <= 0) {
                send_error(p, mg_command_set_settings,
                           len < 0 ? mg_error_code_invalid_value : mg_error_code_invalid_length);
                break;
            }
            size_t had;
            const uint8_t *cur = client_value(p, &had);
            if (had != (size_t)len || memcmp(cur, a + i, (size_t)len)) {
                memcpy(p->client_app, a + i, (size_t)len);
                p->client_gen++;
            }
            i += (size_t)len;
            continue;
        }
        size_t w = setting_width(p, param);
        if (!w) {
            send_error(p, mg_command_set_settings, mg_error_code_unsupported);
            break;
        }
        if (n - i < w) {
            send_error(p, mg_command_set_settings, mg_error_code_invalid_length);
            break;
        }
        uint16_t v = w == 2 ? le16(a + i) : a[i];
        i += w;
        if (stored_setting(param) && !(owner < 0 ? (owner = owner_ok(p)) : owner)) {
            skipped = true;   /* a stored setting from an unproven central: skipped, nothing changed */
            continue;
        }
        if (!setting_set(p, param, v, &persist)) {
            send_error(p, mg_command_set_settings, mg_error_code_invalid_value);
            break;
        }
    }
    /* Parameters before a bad one still apply. */
    if (persist && p->ops.settings_changed) {
        p->ops.settings_changed(p->ops.ctx);
    }
    if (skipped) {
        send_error(p, mg_command_set_settings, mg_error_code_proof_required);   /* once, after the others applied */
    }
}

/* ---- playback ------------------------------------------------------------ */

static uint32_t now_ms(const mg_proto_t *p)
{
    return p->ops.now_ms ? p->ops.now_ms(p->ops.ctx) : 0;
}

static int64_t mono_ms(const mg_proto_t *p)
{
    return p->ops.mono_ms ? p->ops.mono_ms(p->ops.ctx) : (int64_t)now_ms(p);
}

static void kick(mg_proto_t *p)
{
    if (p->ops.play_kick) {
        p->ops.play_kick(p->ops.ctx);
    }
}

/* The playback credit: the ring's room, or for a stream with a live depth
 * (start_streaming's parameter 6, the client's choice) what
 * holds the device's queued ms at that depth plus what it sees in flight
 * (mg_play_credit, with flight_us below). */
static uint32_t play_credit(const mg_proto_t *p)
{
    return mg_play_credit(p->cfg.play, p->flight_us);
}

/* A live depth's in-flight measure (mg_play.h, Live depth): a credit reaches the client, which writes, and
 * its bytes cross the link; until then the device plays on. That loop is what a credit counted against the
 * depth alone would leave short. Each buffer_update that raises the client's allowed total is timed to the
 * first byte past the total before it: bytes only that update allowed. A sample over
 * MG_PLAY_LIVE_FLIGHT_MAX_US is the client having nothing to send (between replies), not the link. Smoothed
 * over 8 samples (the link's jitter is the allowance's). */
#define FLIGHT_SLOTS (sizeof(((mg_proto_t *)0)->flight) / sizeof(((mg_proto_t *)0)->flight[0]))

static void flight_sent(mg_proto_t *p, uint32_t rx, uint32_t avail)
{
    uint32_t edge = rx + avail;
    if ((int32_t)(edge - p->flight_edge) <= 0) {
        return;
    }
    if (p->flight_n == FLIGHT_SLOTS) {
        p->flight_head = (uint8_t)((p->flight_head + 1) % FLIGHT_SLOTS);   /* the oldest gives way */
        p->flight_n--;
    }
    unsigned k = (p->flight_head + p->flight_n++) % FLIGHT_SLOTS;
    p->flight[k].at_ms = now_ms(p);
    p->flight[k].below = p->flight_edge;
    p->flight_edge = edge;
}

static void flight_check(mg_proto_t *p)
{
    uint32_t rx = mg_play_received(p->cfg.play);
    while (p->flight_n && (int32_t)(rx - p->flight[p->flight_head].below) > 0) {
        uint32_t us = (now_ms(p) - p->flight[p->flight_head].at_ms) * 1000u;
        if (us <= MG_PLAY_LIVE_FLIGHT_MAX_US) {
            p->flight_us = p->flight_us ? (uint32_t)((int32_t)p->flight_us + ((int32_t)us - (int32_t)p->flight_us) / 8) : us;
        }
        p->flight_head = (uint8_t)((p->flight_head + 1) % FLIGHT_SLOTS);
        p->flight_n--;
    }
}

static void send_buffer_update(mg_proto_t *p)
{
    /* Credits, then queued ms (mgcommands.h, A/V sync): the audio held ahead
     * of the speaker, output path included (0xffff unknown). */
    uint8_t m[12] = { mg_command_stream_audio, mg_stream_audio_buffer_update };
    uint32_t avail = play_credit(p);
    uint32_t rx = mg_play_received(p->cfg.play);
    put32(m + 2, avail);
    put32(m + 6, rx);
    put16(m + 10, mg_play_queued_ms(p->cfg.play));
    if (send_control_now(p, m, sizeof(m))) {
        p->grant_limit = rx + avail;   /* the latest grant: received + available */
        p->update_ms = now_ms(p);
        p->update_avail = avail;
        p->update_seq = p->ctl_sent;
        p->update_owed = false;
        flight_sent(p, rx, avail);
    }
}

static void send_stop(mg_proto_t *p, uint8_t action)
{
    uint8_t m[3] = { mg_command_stream_audio, mg_stream_audio_stop_streaming, action };
    send_control(p, m, sizeof(m));
}

/* The playing stream's end: one stop_streaming per stop the client sent for it (each answered exactly once,
 * mgcommands.h, Audio playback), or one if it sent none (the device's own end). */
static void send_stops(mg_proto_t *p, uint8_t action)
{
    uint8_t n = p->stops_owed ? p->stops_owed : 1;
    p->stops_owed = 0;
    while (n--) {
        send_stop(p, action);
    }
}

static void send_capabilities(mg_proto_t *p)
{
    uint8_t m[MG_REPLY_AUDIO_CAPS_MAX];
    _Static_assert(sizeof(m) <= MG_CTL_REPLY_MAX, "stream_audio's capabilities fit one notification at MG_MIN_ATT_MTU");
    size_t n = 0;
    m[n++] = mg_command_stream_audio;
    m[n++] = mg_stream_audio_report_capabilities;
    uint8_t *entries = &m[n++];   /* counted as they go, never a hand count */
    uint8_t ne = 0;
    m[n++] = mg_stream_audio_capability_codecs;
    ne++;
    uint8_t *count = &m[n++];
    *count = (uint8_t)mg_play_codecs(m + n, MG_PLAY_CODECS_MAX);
    n += *count;
    m[n++] = mg_stream_audio_capability_buffer_size;
    ne++;
    put32(m + n, p->cfg.play->size);
    n += 4;
    m[n++] = mg_stream_audio_capability_sample_rates;
    ne++;
    uint32_t rates[MG_PLAY_RATES_MAX];
    size_t nr = mg_play_rates(rates, MG_PLAY_RATES_MAX);
    m[n++] = (uint8_t)nr;
    for (size_t i = 0; i < nr; i++) {
        put32(m + n, rates[i]);
        n += 4;
    }
    m[n++] = mg_stream_audio_capability_max_bitrate;
    ne++;
    put16(m + n, MG_PLAY_MAX_BITRATE_KBPS);
    n += 2;
    m[n++] = mg_stream_audio_capability_max_channels;
    ne++;
    m[n++] = MG_PLAY_MAX_CHANNELS;
    m[n++] = mg_stream_audio_capability_flush;   /* takes mg_stream_audio_flush */
    ne++;
    m[n++] = 1;
    /* Takes start_streaming's live depth: the smallest it holds (the start threshold, the decoder's chunk and
     * the board's output path), rounded up. */
    m[n++] = mg_stream_audio_capability_live_depth;
    ne++;
    put16(m + n, (uint16_t)((mg_play_live_floor_us(p->cfg.play) + 999) / 1000));
    n += 2;
    /* With video, a reply's first sample waits for the picture its anchor names. */
    if (p->cfg.vstream && p->cfg.play->shown32) {
        m[n++] = mg_stream_audio_capability_video_sync;
        ne++;
        m[n++] = 1;
    }
    *entries = ne;
    send_control(p, m, n);
}

/* Ends a stream from the device's side: the client hears stop_streaming drop. */
static void rx_audio_ended(mg_proto_t *p, bool device);

static void stream_abort(mg_proto_t *p, bool notify)
{
    mg_play_t *pl = p->cfg.play;
    if (!pl || !mg_play_active(pl)) {
        return;
    }
    rx_audio_ended(p, true);   /* the device's own end */
    mg_play_stop(pl, false);
    if (notify) {
        send_stops(p, mg_stream_audio_buffer_drop);
    }
    p->stops_owed = 0;
    kick(p);
}

static void cmd_start_streaming(mg_proto_t *p, const uint8_t *a, size_t n)
{
    mg_play_params_t pp = { 0 };
    bool unknown = false;
    if (n < 1) {
        send_error(p, mg_command_stream_audio, mg_error_code_invalid_length);
        return;
    }
    size_t count = a[0], i = 1;
    for (size_t e = 0; e < count; e++) {
        if (i >= n) {
            send_error(p, mg_command_stream_audio, mg_error_code_invalid_length);
            return;
        }
        uint8_t param = a[i++];
        size_t w = param == mg_stream_audio_parameter_codec || param == mg_stream_audio_parameter_channels ? 1
                   : param == mg_stream_audio_parameter_sample_rate                                          ? 4
                   : param == mg_stream_audio_parameter_bitrate || param == mg_stream_audio_parameter_frame_size ||
                           param == mg_stream_audio_parameter_live_depth_ms
                       ? 2
                       : 0;
        if (!w) {
            /* Entries carry no length (mgcommands.h, start_streaming): an unknown id can't be skipped, so the
             * list ends there and what was parsed applies, defaults for the rest. */
            unknown = true;
            break;
        }
        if (n - i < w) {
            send_error(p, mg_command_stream_audio, mg_error_code_invalid_length);
            return;
        }
        uint32_t v = a[i];
        if (w >= 2) {
            v |= (uint32_t)a[i + 1] << 8;
        }
        if (w == 4) {
            v |= (uint32_t)a[i + 2] << 16 | (uint32_t)a[i + 3] << 24;
        }
        i += w;
        switch (param) {
        case mg_stream_audio_parameter_codec: pp.codec = (uint8_t)v; break;
        case mg_stream_audio_parameter_sample_rate: pp.rate = v; break;
        case mg_stream_audio_parameter_bitrate: pp.bitrate_kbps = (uint16_t)v; break;
        case mg_stream_audio_parameter_channels: pp.channels = (uint8_t)v; break;
        case mg_stream_audio_parameter_live_depth_ms:
            if (!v || v > MG_PLAY_LIVE_DEPTH_MAX_MS) {
                send_error(p, mg_command_stream_audio, mg_error_code_invalid_value);
                return;
            }
            pp.live_depth_ms = (uint16_t)v;
            break;
        default: pp.frame_bytes = (uint16_t)v; break;
        }
    }
    if (unknown && !pp.codec) {
        uint8_t codecs[8];
        if (mg_play_codecs(codecs, sizeof(codecs))) {
            pp.codec = codecs[0];   /* stopped before the codec: the first reported, as for any omitted parameter */
        }
    }
    if (!pp.codec || mg_play_check(&pp)) {
        send_error(p, mg_command_stream_audio, mg_error_code_invalid_value);
        return;
    }
    if (p->audio_claimed) {
        /* The device's own sound has the audio path. A push-to-talk turn
         * doesn't refuse: the stream waits under credits (p->turn). */
        send_error(p, mg_command_stream_audio, mg_error_code_busy);
        return;
    }
    mg_play_t *pl = p->cfg.play;
    if (mg_play_active(pl)) {
        /* A new stream replaces one still playing, or playing out after
         * stop_streaming keep: that one's stop goes first (keep if it had
         * played out, else drop), so every stop_streaming the client sent is
         * answered once, and before this stream's buffer_update
         * (mgcommands.h, Audio playback). */
        bool kept = pl->state == MG_PLAY_DONE && pl->done_keep;
        rx_audio_ended(p, false);   /* the client's new start ends it */
        send_stops(p, kept ? mg_stream_audio_buffer_keep : mg_stream_audio_buffer_drop);
    }
    if (!mg_play_start(pl, &pp)) {
        pl->state = MG_PLAY_IDLE;
        send_error(p, mg_command_stream_audio, mg_error_code_busy);   /* no memory for the buffer */
        return;
    }
    /* The stream's counters start at 0: nothing it allowed is in flight yet (the measure itself stays). */
    p->flight_edge = 0;
    p->flight_head = p->flight_n = 0;
    /* A second stream: Data stays with the first until change_data_type. After a device end that
     * left Data held, a start with video on keeps it held (the writes in flight are the ended
     * stream's, never the video's); alone, the new stream takes it. */
    if (!p->framed && !(p->rx_hold && p->video.active)) {   /* framed stays framed across a new stream */
        p->rx_type = p->video.active ? p->video.codec : pp.codec;
        p->rx_hold = false;
    }
    p->grant_limit = 0;   /* this stream's first buffer_update grants its credit */
    if (p->ops.stream_starting) {
        p->ops.stream_starting(p->ops.ctx);   /* active asked ahead of the first buffer_update */
    }
    send_buffer_update(p);
    kick(p);
}

static uint32_t rd32(const uint8_t *a)
{
    return (uint32_t)a[0] | (uint32_t)a[1] << 8 | (uint32_t)a[2] << 16 | (uint32_t)a[3] << 24;
}

static void cmd_stream_audio(mg_proto_t *p, const uint8_t *a, size_t n)
{
    mg_play_t *pl = p->cfg.play;
    if (!pl) {
        send_error(p, mg_command_stream_audio, mg_error_code_unsupported);
        return;
    }
    if (n < 1) {
        send_error(p, mg_command_stream_audio, mg_error_code_invalid_length);
        return;
    }
    switch (a[0]) {
    case mg_stream_audio_request_capabilities:
        send_capabilities(p);
        return;
    case mg_stream_audio_start_streaming:
        if (!stream_ok(p)) {
            send_error(p, mg_command_stream_audio, mg_error_code_proof_required);
            return;
        }
        cmd_start_streaming(p, a + 1, n - 1);
        return;
    case mg_stream_audio_stop_streaming: {
        if (n < 2) {
            send_error(p, mg_command_stream_audio, mg_error_code_invalid_length);
            return;
        }
        uint8_t action = a[1];
        if (action != mg_stream_audio_buffer_keep && action != mg_stream_audio_buffer_drop) {
            send_error(p, mg_command_stream_audio, mg_error_code_invalid_value);
            return;
        }
        if (!mg_play_active(pl)) {
            send_stop(p, action);   /* nothing playing: already stopped */
            return;
        }
        /* A stop while a kept one plays out (or has just finished) is owed its own answer too. */
        bool owing = pl->state == MG_PLAY_DRAINING || pl->state == MG_PLAY_DONE;
        p->stops_owed = owing && p->stops_owed < UINT8_MAX ? (uint8_t)(p->stops_owed + 1) : 1;
        mg_play_stop(pl, action == mg_stream_audio_buffer_keep);
        if (pl->state == MG_PLAY_IDLE) {
            rx_audio_ended(p, false);
            send_stops(p, mg_stream_audio_buffer_drop);
        }
        /* keep: the playback task plays out the buffer, then mg_proto_stream_finished notifies. */
        kick(p);
        return;
    }
    case mg_stream_audio_request_buffer_update:
        if (p->credit_coalesce && pl->live_us && (pl->state == MG_PLAY_PREBUFFER || pl->state == MG_PLAY_PLAYING)) {
            /* Bench A/B only (credit.coalesce=1): the stream tick's next update answers it (≤ 10 ms, one in
             * flight). Off (the default): answered at once. */
            p->update_owed = true;
            kick(p);
            return;
        }
        send_buffer_update(p);
        return;
    case mg_stream_audio_video_anchor:
        /* Lip sync (mgcommands.h): the first sample of the next Data write belongs with picture
         * `frame`; applied here, in order with the Data writes around it. Only while video
         * runs (otherwise ignored: the audio plays at once). */
        if (n < 5) {
            send_error(p, mg_command_stream_audio, mg_error_code_invalid_length);
            return;
        }
        if (p->video.active && p->video.content == mg_video_content_video) {
            mg_play_anchor(pl, rd32(a + 1));
        }
        return;
    case mg_stream_audio_flush:
        /* Barge-in (mgcommands.h, Live reply audio): what the stream holds goes at once, here in order
         * with the Data writes around it (mg_ble.c applies it on the host task, as the Data: a flush
         * queued behind the next reply's first Data clipped that reply); the stream and its counters
         * stay; the update shows it emptied (queued ms 0), sent from the worker's next stream tick (never
         * from the host task). The playback task drops its FIFO and resets its decoder when it next looks. */
        if (mg_play_flush(pl)) {
            kick(p);
        }
        p->flush_update_owed = true;
        return;
    default:
        send_error(p, mg_command_stream_audio, mg_error_code_unsupported);
        return;
    }
}


/* ---- video streaming ----------------------------------------------------- */

static void video_send_update(mg_proto_t *p)
{
    uint8_t m[19] = { mg_command_stream_video, mg_stream_video_buffer_update };
    uint32_t avail = muse_vstream_available(p->cfg.vstream);
    put32(m + 2, avail);
    put32(m + 6, muse_vstream_received(p->cfg.vstream));
    put16(m + 10, p->video.shown);
    put16(m + 12, p->video.dropped);
    m[14] = p->video.decode_ms;
    /* The last picture drawn and how long ago, for the client's latency. */
    uint32_t age = p->video.have_shown ? now_ms(p) - p->video.last_shown_ms : 0xffff;
    put16(m + 15, p->video.have_shown ? p->video.last_number : 0);
    put16(m + 17, (uint16_t)(age < 0xfffe || !p->video.have_shown ? age : 0xfffe));
    if (send_control_now(p, m, sizeof(m))) {
        p->video.update_ms = now_ms(p);
        p->video.update_avail = avail;
        p->video.update_recv = muse_vstream_received(p->cfg.vstream);
        p->video.grant = p->video.update_recv + avail;   /* the latest grant */
        p->video.granted = true;
        p->video.update_shown = p->video.shown;
        p->video.update_seq = p->ctl_sent;
    }
}

static void video_send_stop(mg_proto_t *p)
{
    uint8_t m[3] = { mg_command_stream_video, mg_stream_video_stop_streaming, mg_stream_audio_buffer_drop };
    send_control(p, m, sizeof(m));
}

/* Framed Data ends once both streams have ended (mgcommands.h, mg_data_type_framed); with one still open it
 * stays framed, and writes naming the ended stream are discarded and counted (no hold). */
static void framed_check_end(mg_proto_t *p, bool audio_ending)
{
    /* audio_ending: called as the playback ends, before its stop (mg_play_active is still true then). */
    if (p->framed && !p->video.active && (audio_ending || !(p->cfg.play && mg_play_active(p->cfg.play)))) {
        p->framed = false;
        p->rx_type = mg_data_type_unknown;
    }
}

static void rx_hold_start(mg_proto_t *p)
{
    p->rx_type = mg_data_type_unknown;
    p->rx_hold = true;
    p->rx_hold_bytes = 0;
}

/* device: the device ended it (its own stop_streaming): Data selected for it is discarded until the next
 * change_data_type (mgcommands.h); the client's end hands Data to the audio, if on. */
static void video_end(mg_proto_t *p, bool device)
{
    if (!p->video.active) {
        return;
    }
    p->video.active = false;
    if (p->cfg.play) {
        mg_play_anchor_drop(p->cfg.play);   /* the video stopping drops a pending anchor */
    }
    if (p->rx_type == p->video.codec) {
        if (device) {
            rx_hold_start(p);
        } else {
            p->rx_type = p->cfg.play && mg_play_active(p->cfg.play) ? p->cfg.play->params.codec : 0;
        }
    }
    muse_vstream_stop(p->cfg.vstream);
    framed_check_end(p, false);
    if (p->ops.video_end) {
        p->ops.video_end(p->ops.ctx);
    }
}

static void video_send_caps(mg_proto_t *p)
{
    const mg_video_caps_t *c = p->cfg.video;
    /* Room for every capability mgcommands.h defines (still
     * included, MG_REPLY_VIDEO_CAPS_MAX), one notification at the MTU floor. */
    uint8_t m[MG_REPLY_VIDEO_CAPS_MAX];
    _Static_assert(sizeof(m) <= MG_CTL_REPLY_MAX, "stream_video's capabilities fit one notification at MG_MIN_ATT_MTU");
    size_t n = 0;
    m[n++] = mg_command_stream_video;
    m[n++] = mg_stream_video_report_capabilities;
    /* tile_split applies to any two-band stream: video's, or UI's on a
     * device that bands only UI streams. */
    bool split = (c->max_tiles > 1 || (c->ui_w && c->ui_tiles > 1)) && c->tile_split_pct;
    uint8_t *entries = &m[n++];   /* counted as they go, never a hand count */
    uint8_t ne = 0;
    m[n++] = mg_stream_video_capability_codecs;
    ne++;
    uint8_t *ncodecs = &m[n++];
    if (!c->no_h264) {
        m[n++] = mg_data_type_video_h264;
    }
    if (c->vp8) {
        m[n++] = mg_data_type_video_vp8;
    }
    *ncodecs = (uint8_t)(&m[n] - ncodecs - 1);
    m[n++] = mg_stream_video_capability_display;
    ne++;
    put16(m + n, c->display_w);
    put16(m + n + 2, c->display_h);
    m[n + 4] = c->round ? 1 : 0;
    n += 5;
    m[n++] = mg_stream_video_capability_max_size;
    ne++;
    put16(m + n, c->max_w);
    put16(m + n + 2, c->max_h);
    n += 4;
    m[n++] = mg_stream_video_capability_max_fps;
    ne++;
    m[n++] = c->max_fps;
    m[n++] = mg_stream_video_capability_buffer_size;
    ne++;
    put32(m + n, p->cfg.vstream->size);
    n += 4;
    m[n++] = mg_stream_video_capability_suggested;
    ne++;
    put16(m + n, c->sug_w);
    put16(m + n + 2, c->sug_h);
    m[n + 4] = c->sug_fps;
    put16(m + n + 5, c->sug_kbps);
    n += 7;
    if (c->l2cap_psm) {
        m[n++] = mg_stream_video_capability_l2cap_psm;
        ne++;
        put16(m + n, c->l2cap_psm);
        n += 2;
    }
    if (c->max_playout_ms) {
        m[n++] = mg_stream_video_capability_playout_delay;
        ne++;
        put16(m + n, c->max_playout_ms);
        n += 2;
    }
    if (c->max_tiles > 1) {
        m[n++] = mg_stream_video_capability_tiles;
        ne++;
        m[n++] = c->max_tiles;
    }
    if (split) {
        m[n++] = mg_stream_video_capability_tile_split;
        ne++;
        m[n++] = c->tile_split_pct;
    }
    /* Last: a client that doesn't know a capability stops parsing there. */
    if (c->ui_w) {
        m[n++] = mg_stream_video_capability_ui;
        ne++;
        put16(m + n, c->ui_w);
        put16(m + n + 2, c->ui_h);
        m[n + 4] = c->ui_fps;
        put32(m + n + 5, c->ui_max_frame);
        m[n + 9] = c->ui_tiles ? c->ui_tiles : 1;
        n += 10;
        if (c->ui_still) {
            m[n++] = mg_stream_video_capability_ui_still;
            ne++;
            m[n++] = c->ui_still;
        }
    }
    /* The ring takes Data for a stream being started (it starts empty; Data
     * with no open stream is dropped, never fed to another stream). */
    m[n++] = mg_stream_video_capability_flags;
    ne++;
    m[n++] = mg_stream_video_capability_flag_early_keyframe | mg_stream_video_capability_flag_framed;
    *entries = ne;
    send_control(p, m, n);
}

static void video_start(mg_proto_t *p, const uint8_t *a, size_t n)
{
    mg_video_params_t vp = { 0 };
    if (n < 1) {
        send_error(p, mg_command_stream_video, mg_error_code_invalid_length);
        return;
    }
    size_t count = a[0], i = 1;
    for (size_t e = 0; e < count; e++) {
        if (i >= n) {
            send_error(p, mg_command_stream_video, mg_error_code_invalid_length);
            return;
        }
        uint8_t param = a[i++];
        const mg_video_caps_t *vc = p->cfg.video;
        size_t w = param == mg_stream_video_parameter_codec || param == mg_stream_video_parameter_fps ||
                           (param == mg_stream_video_parameter_tiles && (vc->max_tiles > 1 || vc->ui_tiles > 1)) ||
                           (param == mg_stream_video_parameter_content && vc->ui_w)
                       ? 1
                   : param == mg_stream_video_parameter_width || param == mg_stream_video_parameter_height ||
                           param == mg_stream_video_parameter_bitrate ||
                           (param == mg_stream_video_parameter_playout_delay && p->cfg.video->max_playout_ms)
                       ? 2
                       : 0;
        if (!w) {
            send_error(p, mg_command_stream_video, mg_error_code_unsupported);
            return;
        }
        if (n - i < w) {
            send_error(p, mg_command_stream_video, mg_error_code_invalid_length);
            return;
        }
        uint16_t v = w == 2 ? (uint16_t)(a[i] | a[i + 1] << 8) : a[i];
        i += w;
        switch (param) {
        case mg_stream_video_parameter_codec: vp.codec = (uint8_t)v; break;
        case mg_stream_video_parameter_width: vp.width = v; break;
        case mg_stream_video_parameter_height: vp.height = v; break;
        case mg_stream_video_parameter_fps: vp.fps = (uint8_t)v; break;
        case mg_stream_video_parameter_tiles: vp.tiles = (uint8_t)v; break;
        case mg_stream_video_parameter_content: vp.content = (uint8_t)v; break;
        case mg_stream_video_parameter_playout_delay:
            vp.playout_ms = v < p->cfg.video->max_playout_ms ? v : p->cfg.video->max_playout_ms;
            break;
        default: vp.kbps = v; break;
        }
    }
    const mg_video_caps_t *c = p->cfg.video;
    /* Bands are whole macroblock rows of equal height. */
    bool codec_ok = (vp.codec == mg_data_type_video_h264 && !c->no_h264) ||
                    (vp.codec == mg_data_type_video_vp8 && c->vp8);
    bool ok;
    if (vp.content == mg_video_content_ui) {
        /* Up to the ui capability's picture, in up to its bands, each a
         * whole number of macroblock rows of the coded height (the last band
         * carries the crop); no playout delay. */
        uint32_t coded = (vp.height + 15u) & ~15u;
        ok = codec_ok && vp.width && vp.height && vp.width <= c->ui_w && vp.height <= c->ui_h &&
             vp.tiles <= (c->ui_tiles ? c->ui_tiles : 1) && (vp.tiles <= 1 || coded % (16u * vp.tiles) == 0) &&
             !vp.playout_ms;
    } else {
        ok = vp.content == mg_video_content_video && codec_ok && vp.width && vp.height && vp.width <= c->max_w &&
             vp.height <= c->max_h && vp.tiles <= (c->max_tiles > 1 ? c->max_tiles : 1) &&
             (vp.tiles <= 1 || vp.height % (16 * vp.tiles) == 0);
    }
    if (!ok) {
        send_error(p, mg_command_stream_video, mg_error_code_invalid_value);
        return;
    }
    /* A phone's screen on the gadget's (screen mirror): on a set-up gadget only from a proven central, as the
     * apps gate it; a video to play stays open (receive only). */
    if (vp.content == mg_video_content_ui && !owner_ok(p)) {
        send_error(p, mg_command_stream_video, mg_error_code_proof_required);
        return;
    }
    /* A new stream replaces one that's on. Audio playback goes on beside it:
     * the client picks which one its Data writes carry with change_data_type. */
    video_end(p, false);
    /* The ring tells a frame from noise by the codec's own start. */
    p->cfg.vstream->codec = vp.codec == mg_data_type_video_vp8 ? MUSE_VIDEO_CODEC_VP8 : MUSE_VIDEO_CODEC_H264;
    muse_vstream_start(p->cfg.vstream);
    bool audio_on = p->cfg.play && mg_play_active(p->cfg.play);
    if (!p->framed && !(p->rx_hold && audio_on)) {   /* as an audio start does, after a device end */
        p->rx_type = audio_on ? p->cfg.play->params.codec : vp.codec;
        p->rx_hold = false;
    }
    p->video.codec = vp.codec;
    p->video.content = vp.content;
    p->video.active = true;
    if (p->cfg.play) {
        mg_play_anchor_drop(p->cfg.play);   /* ...and so does it (re)starting */
    }
    p->video.shown = p->video.dropped = 0;
    p->video.granted = p->video.overrun_asked = false;   /* a new stream's first update grants its credit */
    p->video.decode_ms = 0;
    p->video.have_shown = false;
    if (p->ops.video_begin && !p->ops.video_begin(p->ops.ctx, &vp)) {
        p->video.active = false;
        muse_vstream_stop(p->cfg.vstream);
        send_error(p, mg_command_stream_video, mg_error_code_busy);
        return;
    }
    video_send_update(p);
}

static void cmd_stream_video(mg_proto_t *p, const uint8_t *a, size_t n)
{
    if (!p->cfg.video || !p->cfg.vstream) {
        send_error(p, mg_command_stream_video, mg_error_code_unsupported);
        return;
    }
    if (n < 1) {
        send_error(p, mg_command_stream_video, mg_error_code_invalid_length);
        return;
    }
    switch (a[0]) {
    case mg_stream_video_request_capabilities:
        video_send_caps(p);
        return;
    case mg_stream_video_start_streaming:
        if (!stream_ok(p)) {
            send_error(p, mg_command_stream_video, mg_error_code_proof_required);
            return;
        }
        video_start(p, a + 1, n - 1);
        return;
    case mg_stream_video_stop_streaming:
        /* The client's own stop isn't answered (mgcommands.h, Video
         * streaming): an echo could land after the client's next
         * start_streaming and read as that stream's end. */
        video_end(p, false);
        return;
    case mg_stream_video_request_buffer_update:
        if (p->video.active) {
            video_send_update(p);
        }
        return;
    default:
        send_error(p, mg_command_stream_video, mg_error_code_unsupported);
        return;
    }
}

bool mg_proto_video_active(const mg_proto_t *p)
{
    return p->video.active;
}

void mg_proto_video_request(mg_proto_t *p, bool want)
{
    p->video.wanted = want;
    if (!p->cfg.video || !mg_proto_ready(p)) {
        return;   /* sent when a client asks for status */
    }
    if (!want && p->video.active) {
        video_end(p, true);
        video_send_stop(p);
    }
    uint8_t m[3] = { mg_command_stream_video, mg_stream_video_request_streaming, want ? 1 : 0 };
    send_control(p, m, sizeof(m));
}

void mg_proto_video_request_keyframe(mg_proto_t *p)
{
    if (p->video.active) {
        uint8_t m[2] = { mg_command_stream_video, mg_stream_video_request_keyframe };
        send_control(p, m, sizeof(m));
    }
}

void mg_proto_video_stop(mg_proto_t *p)
{
    if (p->video.active) {
        video_end(p, true);
        video_send_stop(p);
    }
}

void mg_proto_video_stats(mg_proto_t *p, uint16_t shown, uint16_t dropped, uint8_t decode_ms)
{
    p->video.shown = shown;
    p->video.dropped = dropped;
    p->video.decode_ms = decode_ms;
}

void mg_proto_video_tick(mg_proto_t *p)
{
    if (!p->video.active || !p->connected) {
        return;
    }
    uint32_t since = now_ms(p) - p->video.update_ms;
    uint32_t avail = muse_vstream_available(p->cfg.vstream);
    uint32_t got = muse_vstream_received(p->cfg.vstream) - p->video.update_recv;
    /* Video credit back promptly (a bench test of a reply with video): the apps keep only 2-3 KB of
     * video in flight during a reply, so an update goes once 1 KB has been taken from the ring (the
     * room freed: what avail grew by, plus what came in meanwhile) or 1 KB received, else 20 ms after
     * any news (bytes in, room freed, a picture shown), never held to a decode pass or 100 ms. At most
     * one in flight per connection event: a new one waits for the last to leave the stack
     * (mg_proto_control_sent, or 40 ms), as the reply audio's do. */
    int64_t taken = (int64_t)avail - (int64_t)p->video.update_avail + (int64_t)got;
    bool shown = p->video.shown != p->video.update_shown;
    bool news = avail != p->video.update_avail || got || shown;
    bool ui = p->video.content == mg_video_content_ui;
    if (!ui && since >= MG_VIDEO_KEEPALIVE_MS) {
        video_send_update(p);   /* a camera stream: at least every 250 ms, news or not */
        return;
    }
    /* A UI stream: nothing on the link while the picture is still, only once there is news. */
    bool out = (int32_t)(p->ctl_done - p->video.update_seq) < 0 && since < 40;
    if (!news || out || since < 5) {
        return;
    }
    if (since >= MG_VIDEO_NEWS_MS || taken >= MG_VIDEO_CREDIT_STEP || got >= MG_VIDEO_CREDIT_STEP || (ui && shown)) {
        video_send_update(p);
    }
}

void mg_proto_video_shown(mg_proto_t *p, uint16_t number, uint32_t at_ms)
{
    p->video.have_shown = true;
    p->video.last_number = number;
    p->video.last_shown_ms = at_ms;
}

uint8_t mg_proto_video_content(const mg_proto_t *p)
{
    return p->video.active ? p->video.content : mg_video_content_video;
}

/* ---- UI input -------------------------------------------------------------- */

static void ui_send_status(mg_proto_t *p)
{
    uint8_t m[4] = { mg_command_ui_input, mg_ui_input_hid_status, p->hid_mode, p->hid_flags };
    send_control(p, m, sizeof(m));
}

static void cmd_ui_input(mg_proto_t *p, const uint8_t *a, size_t n)
{
    if (!p->cfg.ui_input) {
        send_error(p, mg_command_ui_input, mg_error_code_unsupported);
        return;
    }
    if (n < 1) {
        send_error(p, mg_command_ui_input, mg_error_code_invalid_length);
        return;
    }
    const uint8_t hid_bits = (1u << mg_ui_input_feature_hid_digitizer) | (1u << mg_ui_input_feature_hid_mouse_abs) |
                             (1u << mg_ui_input_feature_hid_mouse_rel);
    switch (a[0]) {
    case mg_ui_input_touch_enable:
        if (!(p->cfg.ui_input & (1u << mg_ui_input_feature_touch))) {
            send_error(p, mg_command_ui_input, mg_error_code_unsupported);
        } else if (n < 2) {
            send_error(p, mg_command_ui_input, mg_error_code_invalid_length);
        } else if (a[1] > 1) {
            send_error(p, mg_command_ui_input, mg_error_code_invalid_value);
        } else {
            p->touch_enabled = a[1] == 1;
        }
        return;
    case mg_ui_input_hid_map: {
        if (!(p->cfg.ui_input & hid_bits) || !p->ops.hid_map) {
            send_error(p, mg_command_ui_input, mg_error_code_unsupported);
            return;
        }
        mg_hid_map_t m;
        uint16_t err = mg_hid_map_parse(a + 1, n - 1, &m);
        /* A mode the device doesn't list (digitizer = feature 2, and so on). */
        if (!err && m.mode != mg_hid_mode_off && !(p->cfg.ui_input & (1u << (m.mode + 1)))) {
            err = mg_error_code_unsupported;
        }
        if (err) {
            send_error(p, mg_command_ui_input, err);
            return;
        }
        /* The map is kept across restarts: on a set-up gadget only from a proven central. */
        if (!owner_ok(p)) {
            send_error(p, mg_command_ui_input, mg_error_code_proof_required);
            return;
        }
        uint8_t mode = m.mode;
        p->hid_flags = p->ops.hid_map(p->ops.ctx, &m, &mode);
        p->hid_mode = mode;
        ui_send_status(p);   /* the answer, whether or not anything changed */
        return;
    }
    case mg_ui_input_control:
        if (!(p->cfg.ui_input & (1u << mg_ui_input_feature_controls))) {
            send_error(p, mg_command_ui_input, mg_error_code_unsupported);
        } else if (n != 3) {
            send_error(p, mg_command_ui_input, mg_error_code_invalid_length);
        } else if (a[1] == 0 || a[1] >= sizeof(p->ui_ctl) || a[2] > (a[1] == mg_ui_control_overscroll ? 2 : 1)) {
            send_error(p, mg_command_ui_input, mg_error_code_invalid_value);
        } else {
            p->ui_ctl[a[1]] = a[2];
        }
        return;
    case mg_ui_input_hid_key:
        if (!p->ops.hid_key || !(p->cfg.ui_input & ((1u << mg_ui_input_feature_hid_digitizer) |
                                                     (1u << mg_ui_input_feature_hid_mouse_abs) |
                                                     (1u << mg_ui_input_feature_hid_mouse_rel)))) {
            send_error(p, mg_command_ui_input, mg_error_code_unsupported);
        } else if (n != 3) {
            send_error(p, mg_command_ui_input, mg_error_code_invalid_length);
        } else if (!owner_ok(p)) {
            send_error(p, mg_command_ui_input, mg_error_code_proof_required);   /* keys to the HID host: as hid_map */
        } else if (!p->ops.hid_key(p->ops.ctx, (uint16_t)(a[1] | a[2] << 8))) {
            send_error(p, mg_command_ui_input, mg_error_code_busy);
        }
        return;
    default:
        send_error(p, mg_command_ui_input, mg_error_code_unsupported);
        return;
    }
}

uint8_t mg_proto_ui_control(const mg_proto_t *p, uint8_t id)
{
    return id < sizeof(p->ui_ctl) ? p->ui_ctl[id] : 0;
}

bool mg_proto_ui_control_set(mg_proto_t *p, uint8_t id, uint8_t value)
{
    if (id == 0 || id >= sizeof(p->ui_ctl) || !(p->cfg.ui_input & (1u << mg_ui_input_feature_controls))) {
        return false;
    }
    p->ui_ctl[id] = value;
    if (!mg_proto_ready(p)) {
        return false;
    }
    uint8_t m[4] = { mg_command_ui_input, mg_ui_input_control, id, value };
    return send_control(p, m, sizeof(m));
}

bool mg_proto_touch_enabled(const mg_proto_t *p)
{
    return p->connected && p->touch_enabled;
}

bool mg_proto_touch(mg_proto_t *p, const mg_touch_event_t *ev, uint32_t time_ms, uint8_t id)
{
    if (!p->touch_enabled || !mg_proto_ready(p)) {
        return false;
    }
    uint8_t m[10] = { mg_command_ui_input, mg_ui_input_touch, ev->action };
    put16(m + 3, ev->x);
    put16(m + 5, ev->y);
    put16(m + 7, (uint16_t)time_ms);
    m[9] = id;
    /* Now or never (the next event supersedes it), but never uncounted. */
    bool ok = send_control_now(p, m, sizeof(m));
    p->touch_dropped += !ok;
    return ok;
}

void mg_proto_hid_status(mg_proto_t *p, uint8_t mode, uint8_t flags)
{
    if (mode == p->hid_mode && flags == p->hid_flags) {
        return;
    }
    p->hid_mode = mode;
    p->hid_flags = flags;
    if (p->cfg.ui_input && mg_proto_ready(p)) {
        ui_send_status(p);
    }
}

/* Data written by the client: stream audio. */
static bool tp_expired(const mg_proto_t *p);

static void video_bytes(mg_proto_t *p, const uint8_t *d, size_t n)
{
    if (n && p->video.granted && (uint64_t)muse_vstream_received(p->cfg.vstream) + n > p->video.grant) {
        /* mgcommands.h (every build): past the latest buffer_update's received + available (before the first, the
         * one early keyframe goes): the write dropped with its frame and counted in received, then a keyframe asked
         * for (once until a write fits again). */
        muse_vstream_drop(p->cfg.vstream, n);
        p->video_overrun_writes++;
        p->video_overrun_bytes += (uint32_t)n;
        if (!p->video.overrun_asked) {
            p->video.overrun_asked = true;
            mg_proto_video_request_keyframe(p);
        }
        return;
    }
    p->video.overrun_asked = false;
    muse_vstream_write(p->cfg.vstream, d, n);
}

void mg_proto_credit_overruns(const mg_proto_t *p, uint32_t *writes, uint32_t *bytes)
{
    *writes = p->credit_overrun_writes;
    *bytes = p->credit_overrun_bytes;
}

void mg_proto_video_credit_overruns(const mg_proto_t *p, uint32_t *writes, uint32_t *bytes)
{
    *writes = p->video_overrun_writes;
    *bytes = p->video_overrun_bytes;
}

static void audio_bytes(mg_proto_t *p, mg_play_t *pl, const uint8_t *d, size_t n)
{
    if (n && (uint64_t)mg_play_received(pl) + n > p->grant_limit) {
        /* mgcommands.h (every build): past the latest buffer_update's received + available (a
         * live depth's window is less than the ring) dropped whole and counted, never buffered, though it fits;
         * received still counts it, so the client's credit stays in step. */
        mg_play_drop_write(pl, n);
        p->credit_overrun_writes++;
        p->credit_overrun_bytes += (uint32_t)n;
        return;
    }
    if (n) {
        mg_play_write(pl, d, n);
        flight_check(p);
    }
}

static void data_in(mg_proto_t *p, const uint8_t *d, size_t n)
{
    mg_play_t *pl = p->cfg.play;
    if (p->framed) {
        /* Framed Data (mgcommands.h, mg_data_type_framed): the first byte names the write's stream and is
         * not stream data (credits, counts and frame sizes exclude it). A write naming no open stream, or
         * one the device ended, is discarded and counted; no hold. So is a write with no stream data. */
        if (n < 2) {
            p->framed_disc++;   /* the type byte alone, or an empty write: no stream bytes, nothing else changes */
            return;
        }
        uint8_t t = d[0];
        if (p->video.active && t == p->video.codec) {
            p->framed_wr_video++;
            video_bytes(p, d + 1, n - 1);
        } else if (pl && mg_play_active(pl) && t == pl->params.codec) {
            p->framed_wr_audio++;
            audio_bytes(p, pl, d + 1, n - 1);
        } else {
            p->framed_disc++;
            p->rx_discarded += (uint32_t)(n - 1);
        }
        return;
    }
    if (p->video.active && p->rx_type == p->video.codec) {
        video_bytes(p, d, n);
        return;
    }
    if (pl && mg_play_active(pl) && p->rx_type == pl->params.codec) {
        audio_bytes(p, pl, d, n);
        return;
    }
    if (p->tp.mode == mg_throughput_test_receive) {
        if (p->tp.armed && !tp_expired(p)) {
            mg_tp_account(&p->tp.c, d, n, now_ms(p));
        }
        return;
    }
    /* No stream of the selected type is open (audio chunks still on their
     * way after the device ended the playback, say): dropped and counted.
     * Never fed to another stream: interleaved audio in the video ring froze
     * the video until it ended. */
    p->rx_discarded += (uint32_t)n;
    if (p->rx_hold) {
        p->rx_hold_bytes += (uint32_t)n;
    }
}

uint32_t mg_proto_rx_discarded(const mg_proto_t *p)
{
    return p->rx_discarded;
}

/* A playback stream ended. The client ended it (its stop, or a new start): Data selected for it goes to
 * the video, if one is on ("when the client ends either, Data goes to the other"; Android and iOS rely
 * on it). The device ended it (device: its own stop_streaming): the writes in flight were meant for the
 * ended stream, so Data selected for it is discarded until the client's next change_data_type
 * (mgcommands.h). */
static void rx_audio_ended(mg_proto_t *p, bool device)
{
    mg_play_t *pl = p->cfg.play;
    if (pl && p->rx_type == pl->params.codec) {
        if (device) {
            rx_hold_start(p);
        } else {
            p->rx_type = p->video.active ? p->video.codec : mg_data_type_unknown;
        }
    }
    framed_check_end(p, true);
}

static void claim_now(mg_proto_t *p);

void mg_proto_claim_turn(mg_proto_t *p, bool on)
{
    if (on && !p->turn) {
        if (p->capq.gate) {
            p->claim_deferred = true;   /* a held turn: the reply is held (p->turn) until it's released */
        } else {
            claim_now(p);
        }
    }
    if (!on) {
        p->claim_deferred = false;   /* a dropped turn's: the held reply plays on, as after a tap */
    }
    p->turn = on;
    p->barge_paused = false;   /* the turn decides now (on: it holds the speaker; off: it's over) */
}

/* The turn takes the speaker: the reply being talked over goes. */
static void claim_now(mg_proto_t *p)
{
    stream_abort(p, p->connected && mg_proto_ready(p));   /* the reply being talked over */
}

void mg_proto_barge_pause(mg_proto_t *p, bool on)
{
    bool was = mg_proto_barge_held(p);
    p->barge_paused = on;
    p->barge_ms = now_ms(p);
    if (was && !on) {
        kick(p);   /* the reply resumes where it paused */
    }
}

bool mg_proto_barge_held(const mg_proto_t *p)
{
    return p->barge_paused && (uint32_t)(now_ms(p) - p->barge_ms) < MG_PROTO_BARGE_PAUSE_MAX_MS;
}

void mg_proto_claim_audio(mg_proto_t *p, bool claim)
{
    p->audio_claimed = claim;
    if (!claim) {
        return;
    }
    stream_abort(p, p->connected && mg_proto_ready(p));
}

bool mg_proto_play_held(const mg_proto_t *p)
{
    return p->turn || mg_proto_barge_held(p);
}

void mg_proto_stream_tick(mg_proto_t *p)
{
    mg_play_t *pl = p->cfg.play;
    if (p->flush_update_owed && p->connected) {
        /* A flush's update (stream or not: the client asked): first, as the flush's answer. */
        uint32_t sent = p->ctl_sent;
        send_buffer_update(p);
        if (p->ctl_sent != sent) {
            p->flush_update_owed = false;   /* sent (a refusal leaves it owed for the next tick) */
        }
    }
    if (!pl || !p->connected || (pl->state != MG_PLAY_PREBUFFER && pl->state != MG_PLAY_PLAYING)) {
        return;
    }
    uint32_t since = now_ms(p) - p->update_ms;
    uint32_t credit = play_credit(p);
    if (pl->live_us || pl->slow_link) {
        /* The cadence: while a live depth is honored or a
         * start before active is pending, a changed credit goes out every 10 ms of playout (the worker
         * looks every 5 ms), once the last one has left the stack (or 40 ms on), so at most one is in
         * flight per connection event. With a live depth the credit is the depth's room (plus what's in
         * flight, up to the allowance), so a client paused at it would let the ring run dry between
         * slower updates; and a pending notification keeps the peripheral listening at every connection
         * event despite its slave latency, so a reply on a ready link (latency 5) arrives at every
         * event, not in bursts every sixth. */
        bool out = (int32_t)(p->ctl_done - p->update_seq) < 0 && since < 40;
        if (since >= 250 || (!out && since >= 10 && (credit != p->update_avail || p->update_owed))) {
            send_buffer_update(p);
        }
        return;
    }
    /* The whole-buffer path on an active link (a bench test of a reply with video:
     * fixed 15 ms audio updates held the notify slot and made video credit late 5x as often, with no
     * audio gain): on change only, once 1 KB of room was freed since the last update
     * (MG_PLAY_UPDATE_FREED), or 50 ms on with any change (MG_PLAY_UPDATE_CHANGE_MS), never while any
     * Control notification is still in the stack (the one in-flight slot, video credit's first: the
     * worker runs mg_proto_video_tick before this), 250 ms keepalive. */
    bool slot_taken = (int32_t)(p->ctl_done - p->ctl_sent) < 0 && since < 40;
    bool freed = credit >= p->update_avail + MG_PLAY_UPDATE_FREED;
    bool changed = credit != p->update_avail;
    if (since >= 250 || (!slot_taken && (freed || (changed && since >= MG_PLAY_UPDATE_CHANGE_MS)))) {
        send_buffer_update(p);
    }
}

void mg_proto_stream_finished(mg_proto_t *p)
{
    mg_play_t *pl = p->cfg.play;
    if (!pl || pl->state != MG_PLAY_DONE) {
        return;
    }
    bool keep = pl->done_keep;
    rx_audio_ended(p, !keep);   /* keep: the client's stop played out; else the device ended it (starved, no memory) */
    pl->state = MG_PLAY_IDLE;
    mg_play_release(pl);
    if (p->connected) {
        send_stops(p, keep ? mg_stream_audio_buffer_keep : mg_stream_audio_buffer_drop);
    }
}

/* ---- token proof (mgcommands.h, Token proof) ---------------------------- */

static void proof_reset(mg_proto_t *p)
{
    mg_token_proof_wipe(&p->proof, sizeof(p->proof));
}

/* The proof that admitted streams to the gadget ended while the link stays up (mgcommands.h, start_streaming): a
 * re-proof that failed or was abandoned, the token cleared or replaced, the bench switch off. The open streams end
 * as the device ends them, each with its stop_streaming, and playback stops. force: the token is going, whatever
 * stream_ok() says now. A re-proof under way ends nothing until it fails. */
static void streams_end_unproven(mg_proto_t *p, bool force)
{
    if (!p->connected || (!force && stream_ok(p))) {
        return;
    }
    stream_abort(p, true);
    if (p->video.active) {
        video_end(p, true);
        video_send_stop(p);
    }
}

/* ---- mic output only to a proven central (mg_proto.h) ---------- */

bool mg_proto_output_ok(const mg_proto_t *p)
{
    return out_ok(p);
}

bool mg_proto_output_held(const mg_proto_t *p)
{
    return p->connected && !out_ok(p) && (p->live || mg_proto_capture_pending(p));
}

bool mg_proto_output_full(const mg_proto_t *p)
{
    return p->capq.proof_full;
}

bool mg_proto_output_bench(const mg_proto_t *p)
{
    return p->out_bench || p->out_bench_conn;
}

bool mg_proto_output_bench_conn(const mg_proto_t *p)
{
    return p->out_bench_conn;
}

bool mg_proto_capture_visible(const mg_proto_t *p)
{
    return out_ok(p);
}

const char *mg_proto_output_state(const mg_proto_t *p)
{
    return !p->cfg.output_proof ? "open" : mg_proto_output_bench(p) ? "bench" : out_ok(p) ? "proven" : "unproven";
}

uint8_t mg_proto_take_output_note(mg_proto_t *p, uint32_t *bytes)
{
    uint8_t n = p->out_note;
    if (bytes) {
        *bytes = p->out_note_bytes;
    }
    p->out_note = MG_OUT_NOTE_NONE;
    p->out_note_bytes = 0;
    return n;
}

/* The gate opened (a match, or the bench switch): what was held goes, in order, as the link takes it. */
static void output_release(mg_proto_t *p)
{
    mg_capq_t *q = &p->capq;
    if (!out_ok(p) || !(p->live || mg_proto_capture_pending(p))) {
        return;
    }
    if (output_stale(p)) {
        return;   /* past its staleness bound: discarded, not sent */
    }
    if (q->held_turn) {
        capture_pump(p, false);   /* a held live turn goes once its client is ready too (the pump checks) */
        return;
    }
    q->stale_armed = false;
    p->pre_set = false;   /* proven: its own change_data_type goes now, in order */
    bool had = mg_proto_capture_pending(p);
    q->proof_full = false;
    if (had) {
        q->proof_releases++;
        p->out_note = MG_OUT_NOTE_RELEASED;
        p->out_note_bytes = q->wseq - q->rseq;
    }
    capture_pump(p, false);
}

/* The proof failed (or there's no key to prove): what was held is discarded, never sent, and the
 * capture ends (a client's through ops.capture; a press's at its next slice, mg_proto_send_data false).
 * owner_changed: a new setup committed, so whatever was captured is the last owner's and goes whatever the
 * output state, never to the clip queue either. */
static void output_drop(mg_proto_t *p, bool owner_changed)
{
    mg_capq_t *q = &p->capq;
    if ((!owner_changed && out_ok(p)) || !(p->live || mg_proto_capture_pending(p))) {
        return;
    }
    if (!owner_changed && MG_HELD_TURN_FAIL_QUEUES && q->held_turn && mg_proto_held_turn_queueable(p)) {
        q->ht_spill = true;   /* a held live turn goes to the clip queue instead (mg_config.h) */
        return;
    }
    output_drop_held(p);
}

/* What output_drop discards: everything held, counted, the capture ended. */
static void output_drop_held(mg_proto_t *p)
{
    mg_capq_t *q = &p->capq;
    uint32_t bytes = q->wseq - q->rseq;
    bool was_live = p->live;
    q->proof_discards++;
    q->proof_discarded += bytes;
    p->out_note = MG_OUT_NOTE_DISCARDED;
    p->out_note_bytes = bytes;
    p->live = false;
    if (q->gate) {
        /* The bench guard's held turn goes with it: the data type it put aside comes back. */
        p->data_type = p->gate_data_type;
        p->data_fmt = p->gate_data_fmt;
    } else if (p->pre_set) {
        p->data_type = p->pre_data_type;   /* ...or the one from before a capture that began unproven */
        p->data_fmt = p->pre_data_fmt;
    }
    p->pre_set = false;
    capq_reset(p);   /* controls, audio, levels, a guard's held turn: as at the link's end */
    if (was_live && p->ops.capture) {
        p->ops.capture(p->ops.ctx, false, NULL);
    }
}

static void output_discard(mg_proto_t *p)
{
    output_drop(p, false);
}

bool mg_proto_live_discard(mg_proto_t *p)
{
    if (out_ok(p) || !(p->live || mg_proto_capture_pending(p))) {
        return false;
    }
    output_drop_held(p);   /* never to the clip queue either: the user didn't keep it */
    p->out_note = MG_OUT_NOTE_NONE;   /* not a failed proof: the caller says why */
    p->out_note_bytes = 0;
    return true;
}

/* Arms the staleness bound for what's held now (once: the earliest end counts). */
static void stale_arm(mg_proto_t *p)
{
    mg_capq_t *q = &p->capq;
    if (!q->stale_armed) {
        q->stale_armed = true;
        q->stale_at = now_ms(p) + MG_OUTPUT_STALE_MS;
    }
}

/* The held turn's staleness bound: held output not released within MG_OUTPUT_STALE_MS of the capture's
 * end is discarded, logged and counted. A held live turn the clip queue can take goes there instead (the
 * queue wins). True: it was discarded now. */
static bool output_stale(mg_proto_t *p)
{
    mg_capq_t *q = &p->capq;
    if (!q->stale_armed || (int32_t)(now_ms(p) - q->stale_at) < 0) {
        return false;
    }
    if (q->held_turn && mg_proto_held_turn_queueable(p)) {
        return false;
    }
    uint32_t bytes = q->wseq - q->rseq;
    q->proof_stale++;
    q->proof_stale_bytes += bytes;
    p->out_note = MG_OUT_NOTE_STALE;
    p->out_note_bytes = bytes;
    if (q->gate || q->held_turn) {
        p->data_type = p->gate_data_type;   /* the type it put aside comes back */
        p->data_fmt = p->gate_data_fmt;
    } else if (p->pre_set) {
        p->data_type = p->pre_data_type;
        p->data_fmt = p->pre_data_fmt;
    }
    p->pre_set = false;
    bool was_live = p->live;
    p->live = false;
    capq_reset(p);
    if (was_live && p->ops.capture) {
        p->ops.capture(p->ops.ctx, false, NULL);
    }
    return true;
}

bool mg_proto_output_waiting(const mg_proto_t *p)
{
    return p->capq.held_turn || p->capq.stale_armed;
}

void mg_proto_set_output_bench(mg_proto_t *p, bool on)
{
    p->out_bench = on;
    if (on && p->connected) {
        output_release(p);
    } else if (!on) {
        streams_end_unproven(p, false);   /* streams the switch admitted end */
    }
}

void mg_proto_set_output_bench_conn(mg_proto_t *p, bool on)
{
    if (!p->connected) {
        return;   /* this connection's: none, nothing to switch */
    }
    p->out_bench_conn = on;
    if (on) {
        output_release(p);
    } else {
        streams_end_unproven(p, false);   /* streams the switch admitted end */
    }
}

static void proof_challenge(mg_proto_t *p, const uint8_t *a, size_t n)
{
    proof_reset(p);   /* a new challenge restarts the exchange (an untaken match's nonces go too) */
    if (p->cfg.output_proof && !mg_proto_output_bench(p) && !p->live && mg_proto_capture_pending(p)) {
        stale_arm(p);   /* a capture that ended before this re-proof, still going out: held from here */
    }
    mg_token_proof_wipe(&p->proof_out, sizeof(p->proof_out));   /* ...and an answer still waiting */
    /* The client is proving itself again (an iOS
     * relaunch that kept the link up): its push-to-talk enable goes, so a
     * press can't open the mic before the new proof. Presses are gesture-only
     * until the proof is valid and push-to-talk enabled again (the capture
     * codec choice stays). Keyed on the challenge, not request_status. */
    p->ptt_held = true;
    p->ptt_asked = false;
    if (p->ptt_enabled) {
        p->ptt_enabled = false;
        p->ptt_cleared_note = true;
    }
    if (session_supported(p) && mg_session_sealed(&p->session)) {
        /* A re-proof replaces the seal keys: the sealed session it ends
         * takes a new sealed hello. */
        mg_session_reset(&p->session);
        mg_session_set_mtu(&p->session, p->mtu);
    }
    creds_reset(p, true);   /* layer 2: a session never outlives the proof that keyed it */
    if (n < MG_TOKEN_PROOF_NONCE_LEN) {
        send_error(p, mg_command_token_proof, mg_error_code_invalid_length);
        return;
    }
    uint8_t k[MG_TOKEN_PROOF_KEY_LEN];
    if (!p->ops.proof_key(p->ops.ctx, k)) {
        mg_token_proof_wipe(k, sizeof(k));
        send_error(p, mg_command_token_proof, mg_error_code_not_found);
        output_discard(p);   /* no key, no proof can match: a held capture never goes */
        return;
    }
    uint8_t m[2 + MG_TOKEN_PROOF_NONCE_LEN + MG_TOKEN_PROOF_MAC_LEN] = { mg_command_token_proof,
                                                                       mg_token_proof_response };
    memcpy(p->proof.client_nonce, a, MG_TOKEN_PROOF_NONCE_LEN);
    bool ok = p->ops.random(p->ops.ctx, p->proof.device_nonce, MG_TOKEN_PROOF_NONCE_LEN)
              && mg_token_proof_mac(k, true, p->proof.client_nonce, p->proof.device_nonce,
                                    m + 2 + MG_TOKEN_PROOF_NONCE_LEN)
              && mg_token_proof_mac(k, false, p->proof.client_nonce, p->proof.device_nonce, p->proof.client_mac);
    mg_token_proof_wipe(k, sizeof(k));
    if (!ok) {
        proof_reset(p);
        send_error(p, mg_command_token_proof, mg_error_code_busy);
        return;
    }
    memcpy(m + 2, p->proof.device_nonce, MG_TOKEN_PROOF_NONCE_LEN);
    p->proof.state = MG_PROOF_RESPONDED;
    /* Served at once, ahead of anything queued on Control: a stack that refuses it now keeps it at the front of
     * Control, retried by every pump (and before any other Control message),
     * and replaced by token_proof busy if it hasn't left in 1 s. */
    _Static_assert(sizeof(m) <= sizeof(p->proof_out.msg) && sizeof(m) <= MG_CTL_REPLY_MAX, "the answer fits its slot");
    memcpy(p->proof_out.msg, m, sizeof(m));
    p->proof_out.len = sizeof(m);
    if (sizeof(m) > link_max(p)) {
        p->ctlq.held_mtu++;   /* waits whole for the MTU exchange (then busy past MG_PROOF_ANSWER_MS) */
    }
    p->proof_out.busy = false;
    p->proof_out.since_ms = now_ms(p);
    mg_token_proof_wipe(m, sizeof(m));
    proof_flush(p);
}

/* The waiting proof answer (or its busy error): sent if the stack takes it;
 * true once nothing waits. Past MG_PROOF_ANSWER_MS the answer gives way to
 * token_proof busy (the exchange is reset: no stale answer after it). */
static bool proof_flush(mg_proto_t *p)
{
    if (!p->proof_out.len) {
        return true;
    }
    if (!p->connected) {
        memset(&p->proof_out, 0, sizeof(p->proof_out));
        return true;
    }
    if (!p->proof_out.busy && now_ms(p) - p->proof_out.since_ms >= MG_PROOF_ANSWER_MS) {
        proof_reset(p);   /* the client never saw this nonce: a confirm for it is refused */
        streams_end_unproven(p, false);   /* a re-proof that can't complete */
        uint8_t m[5] = { mg_command_error, mg_command_token_proof, mg_error_data_code, mg_error_code_busy, 0 };
        mg_token_proof_wipe(p->proof_out.msg, sizeof(p->proof_out.msg));
        memcpy(p->proof_out.msg, m, sizeof(m));
        p->proof_out.len = sizeof(m);
        p->proof_out.busy = true;
    }
    if (p->proof_out.len > link_max(p) || !raw(p, MG_CH_CONTROL, p->proof_out.msg, p->proof_out.len)) {
        return false;
    }
    p->ctl_sent++;
    mg_token_proof_wipe(&p->proof_out, sizeof(p->proof_out));
    return true;
}

bool mg_proto_proof_answer_pending(const mg_proto_t *p)
{
    return p->proof_out.len != 0;
}

static void send_proof_result(mg_proto_t *p, bool matched)
{
    uint8_t m[3] = { mg_command_token_proof, mg_token_proof_result, matched ? 1 : 0 };
    send_control(p, m, sizeof(m));
}

static void cmd_token_proof(mg_proto_t *p, const uint8_t *a, size_t n)
{
    if (!proof_supported(p)) {
        send_error(p, mg_command_token_proof, mg_error_code_unsupported);
        return;
    }
    if (n < 1) {
        send_error(p, mg_command_token_proof, mg_error_code_invalid_length);
        return;
    }
    switch (a[0]) {
    case mg_token_proof_challenge:
        proof_challenge(p, a + 1, n - 1);
        return;
    case mg_token_proof_confirm: {
        if (p->proof.state != MG_PROOF_RESPONDED) {
            send_error(p, mg_command_token_proof, mg_error_code_invalid_value);
            return;
        }
        if (n < 1 + MG_TOKEN_PROOF_MAC_LEN) {
            send_error(p, mg_command_token_proof, mg_error_code_invalid_length);
            return;
        }
        bool matched = mg_token_proof_equal(a + 1, p->proof.client_mac, MG_TOKEN_PROOF_MAC_LEN);
        if (matched) {
            /* One confirm per challenge. A match keeps its nonces for one
             * sealed hello (mg_proto_proof_take_nonces()). */
            mg_token_proof_wipe(p->proof.client_mac, sizeof(p->proof.client_mac));
            p->proof.nonces_held = true;
        } else {
            proof_reset(p);
        }
        p->proof.state = matched ? MG_PROOF_MATCHED : MG_PROOF_IDLE;
        if (matched && p->ptt_held) {
            p->ptt_held = false;   /* valid: the client's enable counts again (one sent meanwhile too) */
            p->ptt_enabled = p->ptt_asked;
            p->ptt_asked = false;
        }
        send_proof_result(p, matched);
        /* A capture held for this proof goes now, after the result (in order, as the link takes
         * it); a failed proof discards it. */
        if (matched) {
            output_release(p);
        } else {
            output_discard(p);
            streams_end_unproven(p, false);   /* a re-proof that failed: streams it admitted end */
        }
        return;
    }
    case mg_token_proof_clear:
        if (sealed_clear_declared(p)) {
            /* mg_session_cap_sealed_clear declared: the clear only as the sealed mg_session_clear, nothing
             * erased here (a relayed proof mustn't let a proxy erase the gadget); refused in the hello's order
             * (mgcommands.h): not_found, proof_required, insecure. */
            send_error(p, mg_command_token_proof, !have_key(p) ? mg_error_code_not_found
                                                  : p->proof.state != MG_PROOF_MATCHED ? mg_error_code_proof_required
                                                                                     : mg_error_code_insecure);
            return;
        }
        if (p->proof.state != MG_PROOF_MATCHED) {
            send_error(p, mg_command_token_proof, mg_error_code_proof_required);
            return;
        }
        send_proof_result(p, true);   /* answered first: the clear may end the connection */
        streams_end_unproven(p, true);   /* the token goes: streams it admitted end */
        proof_reset(p);
        if (session_supported(p)) {
            mg_session_reset(&p->session);   /* the credential it carried is gone */
            mg_session_set_mtu(&p->session, p->mtu);
        }
        creds_reset(p, true);
        p->ops.proof_clear(p->ops.ctx);
        output_discard(p);   /* the key is gone: a capture still to go never goes */
        return;
    default:
        send_error(p, mg_command_token_proof, mg_error_code_unsupported);
        return;
    }
}

bool mg_proto_clear_owed(const mg_proto_t *p)
{
    return session_supported(p) && mg_session_clear_owed(&p->session);
}

void mg_proto_clear_done(mg_proto_t *p, bool erased)
{
    if (session_supported(p)) {
        mg_session_clear_done(&p->session, erased);
    }
    if (creds_supported(p) && p->creds) {
        mgds_clear_done(p->creds, erased);
        mgds_poll(p->creds, p->ops.now_ms ? p->ops.now_ms(p->ops.ctx) : 0);   /* the answer out now */
    }
}

bool mg_proto_proof_matched(const mg_proto_t *p)
{
    return p->proof.state == MG_PROOF_MATCHED;
}

bool mg_proto_owner_ok(const mg_proto_t *p)
{
    return owner_ok(p);
}

bool mg_proto_proof_take_nonces(mg_proto_t *p, uint8_t client_nonce[MG_TOKEN_PROOF_NONCE_LEN],
                                uint8_t device_nonce[MG_TOKEN_PROOF_NONCE_LEN])
{
    if (p->proof.state != MG_PROOF_MATCHED || !p->proof.nonces_held) {
        return false;
    }
    memcpy(client_nonce, p->proof.client_nonce, MG_TOKEN_PROOF_NONCE_LEN);
    memcpy(device_nonce, p->proof.device_nonce, MG_TOKEN_PROOF_NONCE_LEN);
    /* One sealed session per proof: a second one would reuse its keystream. */
    mg_token_proof_wipe(p->proof.client_nonce, sizeof(p->proof.client_nonce));
    mg_token_proof_wipe(p->proof.device_nonce, sizeof(p->proof.device_nonce));
    p->proof.nonces_held = false;
    return true;
}

void mg_proto_proof_reset(mg_proto_t *p)
{
    streams_end_unproven(p, true);   /* the token this connection proved is gone: streams it admitted end */
    proof_reset(p);
    /* The pairing changed hands: a capture held for a proof, or a held live turn, was the last owner's and
     * never goes to the new one (a new challenge from the same owner keeps buffering: proof_reset alone). */
    output_drop(p, true);
    p->setup_changing = true;   /* a sealed clear taken and not yet run was the old setup's: dropped, unanswered */
    creds_reset(p, true);   /* keyed by the old K */
    if (session_supported(p)) {
        mg_session_reset(&p->session);   /* opened with the old credential */
        mg_session_set_mtu(&p->session, p->mtu);   /* the connection's MTU stays */
    }
    p->setup_changing = false;
}

/* ---- offline clips ------------------------------------------------------ */

static void queue_status(mg_proto_t *p)
{
    mg_queue_t *q = p->cfg.queue;
    /* Clips are the owner's voice: on a set-up gadget, none and no used bytes until this connection's proof
     * matched (mgcommands.h, Audio queue). */
    bool owner = owner_ok(p);
    uint8_t m[14] = { mg_command_audio_queue, mg_audio_queue_command_status, p->settings.queue_enabled };
    put32(m + 3, owner ? mg_queue_used(q) : 0);
    put32(m + 7, mg_queue_capacity(q));
    put16(m + 11, owner ? q->count : 0);
    send_control(p, m, 13);
}

static void cmd_audio_queue(mg_proto_t *p, const uint8_t *a, size_t n)
{
    mg_queue_t *q = p->cfg.queue;
    if (!q) {
        send_error(p, mg_command_audio_queue, mg_error_code_unsupported);
        return;
    }
    if (n < 1) {
        send_error(p, mg_command_audio_queue, mg_error_code_invalid_length);
        return;
    }
    switch (a[0]) {
    case mg_audio_queue_command_status:
        queue_status(p);
        return;
    case mg_audio_queue_command_clear:
        if (!owner_ok(p)) {
            send_error(p, mg_command_audio_queue, mg_error_code_proof_required);
            return;
        }
        if (!mg_proto_queue_forget(p)) {
            send_error(p, mg_command_audio_queue, mg_error_code_busy);
            return;
        }
        queue_status(p);
        return;
    case mg_audio_queue_command_clip_info:
    case mg_audio_queue_command_read_clip: {
        if (n < 3) {
            send_error(p, mg_command_audio_queue, mg_error_code_invalid_length);
            return;
        }
        if (!owner_ok(p)) {
            /* A set-up gadget's clips (and what they are) only to this connection once its proof matched. */
            send_error(p, mg_command_audio_queue, mg_error_code_proof_required);
            return;
        }
        uint16_t idx = le16(a + 1);
        const mg_clip_t *c = mg_queue_clip(q, idx);
        if (!c) {
            send_error(p, mg_command_audio_queue, mg_error_code_invalid_value);
            return;
        }
        if (a[0] == mg_audio_queue_command_clip_info) {
            uint8_t m[28] = { mg_command_audio_queue, mg_audio_queue_command_clip_info };
            put16(m + 2, idx);
            m[4] = c->codec;
            put32(m + 5, c->start_ms);
            put32(m + 9, c->bytes);
            /* The clip's format (change_data_type's params
             * 2-5). Clips are 16 kHz at the codec's default. */
            mg_audio_format_t f;
            if (!mg_codec_format(c->codec, &f)) {
                send_control(p, m, 13);
                return;
            }
            put16(m + 13, f.rate_hz);
            m[15] = f.channels;
            put16(m + 16, f.frame_bytes);
            put16(m + 18, f.frame_us);
            /* + start_unix_ms: the clip's wall-clock start, 0 if the clock wasn't set. */
            uint64_t unix_ms = mg_queue_clip_unix_ms(q, idx);   /* from the clip's header in flash */
            put32(m + 20, (uint32_t)unix_ms);
            put32(m + 24, (uint32_t)(unix_ms >> 32));
            send_control(p, m, 28);
            return;
        }
        if (!out_ok(p)) {
            /* A saved clip is mic audio: only to a central that passed this connection's token
             * proof. The clip stays in the queue for one that has. */
            send_error(p, mg_command_audio_queue, mg_error_code_proof_required);
            return;
        }
        if (p->live || mg_proto_capture_pending(p)) {
            /* Live push-to-talk (and what it still holds) wins over a download. */
            send_error(p, mg_command_audio_queue, mg_error_code_busy);
            return;
        }
        p->data_type = mg_data_type_audio_queue;
        send_data_type(p);
        p->transfer = true;
        p->transfer_index = idx;
        p->transfer_off = 0;
        return;
    }
    default:
        send_error(p, mg_command_audio_queue, mg_error_code_unsupported);
        return;
    }
}

/* ---- throughput test ----------------------------------------------------- */

static void put_le32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

void mg_tp_fill(uint8_t *buf, size_t len, uint32_t seq)
{
    if (len < 4) {
        return;
    }
    put_le32(buf, seq);
    for (size_t i = 4; i < len; i++) {
        buf[i] = (uint8_t)(seq + i);
    }
}

bool mg_tp_account(mg_tp_count_t *c, const uint8_t *buf, size_t len, uint32_t now_ms)
{
    if (len < 4) {
        return false;
    }
    uint32_t seq = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24;
    if (c->packets == 0) {
        c->first_ms = now_ms;
        c->next_seq = seq;
    }
    if (seq > c->next_seq) {
        c->gaps += seq - c->next_seq;
    }
    if (seq >= c->next_seq) {
        c->next_seq = seq + 1;
    }
    c->bytes += len;
    c->packets++;
    c->last_ms = now_ms;
    return true;
}

size_t mg_tp_report(const mg_tp_count_t *c, uint8_t out[19])
{
    out[0] = mg_command_device_action;
    out[1] = mg_device_action_throughput_test;
    out[2] = mg_throughput_test_stop;
    put_le32(&out[3], c->bytes);
    put_le32(&out[7], c->packets);
    put_le32(&out[11], c->packets ? c->last_ms - c->first_ms : 0);
    put_le32(&out[15], c->gaps);
    return 19;
}

/* Ends the running test; with report, tells the client. */
static void tp_finish(mg_proto_t *p, bool report)
{
    if (p->tp.mode) {
        p->tp.last = p->tp.mode;
    }
    p->tp.mode = 0;
    p->tp.armed = false;
    if (report) {
        uint8_t m[19];
        send_control(p, m, mg_tp_report(&p->tp.c, m));
    }
}

static bool tp_expired(const mg_proto_t *p)
{
    return p->tp.duration_ms && now_ms(p) - p->tp.start_ms >= p->tp.duration_ms;
}

/* Clock (mgcommands.h): set_unix_time and get_unix_time. */
static void cmd_clock(mg_proto_t *p, const uint8_t *a, size_t n)
{
    if (!p->ops.clock_set || !p->ops.clock_get) {
        send_error(p, mg_command_device_action, mg_error_code_unsupported);
        return;
    }
    if (a[0] == mg_device_action_get_unix_time && !proof_input_ok(p)) {
        /* The local time and offset are the owner's: a set-up release gadget's
         * unproven central gets proof_required. */
        send_error(p, mg_command_device_action, mg_error_code_proof_required);
        return;
    }
    if (a[0] == mg_device_action_get_unix_time) {
        int64_t ms = INT64_MIN;
        int16_t off = 0;
        uint8_t flags = 0;
        p->ops.clock_get(p->ops.ctx, &ms, &off, &flags);
        uint8_t m[13] = { mg_command_device_action, mg_device_action_report_unix_time };
        for (int i = 0; i < 8; i++) {
            m[2 + i] = (uint8_t)((uint64_t)ms >> (8 * i));
        }
        m[10] = (uint8_t)off;
        m[11] = (uint8_t)((uint16_t)off >> 8);
        m[12] = flags;
        send_control(p, m, sizeof(m));
        return;
    }
    if (n < 9) {
        send_error(p, mg_command_device_action, mg_error_code_invalid_length);
        return;
    }
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u |= (uint64_t)a[1 + i] << (8 * i);
    }
    int64_t ms = (int64_t)u;
    if (ms == INT64_MIN || ms < MG_CLOCK_FLOOR_MS || ms > MG_CLOCK_CEIL_MS) {
        /* mgcommands.h: a time before the clock's floor (2024-01-01T00:00:00Z) or past 9999-12-31T23:59:59Z is
         * refused; the clock keeps its time. */
        send_error(p, mg_command_device_action, mg_error_code_invalid_value);
        return;
    }
    bool has_off = n >= 11, has_flags = n >= 12;
    int16_t off = has_off ? (int16_t)(a[9] | a[10] << 8) : 0;
    uint8_t flags = has_flags ? (uint8_t)(a[11] & mg_clock_flag_24_hour) : 0;
    /* The time always applies; the offset and flags are kept across restarts only from a proven central on a
     * set-up gadget. No reply. */
    p->ops.clock_set(p->ops.ctx, ms, has_off, off, has_flags, flags, owner_ok(p));
}

size_t mg_battery_encode(const mg_battery_status_t *s, uint8_t out[MG_BATTERY_REPORT_LEN])
{
    size_t n = 0;
    out[n++] = mg_command_device_action;
    out[n++] = mg_device_action_report_battery_status;
    out[n++] = (uint8_t)s->voltage_mv;
    out[n++] = (uint8_t)(s->voltage_mv >> 8);
    out[n++] = (uint8_t)s->current_100ua;
    out[n++] = (uint8_t)((uint16_t)s->current_100ua >> 8);
    out[n++] = s->soc_percent;
    out[n++] = (uint8_t)s->temperature_c;
    out[n++] = (uint8_t)((uint16_t)s->temperature_c >> 8);
    out[n++] = (uint8_t)s->capacity_mah;
    out[n++] = (uint8_t)(s->capacity_mah >> 8);
    out[n++] = (uint8_t)s->full_capacity_mah;
    out[n++] = (uint8_t)(s->full_capacity_mah >> 8);
    for (int i = 0; i < 4; i++) {
        out[n++] = (uint8_t)(s->timestamp_ms >> (8 * i));
    }
    out[n++] = s->flags;
    return n;
}

static bool send_battery(mg_proto_t *p)
{
    mg_battery_status_t s = { 0 };
    if (!p->ops.battery_status(p->ops.ctx, &s)) {
        return false;
    }
    uint8_t m[MG_BATTERY_REPORT_LEN];
    send_control(p, m, mg_battery_encode(&s, m));
    return true;
}

void mg_proto_report_battery(mg_proto_t *p)
{
    if (p->ops.battery_status && mg_proto_ready(p)) {
        send_battery(p);
    }
}

static void cmd_device_action(mg_proto_t *p, const uint8_t *a, size_t n)
{
    if (n < 1) {
        send_error(p, mg_command_device_action, mg_error_code_invalid_length);
        return;
    }
    if (a[0] == mg_device_action_request_battery_status) {
        if (!p->ops.battery_status) {
            send_error(p, mg_command_device_action, mg_error_code_unsupported);
        } else if (!send_battery(p)) {
            send_error(p, mg_command_device_action, mg_error_code_busy);   /* no reading yet */
        }
        return;
    }
    if (a[0] == mg_device_action_power_off) {
        if (!power_off_offered(p)) {
            send_error(p, mg_command_device_action, mg_error_code_unsupported);
        } else if (!owner_ok(p)) {
            send_error(p, mg_command_device_action, mg_error_code_proof_required);
        } else {
            p->ops.power_off(p->ops.ctx);   /* no reply: the device goes */
        }
        return;
    }
    if (a[0] == mg_device_action_set_unix_time || a[0] == mg_device_action_get_unix_time) {
        cmd_clock(p, a, n);
        return;
    }
    if (a[0] == mg_device_action_haptic_play || a[0] == mg_device_action_haptic_stop) {
        /* Play (and its patterns) needs this connection's proof on a release build's set-up gadget; stop always
         * runs. */
        uint16_t err = !haptic_offered(p)                                              ? mg_error_code_unsupported
                       : a[0] == mg_device_action_haptic_play && !proof_input_ok(p) ? mg_error_code_proof_required
                                                                                     : p->ops.haptic(p->ops.ctx, a, n);
        if (err) {
            send_error(p, mg_command_device_action, err);
        }
        return;
    }
    if (a[0] != mg_device_action_throughput_test) {
        send_error(p, mg_command_device_action, mg_error_code_unsupported);
        return;
    }
    if (n < 2) {
        send_error(p, mg_command_device_action, mg_error_code_invalid_length);
        return;
    }
    uint8_t dir = a[1];
    if (dir == mg_throughput_test_stop) {
        tp_finish(p, true);
        return;
    }
    if (dir != mg_throughput_test_send && dir != mg_throughput_test_receive) {
        send_error(p, mg_command_device_action, mg_error_code_invalid_value);
        return;
    }
    /* Open on every build, set up or not, with no token proof (mgcommands.h, throughput_test). */
    mg_play_t *pl = p->cfg.play;
    bool playing = pl && (pl->state == MG_PLAY_PREBUFFER || pl->state == MG_PLAY_PLAYING);
    if (p->tp.mode || p->live || mg_proto_capture_pending(p) || p->audio_claimed || p->turn || playing) {
        send_error(p, mg_command_device_action, mg_error_code_busy);
        return;
    }
    memset(&p->tp.c, 0, sizeof(p->tp.c));
    p->tp.mode = dir;
    p->tp.armed = false;
    p->tp.seq = 0;
    p->tp.start_ms = now_ms(p);
    p->tp.duration_ms = n >= 4 ? 1000u * (uint32_t)(a[2] | a[3] << 8) : 0;
    if (dir == mg_throughput_test_send) {
        p->transfer = false;
        uint8_t m[2] = { mg_command_change_data_type, mg_data_type_throughput_test };
        send_control(p, m, sizeof(m));
        if (p->ops.play_kick) {
            p->ops.play_kick(p->ops.ctx);   /* wakes the worker to pump */
        }
    }
}

/* The send test: as many packets as the stack takes now. */
static bool tp_pump(mg_proto_t *p)
{
    uint8_t pkt[256];
    size_t n = mg_proto_data_payload(p);
    if (n > sizeof(pkt)) {
        n = sizeof(pkt);
    }
    for (int i = 0; i < 16 && p->tp.mode == mg_throughput_test_send; i++) {
        if (tp_expired(p) || n < 4) {
            tp_finish(p, true);
            break;
        }
        mg_tp_fill(pkt, n, p->tp.seq);
        if (!send_data(p, pkt, n)) {
            /* The stack is full: next time round, unless it has taken nothing for MG_TP_STALL_MS. */
            uint32_t since = p->tp.c.packets ? p->tp.c.last_ms : p->tp.start_ms;
            if (now_ms(p) - since >= MG_TP_STALL_MS) {
                p->tp.stalls++;
                tp_finish(p, true);
            }
            break;
        }
        uint32_t t = now_ms(p);
        if (p->tp.c.packets == 0) {
            p->tp.c.first_ms = t;
        }
        p->tp.c.bytes += n;
        p->tp.c.packets++;
        p->tp.c.last_ms = t;
        p->tp.seq++;
    }
    return p->tp.mode == mg_throughput_test_send;
}

bool mg_proto_queue_forget(mg_proto_t *p)
{
    p->transfer = false;
    return !p->cfg.queue || mg_queue_clear(p->cfg.queue);
}

bool mg_proto_transfer_active(const mg_proto_t *p)
{
    return p->transfer || p->tp.mode == mg_throughput_test_send;
}

bool mg_proto_pump(mg_proto_t *p)
{
    if (p->tp.mode == mg_throughput_test_send) {
        return tp_pump(p);
    }
    if (!p->transfer || !p->cfg.queue) {
        return false;
    }
    if (!out_ok(p)) {
        /* The proof stopped counting mid-download (a new challenge, a failed confirm, the bench switch off):
         * no more of the clip, which stays queued for a proven central. */
        p->transfer = false;
        send_error(p, mg_command_audio_queue, mg_error_code_proof_required);
        return false;
    }
    const mg_clip_t *c = mg_queue_clip(p->cfg.queue, p->transfer_index);
    if (!c || p->transfer_off >= c->bytes) {
        p->transfer = false;
        return false;
    }
    uint8_t chunk[256];
    size_t n = mg_proto_data_payload(p);
    if (n > sizeof(chunk)) {
        n = sizeof(chunk);
    }
    if (n > c->bytes - p->transfer_off) {
        n = c->bytes - p->transfer_off;
    }
    if (!n || !mg_queue_read(p->cfg.queue, p->transfer_index, p->transfer_off, chunk, n)) {
        p->transfer = false;
        return false;
    }
    if (send_data(p, chunk, n)) {
        p->transfer_off += n;
    }
    if (p->transfer_off >= c->bytes) {
        p->transfer = false;
    }
    return p->transfer;
}

void mg_proto_tick(mg_proto_t *p)
{
    if (p->tp.mode == mg_throughput_test_receive && tp_expired(p)) {
        tp_finish(p, true);
    }
}

/* ---- commands ----------------------------------------------------------- */

void mg_proto_press(mg_proto_t *p, uint8_t src)
{
    if (!mg_press_src_physical(src)) {
        return;   /* a console or Nordic UART word, a protocol command: never a press */
    }
    p->press_seen = true;
    p->press_ms = mono_ms(p);
}

bool mg_proto_press_window(const mg_proto_t *p)
{
    int64_t t = mono_ms(p);
    return p->press_seen && t >= p->press_ms && t - p->press_ms <= MG_PRESS_WINDOW_MS;
}

/* start_mic's press rule: a set-up device's release build takes a client's start_mic within MG_PRESS_WINDOW_MS of
 * a press it detected. */
static bool press_ok(const mg_proto_t *p)
{
    return !p->cfg.press_gate || !have_key(p) || mg_proto_press_window(p);
}

static void cmd_start_mic(mg_proto_t *p, const uint8_t *a, size_t n)
{
    /* The codec asked for if this device can capture it, else the
     * device's own (change_data_type names the one sent); never refused. */
    uint8_t codec = 0;
    p->start_mic_codec = 0;
    if (n >= 1 && a[0] && mg_codec_supported(a[0])) {
        codec = p->start_mic_codec = a[0];
    }
    /* Param 2 the rate, param 3 the kb/s (u16 each, optional): the
     * format is what the device can do nearest to it, never refused. */
    mg_audio_format_t fmt;
    mg_proto_mic_format(p, codec, n >= 3 ? le16(a + 1) : 0, n >= 5 ? le16(a + 3) : 0, &fmt);
    if (p->tp.mode) {
        send_error(p, mg_command_start_mic, mg_error_code_busy);
        return;
    }
    if (!out_ok(p) && (!have_key(p) || p->proof.state != MG_PROOF_RESPONDED)) {
        /* Before the proof a client's start_mic opens the mic only while a token-proof
         * challenge is outstanding on this connection (the app's own proof race), and is held for it; with
         * none outstanding, or no proof key (no proven central can exist), it's refused. */
        p->out_no_key += !have_key(p);
        p->out_start_refused++;
        send_error(p, mg_command_start_mic, mg_error_code_proof_required);
        return;
    }
    if (!press_ok(p)) {
        /* No press of the device's own lately: no capture, no mic ring. */
        send_error(p, mg_command_start_mic, mg_error_code_press_required);
        return;
    }
    p->transfer = false;   /* live audio wins over a download */
    if (p->ops.capture) {
        p->ops.capture(p->ops.ctx, true, &fmt);
    }
}

/*
 * From a client: the receive throughput test, or which stream the following
 * Data writes belong to while video and audio playback are both on.
 */
static void cmd_change_data_type(mg_proto_t *p, const uint8_t *d, size_t n)
{
    if (n < 2) {
        send_error(p, mg_command_change_data_type, mg_error_code_invalid_length);
        return;
    }
    /* The Data channel's selector: it always switches, silently, whatever
     * is on. Data for a type no open stream takes is dropped (data_in). */
    uint8_t type = d[1];
    if (type == mg_data_type_framed && p->framed) {
        return;   /* already framed: a repeated selection is a no-op (mgcommands.h) */
    }
    if (type == mg_data_type_throughput_test && p->tp.mode == mg_throughput_test_receive) {
        p->tp.armed = true;
    }
    if (p->rx_hold) {
        /* The episode after a device end is over: what it discarded, told once. */
        if (p->rx_hold_bytes && p->ops.rx_held) {
            p->ops.rx_held(p->ops.ctx, p->rx_hold_bytes);
        }
        p->rx_hold = false;
        p->rx_hold_bytes = 0;
    }
    /* framed: every following write carries its own type byte; any other type ends framed. */
    p->framed = type == mg_data_type_framed;
    p->framed_selects += p->framed;
    p->rx_switches += !p->framed;
    p->rx_type = type;
}

uint8_t mg_proto_turn_codec(const mg_proto_t *p)
{
    return mg_proto_capture_codec(p);
}

/* A recovery mode serves only its own command set. */
static void recovery_command(mg_proto_t *p, uint8_t cmd, const uint8_t *a, size_t an)
{
    switch (cmd) {
    case mg_command_request_status: {
        send_data_type(p);
        const uint8_t decl[6] = { mg_command_supported_features, mg_command_sub_feature, mg_command_request_status,
                                  p->cfg.recovery_mode, p->cfg.recovery_reason, p->cfg.recovery_flags };
        send_control(p, decl, sizeof(decl));   /* before the command list: the app learns first */
        const uint8_t list[3] = { mg_command_supported_features, mg_command_request_status, mg_command_device_action };
        send_control(p, list, sizeof(list));
        return;
    }
    case mg_command_device_action:
        if (an < 1) {
            send_error(p, cmd, mg_error_code_invalid_length);
        } else if (a[0] == mg_device_action_request_battery_status) {
            cmd_device_action(p, a, an);   /* display only */
        } else if (a[0] == mg_device_action_reboot) {
            if ((p->cfg.recovery_flags & mg_recovery_flag_restart) && p->ops.reboot) {
                p->ops.reboot(p->ops.ctx);   /* no answer: it disconnects cleanly and restarts */
            } else {
                send_error(p, cmd, mg_error_code_unsupported);
            }
        } else {
            send_error(p, cmd, mg_error_code_recovery);
        }
        return;
    default:
        send_error(p, cmd, mg_error_code_recovery);   /* token_proof too: never a response */
        return;
    }
}

static void command(mg_proto_t *p, const uint8_t *d, size_t n)
{
    if (!n) {
        return;
    }
    uint8_t cmd = d[0];
    const uint8_t *a = d + 1;
    size_t an = n - 1;
    if (p->cfg.recovery_mode) {
        recovery_command(p, cmd, a, an);
        return;
    }


    switch (cmd) {
    case mg_command_request_status:
        send_data_type(p);
        send_features(p);
        if (p->video.wanted && p->cfg.video && !p->video.active) {
            uint8_t m[3] = { mg_command_stream_video, mg_stream_video_request_streaming, 1 };
            send_control(p, m, sizeof(m));   /* a pending request, for a client that just arrived */
        }
        break;
    case mg_command_start_mic:
        cmd_start_mic(p, a, an);
        break;
    case mg_command_stop_mic:
        if (p->ops.capture) {
            p->ops.capture(p->ops.ctx, false, NULL);
        }
        break;
    case mg_command_set_mic_gain:
        if (an < 1) {
            send_error(p, cmd, mg_error_code_invalid_length);
        } else if (a[0] < 1 || a[0] > 100) {
            send_error(p, cmd, mg_error_code_invalid_value);
        } else if (p->ops.mic_gain) {
            p->mic_gain = a[0];   /* this connection only: undone at its end */
            p->ops.mic_gain(p->ops.ctx, a[0]);
        }
        break;
    case mg_command_get_settings:
        cmd_get_settings(p, a, an);
        break;
    case mg_command_set_settings:
        cmd_set_settings(p, a, an);
        break;
    case mg_command_audio_queue:
        cmd_audio_queue(p, a, an);
        break;
    case mg_command_stream_audio:
        cmd_stream_audio(p, a, an);
        break;
    case mg_command_stream_video:
        cmd_stream_video(p, a, an);
        break;
    case mg_command_token_proof:
        cmd_token_proof(p, a, an);
        break;
    case mg_command_session:
        if (creds_supported(p)) {
            mgds_rx(p->creds, d, n, p->ops.now_ms ? p->ops.now_ms(p->ops.ctx) : 0);
            if (!mgds_busy(p->creds)) {
                creds_drop_cache(p);
            }
        } else if (!session_supported(p)) {
            send_error(p, cmd, mg_error_code_unsupported);
        } else {
            mg_session_write(&p->session, a, an, mg_proto_proof_matched(p));
        }
        break;
    case mg_command_ui_input:
        cmd_ui_input(p, a, an);
        break;
    case mg_command_device_action:
        cmd_device_action(p, d + 1, n - 1);
        break;
    case mg_command_change_data_type:
        cmd_change_data_type(p, d, n);
        break;
    case mg_command_assistant_state:
        if (!p->cfg.assistant) {
            send_error(p, cmd, mg_error_code_unsupported);
        } else if (an < 1) {
            send_error(p, cmd, mg_error_code_invalid_length);
        } else if (a[0] != mg_assistant_state_idle && a[0] != mg_assistant_state_thinking
                   && a[0] != mg_assistant_state_responding && a[0] != mg_assistant_state_done
                   && a[0] != mg_assistant_state_error) {
            send_error(p, cmd, mg_error_code_invalid_value);
        } else {
            p->reports_state = true;
            if (p->ops.assistant_state) {
                p->ops.assistant_state(p->ops.ctx, a[0]);   /* no reply */
            }
        }
        break;
    default:
        send_error(p, cmd, mg_error_code_unsupported);
        break;
    }
}

void mg_proto_write(mg_proto_t *p, mg_ch_t ch, const uint8_t *data, size_t len)
{
    if (!p->connected) {
        return;
    }
    switch (ch) {
    case MG_CH_CONTROL:
        command_in_order(p, data, len);   /* after the replies already waiting (mg_cmdq_t) */
        break;
    case MG_CH_DATA:
        data_in(p, data, len);   /* stream audio, or throughput test data */
        break;
    default:
        break;
    }
}

/* ---- device session (mg_command_session) -------------------------------- */

bool mg_proto_session_offered(const mg_proto_t *p)
{
    return session_supported(p) || creds_supported(p);
}

bool mg_proto_session_open(const mg_proto_t *p)
{
    return session_supported(p) && p->connected && mg_session_open(&p->session);
}

bool mg_proto_session_sealed(const mg_proto_t *p)
{
    return session_supported(p) && p->connected && mg_session_sealed(&p->session);
}

uint32_t mg_proto_session_id(const mg_proto_t *p)
{
    return mg_proto_session_open(p) ? mg_session_id(&p->session) : 0;
}

void mg_proto_session_reask_marks(const mg_proto_t *p, uint32_t *hellos, uint32_t *saved)
{
    *hellos = *saved = 0;
    if (session_supported(p)) {
        mg_session_reask_marks(&p->session, hellos, saved);
    }
}

bool mg_proto_session_send(mg_proto_t *p, uint32_t session, const uint8_t *msg, size_t len)
{
    return session && mg_proto_session_id(p) == session && mg_session_send(&p->session, msg, len);
}

void mg_proto_session_nudge(mg_proto_t *p)
{
    if (mg_proto_session_open(p)) mg_session_nudge(&p->session);
}

size_t mg_proto_session_room(const mg_proto_t *p)
{
    return mg_proto_session_open(p) ? mg_session_room(&p->session) : 0;
}

uint32_t mg_proto_session_client_max(const mg_proto_t *p)
{
    return session_supported(p) ? mg_session_client_max(&p->session) : 0;
}

size_t mg_proto_session_take(mg_proto_t *p, uint32_t session, uint8_t *out, size_t cap)
{
    return session && mg_proto_session_id(p) == session ? mg_session_take(&p->session, out, cap) : 0;
}

void mg_proto_session_request(mg_proto_t *p, bool open)
{
    if (session_supported(p) && p->connected && p->session.hello) {
        mg_session_request(&p->session, open);
    }
}

bool mg_proto_session_take_update(mg_proto_t *p, char **access, char **refresh, uint64_t *expires, uint32_t *gen)
{
    return session_supported(p) && p->connected && mg_session_take_update(&p->session, access, refresh, expires, gen);
}

void mg_proto_session_update_saved(mg_proto_t *p, uint32_t gen, bool saved)
{
    if (session_supported(p) && p->connected) {
        mg_session_update_saved(&p->session, gen, saved);
    }
}

bool mg_proto_session_pump(mg_proto_t *p)
{
    if (!proof_flush(p) || !ctlq_flush(p)) {
        return true;   /* the token proof answer and waiting replies go ahead of the session's data */
    }
    if (creds_supported(p)) {
        /* Layer 2: what's owed, deferred work; the credential goes once
         * nothing is. */
        if (p->connected && mgds_busy(p->creds)) {
            mgds_poll(p->creds, p->ops.now_ms ? p->ops.now_ms(p->ops.ctx) : 0);
        }
        bool busy = p->connected && mgds_busy(p->creds);
        if (!busy) {
            creds_drop_cache(p);
        }
        return busy;
    }
    return session_supported(p) && p->connected && mg_session_pump(&p->session);
}

bool mg_proto_session_commit(mg_proto_t *p)
{
    return session_supported(p) && p->connected && mg_session_commit(&p->session);
}

/* ---- the audio side ----------------------------------------------------- */

bool mg_proto_ready(const mg_proto_t *p)
{
    if (!p->connected) {
        return false;
    }
    return p->sub[MG_CH_CONTROL] && p->sub[MG_CH_DATA];
}

mg_link_t mg_proto_link(mg_proto_t *p, bool set_up)
{
    if (mg_proto_ready(p)) {
        p->ever_ready = true;
        return p->ptt_enabled ? MG_LINK_READY : MG_LINK_SESSION;
    }
    if (p->connected) {
        return MG_LINK_CONNECTING;
    }
    return p->ever_ready || set_up ? MG_LINK_DISCONNECTED : MG_LINK_NEVER;
}

const char *mg_link_name(mg_link_t link)
{
    switch (link) {
    case MG_LINK_NEVER: return "never connected";
    case MG_LINK_DISCONNECTED: return "disconnected";
    case MG_LINK_CONNECTING: return "connecting";
    case MG_LINK_SESSION: return "app connected";
    default: return "ready";
    }
}

bool mg_proto_ptt_enabled(const mg_proto_t *p)
{
    return p->ptt_enabled;
}

bool mg_proto_take_ptt_cleared(mg_proto_t *p)
{
    bool r = p->ptt_cleared_note;
    p->ptt_cleared_note = false;
    return r;
}

bool mg_proto_haptics(const mg_proto_t *p)
{
    return p->cfg.haptics && p->haptics;
}

uint8_t mg_proto_capture_codec(const mg_proto_t *p)
{
    /* Push-to-talk turns: the audio_codec setting, whatever a start_mic chose
     * (mgcommands.h, Push-to-talk: a client may not decode another on a turn). */
    (void)p->start_mic_codec;
    return p->settings.codec;
}

size_t mg_proto_data_payload(const mg_proto_t *p)
{
    size_t n = p->mtu > 3 ? (size_t)p->mtu - 3 : 0;
    return n > MG_DATA_PAYLOAD_MAX ? MG_DATA_PAYLOAD_MAX : n;
}

/* A capture may queue now: a ready client, or a held live turn's link (before the client subscribes). */
static bool can_capture(const mg_proto_t *p)
{
    return mg_proto_ready(p) || (p->connected && p->capq.held_turn);
}

bool mg_proto_gesture(mg_proto_t *p, uint8_t gesture)
{
    if (!can_capture(p) || p->cfg.recovery_mode) {
        return false;
    }
    /* At once when nothing of a capture's control waits (mgcommands.h):
     * ahead of held audio and queued replies, so a
     * release is heard when it happens. Behind a gesture, change_data_type or
     * end still queued, or when the stack refuses it, it waits in the
     * capture's outbox in order (and governs what follows), never lost. */
    uint8_t m[2] = { mg_command_gesture, gesture };
    if (!p->capq.nctl && !p->capq.gate && !p->capq.held_turn && send_control_now(p, m, sizeof(m))) {
        return true;
    }
    return capq_control(p, m, sizeof(m), 0, false);   /* a held turn's waits there (mg_proto_turn_hold) */
}

bool mg_proto_live_begin(mg_proto_t *p, uint8_t codec)
{
    mg_audio_format_t f;
    return mg_codec_format(codec, &f) && mg_proto_live_begin_fmt(p, &f);
}

bool mg_proto_live_begin_fmt(mg_proto_t *p, const mg_audio_format_t *fmt)
{
    if (!can_capture(p) || !mg_codec_supported(fmt->type) || p->cfg.recovery_mode) {
        return false;
    }
    if (!out_ok(p) && (p->capq.proof_full || !have_key(p))) {
        /* No proof key, so no proof can ever match (nothing for a central), or a hold that
         * filled before its proof (it keeps what it has; nothing more starts). */
        p->out_no_key += !p->capq.proof_full;
        return false;
    }
    p->transfer = false;
    if (p->tp.mode == mg_throughput_test_send) {
        tp_finish(p, true);   /* push-to-talk wins over a test */
    }
    if (!p->capq.gate && p->claim_deferred && p->turn) {
        /* A turn that isn't held follows a dropped one with the speaker still claimed (a press in its
         * post-roll): the claim the dropped turn deferred happens now, after this turn's button_down and
         * before its change_data_type, a turn's usual order. */
        p->claim_deferred = false;
        claim_now(p);
    }
    if (!out_ok(p) && !p->capq.gate && !p->capq.held_turn && !p->pre_set) {
        p->pre_set = true;   /* request_status keeps saying this until the proof */
        p->pre_data_type = p->data_type;
        p->pre_data_fmt = p->data_fmt;
    }
    p->data_type = fmt->type;
    p->data_fmt = *fmt;
    if (p->capq.held_turn) {
        p->capq.ht_fmt = *fmt;   /* what its clip would be */
    }
    p->live = true;
    /* change_data_type through the capture's outbox: in order after what an
     * earlier utterance still holds, and never lost to a full stack. */
    capq_ensure(p, fmt);
    uint8_t m[10];
    size_t n = mg_codec_label(&p->data_fmt, m);
    capq_control(p, m, n, fmt->frame_bytes, false);
    return true;
}

void mg_proto_turn_format(const mg_proto_t *p, mg_audio_format_t *out)
{
    uint8_t codec = mg_proto_turn_codec(p);
    uint16_t rate = capture_rate_listed(p, codec, p->settings.capture_rate) ? p->settings.capture_rate : MG_CODEC_RATE;
    if (!mg_codec_format_at(codec, rate, p->settings.capture_kbps, out)) {
        mg_codec_format(codec, out);
    }
    fit_link(p, out);
}

bool mg_proto_live(const mg_proto_t *p)
{
    return p->live;
}

bool mg_proto_send_data(mg_proto_t *p, const uint8_t *data, size_t len)
{
    if (!p->live || !can_capture(p)) {
        return false;
    }
    bool ok = capq_push(p, data, len);   /* false: the proof hold is full: the capture stops */
    capture_pump(p, false);
    return ok;
}

void mg_proto_live_end(mg_proto_t *p, bool notify_stop)
{
    if (!p->live) {
        return;
    }
    p->live = false;
    if (!mg_proto_output_bench(p) && (!out_ok(p) || p->capq.held_turn)) {
        stale_arm(p);   /* held at its end: released within MG_OUTPUT_STALE_MS or discarded */
    }
    if (notify_stop && can_capture(p)) {
        /* The end of the utterance: after the audio held before it, retried
         * until it goes or the link drops, never dropped (mg_capq_t). */
        uint8_t m = mg_command_stop_mic;
        capq_control(p, &m, 1, 0, true);
    } else {
        capture_pump(p, false);
    }
}

/* ---- the held live turn (mg_proto.h; switches in mg_config.h) ------- */

#define MG_HELD_TURN_QUEUE_ROOM 8192   /* the clip queue keeps room for the rest of the turn (mg_voice's press minimum) */

bool mg_proto_has_key(const mg_proto_t *p)
{
    return have_key(p);
}

bool mg_proto_capture_whole(const mg_proto_t *p)
{
    return p->capq.gate || p->capq.held_turn || !out_ok(p);
}

bool mg_proto_held_turn_begin(mg_proto_t *p, uint64_t unix_ms)
{
    mg_capq_t *q = &p->capq;
    if (!p->connected || q->held_turn || q->gate || p->cfg.recovery_mode) {
        return false;
    }
    q->held_turn = true;
    q->ht_spill = false;
    q->ht_at = q->wseq;
    q->ht_wfb = q->wfb;
    q->ht_ms = now_ms(p);
    q->ht_unix_ms = unix_ms;
    memset(&q->ht_fmt, 0, sizeof(q->ht_fmt));
    p->gate_data_type = p->data_type;   /* request_status says the type from before it, until it goes live */
    p->gate_data_fmt = p->data_fmt;
    return true;
}

bool mg_proto_held_turn(const mg_proto_t *p)
{
    return p->capq.held_turn;
}

bool mg_proto_held_turn_due(const mg_proto_t *p)
{
    const mg_capq_t *q = &p->capq;
    return q->held_turn && (q->ht_spill || now_ms(p) - q->ht_ms >= MG_HELD_TURN_MS);
}

uint32_t mg_proto_held_turn_bytes(const mg_proto_t *p)
{
    const mg_capq_t *q = &p->capq;
    if (!q->held_turn) {
        return 0;
    }
    uint32_t from = (int32_t)(q->ht_at - q->rseq) > 0 ? q->ht_at : q->rseq;
    return q->wseq - from;
}

bool mg_proto_held_turn_queueable(const mg_proto_t *p)
{
    if (!p->capq.held_turn || !p->cfg.queue || !p->settings.queue_enabled) {
        return false;
    }
    /* A clip is SBC at 16 kHz, the format a held turn records (MG_HELD_TURN_SBC). */
    mg_audio_format_t clip;
    const mg_audio_format_t *f = &p->capq.ht_fmt;
    if (f->type) {
        if (!mg_codec_format(mg_data_type_audio_sbc, &clip) || f->type != clip.type || f->rate_hz != clip.rate_hz ||
            f->frame_bytes != clip.frame_bytes || f->frame_us != clip.frame_us) {
            return false;
        }
    }
    return mg_queue_fits(p->cfg.queue, mg_proto_held_turn_bytes(p) + MG_HELD_TURN_QUEUE_ROOM);
}

void mg_proto_held_turn_voice(mg_proto_t *p, bool on)
{
    p->capq.ht_voice = on;
}

/* Proven and ready (held_turn_ready): the turn is an ordinary live turn from here, everything it held going
 * in order as the link takes it (button_down, change_data_type, the audio, its end). */
static void held_turn_release(mg_proto_t *p)
{
    mg_capq_t *q = &p->capq;
    q->held_turn = false;
    q->ht_spill = false;
    q->proof_full = false;
    q->stale_armed = false;
    for (uint8_t i = 0; i < q->nctl; i++) {
        q->ctl[i].held = false;
    }
    q->ht_live++;
    p->out_note = MG_OUT_NOTE_RELEASED;
    p->out_note_bytes = q->wseq - (((int32_t)(q->ht_at - q->rseq) > 0) ? q->ht_at : q->rseq);
}

/* The link went with a held turn the clip queue can take: its audio stays (and nothing else), for
 * mg_proto_held_turn_take. */
static void held_turn_keep(mg_proto_t *p)
{
    mg_capq_t *q = &p->capq;
    uint32_t from = (int32_t)(q->ht_at - q->rseq) > 0 ? q->ht_at : q->rseq;
    if (q->cap) {
        q->rpos = (q->rpos + (from - q->rseq)) % q->cap;
    }
    q->rseq = from;
    q->nctl = 0;
    q->nlevels = 0;
    memset(&q->next_level, 0, sizeof(q->next_level));
    q->utt_sent = q->utt_dropped = q->utt_held_max = 0;
    q->ht_spill = true;
}

size_t mg_proto_held_turn_take(mg_proto_t *p, uint8_t *out, size_t cap)
{
    mg_capq_t *q = &p->capq;
    if (!q->held_turn) {
        return 0;
    }
    uint32_t from = (int32_t)(q->ht_at - q->rseq) > 0 ? q->ht_at : q->rseq;
    uint32_t n = q->wseq - from;
    size_t got = 0;
    if (out && q->cap && n) {
        got = n < cap ? n : cap;
        size_t pos = (q->rpos + (from - q->rseq)) % q->cap;
        size_t first = got < q->cap - pos ? got : q->cap - pos;
        memcpy(out, q->buf + pos, first);
        memcpy(out + first, q->buf, got - first);
    }
    /* Everything it queued goes, unsent; what came before it stays, in order. */
    uint8_t k = 0;
    for (uint8_t i = 0; i < q->nctl; i++) {
        if (!q->ctl[i].held) {
            q->ctl[k++] = q->ctl[i];
        } else if (q->ctl[i].end && q->nlevels) {
            q->nlevels--;
        }
    }
    q->nctl = k;
    q->wseq = from;
    q->wfb = q->ht_wfb;
    q->held_turn = q->ht_spill = q->ht_voice = false;
    q->proof_full = false;
    q->stale_armed = false;
    memset(&q->next_level, 0, sizeof(q->next_level));
    p->live = false;
    p->data_type = p->gate_data_type;
    p->data_fmt = p->gate_data_fmt;
    if (out) {
        q->ht_queued++;
        p->out_note = MG_OUT_NOTE_QUEUED;
        p->out_note_bytes = (uint32_t)got;
    }
    if (p->connected) {
        capq_release(p, false);
    } else {
        capq_reset(p);   /* the link's gone: nothing else was kept */
    }
    return got;
}

/* ---- the bench guard's held turn (ptt.minrise; mg_proto.h) --------------- */

bool mg_proto_turn_hold(mg_proto_t *p, uint8_t min_rise_db)
{
    mg_capq_t *q = &p->capq;
    if (!min_rise_db || q->gate || q->held_turn || !mg_proto_ready(p) || p->cfg.recovery_mode) {
        return false;
    }
    q->gate = true;
    q->gate_db = min_rise_db;
    q->gate_at = q->wseq;
    q->gate_wfb = q->wfb;
    q->gate_dropped = q->utt_dropped;
    q->gate_rise = 0.0f;
    p->gate_data_type = p->data_type;
    p->gate_data_fmt = p->data_fmt;
    return true;
}

bool mg_proto_turn_held(const mg_proto_t *p)
{
    return p->capq.gate;
}

bool mg_proto_turn_rise(mg_proto_t *p, const mg_capture_level_t *lv)
{
    mg_capq_t *q = &p->capq;
    if (!q->gate || !lv->valid) {
        return false;
    }
    /* The utterance summary's own numbers: loud1s (rms where there's no ring) over the floor. */
    q->gate_rise = (lv->loud_valid ? lv->loud1s_db : lv->rms_db) - lv->floor_db;
    if (q->gate_rise < (float)q->gate_db) {
        return false;
    }
    q->gate = false;
    if (p->claim_deferred && p->turn) {
        /* The order a turn that isn't held has: its button_down, then its claim (the reply talked over goes:
         * stop_streaming drop), then change_data_type and the audio. The turn's first
         * held message goes alone (what follows it waits, held), then the claim. */
        for (uint8_t i = 0; i < q->nctl; i++) {
            if (q->ctl[i].held) {
                q->ctl[i].held = false;
                break;
            }
        }
        capture_pump(p, false);
        claim_now(p);
    }
    p->claim_deferred = false;
    for (uint8_t i = 0; i < q->nctl; i++) {
        q->ctl[i].held = false;
    }
    capture_pump(p, false);   /* change_data_type, then the held audio as a burst, as the link takes it */
    return true;
}

bool mg_proto_turn_drop(mg_proto_t *p, bool nothing_heard)
{
    mg_capq_t *q = &p->capq;
    if (!q->gate) {
        return false;
    }
    /* Everything the turn queued goes, unsent; what came before it stays, in order. */
    uint8_t k = 0;
    for (uint8_t i = 0; i < q->nctl; i++) {
        if (!q->ctl[i].held) {
            q->ctl[k++] = q->ctl[i];
        } else if (q->ctl[i].end && q->nlevels) {
            q->nlevels--;   /* its level was queued last (a client's stop_mic ended it) */
        }
    }
    q->nctl = k;
    q->wseq = (int32_t)(q->gate_at - q->rseq) > 0 ? q->gate_at : q->rseq;   /* overflow ate into it: nothing of it left */
    q->wfb = q->gate_wfb;
    q->utt_dropped = q->gate_dropped;
    memset(&q->next_level, 0, sizeof(q->next_level));
    q->gate = false;
    p->live = false;
    p->data_type = p->gate_data_type;
    p->data_fmt = p->gate_data_fmt;
    if (nothing_heard) {
        p->ptt_nothing++;
    }
    capq_release(p, false);
    return true;
}

void mg_proto_haptic_status(mg_proto_t *p, uint8_t id, uint8_t status, uint8_t reason)
{
    if (!mg_proto_ready(p)) {
        return;
    }
    uint8_t m[5] = { mg_command_device_action, mg_device_action_haptic_play, id, status, reason };
    send_control(p, m, sizeof(m));
}
