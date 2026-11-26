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
 * The vibration tests (mg_haptic_test.c) over the vendored haptics core,
 * unchanged: test_mg_haptic_test.py runs each scenario by name under
 * ASan/UBSan. The motor's state is sampled every millisecond the way the
 * board's timer would drive it (mgh_tick at each due step).
 */

#include "mg_haptic_test_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                  \
            s_fail = 1;                                                          \
        }                                                                        \
    } while (0)
#define CHECK_STR(a, b)                                                          \
    do {                                                                         \
        if (strcmp((a), (b))) {                                                  \
            printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
            s_fail = 1;                                                          \
        }                                                                        \
    } while (0)

/* The StopWatch's actuator (main/mg_glue.c): ERM, timing lists, 30 ms, no strength. */
static mgh_caps_t CAPS = { mg_haptic_actuator_erm, mg_haptic_actuator_flag_timing, 30, 0, MG_HAPTIC_MAX_ON_MS };

typedef struct {
    mgh_t h;
    mght_t t;
    mgh_env_t e;
    uint32_t now;
    uint32_t on_ms;            /* motor on time, counted per ms */
    uint8_t motor[200000];     /* per ms, for the duty windows */
} rig_t;

static rig_t R;

static void rig(const mgh_caps_t *caps)
{
    memset(&R, 0, sizeof(R));
    mgh_init(&R.h, caps);
    mght_init(&R.t, &R.h, 80);
    R.now = 1000;   /* not 0: a real board's clock has run a while */
}

/* Time passes 1 ms at a time; the motor follows mgh_motor() after each tick. */
static void run(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        mgh_status_t s[1];
        uint32_t next;
        mgh_tick(&R.h, R.now, s, &next);
        if (mgh_motor(&R.h)) {
            R.on_ms++;
            if (R.now < sizeof(R.motor)) R.motor[R.now] = 1;
        }
        R.now++;
    }
    mgh_status_t s[1];
    uint32_t next;
    mgh_tick(&R.h, R.now, s, &next);
}

static mght_result_t cmd(uint8_t op, uint16_t ms, uint8_t preset, uint8_t strength)
{
    mght_cmd_t c = { .op = op, .ms = ms, .preset = preset, .strength = strength };
    mgh_status_t s[2];
    unsigned n;
    return mght_run(&R.t, &c, &R.e, R.now, s, &n);
}

static mght_status_t status(void)
{
    mght_status_t s;
    mght_status(&R.t, R.now, &s);
    return s;
}

static void console(const char *line, char *out, size_t n)
{
    mgh_status_t s[2];
    unsigned k;
    mght_console(&R.t, line, &R.e, R.now, s, &k, out, n);
}

/* ---- scenarios ---- */

static void refusals(void)
{
    struct {
        uint8_t updating, low, asleep, turn;
        const char *line;
    } cases[] = {
        { 1, 0, 0, 0, "Refused: update running" },
        { 0, 1, 0, 0, "Refused: low battery" },
        { 0, 0, 1, 0, "Refused: asleep" },
        { 0, 0, 0, 1, "Refused: in use (talk or setup)" },
        { 1, 1, 1, 1, "Refused: update running" },          /* precedence: updating first */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        rig(&CAPS);
        R.e = (mgh_env_t){ .enabled = 1, .updating = cases[i].updating, .low_battery = cases[i].low,
                           .asleep = cases[i].asleep, .in_turn = cases[i].turn };
        CHECK(cmd(MGHT_OP_PULSE, 100, 0, 0) == MGHT_REFUSED);
        CHECK(cmd(MGHT_OP_PRESET, 0, MGHT_PRESET_FIRST, 0) == MGHT_REFUSED);
        run(200);
        CHECK(R.on_ms == 0);
        CHECK(status().played == 0);
        CHECK_STR(status().line, cases[i].line);
    }
    /* haptics_enabled 0 (no app, or the app's switch off): a test still plays. */
    rig(&CAPS);
    R.e.enabled = 0;
    CHECK(cmd(MGHT_OP_PULSE, 100, 0, 0) == MGHT_PLAYING);
}

