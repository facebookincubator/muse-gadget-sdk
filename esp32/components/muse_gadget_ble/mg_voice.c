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

#include "mg_voice.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "mg_ble.h"
#include "mg_ble_priv.h"
#include "mg_codec.h"
#include "mg_config.h"
#include "mg_press_src.h"
#include "mgcommands.h"

static const char *TAG = "mg.voice";

#define SLICE 320            /* samples encoded at a time: at most 3 SBC or 2 LC3 frames */
#define OUT_CAP 512
#define QUEUE_MIN_ROOM 8192  /* a press needs at least this much queue left */
#define STAGE_MIN_S 3        /* without PSRAM: stage at least this much of a clip, or don't record */
#define STAGE_HEAP_KEEP (48 * 1024)   /* ...and leave this much internal RAM free */


#if MG_STANDALONE || CONFIG_MUSE_GADGET_BLE_ROUTE_BLE_ONLY
#define POLICY MG_POLICY_BLE_ONLY   /* standalone: there's no Wi-Fi path */
#elif CONFIG_MUSE_GADGET_BLE_ROUTE_WIFI_ONLY
#define POLICY MG_POLICY_WIFI_ONLY
#else
#define POLICY MG_POLICY_AUTO
#endif

static mg_encoder_t *s_enc;
static mg_route_t s_route = MG_ROUTE_NONE;
static bool s_from_client;
static int64_t s_start_us;
static uint32_t s_start_ms;
static uint64_t s_start_unix_ms;   /* a clip's wall-clock start (0: the clock isn't set) */
static uint8_t s_out[OUT_CAP];  /* one slice, encoded (BLE): into the capture's outbox at once */
static uint8_t *s_clip;        /* the clip being recorded (QUEUE) */
static size_t s_clip_len, s_clip_cap;
static uint32_t s_encoded;     /* bytes of this capture handed to the outbox */
static mg_capture_meter_t s_meter;   /* its level, for the summary (rms, peak, floor, loud1s, clipped) */
static bool s_held;            /* the outbox held audio after the last slice */
/* The bench guard (ptt.minrise, mg_proto_turn_hold): N dB, 0 off. RAM only,
 * never stored: off at every boot, so a user's quiet speech is never dropped. */
static volatile uint8_t s_minrise_db;
static bool s_gate;            /* this turn is held until it rises (or dropped at its end) */
static bool s_dropped;         /* the last turn ended held: nothing of it was sent */
static bool s_queued;          /* the last turn (a held live turn) went to the clip queue */

static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

mg_route_t mg_voice_route(bool wifi_ready)
{
    if (!mg_active()) {
        /* As if the option were off; standalone has nowhere else to send it. */
        return MG_STANDALONE ? MG_ROUTE_NONE : MG_ROUTE_WIFI;
    }
#if MG_STANDALONE
    wifi_ready = false;
#endif
    mg_route_inputs_t in = { .wifi_ready = wifi_ready };
    mg_lock();
    const mg_proto_t *p = mg_proto_get();
    mg_queue_t *q = mg_queue_get();
    in.client_connected = p->connected;
    in.client_ready = mg_proto_ready(p);
    in.ptt_enabled = mg_proto_ptt_enabled(p);
    in.queue_available = q && mg_queue_fits(q, QUEUE_MIN_ROOM);
    in.queue_enabled = mg_proto_settings(p)->queue_enabled;
    /* A press during the connect burst (not proven or not ready) is a held live turn on a set-up gadget. */
    in.unproven = !mg_proto_output_ok(p);
    in.held_ok = in.client_connected && mg_proto_has_key(p);
    mg_unlock();
    if (in.held_ok && !(in.queue_available && in.queue_enabled)) {
        /* Only on a board that can hold it: a clip queue, or a hold of
         * MG_HELD_TURN_MIN_HOLD_MS of the held turn's SBC; else the burst's press stays gesture-only. */
        mg_audio_format_t sbc;
        in.held_ok = mg_codec_format(mg_data_type_audio_sbc, &sbc) &&
                     mg_ble_can_hold((size_t)sbc.frame_bytes * (MG_HELD_TURN_MIN_HOLD_MS * 1000u / sbc.frame_us));
    }
    mg_route_t r = mg_route_decide(POLICY, &in);
    ESP_LOGI(TAG, "press goes to %s", mg_route_name(r));
    return r;
}

