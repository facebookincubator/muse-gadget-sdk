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

/*
 * The haptics engine's test side (mg_haptic_test.h) for the preview: the
 * production core (haptics/mg_haptics.c) and test logic
 * (haptics/mg_haptic_test.c) on the simulator's clock, in place of mg_ble.c's
 * esp_timer and motor. The StopWatch has the actuator (its ERM: on/off,
 * timing lists, 30 ms, as main/mg_glue.c gives it); MUSE_SIM_VIB=strength
 * gives one with 4 strength steps (the strength card); other boards have none.
 * The gates: low battery from the scenario's battery/usb/charging, an update
 * from update=true, a talk turn from turn=true. The motor's edges print as
 * "@motor on|off T" for the tests.
 */

#include "mg_haptic_test_core.h"
#include "sim_services.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_state.h"
#include "sim_platform.h"

static mgh_t s_h;
static mgh_caps_t s_caps;
static mght_t s_t;
static bool s_init, s_present, s_motor, s_updating, s_turn;

static void init(void)
{
    if (s_init) {
        return;
    }
    s_init = true;
    const char *board = getenv("MUSE_SIM_BOARD");
    const char *vib = getenv("MUSE_SIM_VIB");
    s_present = (board && !strcmp(board, "stopwatch")) || (vib && vib[0]);
    if (vib && !strcmp(vib, "none")) {
        s_present = false;
    }
    bool strength = vib && !strcmp(vib, "strength");
    s_caps = (mgh_caps_t){
        .kind = mg_haptic_actuator_erm,
        .flags = (uint8_t)(mg_haptic_actuator_flag_timing | (strength ? mg_haptic_actuator_flag_strength : 0)),
        .min_on_ms = 30,
        .strength_steps = strength ? 4 : 0,
        .max_on_ms = MG_HAPTIC_MAX_ON_MS,
    };
    mgh_init(&s_h, &s_caps);
    mght_init(&s_t, &s_h, 80);   /* ptt_buzz_volume_percent's default */
}

static uint32_t now_ms(void)
{
    return sim_time_tick_ms();
}

static void env(mgh_env_t *e)
{
    memset(e, 0, sizeof(*e));
    muse_power_t p = muse_state_power();
    e->updating = s_updating;
    e->low_battery = p.battery_pct >= 0 && p.battery_pct <= 10 && !p.usb && !p.charging;
    e->in_turn = s_turn;
}

/* The engine's timer, lazily: every call settles the steps due by now. */
static void tick(uint32_t now)
{
    mgh_env_t e;
    env(&e);
    mgh_status_t s[1];
    if (s_h.active && (e.updating || e.low_battery)) {
        mgh_interrupt(&s_h, e.updating ? mg_haptic_reason_updating : mg_haptic_reason_low_battery, now, s);
    }
    uint32_t next;
    mgh_tick(&s_h, now, s, &next);
    if (mgh_motor(&s_h) != s_motor) {
        s_motor = mgh_motor(&s_h);
        printf("@motor %s %u\n", s_motor ? "on" : "off", (unsigned)now);
    }
}

void sim_haptics_set_gates(bool updating, bool turn)
{
    s_updating = updating;
    s_turn = turn;
}

void sim_haptics_poll(void)
{
    init();
    if (s_present) {
        tick(now_ms());
    }
}

bool mg_ble_haptic_test_caps(mght_caps_t *c)
{
    init();
    memset(c, 0, sizeof(*c));
    if (!s_present) {
        return false;
    }
    mght_caps(&s_t, c);
    return true;
}

mght_result_t mg_ble_haptic_test(const mght_cmd_t *c)
{
    init();
    if (!s_present) {
        return MGHT_INVALID;
    }
    uint32_t now = now_ms();
    tick(now);
    mgh_env_t e;
    env(&e);
    mgh_status_t s[2];
    unsigned n;
    mght_result_t r = mght_run(&s_t, c, &e, now, s, &n);
    tick(now);
    printf("@vib result %d\n", (int)r);
    return r;
}

void mg_ble_haptic_test_status(mght_status_t *st)
{
    init();
    memset(st, 0, sizeof(*st));
    if (!s_present) {
        return;
    }
    uint32_t now = now_ms();
    tick(now);
    mght_status(&s_t, now, st);
}

bool mg_ble_haptic_test_key(void)
{
    init();
    if (!s_present) {
        return false;
    }
    uint32_t now = now_ms();
    tick(now);
    bool stopped = mght_stop(&s_t, now);
    tick(now);
    return stopped;
}

bool mg_ble_haptic_busy(void)
{
    return s_present && s_h.active;
}

void mg_ble_haptic_console(const char *line, char *out, size_t n)
{
    init();
    if (!s_present) {
        snprintf(out, n, "vib: no vibration motor on this board");
        return;
    }
    uint32_t now = now_ms();
    tick(now);
    mgh_env_t e;
    env(&e);
    mgh_status_t s[2];
    unsigned k;
    mght_console(&s_t, line, &e, now, s, &k, out, n);
    tick(now);
}
