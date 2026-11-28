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

/* The vibration tests over the haptics core (mg_haptic_test_core.h). */
#include "mg_haptic_test_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(MGHT_PRESET_FIRST == mg_haptic_preset_tap && MGHT_PRESET_RING == mg_haptic_preset_ring &&
                   MGHT_PRESET_LAST == mg_haptic_preset_error && MGHT_FLAG_STRENGTH == mg_haptic_actuator_flag_strength,
               "mgcommands.h");

static const char *const NAMES[] = { "tap", "double", "long", "ring", "success", "error" };
static const char *const TITLES[] = { "Tap", "Double", "Long", "Ring", "Success", "Error" };

const char *mght_preset_name(uint8_t preset, bool title)
{
    if (preset < mg_haptic_preset_tap || preset > mg_haptic_preset_error) return NULL;
    return (title ? TITLES : NAMES)[preset - mg_haptic_preset_tap];
}

uint16_t mght_pulse_played_ms(const mght_caps_t *c, uint16_t ms)
{
    return ms < c->min_on_ms ? c->min_on_ms : ms;
}

static uint8_t repeat_of(uint8_t preset)
{
    return preset == mg_haptic_preset_ring ? MGHT_RING_CYCLES : 1;
}

/* A request's whole length as the core plays it (passes, the gaps between passes of an odd list). */
static uint32_t req_ms(uint8_t min_on, const mgh_req_t *r)
{
    uint32_t pass = 0;
    for (unsigned k = 0; k < r->count; k++) {
        pass += (!(k & 1u) && r->steps[k] < min_on) ? min_on : r->steps[k];
    }
    uint32_t gap = (r->count & 1u) ? MG_HAPTIC_REPEAT_GAP_MS : 0;
    return pass * r->repeat + gap * (r->repeat - 1u);
}

uint32_t mght_preset_ms(const mght_caps_t *c, uint8_t preset)
{
    mgh_req_t r = { .repeat = repeat_of(preset) };
    if (!mgh_preset(preset, &r)) return 0;
    return req_ms(c->min_on_ms, &r);
}

void mght_actuator_text(const mght_caps_t *c, char *out, size_t n)
{
    snprintf(out, n, "%s, %s", c->kind == mg_haptic_actuator_lra ? "LRA" : "ERM",
             (c->flags & mg_haptic_actuator_flag_strength) ? "strength" : "on/off");
}

uint8_t mght_strength_pct(const mght_caps_t *c, int step)
{
    int steps = c->strength_steps ? c->strength_steps : 1;
    step = step < 1 ? 1 : step > steps ? steps : step;
    return (uint8_t)((step * 100 + steps / 2) / steps);
}

int mght_strength_step(const mght_caps_t *c, uint8_t pct)
{
    int steps = c->strength_steps ? c->strength_steps : 1;
    int s = (pct * steps + 50) / 100;
    return s < 1 ? 1 : s > steps ? steps : s;
}

void mght_init(mght_t *t, mgh_t *h, uint8_t default_strength)
{
    memset(t, 0, sizeof(*t));
    t->h = h;
    t->default_strength = default_strength;
}

void mght_caps(const mght_t *t, mght_caps_t *c)
{
    const mgh_caps_t *k = t->h->caps;
    *c = (mght_caps_t){
        .present = true, .kind = k->kind, .flags = k->flags, .min_on_ms = k->min_on_ms,
        .strength_steps = k->strength_steps,
        .max_on_ms = k->max_on_ms && k->max_on_ms < MG_HAPTIC_MAX_ON_MS ? k->max_on_ms : MG_HAPTIC_MAX_ON_MS,
        .default_strength = t->default_strength,
    };
}

static bool test_plays(const mght_t *t)
{
    return t->state == MGHT_ST_PLAYING && t->h->active && t->h->src == MGH_SRC_TEST && t->h->id == t->id;
}