bool mg_voice_gesture(bool down)
{
    if (!mg_active()) {
        return false;
    }
    mg_lock();
    bool held = mg_proto_turn_held(mg_proto_get());   /* the bench guard's: the client hears of it at its release */
    /* A held live turn's press, or any before the proof, doesn't move the link. */
    bool quiet = mg_proto_held_turn(mg_proto_get()) || !mg_proto_capture_visible(mg_proto_get());
    mg_unlock();
    if (down) {
        if (!quiet) {
            mg_ble_link_activity();   /* a talk press: the link listens at every event */
        }
        if (!held) {
            mg_haptic_interrupt(mg_haptic_reason_in_use);   /* a turn's button_down cuts a client's pattern */
        }
    }
    mg_lock();
    bool sent = mg_proto_gesture(mg_proto_get(), down ? mg_gesture_button_down : mg_gesture_button_up);
    mg_unlock();
    if (held) {
        return sent;   /* held: its link turn comes with its release (released()), if it rises */
    }
    if (down && sent && !quiet) {
        mg_ble_link_turn();   /* the gadget's own turn: active until its reply has played */
    }
    return sent;
}

void mg_voice_set_minrise(uint8_t db)
{
    s_minrise_db = db;
}

uint8_t mg_voice_minrise(void)
{
    return s_minrise_db;
}

uint32_t mg_voice_nothing_heard(void)
{
    if (!mg_active()) {
        return 0;
    }
    mg_lock();
    uint32_t n = mg_proto_get()->ptt_nothing;
    mg_unlock();
    return n;
}

bool mg_voice_hold(void)
{
    uint8_t db = s_minrise_db;
    if (!mg_active() || !db) {
        return false;
    }
    mg_lock();
    bool ok = mg_proto_turn_hold(mg_proto_get(), db);
    mg_unlock();
    if (ok) {
        ESP_LOGI(TAG, "ptt: held until it rises %u dB over its floor (ptt.minrise)", (unsigned)db);
    }
    return ok;
}

bool mg_voice_turn_dropped(void)
{
    return s_dropped;
}

bool mg_voice_held_begin(void)
{
    if (!mg_active()) {
        return false;
    }
    uint64_t unix_ms = (uint64_t)mg_unix_ms_now();
    mg_lock();
    bool ok = mg_proto_held_turn_begin(mg_proto_get(), unix_ms);
    if (ok) {
        mg_proto_held_turn_voice(mg_proto_get(), true);
    }
    mg_unlock();
    if (ok) {
        ESP_LOGI(TAG, "ptt: a held live turn (the client isn't proven and ready yet): live once it is, the clip queue "
                      "after %u ms", (unsigned)MG_HELD_TURN_MS);
    }
    return ok;
}

bool mg_voice_turn_queued(void)
{
    return s_queued;
}

/* A clip buffer for the rest of a turn (MG_MAX_UTTERANCE_S of f): PSRAM, else what internal RAM can spare
 * (at least STAGE_MIN_S and `need`). 0: none. */
static size_t clip_alloc(const mg_audio_format_t *f, size_t need)
{
    const size_t per_s = 1000000u / f->frame_us * f->frame_bytes;
    s_clip_cap = (size_t)MG_MAX_UTTERANCE_S * per_s;
    if (s_clip_cap < need) {
        s_clip_cap = need;
    }
    s_clip = heap_caps_malloc(s_clip_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_clip) {
        /* No PSRAM: stage what internal RAM can spare (about 8 s of SBC in 60 KB). */
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t cap = largest > STAGE_HEAP_KEEP ? largest - STAGE_HEAP_KEEP : 0;
        cap = cap < s_clip_cap ? cap / f->frame_bytes * f->frame_bytes : s_clip_cap;
        size_t min = STAGE_MIN_S * per_s > need ? STAGE_MIN_S * per_s : need;
        if (cap >= min && (s_clip = heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT))) {
            s_clip_cap = cap;
            ESP_LOGI(TAG, "staging up to %.1f s of the clip in internal RAM", (double)cap / per_s);
        }
    }
    return s_clip ? s_clip_cap : 0;
}

static void released(void);

