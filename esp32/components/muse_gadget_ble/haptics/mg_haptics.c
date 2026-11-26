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

/* The haptics core: mg_haptics.h (mgcommands.h, Haptics). */
#include "mg_haptics.h"
#include <string.h>

/* The presets, ms, on first. */
static const uint16_t mgh_p_tap[] = {50};
static const uint16_t mgh_p_double[] = {50, 120, 50};
static const uint16_t mgh_p_long[] = {400};
static const uint16_t mgh_p_ring[] = {400, 200, 400, 1000};
static const uint16_t mgh_p_success[] = {40, 100, 120};
static const uint16_t mgh_p_error[] = {120, 80, 120, 80, 120};
static const uint16_t *const mgh_presets[] = {mgh_p_tap, mgh_p_double, mgh_p_long, mgh_p_ring, mgh_p_success,
                                              mgh_p_error};
static const uint8_t mgh_preset_n[] = {1, 3, 1, 4, 3, 5};

bool mgh_preset(uint8_t preset, mgh_req_t *r)
{
    if (preset < mg_haptic_preset_tap || preset > mg_haptic_preset_error) return false;
    unsigned i = preset - mg_haptic_preset_tap;
    r->count = mgh_preset_n[i];
    for (unsigned k = 0; k < r->count; k++) r->steps[k] = mgh_presets[i][k];
    return true;
}

static uint32_t mgh_step_len(uint8_t min_on, const uint16_t *s, unsigned k)
{
    return (!(k & 1u) && s[k] < min_on) ? min_on : s[k];
}

/* One pass, the gap after it, the whole request (passes and gaps) and its on time, as played. */
static void mgh_lengths(uint8_t min_on, const uint16_t *s, unsigned count, unsigned repeat, uint32_t *pass,
                        uint32_t *total, uint32_t *on)
{
    uint32_t p = 0, o = 0;
    for (unsigned k = 0; k < count; k++) {
        uint32_t l = mgh_step_len(min_on, s, k);
        p += l;
        if (!(k & 1u)) o += l;
    }
    uint32_t gap = (count & 1u) ? MG_HAPTIC_REPEAT_GAP_MS : 0;
    if (pass) *pass = p;
    if (total) *total = p * repeat + gap * (repeat ? repeat - 1u : 0);
    if (on) *on = o * repeat;
}

uint16_t mgh_parse_play(const mgh_caps_t *caps, const uint8_t *p, unsigned n, mgh_req_t *r)
{
    if (n < MGH_PLAY_MIN_LEN) return mg_error_code_invalid_length;
    if (p[1]) return mg_error_code_unsupported;                 /* mb's waveform-library effect ids */
    if (p[0] != 0 && p[0] != MG_HAPTIC_MOTOR_ALL) return mg_error_code_invalid_value;
    uint8_t kind = p[6];
    if (kind != mg_haptic_kind_preset && kind != mg_haptic_kind_timing) return mg_error_code_unsupported;
    if (kind == mg_haptic_kind_timing && !(caps->flags & mg_haptic_actuator_flag_timing))
        return mg_error_code_unsupported;
    unsigned count = 0;
    if (kind == mg_haptic_kind_preset) {
        if (n != MGH_PLAY_MIN_LEN + 1u) return mg_error_code_invalid_length;
    } else {
        if (n < MGH_PLAY_MIN_LEN + 1u) return mg_error_code_invalid_length;
        count = p[7];
        if (n != MGH_PLAY_MIN_LEN + 1u + 2u * count) return mg_error_code_invalid_length;
    }
    r->id = p[2];
    r->flags = p[3];
    r->strength = p[4];
    r->repeat = p[5];
    if (r->strength > 100 || !r->repeat) return mg_error_code_invalid_value;
    if (kind == mg_haptic_kind_timing) {
        if (!count || count > MG_HAPTIC_MAX_STEPS) return mg_error_code_invalid_value;
        r->count = (uint8_t)count;
        for (unsigned k = 0; k < count; k++) r->steps[k] = (uint16_t)(p[8 + 2 * k] | p[9 + 2 * k] << 8);
    } else if (!mgh_preset(p[7], r)) {
        return mg_error_code_unsupported;
    }
    uint32_t cap = caps->max_on_ms && caps->max_on_ms < MG_HAPTIC_MAX_ON_MS ? caps->max_on_ms : MG_HAPTIC_MAX_ON_MS;
    for (unsigned k = 0; k < r->count; k++) {
        if (!r->steps[k]) return mg_error_code_invalid_value;
        if (!(k & 1u) && mgh_step_len(caps->min_on_ms, r->steps, k) > cap) return mg_error_code_invalid_value;
    }
    uint32_t pass, total;
    mgh_lengths(caps->min_on_ms, r->steps, r->count, r->repeat, &pass, &total, 0);
    if (pass > MG_HAPTIC_MAX_PATTERN_MS || total > MG_HAPTIC_MAX_REQUEST_MS) return mg_error_code_invalid_value;
    return 0;
}