/* Settle what came of the last test once the core no longer plays it. */
static void sync(mght_t *t, uint32_t now)
{
    if (t->state != MGHT_ST_PLAYING || test_plays(t)) return;
    if ((int32_t)(now - t->end) >= 0) {
        t->state = MGHT_ST_DONE;
    } else if (t->h->last_src == MGH_SRC_TEST && t->h->last_status == mg_haptic_status_interrupted) {
        t->state = MGHT_ST_CUT;            /* an update started, the battery went low */
        t->reason = t->h->last_reason;
    } else {
        t->state = MGHT_ST_CUT;            /* a higher source took the motor (the talk buzz) */
        t->reason = mg_haptic_reason_in_use;
    }
}

mght_result_t mght_run(mght_t *t, const mght_cmd_t *c, const mgh_env_t *e, uint32_t now, mgh_status_t out[2],
                       unsigned *n)
{
    *n = 0;
    sync(t, now);
    if (c->op == MGHT_OP_STOP) {
        return mght_stop(t, now) ? MGHT_STOPPED : MGHT_IDLE;
    }
    /* Ring again: the second tap stops it. */
    if (c->op == MGHT_OP_PRESET && c->preset == mg_haptic_preset_ring && test_plays(t) &&
        t->op == MGHT_OP_PRESET && t->preset == mg_haptic_preset_ring) {
        mght_stop(t, now);
        return MGHT_STOPPED;
    }
    const mgh_caps_t *k = t->h->caps;
    uint8_t strength = (k->flags & mg_haptic_actuator_flag_strength) ? c->strength : 0;
    if (strength > 100) return MGHT_INVALID;
    /* The haptic_play a client's bench request would send: the same checks. */
    uint8_t b[MGH_PLAY_MIN_LEN + 3];
    unsigned len;
    uint8_t id = (uint8_t)(t->id + 1u);
    b[0] = 0;   /* motor */
    b[1] = 0;   /* no mb library effects */
    b[2] = id;
    b[3] = mg_haptic_flag_bench;
    b[4] = strength;
    if (c->op == MGHT_OP_PULSE) {
        if (!(k->flags & mg_haptic_actuator_flag_timing)) return MGHT_INVALID;
        b[5] = 1;
        b[6] = mg_haptic_kind_timing;
        b[7] = 1;
        b[8] = (uint8_t)c->ms;
        b[9] = (uint8_t)(c->ms >> 8);
        len = 10;
    } else if (c->op == MGHT_OP_PRESET) {
        b[5] = repeat_of(c->preset);
        b[6] = mg_haptic_kind_preset;
        b[7] = c->preset;
        len = 8;
    } else {
        return MGHT_INVALID;
    }
    mgh_req_t r;
    if (mgh_parse_play(k, b, len, &r)) return MGHT_INVALID;
    uint16_t played = t->h->played;
    *n = mgh_play(t->h, &r, MGH_SRC_TEST, e, now, out);
    t->op = c->op;
    t->preset = c->op == MGHT_OP_PRESET ? c->preset : 0;
    t->ms = c->op == MGHT_OP_PULSE ? c->ms : 0;
    if (t->h->played == played) {
        t->state = MGHT_ST_REFUSED;
        t->reason = t->h->last_reason;
        return MGHT_REFUSED;
    }
    t->id = id;
    t->state = MGHT_ST_PLAYING;
    t->reason = mg_haptic_reason_none;
    t->start = now;
    t->end = now + req_ms(k->min_on_ms, &r);
    return MGHT_PLAYING;
}

bool mght_stop(mght_t *t, uint32_t now)
{
    sync(t, now);
    if (!test_plays(t)) return false;
    mgh_stop_test(t->h, now);
    t->state = MGHT_ST_STOPPED;
    return true;
}

static const char *reason_text(uint8_t reason)
{
    switch (reason) {
    case mg_haptic_reason_updating: return "update running";
    case mg_haptic_reason_low_battery: return "low battery";
    case mg_haptic_reason_asleep: return "asleep";
    case mg_haptic_reason_disabled: return "vibration off";
    case mg_haptic_reason_in_use: return "in use (talk or setup)";
    case mg_haptic_reason_duty_limit: return "duty limit";
    default: return "?";
    }
}