/* A held live turn not proven and ready in time (or its link gone): what it held becomes the start of
 * an offline clip, and the capture goes on into it (the queue route from here). False: no memory (dropped). */
static bool held_to_clip(void)
{
    mg_lock();
    uint32_t n = mg_proto_held_turn_bytes(mg_proto_get());
    mg_unlock();
    if (!clip_alloc(&s_enc->fmt, n + s_enc->fmt.frame_bytes * 3u)) {
        ESP_LOGW(TAG, "held live turn: no memory for its clip (%u bytes held)", (unsigned)n);
        mg_lock();
        mg_proto_held_turn_take(mg_proto_get(), NULL, 0);
        mg_unlock();
        return false;
    }
    mg_lock();
    /* The allocation ran unlocked: the turn may have gone live (or been taken) meanwhile. */
    bool still = mg_proto_held_turn(mg_proto_get());
    bool live = mg_proto_live(mg_proto_get());
    if (still) {
        s_clip_len = mg_proto_held_turn_take(mg_proto_get(), s_clip, s_clip_cap);
    }
    mg_unlock();
    if (!still) {
        free(s_clip);
        s_clip = NULL;
        if (!live) {
            return false;
        }
        s_route = MG_ROUTE_BLE;   /* as the caller's own release: the whole turn live, none of it a clip */
        ESP_LOGI(TAG, "held live turn: proven and ready as its clip was allocated: live");
        released();
        return true;
    }
    s_route = MG_ROUTE_QUEUE;
    s_queued = true;
    ESP_LOGI(TAG, "held live turn: not proven and ready in %u ms: %u bytes held start an offline clip, the turn goes "
                  "on into it", (unsigned)MG_HELD_TURN_MS, (unsigned)s_clip_len);
    return true;
}

/* The held turn rose (mg_proto_turn_rise released it): what its button_down
 * does at once on a turn that isn't held. */
static void released(void)
{
    mg_haptic_interrupt(mg_haptic_reason_in_use);
    mg_ble_link_turn();
    mg_ble_kick();   /* the worker drains the burst every 5 ms too */
    mg_play_kick();  /* the claim: the reply talked over goes (mg_proto_claim_turn) */
}

bool mg_voice_client_reports_state(void)
{
    if (!mg_active()) {
        return false;
    }
    mg_lock();
    bool r = mg_proto_get()->reports_state;
    mg_unlock();
    return r;
}

bool mg_voice_client_linked(void)
{
    if (!mg_active()) {
        return false;
    }
    mg_lock();
    bool r = mg_proto_get()->connected;
    mg_unlock();
    return r;
}

void mg_voice_face(const char *what)
{
    ESP_LOGI("mg.face", "%s", what);
}

void mg_voice_claim_turn(bool on)
{
    if (!mg_active()) {
        return;
    }
    mg_lock();
    mg_proto_claim_turn(mg_proto_get(), on);
    mg_unlock();
    mg_play_kick();
}

void mg_voice_barge_pause(bool on)
{
    if (!mg_active()) {
        return;
    }
    mg_lock();
    mg_proto_barge_pause(mg_proto_get(), on);
    mg_unlock();
    mg_play_kick();
}

void mg_voice_claim(bool claim)
{
    if (!mg_active()) {
        return;
    }
    mg_lock();
    mg_proto_claim_audio(mg_proto_get(), claim);
    mg_unlock();
    mg_play_kick();
}

bool mg_voice_playing(void)
{
    if (!mg_active()) {
        return false;
    }
    mg_lock();
    const mg_play_t *pl = mg_proto_get()->cfg.play;
    bool r = pl && mg_play_active(pl);
    mg_unlock();
    return r;
}

bool mg_voice_audible(void)
{
    if (!mg_active()) {
        return false;
    }
    mg_lock();
    const mg_play_t *pl = mg_proto_get()->cfg.play;
    bool r = pl && mg_play_audible(pl);
    mg_unlock();
    return r;
}