static void cut_while_playing(void)
{
    /* An update starting, the battery going low: the board cuts what plays. */
    rig(&CAPS);
    CHECK(cmd(MGHT_OP_PRESET, 0, MGHT_PRESET_RING, 0) == MGHT_PLAYING);
    run(100);
    mgh_status_t s[1];
    mgh_interrupt(&R.h, mg_haptic_reason_updating, R.now, s);
    CHECK(!mgh_motor(&R.h));
    CHECK_STR(status().line, "Stopped: update running");
    rig(&CAPS);
    CHECK(cmd(MGHT_OP_PULSE, 500, 0, 0) == MGHT_PLAYING);
    run(10);
    mgh_interrupt(&R.h, mg_haptic_reason_low_battery, R.now, s);
    CHECK_STR(status().line, "Stopped: low battery");
    /* The talk buzz outranks a test. */
    rig(&CAPS);
    CHECK(cmd(MGHT_OP_PRESET, 0, MGHT_PRESET_RING, 0) == MGHT_PLAYING);
    run(100);
    mgh_req_t talk = { .repeat = 1, .count = 1, .steps = { 100 } };
    mgh_env_t on = { .enabled = 1 };
    mgh_status_t out[2];
    mgh_play(&R.h, &talk, MGH_SRC_TALK, &on, R.now, out);
    run(200);
    CHECK_STR(status().line, "Stopped: in use (talk or setup)");
    /* A test cuts a client's pattern (the client hears interrupted, in_use). */
    rig(&CAPS);
    mgh_req_t client = { .id = 9, .repeat = 1, .count = 1, .steps = { 400 } };
    mgh_play(&R.h, &client, MGH_SRC_CLIENT, &on, R.now, out);
    mght_cmd_t c = { .op = MGHT_OP_PULSE, .ms = 50 };
    unsigned n;
    CHECK(mght_run(&R.t, &c, &R.e, R.now, out, &n) == MGHT_PLAYING);
    CHECK(n == 1 && out[0].id == 9 && out[0].status == mg_haptic_status_interrupted &&
          out[0].reason == mg_haptic_reason_in_use);
}

/* Any 10 s window and any minute of the motor's on time (sampled per ms). */
static void windows(uint32_t *max10, uint32_t *max60)
{
    uint32_t w10 = 0, w60 = 0;
    *max10 = *max60 = 0;
    for (uint32_t t = 0; t < R.now && t < sizeof(R.motor); t++) {
        w10 += R.motor[t];
        w60 += R.motor[t];
        if (t >= 10000) w10 -= R.motor[t - 10000];
        if (t >= 60000) w60 -= R.motor[t - 60000];
        if (w10 > *max10) *max10 = w10;
        if (w60 > *max60) *max60 = w60;
    }
}

static void duty(void)
{
    /* Ring (4 s on) twice in a row: the second doesn't fit 5 s in 10 s. */
    rig(&CAPS);
    CHECK(cmd(MGHT_OP_PRESET, 0, MGHT_PRESET_RING, 0) == MGHT_PLAYING);
    run(10000);
    CHECK(cmd(MGHT_OP_PRESET, 0, MGHT_PRESET_RING, 0) == MGHT_REFUSED);
    CHECK_STR(status().line, "Cooling down: duty limit");
    CHECK_STR(status().budget, "Last minute 4 s of 20 s on");
    CHECK_STR(status().boot, "1 played this boot");
    CHECK(status().minute_on_ms == 4000);
    /* A bench hammering the tests: every 250 ms a 500 ms pulse or a ring for
     * 150 s. The motor never exceeds either budget; refusals say duty limit. */
    rig(&CAPS);
    unsigned played = 0, refused = 0;
    for (int i = 0; i < 600; i++) {
        mght_result_t r = (i % 7 == 3) ? cmd(MGHT_OP_PRESET, 0, MGHT_PRESET_RING, 0) : cmd(MGHT_OP_PULSE, 500, 0, 0);
        if (r == MGHT_PLAYING) played++;
        if (r == MGHT_REFUSED) {
            refused++;
            CHECK_STR(status().line, "Cooling down: duty limit");
        }
        run(250);
    }
    uint32_t m10, m60;
    windows(&m10, &m60);
    printf("hammer: %u played, %u refused, max %u ms in 10 s, %u ms in a minute\n", played, refused, m10, m60);
    CHECK(played > 0 && refused > 0);
    CHECK(m10 <= MG_HAPTIC_DUTY_MAX_ON_MS);
    CHECK(m60 <= MG_HAPTIC_MINUTE_MAX_ON_MS);
}

/* At rest, before any test: "Ready" (the line names no pattern: preset 0 has no name, never printed as one). */
static void ready_at_rest(void)
{
    rig(&CAPS);
    mght_status_t s = status();
    CHECK_STR(s.line, "Ready");
}

static void caps(void)
{
    rig(&CAPS);
    CHECK(cmd(MGHT_OP_PULSE, 501, 0, 0) == MGHT_INVALID);   /* an on step over 500 ms */
    CHECK(cmd(MGHT_OP_PULSE, 0, 0, 0) == MGHT_INVALID);
    CHECK(cmd(MGHT_OP_PRESET, 0, 7, 0) == MGHT_INVALID);
    CHECK(cmd(MGHT_OP_PULSE, 500, 0, 0) == MGHT_PLAYING);
    run(600);
    CHECK(R.on_ms == 500);
    /* A board's own lower cap holds too. */
    mgh_caps_t low = CAPS;
    low.max_on_ms = 200;
    rig(&low);
    CHECK(cmd(MGHT_OP_PULSE, 201, 0, 0) == MGHT_INVALID);
    CHECK(cmd(MGHT_OP_PRESET, 0, MGHT_PRESET_RING, 0) == MGHT_INVALID);   /* its 400 ms steps */
}

