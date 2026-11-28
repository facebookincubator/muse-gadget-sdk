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

#include "mg_power.h"

#include <string.h>

#include "mgcommands.h"

/* Single-cell LiPo, resting voltage to charge (mV, %). */
static const struct {
	uint16_t mv;
	uint8_t pct;
} curve[] = {
	{4150, 100}, {4050, 90}, {3970, 80}, {3900, 70}, {3840, 60}, {3790, 50},
	{3750, 40},  {3710, 30}, {3670, 20}, {3610, 10}, {3500, 5},  {3300, 0},
};

static struct mg_battery_state bat = {.percent = 100};
static int shutdown_mv = 3300;

void mg_power_set_shutdown_mv(int mv)
{
	shutdown_mv = mv;
}

uint8_t mg_power_percent_from_mv(int mv)
{
	if (mv >= curve[0].mv) {
		return 100;
	}
	for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
		if (mv >= curve[i].mv) {
			int span = curve[i - 1].mv - curve[i].mv;

			return curve[i].pct +
			       (curve[i - 1].pct - curve[i].pct) * (mv - curve[i].mv) / span;
		}
	}
	return 0;
}

void mg_power_reset(void)
{
	memset(&bat, 0, sizeof(bat));
	bat.percent = 100;
}


static bool reported_present; /* the state the last report showed */
static uint8_t present_streak;
static uint8_t reported_pct;  /* the charge the last report showed */

/* Crossing 15 % / 10 % (or clearing) is a report too; a report's charge is
 * what the next 5 % counts from. */
static uint32_t report(uint32_t ev)
{
	if (ev & (MG_POWER_EV_LOW | MG_POWER_EV_CRITICAL | MG_POWER_EV_RECOVERED)) {
		ev |= MG_POWER_EV_REPORT;
	}
	if (ev & MG_POWER_EV_REPORT) {
		reported_pct = bat.percent;
		reported_present = bat.present;
	}
	return ev;
}

uint32_t mg_power_reading(int mv, uint32_t now_ms)
{
	uint32_t ev = 0;
	bool seen = bat.measured;

	bat.at_ms = now_ms;
	bat.measured = mv >= 0;
	bat.mv = mv;
	bat.present = mv >= MG_POWER_NO_CELL_MV;
	/* The report goes unsolicited when the battery changes
	 * state; the XIAO can't read its charger, so a cell appearing or going
	 * is the change it can see. Two readings in a row in the new state (on
	 * USB without a cell the pin can read the charger's 4.2 V one time and
	 * nothing the next: no report for that). */
	bat.percent = bat.present ? mg_power_percent_from_mv(mv) : 100;
	if (!seen || !bat.measured) {
		reported_present = bat.present;
		reported_pct = bat.percent;
		present_streak = 0;
	} else if (bat.present != reported_present) {
		if (++present_streak >= 2) {
			reported_present = bat.present;
			present_streak = 0;
			ev |= MG_POWER_EV_REPORT;
		}
	} else {
		present_streak = 0;
	}
	/* And every 5 % of charge since the last report (apps stop
	 * trusting BAS once a report came and re-ask only every 15 min);
	 * counted from the last report, so a reading wobbling on a boundary
	 * sends nothing. Crossing 15 % / 10 % (and back) reports too, below. */
	if (bat.present && reported_present && !(ev & MG_POWER_EV_REPORT) &&
	    (bat.percent >= reported_pct + 5 || bat.percent + 5 <= reported_pct)) {
		ev |= MG_POWER_EV_REPORT;
	}

	if (!bat.present) {
		/* No cell (or USB without one): never low, never a shutdown. */
		if (bat.low || bat.critical) {
			ev |= MG_POWER_EV_RECOVERED;
		}
		bat.low = bat.critical = false;
		bat.below = 0;
		return report(ev);
	}
	if (bat.percent >= MG_POWER_CLEAR_PCT && (bat.low || bat.critical)) {
		bat.low = bat.critical = false;
		ev |= MG_POWER_EV_RECOVERED;
	}
	if (bat.percent <= MG_POWER_LOW_PCT && !bat.low) {
		bat.low = true;
		ev |= MG_POWER_EV_LOW;
	}
	if (bat.percent <= MG_POWER_CRITICAL_PCT && !bat.critical) {
		bat.critical = true;
		ev |= MG_POWER_EV_CRITICAL;
	}
	if (mv <= shutdown_mv) {
		if (bat.below < 255) {
			bat.below++;
		}
		if (bat.below >= MG_POWER_SHUTDOWN_READINGS) {
			ev |= MG_POWER_EV_SHUTDOWN;
		}
	} else {
		bat.below = 0;
	}
	return report(ev);
}

const struct mg_battery_state *mg_power_battery(void)
{
	return &bat;
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

size_t mg_power_report(uint8_t out[MG_POWER_REPORT_LEN])
{
	memset(out, 0, MG_POWER_REPORT_LEN);
	out[0] = mg_command_device_action;
	out[1] = mg_device_action_report_battery_status;
	if (bat.present) {
		put16(&out[2], (uint16_t)bat.mv);
		out[6] = bat.percent;
		out[17] = mg_battery_status_flag_present;
	}
	/* out[4..5] current, [7..8] temperature, [9..12] capacities: unknown, 0. */
	put16(&out[13], (uint16_t)bat.at_ms);
	put16(&out[15], (uint16_t)(bat.at_ms >> 16));
	return MG_POWER_REPORT_LEN;
}

enum mg_boot_power mg_power_boot_decision(bool woke_from_off, enum mg_off_kind off, int usb,
					  int mv, int resume_mv)
{
	/* Off on USB and woken with USB gone: the unplug. Arm the plug. */
	if (woke_from_off && off == MG_OFF_ON_USB && usb == 0) {
		return MG_BOOT_OFF;
	}
	/* A low cell (a cell is there: no cell reads under 2.5 V). */
	if (mv >= MG_POWER_NO_CELL_MV && mv < resume_mv) {
		if (usb > 0) {
			return MG_BOOT_CHARGE_DARK;
		}
		if (usb == 0) {
			return MG_BOOT_OFF;
		}
	}
	return MG_BOOT_RUN;
}