bool mg_voice_capture_requested(void)
{
    if (!mg_active()) {
        return false;
    }
    mg_lock();
    mg_capture_req_t *req = mg_capture_req();
    /* Armed by a start_mic that rode on a press (mg_proto press_ok): a capture that hasn't begun within the press's
     * window never begins (the link wasn't ready, the voice task was busy): the client asks again after a new
     * press. On the 64-bit clock, so it never comes back around. */
    bool stale = req->requested && esp_timer_get_time() - req->armed_us > MG_PRESS_WINDOW_MS * 1000LL;
    if (stale) {
        req->requested = false;
    }
    bool r = req->requested && mg_proto_ready(mg_proto_get());
    mg_unlock();
    if (stale) {
        ESP_LOGI(TAG, "client capture not begun within %u ms of its start_mic: dropped", (unsigned)MG_PRESS_WINDOW_MS);
    }
    return r;
}

/* The rate the voice loop hands over (muse_audio's): every capture here is
 * at it until the board's mic runs at 24 kHz (cfg.capture_24k). */
#define MG_MIC_RATE_HZ 16000

static bool begin(mg_route_t route, bool from_client);

bool mg_voice_begin(mg_route_t route, bool from_client)
{
    if (!mg_active()) {
        return false;
    }
    mg_lock();
    bool quiet = route == MG_ROUTE_HELD || !mg_proto_capture_visible(mg_proto_get());
    mg_unlock();
    if (!quiet) {
        mg_ble_link_action("push-to-talk capture");   /* link modes: active; not for a held capture */
    }
    s_dropped = false;
    s_queued = false;
    if (begin(route, from_client)) {
        mg_lock();
        s_gate = !from_client && route == MG_ROUTE_BLE && mg_proto_turn_held(mg_proto_get());
        mg_unlock();
        return true;
    }
    mg_lock();
    mg_proto_turn_drop(mg_proto_get(), false);   /* a held turn that couldn't start: none of it goes */
    mg_proto_held_turn_take(mg_proto_get(), NULL, 0);   /* ...nor a held live turn's */
    mg_unlock();
    s_gate = false;
    if (from_client) {
        /* Don't keep asking the voice task for a capture that can't start. */
        mg_lock();
        mg_capture_req()->requested = false;
        mg_unlock();
    }
    return false;
}

static bool begin(mg_route_t route, bool from_client)
{
    if (route != MG_ROUTE_BLE && route != MG_ROUTE_QUEUE && route != MG_ROUTE_HELD) {
        return false;
    }
    if (!s_enc && !(s_enc = big_alloc(sizeof(*s_enc)))) {
        ESP_LOGE(TAG, "no memory for the encoder");
        return false;
    }
    s_encoded = 0;
    s_held = false;
    if (!s_meter.loud.e) {
        /* The loudest second's ring (MG_LOUD1S_RING_BYTES), PSRAM where there is
         * some, kept for good; without it the summary just leaves loud1s out. */
        mg_capture_meter_set_ring(&s_meter, big_alloc(MG_LOUD1S_RING_BYTES));
    }
    mg_capture_meter_reset(&s_meter);
    s_from_client = from_client;
    s_start_us = esp_timer_get_time();
    s_start_ms = mg_now_ms();
    s_start_unix_ms = (uint64_t)mg_unix_ms_now();

    mg_lock();
    mg_proto_t *p = mg_proto_get();
    const mg_settings_t *set = mg_proto_settings(p);
    /* Offline-note clips stay SBC at 16 kHz, whatever setting 7 says. A client's
     * start_mic brings its format; the user's own turns take settings 7, 13
     * and 14. */
    mg_audio_format_t fmt;
    if (route == MG_ROUTE_QUEUE || (route == MG_ROUTE_HELD && MG_HELD_TURN_SBC)) {
        /* A held live turn records the clip format, so it can always become a clip; its own
         * change_data_type announces SBC if it goes live (mg_config.h). */
        mg_codec_format(mg_data_type_audio_sbc, &fmt);
    } else if (from_client) {
        fmt = mg_capture_req()->fmt;
    } else {
        mg_proto_turn_format(p, &fmt);
    }
    if (fmt.rate_hz != MG_MIC_RATE_HZ) {
        /* Not what this mic delivers (capture_24k without the bus at 24 kHz
         * can't happen; this keeps the label truthful regardless). */
        uint16_t kbps = (uint16_t)(fmt.frame_bytes * 8000u / (fmt.frame_us ? fmt.frame_us : 10000));
        mg_codec_format_at(fmt.type, MG_MIC_RATE_HZ, kbps, &fmt);
    }
    bool haptics = route == MG_ROUTE_BLE && !from_client && mg_proto_haptics(p);
    uint16_t freq = set->buzz_freq_hz, ms = set->buzz_ms;
    uint8_t vol = set->buzz_volume;
    bool ready = route == MG_ROUTE_QUEUE || mg_proto_ready(p) || (route == MG_ROUTE_HELD && mg_proto_held_turn(p));
    mg_unlock();
    if (!ready || !mg_encoder_open_fmt(s_enc, &fmt)) {
        return false;
    }

    if (route == MG_ROUTE_QUEUE) {
        if (!clip_alloc(&s_enc->fmt, 0)) {
            ESP_LOGW(TAG, "no memory to record a clip");
            return false;
        }
        s_clip_len = 0;
        s_route = route;
        return true;
    }

    if (haptics) {
        mg_vibrate(freq, ms, vol);   /* only boards with a vibration motor list haptics */
    }
    mg_lock();
    bool ok = mg_proto_live_begin_fmt(mg_proto_get(), &fmt);
    bool gated = !ok && !mg_proto_output_ok(mg_proto_get());
    bool full = mg_proto_output_full(mg_proto_get());
    bool held = ok && !mg_proto_output_ok(mg_proto_get());
    mg_unlock();
    if (ok) {
        s_route = route;
        if (held) {
            ESP_LOGI(TAG, "capture held for the token proof: sent once this connection's proof matches");
        }
    } else if (gated) {
        ESP_LOGI(TAG, "no capture for the central: %s",
                 full ? "the hold for its token proof is full" : "no proof key (not set up), so no proven central");
    }
    return ok;
}