uint16_t mgh_parse_stop(const uint8_t *p, unsigned n)
{
    if (n != 1) return mg_error_code_invalid_length;
    return (p[0] == 0 || p[0] == MG_HAPTIC_MOTOR_ALL) ? 0 : mg_error_code_invalid_value;
}

uint32_t mgh_on_ms(const mgh_t *h, const mgh_req_t *r)
{
    uint32_t on;
    mgh_lengths(h->caps->min_on_ms, r->steps, r->count, r->repeat, 0, 0, &on);
    return on;
}

void mgh_init(mgh_t *h, const mgh_caps_t *caps)
{
    memset(h, 0, sizeof(*h));
    h->caps = caps;
}

/* ---- duty ---- */

static void mgh_duty_roll(mgh_t *h, uint32_t now)
{
    uint32_t slot = now / MGH_DUTY_BUCKET_MS, d = slot - h->duty_slot;
    if ((int32_t)d <= 0) return; /* same or an older bucket: never roll backwards */
    if (d >= MGH_DUTY_BUCKETS) memset(h->duty, 0, sizeof(h->duty));
    else for (uint32_t i = 1; i <= d; i++) h->duty[(h->duty_slot + i) % MGH_DUTY_BUCKETS] = 0;
    h->duty_slot = slot;
}

void mgh_duty_add(mgh_t *h, uint32_t now, uint32_t ms)
{
    mgh_duty_roll(h, now);
    /* a step that ended in an older bucket (accounted after a newer roll) counts in its own bucket */
    uint32_t slot = now / MGH_DUTY_BUCKET_MS, back = h->duty_slot - slot;
    if ((int32_t)back < 0) back = 0;
    if (back >= MGH_DUTY_BUCKETS) return;
    uint16_t *b = &h->duty[(h->duty_slot - back) % MGH_DUTY_BUCKETS];
    *b = (uint16_t)(*b + ms > 0xFFFFu ? 0xFFFFu : *b + ms);
}

uint32_t mgh_duty_used(mgh_t *h, uint32_t now, uint32_t window_ms)
{
    mgh_duty_roll(h, now);
    uint32_t k = window_ms / MGH_DUTY_BUCKET_MS + 1u, sum = 0;
    if (k > MGH_DUTY_BUCKETS) k = MGH_DUTY_BUCKETS;
    for (uint32_t i = 0; i < k; i++) sum += h->duty[(h->duty_slot + MGH_DUTY_BUCKETS - i) % MGH_DUTY_BUCKETS];
    if (mgh_motor(h)) sum += now - h->t;
    return sum;
}

/* ---- the step engine ---- */

static uint32_t mgh_cur_len(const mgh_t *h)
{
    return h->gap ? MG_HAPTIC_REPEAT_GAP_MS : h->steps[h->step];
}

/* The motor's on time so far goes into the duty buckets (at the step's end, or a cut at now). */
static void mgh_account(mgh_t *h, uint32_t end)
{
    if (mgh_motor(h)) mgh_duty_add(h, end, end - h->t);
}

static void mgh_end(mgh_t *h, uint32_t now, uint8_t status, uint8_t reason)
{
    mgh_account(h, now);
    h->last_src = h->src;
    h->last_status = status;
    h->last_reason = reason;
    h->active = 0;
    h->on = 0;
}

static unsigned mgh_put(mgh_status_t *out, uint8_t id, uint8_t status, uint8_t reason)
{
    out->id = id;
    out->status = status;
    out->reason = reason;
    return 1;
}

