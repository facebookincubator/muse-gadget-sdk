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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "muse_state.h"

/*
 * Battery meter, for power work. None of the boards can measure current, so a
 * measurement pairs the fuel gauge (1% steps) with what the chip did: how long
 * the screen was off, how long it really spent in light sleep, and, with
 * CONFIG_PM_PROFILING, how long a core was busy and which power locks kept
 * it awake. A measurement starts when USB is unplugged (or at boot on
 * battery) and stops when USB comes back, keeping its numbers until the next.
 * On the bench, ">nap" on USB measures from then until the board wakes.
 */

typedef struct {
    bool started;         /* false until the board has been on battery */
    bool running;         /* on battery now; false once USB is back */
    int64_t secs;         /* time on battery */
    int pct_start, pct_now;   /* -1 unknown */
    int mv_start, mv_now;     /* 0 unknown */
    int screen_off_pm;    /* per mille of the time on battery */
    int resting_pm;       /* display stopped and the CPU allowed to sleep */
    int slept_pm;         /* actually in light sleep; -1 unknown */
    uint32_t sleeps;      /* light sleeps, each ended by a wake */
    int busy_pm;          /* a core running a task; -1 unknown */
    char awake[48];       /* the locks that kept it awake longest: "wifi 4%, bt 1%" */
} muse_battery_t;

void muse_battery_init(void);

/* From the input task: every battery reading, with muse_state_on_battery(),
 * which ">nap" sets on USB so the bench can measure; and the screen and CPU
 * state. */
void muse_battery_note_power(const muse_power_t *p, bool on_battery);
void muse_battery_note_state(bool screen_off, bool resting);

/* Whether the CPU powers down in light sleep, for the JSON; may come before init. */
void muse_battery_note_cpu_pd(bool on);

/* Starts over: from now if on battery, else at the next unplug. */
void muse_battery_reset(void);

void muse_battery_read(muse_battery_t *out);

/* Once the gauge has moved, after at least 10 minutes: the drain in tenths of a
 * percent an hour, and how many hours a full charge lasts at that rate. */
bool muse_battery_drain(const muse_battery_t *b, int *rate10, int *full_h);

/* The same with every lock and CPU mode, as one line of JSON, and internal RAM
 * ("heap": free, largest block and lowest since boot now, the lowest seen
 * resting on battery in this run, the retention-capable free and largest
 * block, and "cpu_pd"). */
int muse_battery_json(char *buf, size_t cap);

/* The JSON last saved before this boot, kept in RTC memory across resets (not
 * power loss) until muse_battery_reset(); NULL if none. It survives a board
 * that resets when its port opens, or one that crashed on battery. */
const char *muse_battery_saved_json(void);
