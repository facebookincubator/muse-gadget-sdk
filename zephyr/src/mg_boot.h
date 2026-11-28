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

#ifndef MG_BOOT_H_
#define MG_BOOT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The boot record: kept in RAM that survives a
 * reset (no-init RAM; a power-on or a wake from System OFF loses it, which
 * starts the count over), checked by a magic number and a CRC.
 *
 * - unhealthy: boots in a row that never got healthy (BLE up and
 *   advertising, then CONFIG_MG_HEALTHY_AFTER_MS with the watchdog's
 *   threads alive). Each boot adds one; getting healthy clears it. When the
 *   boots before this one reach CONFIG_MG_SAFE_MODE_BOOTS, this boot runs in
 *   SAFE mode (BLE, firmware updates, no audio or app logic): a crash loop
 *   escapes by itself to a mode the app can update over BLE.
 * - crash: a breadcrumb the fatal error handler or the watchdog supervisor
 *   writes just before the reset: what happened, the PC, the thread, the
 *   uptime. The next boot logs it, keeps it (settings mg/crash) and clears
 *   it here.
 *
 * Portable: no driver calls (mg_health.c owns the RAM and the hardware).
 */

#define MG_BOOT_MAGIC 0x4d474231u /* "MGB1" */
#define MG_BOOT_WHAT  40

struct mg_boot_crash {
	uint32_t reason; /* k_fatal_error_reason, or MG_BOOT_REASON_WATCHDOG */
	uint32_t pc;
	uint32_t uptime_s;
	char what[MG_BOOT_WHAT]; /* NUL-terminated */
};

/* A slot of `bytes` copied at `kbps` KB/s by a bootloader that doesn't
 * feed a running watchdog, plus `margin_ms` for its own start and the image's
 * boot to the early watchdog start, fits the watchdog's window. */
#define MG_BOOT_COPY_FITS(bytes, kbps, timeout_ms, margin_ms)                                  \
	((uint64_t)(bytes) * 1000u / ((uint64_t)(kbps) * 1024u) + (margin_ms) < (timeout_ms))

#define MG_BOOT_REASON_WATCHDOG   0x100
#define MG_BOOT_REASON_UNREACHABLE 0x101 /* mg_reach_policy.h: a loss no peer could cause */
#define MG_BOOT_REASON_PEER_LOSS   0x102 /* a restart a peer could cause */

/* mg_boot_rec.flags */
#define MG_BOOT_F_COUNTED    0x01 /* this boot is counted unhealthy (until healthy) */
#define MG_BOOT_F_SAFE_REACH 0x02 /* this SAFE came from reachability losses */
#define MG_BOOT_F_PEER_SHIFT 2    /* bits 2-3: peer restarts since the last owner clear */
#define MG_BOOT_F_PEER_MASK  0x0c
/* CONFIG_MG_BENCH_MODE's scripted requests for the next boot (a bench word sets
 * one, then a warm restart; main.c takes it as the boot hold would). */
#define MG_BOOT_F_BENCH_SAFE  0x10 /* SAFE, as the button's (reason bench) */
#define MG_BOOT_F_BENCH_RESET 0x20 /* the setup reset hold */
#define MG_BOOT_F_BENCH_MASK  (MG_BOOT_F_BENCH_SAFE | MG_BOOT_F_BENCH_RESET)

struct mg_boot_rec {
	uint32_t magic;
	uint8_t unhealthy;
	uint8_t has_crash;
	uint16_t boots; /* since power-on, for the log */
	uint8_t reach_resets; /* reachability losses in a row (mg_boot_reach_reset) */
	uint8_t safe_reach;   /* unreachable restarts in a row inside SAFE */
	uint8_t att_dead;     /* ATT-dead links in a row, across a peer restart */
	uint8_t flags;        /* MG_BOOT_F_* */
	struct mg_boot_crash crash;
	uint32_t crc;
};

/* Board-ready parts (mg_health_ready()). */
#define MG_READY_CORE  0x01 /* settings loaded, the Muse layer (core, session, setup) up */
#define MG_READY_AUDIO 0x02 /* the mic path: its source (PDM, a board's mic) is ready */
#define MG_READY_QUEUE 0x04 /* the clip store mounted (where the build has one) */
#define MG_READY_BOARD 0x08 /* mg_board_ready() */

static inline bool mg_boot_board_ready(uint32_t need, uint32_t have)
{
	return (have & need) == need;
}

