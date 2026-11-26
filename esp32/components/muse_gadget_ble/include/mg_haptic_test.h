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

/*
 * Vibration tests: Developer >
 * Tests > Vibration and the console's vib words, both through the production
 * haptics engine (the haptics core in haptics/, the motor's timer and gates in
 * mg_ble.c). A test is the core's MGH_SRC_TEST source: it skips
 * haptics_enabled and nothing else (the update, battery, sleep and talk-turn
 * gates, the caps and both duty budgets). Nothing runs one
 * on its own: only a tap on the page, a console word or the Nordic UART's
 * !IN starts one (no pattern at boot or from a timer).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The page's test pulses, ms. */
#define MGHT_PULSES { 20, 50, 100, 200, 500 }
#define MGHT_PULSE_COUNT 5
/* Ring plays cycles until a second tap, a key, Stop, or 10 s (5 cycles of 2 s:
 * MG_HAPTIC_MAX_REQUEST_MS). */
#define MGHT_RING_CYCLES 5
#define MGHT_LINE_MAX 72
/* mg_haptic_preset_t's numbers (mgcommands.h; checked in mg_haptic_test.c),
 * for callers without the protocol header. */
#define MGHT_PRESET_FIRST 1   /* tap */
#define MGHT_PRESET_RING 4
#define MGHT_PRESET_LAST 6    /* error */
#define MGHT_FLAG_STRENGTH 0x01   /* mg_haptic_actuator_flag_strength */

typedef enum {
    MGHT_OP_PULSE = 1,   /* one on step of ms */
    MGHT_OP_PRESET,      /* a preset (mg_haptic_preset_t); ring again stops it */
    MGHT_OP_STOP,        /* whatever test plays */
} mght_op_t;

typedef struct {
    uint8_t op;          /* mght_op_t */
    uint8_t preset;      /* MGHT_OP_PRESET: mg_haptic_preset_t */
    uint16_t ms;         /* MGHT_OP_PULSE: 1..the board's on-step cap */
    uint8_t strength;    /* 0: the gadget's own; 1-100 %, only with the strength flag; never saved */
} mght_cmd_t;

typedef enum {
    MGHT_PLAYING = 0,    /* the test plays */
    MGHT_STOPPED,        /* a test stopped (Stop, a key, Ring's second tap) */
    MGHT_IDLE,           /* Stop with nothing playing */
    MGHT_REFUSED,        /* a rule refused it (the reason is in the status line) */
    MGHT_INVALID,        /* out of range (a console word's numbers), unknown preset, no actuator */
} mght_result_t;

/* The actuator (setting 15's four bytes), the board's on-step cap and the
 * gadget's own strength (ptt_buzz_volume_percent). */
typedef struct {
    bool present;
    uint8_t kind, flags, min_on_ms, strength_steps;
    uint16_t max_on_ms;
    uint8_t default_strength;
} mght_caps_t;

typedef struct {
    bool playing;              /* a test plays now */
    uint8_t preset;            /* the playing test's preset (0: a pulse) */
    uint32_t minute_on_ms;     /* on time in the last minute (every source), against MG_HAPTIC_MINUTE_MAX_ON_MS */
    uint16_t played;           /* patterns started since boot (every source) */
    char line[MGHT_LINE_MAX];  /* the last result: "Played tap, 50 ms", "Refused: low battery", ... */
    char budget[MGHT_LINE_MAX];/* "Last minute 4.2 s of 20 s on" */
    char boot[MGHT_LINE_MAX];  /* "7 played this boot" */
} mght_status_t;

/* Names: "tap", "double", ... ("Tap" capitalized for the page); NULL for an unknown preset. */
const char *mght_preset_name(uint8_t preset, bool title);
/* A pulse as the actuator plays it: lengthened to min_on_ms. */
uint16_t mght_pulse_played_ms(const mght_caps_t *c, uint16_t ms);
/* A preset's length as the page plays it (ring: all its cycles). */
uint32_t mght_preset_ms(const mght_caps_t *c, uint8_t preset);
/* "ERM, on/off", "LRA, strength". */
void mght_actuator_text(const mght_caps_t *c, char *out, size_t n);
/* The strength steps of a strength flag actuator, as 1-100 %: step 1..strength_steps. */
uint8_t mght_strength_pct(const mght_caps_t *c, int step);
int mght_strength_step(const mght_caps_t *c, uint8_t pct);

/*
 * The engine's side (mg_ble.c on a gadget, sim_haptics.c in the simulator):
 * each call takes the board's gates at that moment, drives the motor and
 * forwards any status a cut client pattern gets.
 */
bool mg_ble_haptic_test_caps(mght_caps_t *c);         /* false: no actuator (no Vibration page, no vib words) */
mght_result_t mg_ble_haptic_test(const mght_cmd_t *c);
void mg_ble_haptic_test_status(mght_status_t *s);
/* Any hardware key: stops a test that plays (true: it did, and the key is spent on it). */
bool mg_ble_haptic_test_key(void);
/* A pattern of any source plays (it vetoes sleep until it ends). */
bool mg_ble_haptic_busy(void);
/* The console's vib words (vib=pulse,MS[,PCT] / vib=preset,NAME[,PCT] / vib=stop / vib?): one answer line in out. */
void mg_ble_haptic_console(const char *line, char *out, size_t n);

#ifdef __cplusplus
}
#endif
