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

/* muse_restart.h's logic that doesn't need the chip (the host tests run it). */

#include "muse_restart.h"

void muse_restart_mark_set(muse_restart_mark_t *m, muse_restart_why_t why, uint32_t core)
{
    m->magic = MUSE_RESTART_MAGIC;
    m->why = (uint32_t)why;
    m->core = core;
    m->check = ~(m->magic ^ m->why ^ m->core);
}

bool muse_restart_mark_valid(const muse_restart_mark_t *m)
{
    return m->magic == MUSE_RESTART_MAGIC && m->why >= MUSE_RESTART_OTHER && m->why < MUSE_RESTART_WHY_COUNT &&
           m->core < 8 && m->check == ~(m->magic ^ m->why ^ m->core);
}

void muse_restart_mark_clear(muse_restart_mark_t *m)
{
    m->magic = 0;
    m->check = 0;
}

int muse_restart_fail_reason(const muse_restart_mark_t *m)
{
    if (m->magic != MUSE_RESTART_FAIL_MAGIC || m->core >= 8 || m->check != ~(m->magic ^ m->why ^ m->core) ||
        m->why > 31) {
        return 0;
    }
    return m->why == MUSE_RST_UNKNOWN ? MUSE_RST_SW : (int)m->why;
}

int muse_restart_effective_reason2(int chip_reason, bool marked, int fail_reason)
{
    if (!marked && fail_reason > 0 && (chip_reason == MUSE_RST_SW || chip_reason == MUSE_RST_WDT)) {
        return fail_reason;
    }
    return muse_restart_effective_reason(chip_reason, marked);
}

int muse_restart_effective_reason(int chip_reason, bool marked)
{
    if (marked && (chip_reason == MUSE_RST_SW || chip_reason == MUSE_RST_WDT)) {
        return MUSE_RST_SW;
    }
    return chip_reason;
}

const char *muse_restart_reason_name(int reason)
{
    static const char *const names[] = {
        "unknown", "power-on", "external", "software", "panic", "interrupt watchdog", "task watchdog",
        "watchdog", "deep sleep", "brownout", "SDIO", "USB", "JTAG", "eFuse", "power glitch", "CPU lockup",
    };
    return reason >= 0 && reason < (int)(sizeof(names) / sizeof(names[0])) ? names[reason] : "other";
}

const char *muse_restart_why_name(muse_restart_why_t why)
{
    static const char *const names[] = { "?", "other", "console", "OTA", "setup", "SAFE off", "SAFE on" };
    _Static_assert(sizeof(names) / sizeof(names[0]) == MUSE_RESTART_WHY_COUNT, "a name for every why");
    return why >= MUSE_RESTART_OTHER && why < MUSE_RESTART_WHY_COUNT ? names[why] : "?";
}