enum mg_boot_mode {
	MG_BOOT_NORMAL,
	MG_BOOT_SAFE,
};

/*
 * At boot: validates the record (zeroes it if the magic or CRC is wrong),
 * copies a breadcrumb into *crash (has_crash true) and clears it, counts
 * this boot as unhealthy until mg_boot_healthy(), and picks the mode:
 * SAFE when safe_after boots before this one never got healthy.
 */
enum mg_boot_mode mg_boot_start(struct mg_boot_rec *r, uint8_t safe_after,
				struct mg_boot_crash *crash, bool *has_crash);
/* This boot got healthy: the unhealthy count starts over. SAFE's unreachable
 * restarts don't: only reachability itself clears them (mg_boot_reach_clear();
 * the healthy timer can fire during an unreachable stretch). */
void mg_boot_healthy(struct mg_boot_rec *r);
/* The record's magic and CRC hold. */
bool mg_boot_valid(const struct mg_boot_rec *r);
/* A restart a peer could cause (a link with no completed exchange, three
 * ATT-dead links, a stuck TX path). It sheds the peer and never counts toward
 * SAFE: this boot's own unhealthy count is taken back, and the ATT-dead run
 * starts over. */
void mg_boot_peer_restart(struct mg_boot_rec *r);
/* Peer restarts since the last token-proven session (0-3; the restart rate
 * bound): only a token proof, the user's button or a new image clears them
 * (mg_boot_peer_bound_clear()); a plain peer exchange, a forced restart or a
 * healthy boot never does. */
unsigned int mg_boot_peer_restarts(const struct mg_boot_rec *r);
void mg_boot_peer_bound_clear(struct mg_boot_rec *r);
/* A completed ATT exchange: the ATT-dead run starts over. */
void mg_boot_exchange(struct mg_boot_rec *r);
/* mg_reach_policy.h: an unreachable boot (lost: after it had been
 * reachable) is about to be reset. A loss counts across boots: the boot after
 * one gets healthy, so the count (cleared only by mg_boot_reach_clear()) is
 * carried into the unhealthy count, and three in a row reach SAFE. Inside SAFE
 * (nothing left to escalate to) the restarts are bounded: false after
 * MG_BOOT_SAFE_REACH_MAX in a row, and the caller stays up. */
#define MG_BOOT_SAFE_REACH_MAX 3
bool mg_boot_reach_reset(struct mg_boot_rec *r, bool safe, bool lost);
/* Reachable long enough (MG_REACH_CLEAR_MS): the loss count and SAFE's
 * unreachable restarts start over. True when this was a SAFE that came from
 * reachability losses (MG_BOOT_F_SAFE_REACH): the unhealthy count is cleared
 * too, so the caller restarts it into normal mode. */
bool mg_boot_reach_clear(struct mg_boot_rec *r, bool safe_by_button);
/* The run of ATT-dead links, kept here so a reset doesn't lose it; a healthy
 * boot leaves it, only a completed exchange or the peer restart its third
 * link makes clears it. */
void mg_boot_att_dead_set(struct mg_boot_rec *r, uint8_t n);
/* Leaves a breadcrumb for the next boot (from a fault handler: no locks). */
void mg_boot_crash(struct mg_boot_rec *r, uint32_t reason, uint32_t pc, uint32_t uptime_s,
		   const char *what);
/* "what (reason N) at pc 0x..., up N s" */
size_t mg_boot_crash_text(const struct mg_boot_crash *c, char *buf, size_t len);

/* The mode this boot runs in (mg_health.c sets it). */
bool mg_health_safe(void);
void mg_health_set_mode(enum mg_boot_mode m);
/* SAFE as the button's, asked by a bench-mode image's scripted request
 * (declaration reason bench, mgcommands.h; CONFIG_MG_BENCH_MODE only). */
void mg_health_set_safe_bench(void);
bool mg_health_safe_bench(void);
/* A bench request for the next boot, kept in the boot record (MG_BOOT_F_BENCH_*),
 * and its take at boot (cleared then; always 0 without CONFIG_MG_BENCH_MODE, a
 * stale one dropped). */
void mg_boot_bench_set(struct mg_boot_rec *r, uint8_t req);
uint8_t mg_boot_bench_take(struct mg_boot_rec *r, bool bench_mode);
/* SAFE mode asked by the user's button at power-up (the button recovery
 * tier), not reached by failed boots: update writes stay open in it. */
void mg_health_set_safe_by_button(void);
bool mg_health_safe_by_button(void);

#endif