/*
 * Hands one encoded slice to the capture's outbox (mg_capq_t, mg_proto.h):
 * it sends what the link takes now and holds the rest, so the voice task
 * never waits on the link (a turn on a sleep link that waited out its sends
 * here, 4 ms at a time, lost half its frames). False once the utterance
 * is closed (the client went, or stopped it).
 */
static bool queue_out(size_t n)
{
    mg_lock();
    bool ok = mg_proto_send_data(mg_proto_get(), s_out, n);
    bool held = mg_proto_capture_held(mg_proto_get()) != 0;
    mg_unlock();
    if (ok) {
        s_encoded += n;
    }
    if (held && !s_held) {
        mg_ble_kick();   /* the worker drains it every 5 ms too, between the voice task's slices */
    }
    s_held = held;
    return ok;
}

bool mg_voice_audio(const int16_t *pcm, size_t frames)
{
    if (s_route == MG_ROUTE_NONE) {
        return false;
    }
    if (esp_timer_get_time() - s_start_us > MG_MAX_UTTERANCE_S * 1000000LL) {
        return false;
    }
    while (frames) {
        size_t n = frames < SLICE ? frames : SLICE;
        if (s_route == MG_ROUTE_HELD) {
            /* The held live turn: live once the client is proven and ready (the pump released it);
             * not in time, or its link gone, its audio starts an offline clip and the turn goes on into it. */
            mg_lock();
            mg_proto_t *p = mg_proto_get();
            bool still = mg_proto_held_turn(p);
            bool due = still && mg_proto_held_turn_due(p);
            bool queueable = due && mg_proto_held_turn_queueable(p);
            bool live = mg_proto_live(p);
            if (due && !queueable && !MG_HELD_TURN_NO_QUEUE_HOLDS) {
                mg_proto_held_turn_take(p, NULL, 0);   /* mg_config.h: no queue at the deadline discards */
            }
            mg_unlock();
            if (!still && live) {
                s_route = MG_ROUTE_BLE;
                ESP_LOGI(TAG, "held live turn: the client is proven and ready: live, %u bytes held go first",
                         (unsigned)s_encoded);
                released();
            } else if (queueable && !held_to_clip()) {
                return false;
            } else if (due && !queueable && !MG_HELD_TURN_NO_QUEUE_HOLDS) {
                ESP_LOGI(TAG, "held live turn: not proven and ready in time and no clip queue: discarded");
                return false;
            }
        }
        if (s_route == MG_ROUTE_QUEUE) {
            size_t room = s_clip_cap - s_clip_len;
            s_clip_len += mg_encoder_feed(s_enc, pcm, n, s_clip + s_clip_len, room);
            if (s_clip_cap - s_clip_len < s_enc->fmt.frame_bytes * 3u) {
                return false;   /* full */
            }
        } else {
            mg_capture_meter_add(&s_meter, pcm, n);   /* what the mic heard: the summary's level */
            size_t out = mg_encoder_feed(s_enc, pcm, n, s_out, sizeof(s_out));
            mg_lock();
            bool live = mg_proto_live(mg_proto_get());
            /* A new token proof's challenge turned push-to-talk off: a
             * press's turn ends at once (device-ended: no post-roll). */
            bool cut = !s_from_client && s_route == MG_ROUTE_BLE && !mg_proto_ptt_enabled(mg_proto_get());
            mg_unlock();
            if (cut) {
                ESP_LOGI(TAG, "push-to-talk turn ends: a new token proof turned push-to-talk off");
                return false;
            }
            if (!live || (out && !queue_out(out))) {
                mg_lock();
                bool full = mg_proto_output_full(mg_proto_get());
                uint32_t held = mg_proto_capture_held(mg_proto_get());
                mg_unlock();
                if (full) {
                    /* Held for a token proof that hasn't come, and the hold is full: the capture
                     * stops here (the mic ring with it); what's held goes once the proof matches. */
                    ESP_LOGI(TAG, "capture stops: the hold filled before the token proof (%u bytes held for it)",
                             (unsigned)held);
                }
                return false;
            }
            if (s_gate) {
                /* The bench guard: released once it rises N dB over its floor (mg_proto_turn_rise). */
                mg_capture_level_t lv;
                mg_capture_meter_level(&s_meter, MG_MIC_RATE_HZ, &lv);
                mg_lock();
                bool rose = mg_proto_turn_rise(mg_proto_get(), &lv);
                float rise = mg_proto_get()->capq.gate_rise;
                bool gone = !mg_proto_turn_held(mg_proto_get());
                uint8_t db = mg_proto_get()->capq.gate_db;
                mg_unlock();
                if (rose) {
                    s_gate = false;
                    ESP_LOGI(TAG, "ptt: rose %.1f dB over its floor at %.2f s (>= %u): turn released, %u bytes held go first",
                             (double)rise, lv.ms / 1000.0, (unsigned)db, (unsigned)s_encoded);
                    released();
                } else if (gone) {
                    s_gate = false;   /* the link took it */
                }
            }
        }
        pcm += n;
        frames -= n;
    }
    if (s_route == MG_ROUTE_BLE && s_from_client) {
        mg_lock();
        bool want = mg_capture_req()->requested;
        mg_unlock();
        return want;
    }
    return true;
}

