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

#ifndef MG_POWER_H_
#define MG_POWER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The battery as the gadget sees it, from the VBAT
 * divider's readings (mg_battery.c, polled by main.c). Portable: no driver
 * calls, so the host tests run it.
 *
 * Below MG_POWER_NO_CELL_MV there is no cell (or one past use): Battery
 * Level reads 100 and report_battery_status says the battery isn't present.
 * On USB without a cell the pin may read the charger's output instead
 * (about 4.2 V), which can't be told from a full cell: 100% then too.
 */

#define MG_POWER_NO_CELL_MV 2500

/* report_battery_status: [device_action, 9, u16 mV, i16 current, u8 SoC,
 * i16 temperature, u16 capacity, u16 full capacity, u32 timestamp, flags]. */
#define MG_POWER_REPORT_LEN 18

/*
 * Low battery, only while a cell is present (never on USB without
 * one, which reads 100 %):
 *   <= MG_POWER_LOW_PCT (15 %)       low: logged (the app reads the charge)
 *   <= MG_POWER_CRITICAL_PCT (10 %)  critical: the LED's low-battery flash
 *                                    (one 80 ms flash every 30 s),
 *                                    readings every 10 s
 *   <= shutdown_mv twice in a row    a clean power off before brownout
 * Both warnings clear with hysteresis (MG_POWER_CLEAR_PCT, 20 %), so a
 * charging cell turns them off.
 */
#define MG_POWER_LOW_PCT      15
#define MG_POWER_CRITICAL_PCT 10
#define MG_POWER_CLEAR_PCT    20
#define MG_POWER_SHUTDOWN_READINGS 2

/* What a reading changed (mg_power_reading()'s result). */
#define MG_POWER_EV_LOW           (1u << 0) /* became low */
#define MG_POWER_EV_CRITICAL      (1u << 1) /* became critical */
#define MG_POWER_EV_RECOVERED     (1u << 2) /* low / critical cleared */
#define MG_POWER_EV_SHUTDOWN      (1u << 3) /* power off now */
#define MG_POWER_EV_REPORT        (1u << 4) /* send report_battery_status: a cell appeared or went, 5 % since the last, 15 % / 10 % crossed */

struct mg_battery_state {
	bool measured;    /* a reading exists (the ADC works) */
	bool present;     /* a cell is connected */
	int mv;           /* last reading */
	uint8_t percent;  /* from the LiPo curve; 100 without a cell */
	uint32_t at_ms;   /* uptime of the last reading */
	bool low;         /* <= 15 % */
	bool critical;    /* <= 10 % */
	uint8_t below;    /* readings in a row at or under the shutdown voltage */
};

/* Single-cell LiPo resting voltage to charge. */
uint8_t mg_power_percent_from_mv(int mv);

/* The voltage of the clean shutdown (mV), CONFIG_MG_BATTERY_SHUTDOWN_MV. */
void mg_power_set_shutdown_mv(int mv);

/* A new reading (mv < 0: the ADC failed) at uptime now_ms; returns the
 * MG_POWER_EV_* it caused. */
uint32_t mg_power_reading(int mv, uint32_t now_ms);
const struct mg_battery_state *mg_power_battery(void);
void mg_power_reset(void);

/* report_battery_status from the last reading: voltage and SoC when a cell
 * is present (flag present), every value it can't measure 0 (current,
 * temperature, capacities; charging isn't wired to a pin it can read). */
size_t mg_power_report(uint8_t out[MG_POWER_REPORT_LEN]);

/*
 * Plugging in turns it on. What a boot does, decided before
 * Bluetooth starts:
 *   - Off on USB (a power off while plugged in, MG_OFF_ON_USB): it stays off,
 *     dark and quiet, until a key press or an unplug and replug. The unplug
 *     wakes the chip only to arm the plug wake again (MG_BOOT_OFF).
 *   - A cell under resume_mv: on USB it charges dark (MG_BOOT_CHARGE_DARK: no
 *     radio, LED off) and boots once the cell reaches resume_mv; off USB it
 *     stays off (MG_BOOT_OFF), so a low cell never loops.
 *   - Otherwise it runs. usb < 0 (no way to tell) never holds a boot.
 */
enum mg_boot_power {
	MG_BOOT_RUN,
	MG_BOOT_OFF,
	MG_BOOT_CHARGE_DARK,
};

/* The power off's kind, kept across System OFF (settings mg/off). */
enum mg_off_kind {
	MG_OFF_NONE = 0,   /* not off, or woken by a plug or a key */
	MG_OFF_PLAIN = 1,  /* off on battery: a plug or a key wakes it */
	MG_OFF_ON_USB = 2, /* off while plugged in: a key, or an unplug then a plug */
};

enum mg_boot_power mg_power_boot_decision(bool woke_from_off, enum mg_off_kind off, int usb,
					  int mv, int resume_mv);

#endif
