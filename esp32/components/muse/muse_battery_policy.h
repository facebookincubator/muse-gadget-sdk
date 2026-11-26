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

/*
 * Low battery: warn at 15 % (the battery icon) and
 * 10 % (a toast), and shut down cleanly at a voltage measured before the
 * board browns out. Only on battery: never with USB power. Pure logic, no
 * IDF, so the host tests run it (tests/test_muse_battery.py).
 */

typedef enum {
    MUSE_BATTERY_LEVEL_OK = 0,
    MUSE_BATTERY_LEVEL_WARN15,     /* <= 15 %: the battery icon turns low */
    MUSE_BATTERY_LEVEL_WARN10,     /* <= 10 %: a "Battery low" toast once */
    MUSE_BATTERY_LEVEL_CRITICAL,   /* at the shutdown voltage: power off now */
} muse_battery_level_t;

#define MUSE_BATTERY_WARN_PCT 15
#define MUSE_BATTERY_TOAST_PCT 10
/* Back above a warning once the percentage is this much over it (a gauge's
 * reading wobbles by a percent under load). */
#define MUSE_BATTERY_HYSTERESIS_PCT 2
/* Readings in a row at or under the shutdown voltage before it shuts down:
 * a speaker or radio burst dips the cell for a moment. At the input task's
 * 2 s readings (10 s while resting) that's 10 to 50 s. */
#define MUSE_BATTERY_SHUTDOWN_READINGS 5

typedef struct {
    int shutdown_mv;   /* 0: no automatic shutdown (the board's voltage isn't trusted) */
} muse_battery_thresholds_t;

typedef struct {
    muse_battery_level_t level;
    int low_readings;   /* consecutive readings at or under shutdown_mv */
    bool toasted;       /* the 10 % toast was shown since the last charge */
} muse_battery_policy_t;

/* One reading: pct -1 = no battery, mv 0 = not measured, usb = external
 * power (charging or not). Returns the level now; a change from the last
 * call is what the caller acts on (icon, toast, shutdown). */
muse_battery_level_t muse_battery_policy_step(muse_battery_policy_t *p, const muse_battery_thresholds_t *t,
                                              int pct, int mv, bool usb);