void mg_voice_end(bool keep)
{
    mg_route_t route = s_route;
    s_route = MG_ROUTE_NONE;
    if (route == MG_ROUTE_QUEUE) {
        s_clip_len += mg_encoder_flush(s_enc, s_clip + s_clip_len, s_clip_cap - s_clip_len);
        if (keep && s_clip_len) {
            ESP_LOGI(TAG, "recorded a %u-byte clip for the queue", (unsigned)s_clip_len);
            mg_queue_submit(s_enc->fmt.type, s_start_ms, s_start_unix_ms, s_clip, (uint32_t)s_clip_len);
        } else {
            free(s_clip);
        }
        s_clip = NULL;
        return;
    }
    if (route != MG_ROUTE_BLE && route != MG_ROUTE_HELD) {
        return;
    }
    if (!keep) {
        /* A turn not kept (the mute switch, a press too short, a capture that couldn't start) sends nothing more:
         * the encoder's residue is dropped, not flushed; a turn still held (the bench guard's, a held live turn, or
         * one waiting on the token proof) goes unsent with everything it queued, and no stop_mic (it never went),
         * so a proof that matches later releases none of it; a turn already live ends with its stop_mic and
         * nothing after it. */
        (void)mg_encoder_flush(s_enc, s_out, sizeof(s_out));   /* the residue, discarded */
        mg_lock();
        mg_proto_t *p = mg_proto_get();
        if (s_from_client) {
            mg_capture_req()->requested = false;
        }
        bool unsent = s_gate ? mg_proto_turn_drop(p, false) : false;
        if (!unsent && mg_proto_held_turn(p)) {
            (void)mg_proto_held_turn_take(p, NULL, 0);
            unsent = true;
        }
        if (!unsent) {
            unsent = mg_proto_live_discard(p);   /* still waiting on the proof: discarded, never sent after it */
        }
        if (!unsent) {
            mg_proto_live_end(p, true);   /* live already: its stop_mic, after what had gone */
        }
        mg_unlock();
        s_gate = false;
        ESP_LOGI(TAG, "capture over, not kept: %s", unsent ? "nothing of it sent" : "ended (stop_mic), nothing more sent");
        return;
    }
    if (s_gate) {
        /* The bench guard: a turn that never rose is never sent (no gesture, change_data_type, Data or
         * stop_mic: a stop_mic without its button_down would end a recording the client never started). */
        s_gate = false;
        mg_capture_level_t lv;
        mg_capture_meter_level(&s_meter, MG_MIC_RATE_HZ, &lv);
        mg_lock();
        mg_proto_t *p = mg_proto_get();
        float rise = p->capq.gate_rise;
        uint8_t db = p->capq.gate_db;
        bool dropped = mg_proto_turn_drop(p, true);
        uint32_t n = p->ptt_nothing;
        mg_unlock();
        if (dropped) {
            s_dropped = true;
            ESP_LOGI(TAG, "ptt: nothing heard (rise %.1f dB < %u): turn not sent (%.1f s, floor %.1f dBFS, loud1s %.1f "
                          "dBFS; ptt_nothing=%u)",
                     (double)rise, (unsigned)db, lv.ms / 1000.0, (double)lv.floor_db,
                     (double)(lv.loud_valid ? lv.loud1s_db : lv.rms_db), (unsigned)n);
            return;
        }
    }
    size_t out = mg_encoder_flush(s_enc, s_out, sizeof(s_out));
    if (out) {
        queue_out(out);
    }
    mg_capture_level_t level;
    mg_capture_meter_level(&s_meter, MG_MIC_RATE_HZ, &level);
    mg_lock();
    if (s_from_client) {
        mg_capture_req()->requested = false;
    }
    mg_proto_capture_level(mg_proto_get(), &level);   /* the summary carries it once the end has gone */
    /* stop_mic ends every capture, whichever side started or stopped it: queued
     * after the audio still held, retried until it goes (mg_capq_t). */
    mg_proto_live_end(mg_proto_get(), true);
    uint32_t held = mg_proto_capture_held(mg_proto_get());
    bool pending = mg_proto_capture_pending(mg_proto_get());
    ESP_LOGI(TAG, "capture over: %.1f s, %u bytes, %u still held for the link%s",
             (esp_timer_get_time() - s_start_us) / 1e6, (unsigned)s_encoded, (unsigned)held,
             pending ? "; its end follows them" : "");
    mg_capture_note();   /* "utterance over BLE: end sent ..." when it went already */
    /* A held live turn still held at its end: to the clip queue now if it's due (else the worker
     * takes it at its deadline, or it goes live once the client is proven and ready). */
    uint8_t *clip = NULL;
    size_t clip_len = 0;
    if (route == MG_ROUTE_HELD && mg_proto_held_turn(mg_proto_get())) {
        mg_proto_t *p = mg_proto_get();
        if (mg_proto_held_turn_due(p) && mg_proto_held_turn_queueable(p)) {
            uint32_t n = mg_proto_held_turn_bytes(p);
            clip = n ? big_alloc(n) : NULL;
            clip_len = mg_proto_held_turn_take(p, clip, n);
        } else if (mg_proto_held_turn_due(p) && !MG_HELD_TURN_NO_QUEUE_HOLDS) {
            mg_proto_held_turn_take(p, NULL, 0);
        } else {
            mg_proto_held_turn_voice(p, false);   /* the worker watches its deadline */
            pending = true;
        }
    }
    mg_unlock();
    if (clip && clip_len) {
        s_queued = true;
        ESP_LOGI(TAG, "held live turn: not proven and ready in %u ms: its %u bytes saved as an offline clip",
                 (unsigned)MG_HELD_TURN_MS, (unsigned)clip_len);
        mg_queue_submit(mg_data_type_audio_sbc, s_start_ms, s_start_unix_ms, clip, (uint32_t)clip_len);
    } else {
        free(clip);
    }
    if (pending) {
        mg_ble_kick();   /* the worker drains the rest every 5 ms */
    }
}
