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

#ifndef MG_HEALTH_H_
#define MG_HEALTH_H_

#include <stdbool.h>
#include <stddef.h>

#include "mg_boot.h"

/*
 * Recovery and diagnostics:
 * - the boot record (mg_boot.h): unhealthy boots in a row, SAFE mode after
 *   CONFIG_MG_SAFE_MODE_BOOTS of them, the crash breadcrumb;
 * - the hardware watchdog (CONFIG_MG_WATCHDOG, the nRF54L15's WDT31), fed
 *   by a supervisor on the system work queue only while the gadget's
 *   threads are alive: the app work queue answers a ping, and the audio and
 *   Bluetooth TX threads beat while they work (mg_health_busy / beat);
 * - the last reset reason (hwinfo) and the breadcrumb, logged at boot and
 *   readable over the Nordic UART console ("reset").
 */

enum mg_health_src {
	MG_HEALTH_AUDIO,
	MG_HEALTH_BLE_TX,
	MG_HEALTH_NSRC,
};

/* First thing in main(): reads the reset cause and the boot record, picks
 * the mode (mg_health_safe()), starts the watchdog. */
void mg_health_init(void);
/* After settings are up: keeps a new breadcrumb (mg/crash) for later. */
void mg_health_persist(void);
/* BLE is up and advertising: healthy CONFIG_MG_HEALTHY_AFTER_MS later
 * (the count starts over and, in normal mode, a test image is confirmed). */
void mg_health_advertising(void);
/* mg_reach_policy.h: starts the reachability check (after the power
 * gate: a charge-dark boot isn't an app boot). Unreachable 30 s after it, or
 * 30 s without advertising or a link once reachable, restarts as a failed
 * boot; bounded inside SAFE. */
void mg_health_reach_start(void);
/* Power off on its way to System OFF: not a loss of reachability. */
void mg_health_reach_exempt(bool on);
/* A link ended (mg_ble.c): counts the ATT-dead run in the boot record
 * (mg_reach_link_ended()); true on the third, which the reach check turns into
 * a loss. */
bool mg_health_link_ended(bool att_ok, bool own_failed);
/* A completed ATT exchange (mg_ble.c): clears the dead-link run. */
void mg_health_att_exchange(void);
/* The owner came back (a token proof, the user's button, a new image): the
 * peer restart bound starts over. */
void mg_health_peer_bound_clear(const char *why);
/* CONFIG_MG_BENCH_MODE: a scripted SAFE or setup-reset request
 * (MG_BOOT_F_BENCH_SAFE / _RESET, mg_boot.h) for the next boot, taken then as
 * the button's boot hold would be; the caller restarts. This boot's taken
 * request (always 0 without bench mode: a stale one is dropped). */
void mg_health_bench_request(uint8_t req);
/* Board-ready (an image whose board init blocked would otherwise count
 * healthy on uptime and advertising):
 * a boot counts healthy only once every part it needs is up (MG_READY_*,
 * mg_boot.h) and mg_board_ready() says so; otherwise a test image stays on
 * test (MCUboot reverts it) and the boot counts unhealthy toward SAFE. */
void mg_health_ready_need(uint32_t parts);
void mg_health_ready(uint32_t part);
bool mg_board_ready(void);
uint8_t mg_health_bench_taken(void);
/* A peer loss now would restart the gadget (not on test, not in SAFE, inside
 * the rate bound): the destructive bench rows need that way back. */
bool mg_health_peer_restart_allowed(void);
/* A loss a peer could cause ("peer: ..." names it).
 * Restarts the gadget (never counted toward SAFE; returns only if not), or,
 * past the restart rate bound, drops the link and returns false. */
bool mg_health_peer_loss(const char *what);
unsigned int mg_health_att_dead_run(void);
/* A supervised thread is working (busy) or waiting (idle); while busy it
 * must beat at least every CONFIG_MG_WDT_STALE_MS. */
void mg_health_busy(enum mg_health_src src, bool busy);
void mg_health_beat(enum mg_health_src src);
/* Before System OFF. */
void mg_health_stop_watchdog(void);
/* Feeds the watchdog now (just before an SMP reset, mg_ota.c), unless the
 * supervisor has stopped feeding it. */
void mg_health_feed_now(void);
/* One line: reset reason, unhealthy boots, mode, last crash. */
size_t mg_health_status(char *buf, size_t len);

/* The reset cause hwinfo gave at boot (RESET_* flags; 0 without hwinfo). */
uint32_t mg_health_reset_cause(void);

/* The watchdog supervisor's starving mark at a tick where no thread is stale:
 * a stall that recovered before the watchdog fired ends it, so the boot can
 * still reach its healthy mark (an update keeps itself) and the feeds that
 * check it resume. */
static inline bool mg_health_starving_after(bool starving, bool stale)
{
	return stale && starving;
}

#endif