static void ms_text(char *out, size_t n, uint32_t ms)
{
    if (ms < 1000) snprintf(out, n, "%u ms", (unsigned)ms);
    else if (ms % 1000) snprintf(out, n, "%u.%u s", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
    else snprintf(out, n, "%u s", (unsigned)(ms / 1000));
}

/* "pulse" / "ring". */
static void what_text(const mght_t *t, char *out, size_t n)
{
    const char *name = t->op == MGHT_OP_PULSE ? NULL : mght_preset_name(t->preset, false);
    if (t->op == MGHT_OP_PULSE) snprintf(out, n, "%u ms pulse", (unsigned)t->ms);
    else snprintf(out, n, "%s", name ? name : "pattern");   /* (no test yet: preset 0, never a name) */
}

static void line_text(const mght_t *t, char *out, size_t n)
{
    char what[24], len[16];
    what_text(t, what, sizeof(what));
    const mgh_caps_t *k = t->h->caps;
    switch (t->state) {
    case MGHT_ST_NONE:
        snprintf(out, n, "Ready");
        break;
    case MGHT_ST_PLAYING:
        if (t->op == MGHT_OP_PRESET && t->preset == mg_haptic_preset_ring) snprintf(out, n, "Ringing, tap again to stop");
        else snprintf(out, n, "Playing %s", what);
        break;
    case MGHT_ST_DONE:
        if (t->op == MGHT_OP_PULSE) {
            if (t->ms < k->min_on_ms) snprintf(out, n, "Played %s as %u ms", what, (unsigned)k->min_on_ms);
            else snprintf(out, n, "Played %s", what);
        } else {
            ms_text(len, sizeof(len), t->end - t->start);
            snprintf(out, n, "Played %s, %s", what, len);
        }
        break;
    case MGHT_ST_STOPPED:
        snprintf(out, n, "Stopped %s", what);
        break;
    case MGHT_ST_REFUSED:
        if (t->reason == mg_haptic_reason_duty_limit) snprintf(out, n, "Cooling down: duty limit");
        else snprintf(out, n, "Refused: %s", reason_text(t->reason));
        break;
    case MGHT_ST_CUT:
        snprintf(out, n, "Stopped: %s", reason_text(t->reason));
        break;
    }
}

void mght_status(mght_t *t, uint32_t now, mght_status_t *s)
{
    sync(t, now);
    memset(s, 0, sizeof(*s));
    s->playing = test_plays(t);
    s->preset = s->playing ? t->preset : 0;
    s->minute_on_ms = mgh_duty_used(t->h, now, 60000u);
    s->played = t->h->played;
    line_text(t, s->line, sizeof(s->line));
    char used[16];
    ms_text(used, sizeof(used), s->minute_on_ms);
    snprintf(s->budget, sizeof(s->budget), "Last minute %s of %u s on", used, (unsigned)(MG_HAPTIC_MINUTE_MAX_ON_MS / 1000));
    snprintf(s->boot, sizeof(s->boot), "%u played this boot", (unsigned)s->played);
}

/* An unsigned decimal in [lo, hi]; *end past it. */
static bool num(const char *p, unsigned lo, unsigned hi, unsigned *v, const char **end)
{
    char *e;
    if (*p < '0' || *p > '9') return false;
    unsigned long x = strtoul(p, &e, 10);
    if (x < lo || x > hi) return false;
    *v = (unsigned)x;
    *end = e;
    return true;
}

int mght_parse(const mght_t *t, const char *line, mght_cmd_t *c, bool *status, char *out, size_t n)
{
    memset(c, 0, sizeof(*c));
    *status = false;
    if (!strcmp(line, "vib?")) {
        *status = true;
        return 0;
    }
    if (strncmp(line, "vib=", 4) != 0) return -1;
    const char *a = line + 4, *p;
    const mgh_caps_t *k = t->h->caps;
    unsigned cap = k->max_on_ms && k->max_on_ms < MG_HAPTIC_MAX_ON_MS ? k->max_on_ms : MG_HAPTIC_MAX_ON_MS;
    unsigned v = 0, pct = 0;
    if (!strcmp(a, "stop")) {
        c->op = MGHT_OP_STOP;
        return 0;
    }
    if (!strncmp(a, "pulse,", 6)) {
        if (!num(a + 6, 1, cap, &v, &p) || (*p && (*p != ',' || !num(p + 1, 1, 100, &pct, &p) || *p))) {
            snprintf(out, n, "vib %s: pulse,MS[,PCT] with MS 1 to %u", a, cap);
            return -2;
        }
        c->op = MGHT_OP_PULSE;
        c->ms = (uint16_t)v;
        c->strength = (uint8_t)pct;
        return 0;
    }
    if (!strncmp(a, "preset,", 7)) {
        const char *name = a + 7;
        size_t len = strcspn(name, ",");
        for (uint8_t i = mg_haptic_preset_tap; i <= mg_haptic_preset_error; i++) {
            const char *nm = mght_preset_name(i, false);
            if (strlen(nm) == len && !strncmp(name, nm, len)) {
                p = name + len;
                if (*p && (*p != ',' || !num(p + 1, 1, 100, &pct, &p) || *p)) break;
                c->op = MGHT_OP_PRESET;
                c->preset = i;
                c->strength = (uint8_t)pct;
                return 0;
            }
        }
        snprintf(out, n, "vib %s: preset,tap|double|long|ring|success|error[,PCT]", a);
        return -2;
    }
    snprintf(out, n, "vib %s: vib=pulse,MS[,PCT] | vib=preset,NAME[,PCT] | vib=stop | vib?", a);
    return -2;
}

void mght_console(mght_t *t, const char *line, const mgh_env_t *e, uint32_t now, mgh_status_t out[2], unsigned *n,
                  char *answer, size_t an)
{
    *n = 0;
    mght_cmd_t c;
    bool status;
    int rc = mght_parse(t, line, &c, &status, answer, an);
    if (rc == -1) {
        snprintf(answer, an, "vib: not a vib word");
        return;
    }
    if (rc) return;
    mght_status_t s;
    char act[48];
    mght_caps_t caps;
    mght_caps(t, &caps);
    if (status) {
        char kind[24];
        mght_actuator_text(&caps, kind, sizeof(kind));
        mght_status(t, now, &s);
        snprintf(answer, an, "vib? %s; %s, %s; %s; %s, min %u ms", s.line, s.budget, s.boot,
                 s.playing ? "playing" : "idle", kind, (unsigned)caps.min_on_ms);
        return;
    }
    if (c.op == MGHT_OP_PULSE) snprintf(act, sizeof(act), "pulse,%u", (unsigned)c.ms);
    else if (c.op == MGHT_OP_PRESET) snprintf(act, sizeof(act), "preset,%s", mght_preset_name(c.preset, false));
    else snprintf(act, sizeof(act), "stop");
    mght_result_t r = mght_run(t, &c, e, now, out, n);
    mght_status(t, now, &s);
    switch (r) {
    case MGHT_PLAYING:
        if (c.op == MGHT_OP_PULSE && c.ms < caps.min_on_ms) {
            snprintf(answer, an, "vib %s: playing, lengthened to %u ms", act, (unsigned)caps.min_on_ms);
        } else {
            char len[16];
            ms_text(len, sizeof(len), t->end - t->start);
            snprintf(answer, an, "vib %s: playing, %s", act, len);
        }
        break;
    case MGHT_STOPPED: snprintf(answer, an, "vib %s: %s", act, s.line); break;
    case MGHT_IDLE: snprintf(answer, an, "vib %s: nothing playing", act); break;
    case MGHT_REFUSED: snprintf(answer, an, "vib %s: %s; %s, %s", act, s.line, s.budget, s.boot); break;
    default: snprintf(answer, an, "vib %s: invalid", act); break;
    }
}