/* The console's words do what the page's taps do: the same engine state. */
static void console_words(void)
{
    char out[192];
    struct {
        const char *word;
        uint8_t op, preset;
        uint16_t ms;
    } map[] = {
        { "vib=pulse,50", MGHT_OP_PULSE, 0, 50 },     { "vib=pulse,20", MGHT_OP_PULSE, 0, 20 },
        { "vib=pulse,500", MGHT_OP_PULSE, 0, 500 },   { "vib=preset,tap", MGHT_OP_PRESET, 1, 0 },
        { "vib=preset,double", MGHT_OP_PRESET, 2, 0 }, { "vib=preset,long", MGHT_OP_PRESET, 3, 0 },
        { "vib=preset,ring", MGHT_OP_PRESET, 4, 0 },  { "vib=preset,success", MGHT_OP_PRESET, 5, 0 },
        { "vib=preset,error", MGHT_OP_PRESET, 6, 0 }, { "vib=stop", MGHT_OP_STOP, 0, 0 },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        rig(&CAPS);
        mght_cmd_t c;
        bool st;
        CHECK(mght_parse(&R.t, map[i].word, &c, &st, out, sizeof(out)) == 0 && !st);
        CHECK(c.op == map[i].op && c.preset == map[i].preset && c.ms == map[i].ms);
        /* Run the word and the tap side by side: the same motor timeline and status line. */
        console(map[i].word, out, sizeof(out));
        run(12000);
        uint32_t on_console = R.on_ms;
        mght_status_t s_console = status();
        rig(&CAPS);
        cmd(map[i].op, map[i].ms, map[i].preset, 0);
        run(12000);
        CHECK(R.on_ms == on_console);
        CHECK_STR(status().line, s_console.line);
    }
    rig(&CAPS);
    console("vib=pulse,20", out, sizeof(out));
    CHECK_STR(out, "vib pulse,20: playing, lengthened to 30 ms");
    run(100);
    console("vib=preset,ring", out, sizeof(out));
    CHECK_STR(out, "vib preset,ring: playing, 10 s");
    run(1000);
    console("vib?", out, sizeof(out));
    CHECK_STR(out, "vib? Ringing, tap again to stop; Last minute 830 ms of 20 s on, 2 played this boot; "
                   "playing; ERM, on/off, min 30 ms");
    console("vib=preset,ring", out, sizeof(out));   /* again: stops it, as a second tap does */
    CHECK_STR(out, "vib preset,ring: Stopped ring");
    console("vib=stop", out, sizeof(out));
    CHECK_STR(out, "vib stop: nothing playing");
    console("vib=preset,ring", out, sizeof(out));
    run(10);
    console("vib=stop", out, sizeof(out));
    CHECK_STR(out, "vib stop: Stopped ring");
    R.e.low_battery = 1;
    console("vib=pulse,50", out, sizeof(out));
    CHECK(!strncmp(out, "vib pulse,50: Refused: low battery; Last minute", 47));
    R.e.low_battery = 0;
    console("vib=pulse,50,60", out, sizeof(out));
    CHECK_STR(out, "vib pulse,50: playing, 50 ms");
    /* Malformed words: an answer that says what's accepted, and nothing plays. */
    const char *bad[] = { "vib=pulse,0", "vib=pulse,501", "vib=pulse,", "vib=pulse,50x", "vib=pulse,50,101",
                          "vib=preset,buzz", "vib=preset,", "vib=preset,ring,0", "vib=go", "vib=" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        rig(&CAPS);
        console(bad[i], out, sizeof(out));
        run(100);
        CHECK(R.on_ms == 0);
        CHECK(strstr(out, "vib") == out && (strstr(out, "MS") || strstr(out, "NAME") || strstr(out, "preset,tap")));
    }
    rig(&CAPS);
    console("vibrate", out, sizeof(out));
    CHECK_STR(out, "vib: not a vib word");
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*fn)(void);
    } T[] = {
        { "refusals", refusals }, { "cut_while_playing", cut_while_playing }, { "duty", duty },
        { "caps", caps },         { "console_words", console_words },
        { "ready_at_rest", ready_at_rest },
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        if (argc < 2 || !strcmp(argv[1], T[i].name)) {
            T[i].fn();
            printf("%s %s\n", T[i].name, s_fail ? "FAILED" : "ok");
        }
    }
    return s_fail;
}
