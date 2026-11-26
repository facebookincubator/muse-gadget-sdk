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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Charge dark (plugging in turns the gadget on).
 *
 * Plugging USB into a gadget that is off turns it on; the power chip does
 * that (the StopWatch's M5PM1 on 5VIN insertion, the AXP2101 on VBUS
 * insertion, the Watcher's VBUS path straight into its 3.3 V buck). On a
 * healthy cell the gadget then boots as usual. On a low cell it charges
 * dark: no display, no radio, no advert, no boot loop, and it boots by
 * itself once the cell reads the board's low-cell level
 * (muse_board_t.battery_shutdown_mv, the shutdown voltage: one constant
 * per board) plus MUSE_CD_CHARGE_MARGIN_MV while charging. The margin is
 * there because a charging cell reads above its resting voltage, and a boot
 * at the shutdown level itself would power off again soon after the unplug.
 *
 * The hold runs in app_main right after the boot guard (muse_bootguard.h)
 * and before Link, BLE or the display start, only on a fresh power-up (an
 * RTC marker tells ESP32 resets, flashes and deep-sleep wakes apart) that
 * the board's power chip says USB caused (muse_board_t.early_power). While
 * it holds, the USB recovery tier answers (the chip runs, USB Serial/JTAG or
 * the board's bridge is untouched: esptool resets it into download mode as
 * usual) and so do the keys (the power chip's own actions; a press boots),
 * while the BLE recovery tier is down: charge dark sends no advert, and
 * BLE recovery may be down during the hold while USB and the keys remain. No app code runs before or during it, and
 * SAFE mode never holds. It ends:
 *   - healthy: MUSE_CD_CONFIRM readings in a row at or above the threshold;
 *   - a key: a press turned it on, or a key goes down during the hold;
 *   - unplugged below the threshold: the board powers off again
 *     (early_power_off), as it was before USB came;
 *   - MUSE_CD_MAX_MS in the hold, or a reading that fails: boot anyway
 *     (a charger that never gets there, no battery; USB powers the board).
 *
 * The decision is pure (muse_charge_dark_core.c) so the host tests run it
 * (tests/test_muse_charge_dark.py).
 */

#define MUSE_CD_CHARGE_MARGIN_MV 300            /* over battery_shutdown_mv, read while charging */
#define MUSE_CD_POLL_MS 5000
#define MUSE_CD_CONFIRM 2                       /* readings in a row */
#define MUSE_CD_MAX_MS (2LL * 60 * 60 * 1000)   /* then boot anyway */
#define MUSE_CD_LOG_EVERY 12                    /* readings between progress lines (a minute) */

/* One reading of the board's power, from muse_board_t.early_power. */
typedef struct {
    bool usb;          /* USB (VBUS / 5VIN) present now */
    bool usb_on;       /* this power-up came from USB being plugged in */
    bool by_key;       /* a key turned it on, or a key is down now */
    int battery_mv;    /* 0: unknown */
} muse_cd_reading_t;

typedef enum {
    MUSE_CD_BOOT = 0,  /* go on booting */
    MUSE_CD_WAIT,      /* stay dark, read again in MUSE_CD_POLL_MS */
    MUSE_CD_OFF,       /* power the board off again */
} muse_cd_action_t;

typedef enum {
    MUSE_CD_WHY_NOT_USB = 0,   /* not a USB power-up, or no USB: a normal boot */
    MUSE_CD_WHY_UNREADABLE,    /* the reading failed: boot */
    MUSE_CD_WHY_NO_BATTERY,    /* no battery voltage: boot */
    MUSE_CD_WHY_HEALTHY,       /* the cell is healthy: boot */
    MUSE_CD_WHY_LOW,           /* low cell on USB: dark */
    MUSE_CD_WHY_KEY,           /* a key: boot */
    MUSE_CD_WHY_UNPLUGGED,     /* USB went away below the threshold: off */
    MUSE_CD_WHY_LIMIT,         /* MUSE_CD_MAX_MS in the hold: boot */
} muse_cd_why_t;

typedef struct {
    bool dark;          /* holding */
    int healthy_n;      /* readings in a row at or above the threshold */
    int unplugged_n;    /* readings in a row without USB */
    int64_t since_ms;   /* when the hold began */
} muse_cd_state_t;

/* The board's boot threshold: its low-cell level plus the charging margin;
 * 0 (no charge dark) for a board without one. */
int muse_cd_healthy_mv(int battery_shutdown_mv);

/* The pure decision for one reading. ok: the reading succeeded. healthy_mv:
 * muse_cd_healthy_mv() (0 = no charge dark). */
muse_cd_action_t muse_cd_decide(muse_cd_state_t *st, bool ok, const muse_cd_reading_t *r, int healthy_mv,
                                int64_t now_ms, muse_cd_why_t *why);
const char *muse_cd_why_name(muse_cd_why_t why);

/* ---- on the device ---------------------------------------------------------- */

/* From app_main after muse_bootguard_begin(), in a normal (not SAFE) boot:
 * returns once the gadget may boot; powers the board off instead when USB
 * goes away during the hold (and returns if that fails). Without
 * CONFIG_MUSE_CHARGE_DARK, or a board without early_power or a low-cell
 * level (battery_shutdown_mv): returns at once. */
void muse_charge_dark_hold(void);

#ifdef __cplusplus
}
#endif
