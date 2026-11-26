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
 * "Charged" for the screen timeout (Display >
 * Sleep when charged). On USB the screen times out as on battery. With the
 * setting at Never, a lit screen stays lit while USB powers the gadget and
 * the battery counts as charged; it never lights a dark one, and a USB
 * plug-in is a wake.
 *
 * "Charged" is the charger's own report (only when the charger
 * itself reports full): its charge-done or termination status
 * (muse_power_t.charge_done). It latches at the first done of a plug-in,
 * whatever the setting, and only unplug lets it go:
 * the charger's top-ups after termination (done dropping while it tops up)
 * don't release it, and the setting only decides whether it holds, so
 * switching to Never on a gadget already charged holds at once, even during
 * a top-up. A failed read keeps the latch (it isn't an unplug) but nothing
 * holds until a good reading shows USB still in (an
 * unknown reading never holds the screen, so failed reads while unplugged
 * can't keep it lit on battery); the next good one on USB holds again at
 * once, with no new done, and one without USB lets the latch go. A restart
 * starts over (RAM only: not charged until the charger's next done). A board
 * whose charger has no done signal falls back to the battery percentage,
 * taken at 90 % and let go below 85 % (hysteresis, no latch: a % estimate can
 * read 90 early, and the 85 % release lets go if the lit panel stalls the
 * charge); esp32/AGENTS.md lists those boards. A board with no battery
 * reading or no USB sense hides the setting. On those % boards an unknown
 * reading (a failed read, no percentage) counts as not charged.
 *
 * Pure C, so the host tests run it.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSE_CHARGED_NONE = 0,   /* no battery reading or no USB sense: the setting is hidden */
    MUSE_CHARGED_DONE,       /* the charger's charge-done status: muse_power_t.charge_done */
    MUSE_CHARGED_PERCENT,    /* no done signal: the battery percentage, with hysteresis */
} muse_charged_src_t;

#define MUSE_CHARGED_TAKE_PCT 90      /* % fallback: charged from here up */
#define MUSE_CHARGED_RELEASE_PCT 85   /* ...and no longer below this */

typedef struct {
    bool charged;   /* the battery counts as charged (only ever on USB); a done board's latch */
    bool usb;       /* the last reading's USB */
    bool seen;      /* a reading has come in: the first is never a plug-in */
    bool known;     /* the latest reading succeeded: only then can it hold */
} muse_charged_t;

typedef struct {
    bool usb;
    int battery_pct;    /* -1: unknown */
    bool charge_done;   /* the charger reports charge done (MUSE_CHARGED_DONE boards) */
} muse_charged_reading_t;

/* One power reading; the setting plays no part in what counts as charged.
 * Returns true when USB has just been plugged in (a reading on USB after one
 * without it): the caller wakes the screen. */
bool muse_charged_read(muse_charged_t *st, muse_charged_src_t src, const muse_charged_reading_t *r);

/* A reading that failed: on a % board not charged until a good one says
 * otherwise; a done board keeps its latch (only unplug lets it go). Nothing
 * holds until the next good reading. USB's state is kept, so the next good
 * reading is a plug-in only if it is one. */
void muse_charged_unknown(muse_charged_t *st, muse_charged_src_t src);

/* Whether a lit screen is held on: the setting at Never (never_sleep), the
 * gadget on USB (on_battery false), the latest reading a good one on USB and
 * the battery charged. The only place the setting enters. */
bool muse_charged_holds(muse_charged_src_t src, bool never_sleep, bool on_battery, const muse_charged_t *st);

#ifdef __cplusplus
}
#endif