unsigned mgh_play(mgh_t *h, const mgh_req_t *r, uint8_t src, const mgh_env_t *e, uint32_t now, mgh_status_t out[2])
{
    uint8_t reason = mg_haptic_reason_none;
    bool needs_switch = src == MGH_SRC_TALK || src == MGH_SRC_SETUP ||
                        (src == MGH_SRC_CLIENT && !(r->flags & mg_haptic_flag_bench));
    uint32_t need = mgh_on_ms(h, r);
    if (e->updating) reason = mg_haptic_reason_updating;
    else if (e->low_battery) reason = mg_haptic_reason_low_battery;
    else if (e->asleep) reason = mg_haptic_reason_asleep;
    else if (needs_switch && !e->enabled) reason = mg_haptic_reason_disabled;
    else if ((src != MGH_SRC_TALK && e->in_turn) || (h->active && h->src > src)) reason = mg_haptic_reason_in_use;
    else if (mgh_duty_used(h, now, MG_HAPTIC_DUTY_WINDOW_MS) + need > MG_HAPTIC_DUTY_MAX_ON_MS ||
             mgh_duty_used(h, now, 60000u) + need > MG_HAPTIC_MINUTE_MAX_ON_MS)
        reason = mg_haptic_reason_duty_limit;
    if (reason) {
        h->last_src = src;
        h->last_status = mg_haptic_status_refused;
        h->last_reason = reason;
        return src == MGH_SRC_CLIENT ? mgh_put(&out[0], r->id, mg_haptic_status_refused, reason) : 0;
    }
    unsigned n = 0;
    if (h->active) {
        bool client = h->src == MGH_SRC_CLIENT;
        uint8_t st = src == MGH_SRC_CLIENT ? mg_haptic_status_stopped : mg_haptic_status_interrupted;
        uint8_t why = src == MGH_SRC_CLIENT ? mg_haptic_reason_replaced : mg_haptic_reason_in_use;
        uint8_t id = h->id;
        mgh_end(h, now, st, why);
        if (client) n += mgh_put(&out[n], id, st, why);
    }
    for (unsigned k = 0; k < r->count; k++) h->steps[k] = (uint16_t)mgh_step_len(h->caps->min_on_ms, r->steps, k);
    h->count = r->count;
    h->passes_left = r->repeat;
    h->step = 0;
    h->gap = 0;
    h->t = now;
    h->src = src;
    h->id = r->id;
    h->active = 1;
    h->on = 1;
    h->last_src = src;
    h->last_status = mg_haptic_status_playing;
    h->last_reason = mg_haptic_reason_none;
    h->last_count = r->count;
    h->last_on_ms = (uint16_t)(need > 0xFFFFu ? 0xFFFFu : need);
    h->played++;
    if (src == MGH_SRC_CLIENT) n += mgh_put(&out[n], r->id, mg_haptic_status_playing, mg_haptic_reason_none);
    return n;
}

unsigned mgh_stop(mgh_t *h, uint32_t now, mgh_status_t out[1])
{
    if (!h->active || h->src != MGH_SRC_CLIENT) return 0;
    uint8_t id = h->id;
    mgh_end(h, now, mg_haptic_status_stopped, mg_haptic_reason_none);
    return mgh_put(&out[0], id, mg_haptic_status_stopped, mg_haptic_reason_none);
}

bool mgh_stop_test(mgh_t *h, uint32_t now)
{
    if (!h->active || h->src != MGH_SRC_TEST) return false;
    mgh_end(h, now, mg_haptic_status_stopped, mg_haptic_reason_none);
    return true;
}

unsigned mgh_interrupt(mgh_t *h, uint8_t reason, uint32_t now, mgh_status_t out[1])
{
    if (!h->active || (reason == mg_haptic_reason_in_use && h->src == MGH_SRC_TALK)) return 0;
    bool client = h->src == MGH_SRC_CLIENT;
    uint8_t id = h->id;
    mgh_end(h, now, mg_haptic_status_interrupted, reason);
    return client ? mgh_put(&out[0], id, mg_haptic_status_interrupted, reason) : 0;
}

void mgh_link_drop(mgh_t *h, uint32_t now)
{
    if (h->active && h->src == MGH_SRC_CLIENT) mgh_end(h, now, mg_haptic_status_stopped, mg_haptic_reason_none);
}

unsigned mgh_tick(mgh_t *h, uint32_t now, mgh_status_t out[1], uint32_t *next_ms)
{
    unsigned n = 0;
    while (h->active && now - h->t >= mgh_cur_len(h)) {
        uint32_t end = h->t + mgh_cur_len(h);
        mgh_account(h, end);
        h->t = end;
        if (h->gap) {
            h->gap = 0;
            h->step = 0;
        } else if (++h->step >= h->count) {
            if (h->passes_left <= 1) {
                bool client = h->src == MGH_SRC_CLIENT;
                uint8_t id = h->id;
                h->on = 0;
                mgh_end(h, end, mg_haptic_status_done, mg_haptic_reason_none);
                if (client) n = mgh_put(&out[0], id, mg_haptic_status_done, mg_haptic_reason_none);
                break;
            }
            h->passes_left--;
            h->step = 0;
            h->gap = (h->count & 1u) != 0;
        }
        h->on = !h->gap && !(h->step & 1u);
    }
    *next_ms = h->active ? mgh_cur_len(h) - (now - h->t) : 0;
    return n;
}
