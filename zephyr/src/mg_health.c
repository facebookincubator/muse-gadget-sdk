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

#include "mg_health.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/reboot.h>
#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif
#if defined(CONFIG_MG_WATCHDOG)
#include <zephyr/drivers/watchdog.h>
#endif

#include "mg_app.h"
#include "mg_clock.h"
#include "mg_reach_policy.h"
#include "mg_safe_policy.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_DFU)
#include "mg_ota.h"
#endif

LOG_MODULE_REGISTER(mg_health, CONFIG_MG_LOG_LEVEL);

#define CRASH_KEY "mg/crash"

/* Survives a reset (not a power-on, not a wake from System OFF): only if no
 * bootloader zeroes it on the way (a compressed-image MCUboot, for one,
 * zeroes its large LZMA dictionary in RAM at every boot, where the app's
 * .noinit can land). A board reserves a block at the top of SRAM, outside
 * the app's RAM and above anything its shipped MCUboot touches (devicetree
 * node mg_retained, "zephyr,memory-region", NOLOAD so the app's startup
 * doesn't clear it either), and says how high its MCUboot's RAM reaches
 * (CONFIG_MG_BOOT_RAM_END); the build fails if the record could overlap it. The record carries its own
 * magic and CRC. */
#define RETAINED DT_NODELABEL(mg_retained)
#if DT_NODE_EXISTS(RETAINED)
#include <zephyr/linker/devicetree_regions.h>
static struct mg_boot_rec rec __attribute__((section(LINKER_DT_NODE_REGION_NAME(RETAINED))))
	__used;
BUILD_ASSERT(sizeof(struct mg_boot_rec) <= DT_REG_SIZE(RETAINED), "mg_retained too small");
BUILD_ASSERT(DT_REG_ADDR(RETAINED) >= CONFIG_MG_BOOT_RAM_END,
	     "the boot record overlaps RAM the bootloader uses (CONFIG_MG_BOOT_RAM_END)");
BUILD_ASSERT(DT_REG_ADDR(RETAINED) >= DT_REG_ADDR(DT_CHOSEN(zephyr_sram)) +
					     DT_REG_SIZE(DT_CHOSEN(zephyr_sram)) ||
		     DT_REG_ADDR(RETAINED) + DT_REG_SIZE(RETAINED) <=
			     DT_REG_ADDR(DT_CHOSEN(zephyr_sram)),
	     "mg_retained must lie outside the app's RAM (shrink zephyr,sram)");
#if defined(USE_PARTITION_MANAGER) && USE_PARTITION_MANAGER
/* The nRF Connect SDK's partition manager gives the app its RAM from
 * pm_static's sram_primary, not zephyr,sram: shrink both. */
#include <pm_config.h>
#if defined(PM_SRAM_ADDRESS) && defined(PM_SRAM_SIZE)
BUILD_ASSERT(DT_REG_ADDR(RETAINED) >= PM_SRAM_ADDRESS + PM_SRAM_SIZE ||
		     DT_REG_ADDR(RETAINED) + DT_REG_SIZE(RETAINED) <= PM_SRAM_ADDRESS,
	     "mg_retained must lie outside the partition manager's sram_primary");
#endif
#endif
#else
BUILD_ASSERT(CONFIG_MG_BOOT_RAM_END == 0,
	     "a board that names its bootloader's RAM (CONFIG_MG_BOOT_RAM_END) needs an "
	     "mg_retained block for the boot record");
static __noinit struct mg_boot_rec rec;
#endif

/* Every read-modify-seal of rec takes this (it masks interrupts, so the
 * watchdog's callback is serialized too); the fatal error handler alone
 * doesn't, since it never returns. */
static struct k_spinlock rec_lock;
/* The last committed record (this boot's RAM): a fault inside a locked change
 * finds rec half-sealed, so the fatal handler starts from this copy instead of
 * losing every count. */
static struct mg_boot_rec rec_committed;
static volatile bool rec_committed_ok;
#define REC_COMMIT()                                                                               \
	do {                                                                                       \
		rec_committed_ok = false;                                                          \
		rec_committed = rec;                                                               \
		rec_committed_ok = true;                                                           \
	} while (0)
#define REC_LOCKED(stmt)                                                                           \
	do {                                                                                       \
		k_spinlock_key_t _k = k_spin_lock(&rec_lock);                                      \
		stmt;                                                                              \
		REC_COMMIT();                                                                      \
		k_spin_unlock(&rec_lock, _k);                                                      \
	} while (0)

static uint32_t reset_cause;
static uint8_t unhealthy_before;
static enum mg_boot_mode boot_mode;
static bool healthy;
static struct mg_boot_crash new_crash;
static bool have_new_crash;
static char last_crash[96]; /* this boot's breadcrumb, or the kept one */

static atomic_t busy[MG_HEALTH_NSRC];
static atomic_t last[MG_HEALTH_NSRC];
static const char *const SRC_NAMES[] = {"audio thread", "Bluetooth TX thread"};

static struct k_work_delayable healthy_work;

void mg_health_beat(enum mg_health_src src)
{
	atomic_set(&last[src], (atomic_val_t)k_uptime_get_32());
}

void mg_health_busy(enum mg_health_src src, bool b)
{
	mg_health_beat(src);
	atomic_set(&busy[src], b);
}

static const char *cause_text(uint32_t c, char *buf, size_t len)
{
	size_t n = 0;

	buf[0] = '\0';
#if defined(CONFIG_HWINFO)
	static const struct {
		uint32_t bit;
		const char *name;
	} names[] = {
		{RESET_POR, "power-on"},        {RESET_PIN, "reset pin"},
		{RESET_SOFTWARE, "software"},   {RESET_WATCHDOG, "watchdog"},
		{RESET_BROWNOUT, "brownout"},   {RESET_CPU_LOCKUP, "CPU lockup"},
		{RESET_DEBUG, "debugger"},      {RESET_LOW_POWER_WAKE, "wake from power off"},
		{RESET_SECURITY, "security"},   {RESET_HARDWARE, "hardware"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		if ((c & names[i].bit) && n + strlen(names[i].name) + 3 < len) {
			n += snprintf(&buf[n], len - n, "%s%s", n ? ", " : "", names[i].name);
		}
	}
#endif
	if (n == 0) {
		snprintf(buf, len, c ? "0x%x" : "power-on", (unsigned int)c);
	}
	return buf;
}

size_t mg_health_status(char *buf, size_t len)
{
	char cause[64];
	int n = snprintf(buf, len, "reset: %s; unhealthy boots before: %u; mode: %s%s; crash: %s",
			 cause_text(reset_cause, cause, sizeof(cause)), unhealthy_before,
			 mg_health_safe() ? "SAFE" : "normal", healthy ? ", healthy" : "",
			 last_crash[0] ? last_crash : "none");

	return n < 0 ? 0 : MIN((size_t)n, len ? len - 1 : 0);
}

/* ---- peer-caused losses ---- */

static bool peer_late_used;

/* The board hears about a peer restart right before it (mg_app.h: e.g. a
 * loader that counts boot attempts); nothing by default. */
__weak void mg_board_peer_restart(void)
{
}

/* A peer loss never restarts while the image is on test (MCUboot would
 * revert the update) or in SAFE mode (a restart buys nothing there, and out
 * of the button's SAFE it would put a broken image back in the owner's way).
 * Read before rec_lock: a flash read. */
static bool peer_no_restart(void)
{
#if defined(CONFIG_MG_DFU)
	if (!mg_ota_confirmed()) {
		return true;
	}
#endif
	return mg_health_safe();
}

bool mg_health_peer_restart_allowed(void)
{
	return mg_reach_peer_restart_ok(mg_boot_peer_restarts(&rec), k_uptime_get(),
					peer_late_used, peer_no_restart());
}

bool mg_health_peer_loss(const char *what)
{
	bool restart;
	unsigned int n;
	bool no_restart = peer_no_restart();
	k_spinlock_key_t key = k_spin_lock(&rec_lock);

	n = mg_boot_peer_restarts(&rec);
	restart = mg_reach_peer_restart_ok(n, k_uptime_get(), peer_late_used, no_restart);
	if (restart) {
		mg_boot_peer_restart(&rec);
		mg_boot_crash(&rec, MG_BOOT_REASON_PEER_LOSS, 0, k_uptime_get_32() / 1000, what);
	} else {
		mg_boot_att_dead_set(&rec, 0);
	}
	REC_COMMIT();
	k_spin_unlock(&rec_lock, key);
	if (!restart) {
		/* The bound: the link goes, the gadget stays up and advertising. */
		LOG_WRN("%s: %s; dropping the link instead", what,
			no_restart ? "on test or in SAFE mode (no restart there)"
				   : "3 restarts since the last proven session");
		mg_transport_disconnect();
		return false;
	}
	if (n >= MG_REACH_PEER_RESTARTS) {
		peer_late_used = true;
	}
	mg_board_peer_restart();
	LOG_ERR("%s: restarting (sheds the peer; never counts toward SAFE)", what);
	LOG_PANIC();
	sys_reboot(SYS_REBOOT_WARM);
	return true;
}

/* The peer restart bound starts over only with the owner: a token proof
 * (mg_token_proof.c), the user's button at power-up (main.c), or a new image
 * (mg_ota.c). */
void mg_health_peer_bound_clear(const char *why)
{
	if (mg_boot_peer_restarts(&rec)) {
		REC_LOCKED(mg_boot_peer_bound_clear(&rec));
		LOG_INF("peer restart bound starts over (%s)", why);
	}
}

/* mg_token_proof.c's hook on a match (weak there): the peer restart bound
 * starts over, clock settings this connection changed before the proof are
 * saved now (mg_clock.c), and the link's ATT evidence moves recovery state
 * from now (mg_transport_gate_passed()). */
void mg_token_proof_on_match(void)
{
	mg_health_peer_bound_clear("token proof");
	mg_clock_proof_matched();
	mg_transport_gate_passed(); /* the link's ATT evidence counts from now */
}

/* ---- watchdog ---- */

#if defined(CONFIG_MG_WATCHDOG)
static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;
static atomic_t supervising;
static bool starving;
static struct k_work_delayable supervise_work;
static struct k_work ping_work;
static atomic_t ping_pending;
static uint32_t ping_sent;

static void wdt_cb(const struct device *dev, int channel)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel);
	/* About 60 us before the reset: no logging, only the breadcrumb (kept
	 * if the supervisor already wrote one). */
	REC_LOCKED(mg_boot_crash(&rec, MG_BOOT_REASON_WATCHDOG, 0, k_uptime_get_32() / 1000,
				 atomic_get(&supervising) ? "watchdog: system work queue stuck"
							  : "watchdog: hung before the supervisor"));
}

static const struct wdt_timeout_cfg wdt_cfg = {
	.window.max = CONFIG_MG_WDT_TIMEOUT_MS,
	.callback = wdt_cb,
	.flags = WDT_FLAG_RESET_SOC,
};

/* The watchdog runs from the first moments of a boot, not from the
 * supervisor's start, so a hang anywhere between a reset and main() (a
 * driver's init, a kernel service) still resets the gadget.
 * Same timeout (the WDT can't be changed once running, and a cold boot to
 * main() takes well under a second); the supervisor takes the channel over. */
static int wdt_early_start(void)
{
	int ch;
	int err;

	if (!device_is_ready(wdt)) {
		return 0; /* the supervisor logs it */
	}
	ch = wdt_install_timeout(wdt, &wdt_cfg);
	if (ch < 0) {
		return 0;
	}
	/* Never WDT_OPT_PAUSE_IN_SLEEP: it runs while the CPU sleeps (nRF
	 * CONFIG.SLEEP = Run), so a hang in idle resets too. A debugger's halt
	 * pauses it only in test and bench builds; a release keeps counting. */
	err = wdt_setup(wdt, (IS_ENABLED(CONFIG_MG_AUTOMATION) || IS_ENABLED(CONFIG_MG_BENCH_MODE))
				     ? WDT_OPT_PAUSE_HALTED_BY_DBG
				     : 0);
	/* An error: still running from before a soft reset, with the same
	 * settings; channel 0 still feeds it. Fed now either way: after a soft
	 * reset the bootloader's copy has used part of the window, and the rest
	 * of the boot to the supervisor gets a whole one. */
	ARG_UNUSED(err);
	wdt_channel = ch;
	(void)wdt_feed(wdt, ch);
	return 0;
}

/* The board feeds what else it must each time the app feeds the watchdog
 * (mg_app.h: a loader's watchdog channels or a second instance); nothing by
 * default. */
__weak void mg_board_wdt_fed(void)
{
}

static void feed(void)
{
	(void)wdt_feed(wdt, wdt_channel);
	mg_board_wdt_fed();
}

void mg_health_feed_now(void)
{
	if (wdt_channel >= 0 && !starving) {
		feed();
	}
}

/* A bootloader that doesn't feed a running WDT (CONFIG_MG_WDT_BOOT_COPY_KBPS
 * set: an overwrite-only MCUboot built without CONFIG_BOOT_WATCHDOG_FEED)
 * copies a whole slot inside one window after an SMP reset; the build refuses
 * a slot too big for it. */
#if CONFIG_MG_WDT_BOOT_COPY_KBPS > 0
#if defined(USE_PARTITION_MANAGER) && USE_PARTITION_MANAGER
/* The partition manager's slot, not the board dtsi's: under the nRF Connect
 * SDK's partition manager the devicetree's slot0_partition can keep the
 * board's size while pm_static gives the slot another. */
#include <pm_config.h>
#define MG_SLOT0_BYTES PM_MCUBOOT_PRIMARY_SIZE
#elif DT_NODE_EXISTS(DT_NODELABEL(slot0_partition))
#define MG_SLOT0_BYTES DT_REG_SIZE(DT_NODELABEL(slot0_partition))
#endif
#endif
#if CONFIG_MG_WDT_BOOT_COPY_KBPS > 0 && defined(MG_SLOT0_BYTES)
BUILD_ASSERT(MG_BOOT_COPY_FITS(MG_SLOT0_BYTES,
			       CONFIG_MG_WDT_BOOT_COPY_KBPS, CONFIG_MG_WDT_TIMEOUT_MS,
			       CONFIG_MG_WDT_BOOT_MARGIN_MS),
	     "the bootloader's slot copy at CONFIG_MG_WDT_BOOT_COPY_KBPS doesn't fit the "
	     "watchdog's window: raise CONFIG_MG_WDT_TIMEOUT_MS");
#endif
SYS_INIT(wdt_early_start, PRE_KERNEL_2, 1);

#if defined(CONFIG_MG_BENCH_EARLY_HANG)
/* Bench check only: hangs after the early start and before the supervisor, so
 * the watchdog must reset the gadget (a test image: MCUboot then swaps the
 * previous one back). */
static int bench_early_hang(void)
{
	printk("bench: hanging before the watchdog supervisor (CONFIG_MG_BENCH_EARLY_HANG)\n");
	for (;;) {
	}
	return 0;
}
SYS_INIT(bench_early_hang, APPLICATION, 0);
#endif

static void ping_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	atomic_set(&ping_pending, 0);
}

static const char *stale_thread(uint32_t now)
{
	for (int i = 0; i < MG_HEALTH_NSRC; i++) {
		if (atomic_get(&busy[i]) &&
		    now - (uint32_t)atomic_get(&last[i]) > CONFIG_MG_WDT_STALE_MS) {
			return SRC_NAMES[i];
		}
	}
	if (atomic_get(&ping_pending) && now - ping_sent > CONFIG_MG_WDT_STALE_MS) {
		return "app work queue";
	}
	if (mg_transport_tx_dead()) {
		return "Bluetooth TX path"; /* progress includes the TX path */
	}
	return NULL;
}

static void supervise_fn(struct k_work *w)
{
	uint32_t now = k_uptime_get_32();
	const char *stale = stale_thread(now);

	ARG_UNUSED(w);
	if (!stale) {
		if (starving) {
			LOG_WRN("watchdog: the stall recovered before the reset");
		}
		starving = mg_health_starving_after(starving, false);
		feed();
	} else if (!starving) {
		char what[MG_BOOT_WHAT];

		starving = true;
		if (strcmp(stale, "app work queue") == 0 &&
		    mg_transport_evt_saturated(CONFIG_MG_WDT_STALE_MS)) {
			/* A central's traffic held the app work queue
			 * (mg_evt_policy.h). A peer can cause that, so a clean restart
			 * sheds it and never counts toward SAFE; past the bound the
			 * link goes instead, and the queue gets a fresh window to
			 * catch up (with no link it's no longer saturated, so a
			 * queue still stuck after it is a crash). */
			if (!mg_health_peer_loss("peer: event queue saturated")) {
				starving = false;
				ping_sent = now;
				(void)wdt_feed(wdt, wdt_channel);
				mg_board_wdt_fed();
				goto ping;
			}
		}
		if (strcmp(stale, "Bluetooth TX path") == 0) {
			/* The TX path alone is stuck, every thread answering: a
			 * peer can cause that (it stops acknowledging), so a clean
			 * restart sheds it and never counts toward SAFE; past the
			 * rate bound the link goes instead and the watchdog keeps
			 * being fed. */
			if (!mg_health_peer_loss("peer: TX path stuck")) {
				starving = false;
				feed();
				goto ping;
			}
		}
		snprintf(what, sizeof(what), "watchdog: %s stuck", stale);
		REC_LOCKED(mg_boot_crash(&rec, MG_BOOT_REASON_WATCHDOG, 0, now / 1000, what));
		LOG_ERR("%s: the watchdog resets the gadget", what);
		LOG_PANIC();
	}
ping:
	if (!atomic_get(&ping_pending)) {
		atomic_set(&ping_pending, 1);
		ping_sent = now;
		k_work_submit_to_queue(mg_app_wq(), &ping_work);
	}
	k_work_reschedule(&supervise_work, K_MSEC(1000));
}

static void watchdog_start(void)
{
	if (!device_is_ready(wdt)) {
		LOG_ERR("watchdog not ready");
		return;
	}
	if (wdt_channel < 0) {
		/* The early start didn't get it (logged here, where logging runs). */
		wdt_channel = wdt_install_timeout(wdt, &wdt_cfg);
		if (wdt_channel < 0) {
			LOG_ERR("watchdog: no channel (%d)", wdt_channel);
			return;
		}
		int err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);

		if (err) {
			/* Still running from before a soft reset: it keeps the
			 * old settings, and channel 0 still feeds it. */
			LOG_WRN("watchdog setup %d (already running?)", err);
		}
	} else {
		LOG_INF("watchdog: running since early boot; the supervisor takes it over");
	}
	atomic_set(&supervising, 1);
	k_work_init(&ping_work, ping_fn);
	k_work_init_delayable(&supervise_work, supervise_fn);
	k_work_reschedule(&supervise_work, K_NO_WAIT);
	LOG_INF("watchdog: %d ms, threads stale after %d ms", CONFIG_MG_WDT_TIMEOUT_MS,
		CONFIG_MG_WDT_STALE_MS);
}

void mg_health_stop_watchdog(void)
{
	(void)k_work_cancel_delayable(&supervise_work);
	if (wdt_channel >= 0 && wdt_disable(wdt) != 0) {
		/* Can't stop it: feed it one last time before System OFF. */
		feed();
	}
}

static bool threads_alive(void)
{
	return stale_thread(k_uptime_get_32()) == NULL && !starving;
}
#else
void mg_health_stop_watchdog(void)
{
}

void mg_health_feed_now(void)
{
}

static bool threads_alive(void)
{
	return true;
}
#endif

/* ---- faults ---- */

#if defined(CONFIG_MG_FATAL_REBOOT)
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	uint32_t pc = 0;
	char what[MG_BOOT_WHAT];
	const char *name = k_thread_name_get(k_current_get());

#if defined(CONFIG_ARM)
	if (esf) {
		pc = esf->basic.pc;
	}
#else
	ARG_UNUSED(esf);
#endif
	snprintf(what, sizeof(what), "fault in %s", name && name[0] ? name : "a thread");
	if (!mg_boot_valid(&rec) && rec_committed_ok) {
		rec = rec_committed; /* a fault mid-change: keep the last committed counts */
	}
	mg_boot_crash(&rec, reason, pc, k_uptime_get_32() / 1000, what);
	LOG_PANIC();
	LOG_ERR("fatal error %u in %s at 0x%08x: restarting", reason, what, pc);
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}
#endif

/* ---- boot ---- */

/* The reset cause as the board knows it (mg_app.h): hwinfo's by default. */
__weak uint32_t mg_board_reset_cause(uint32_t hw)
{
	return hw;
}

/* The board hears about the healthy point (mg_app.h); nothing by default. */
__weak void mg_board_healthy(void)
{
}

/* Board-ready: what this boot needs up before it
 * counts healthy (MG_READY_*, main.c), and what came up. */
static atomic_t ready_need = ATOMIC_INIT(MG_READY_CORE);
static atomic_t ready_have;
static bool ready_logged;

void mg_health_ready_need(uint32_t parts)
{
	atomic_set(&ready_need, (atomic_val_t)parts);
}

void mg_health_ready(uint32_t part)
{
	atomic_or(&ready_have, (atomic_val_t)part);
}

/* A board's own readiness (its mic, sensor or codec up): weak, ready. */
__weak bool mg_board_ready(void)
{
	return !IS_ENABLED(CONFIG_MG_BENCH_BOARD_NOT_READY);
}

static void healthy_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	uint32_t need = (uint32_t)atomic_get(&ready_need), have = (uint32_t)atomic_get(&ready_have);

	if (mg_board_ready()) {
		have |= MG_READY_BOARD;
	}
	if (!threads_alive() || !mg_boot_board_ready(need, have)) {
		if (!ready_logged && !mg_boot_board_ready(need, have)) {
			ready_logged = true;
			LOG_WRN("not healthy yet: the board isn't ready (missing 0x%x: 1 core, 2 audio, "
				"4 clip store, 8 board)",
				need & ~have);
		}
		k_work_reschedule_for_queue(mg_app_wq(), &healthy_work, K_SECONDS(5));
		return;
	}
	healthy = true;
	REC_LOCKED(mg_boot_healthy(&rec));
	mg_board_healthy();
	LOG_INF("boot healthy (%d s advertising, threads alive, board ready): the failed-boot count "
		"starts over",
		CONFIG_MG_HEALTHY_AFTER_MS / 1000);
#if defined(CONFIG_MG_DFU)
	/* In normal mode a test image is kept now (mg_ota_policy.h), and the
	 * clip store comes back. */
	mg_ota_healthy();
#endif
}

/* ---- reachability (mg_reach_policy.h) ---- */

#if defined(CONFIG_MG_REACH_RESET)
static struct k_work_delayable reach_work;
static struct mg_reach reach;
static atomic_t reach_exempt;

static atomic_t att_loss;

bool mg_health_link_ended(bool att_ok, bool own_failed)
{
	bool loss;
	k_spinlock_key_t key = k_spin_lock(&rec_lock);
	struct mg_reach_links l = {.dead = rec.att_dead};

	loss = mg_reach_link_ended(&l, att_ok, own_failed);
	if (l.dead != rec.att_dead) {
		mg_boot_att_dead_set(&rec, l.dead);
	}
	REC_COMMIT();
	k_spin_unlock(&rec_lock, key);
	if (loss) {
		atomic_set(&att_loss, 1);
	}
	return loss;
}

void mg_health_att_exchange(void)
{
	/* Only an exchange clears the dead-link run, and a loss the last dead
	 * link left pending (a later good link isn't dropped for the one
	 * before it). */
	REC_LOCKED(mg_boot_exchange(&rec));
	atomic_clear(&att_loss);
}

unsigned int mg_health_att_dead_run(void)
{
	return rec.att_dead;
}

static void reach_fn(struct k_work *w)
{
	int64_t now = k_uptime_get();
	/* A link that came and went between two samples counts too:
	 * connected() latches it, taken here (this sampler's own bit). */
	bool link = mg_transport_link_seen_take(MG_LINK_SEEN_REACH) | mg_transport_link_up();
	enum mg_reach_act a = mg_reach_step_link(&reach, now, mg_transport_reachable(),
						 atomic_get(&reach_exempt) != 0, link);
	bool dead_links = false;

	ARG_UNUSED(w);
#if defined(CONFIG_MG_DFU)
	bool smp_busy = mg_smp_mem_blocks() > 0; /* a request or an upload's writer */
#else
	bool smp_busy = false; /* no update support: SMP holds no upload */
#endif
	/* SAFE frees the BLE tier from a silent holder: one slot, held by a
	 * central that writes nothing, is given back (mg_safe_policy.h). */
	if (mg_safe_idle_drop(mg_health_safe(), mg_transport_link_up(), k_uptime_get_32(),
			      mg_transport_peer_write_ms(), smp_busy)) {
		LOG_WRN("safe: idle link dropped (no write in %u s)", MG_SAFE_IDLE_MS / 1000u);
		mg_transport_disconnect();
	}
	if (a == MG_REACH_NONE && !reach.done && atomic_get(&att_loss)) {
		/* Three ATT-dead links in a row; a peer can cause that. */
		reach.done = true;
		a = MG_REACH_RESET_LOST;
		dead_links = true;
	}
	if (a == MG_REACH_CLEAR) {
		bool back;

		REC_LOCKED(back = mg_boot_reach_clear(&rec, mg_health_safe_by_button()));
		LOG_INF("reachable %d min: the reachability count starts over",
			MG_REACH_CLEAR_MS / 60000);
		if (back && mg_health_safe()) {
			/* A SAFE that came from reachability losses goes
			 * back to normal by itself. */
			LOG_WRN("SAFE from reachability losses, reachable %d min: restarting "
				"in normal mode", MG_REACH_CLEAR_MS / 60000);
			LOG_PANIC();
			sys_reboot(SYS_REBOOT_WARM);
		}
	} else if (dead_links || ((a == MG_REACH_RESET_BOOT || a == MG_REACH_RESET_LOST) &&
				  mg_reach_peer_loss(&reach))) {
		(void)mg_health_peer_loss(dead_links ? "peer: ATT dead on 3 links"
						     : "peer: link without ATT 30 s");
		/* Past the rate bound: the link went; watch again from here. */
		atomic_clear(&att_loss);
		mg_reach_init(&reach, now);
		reach.reached = true;
	} else if (a == MG_REACH_RESET_BOOT || a == MG_REACH_RESET_LOST) {
		bool lost = a == MG_REACH_RESET_LOST;
		bool go;

		REC_LOCKED(go = mg_boot_reach_reset(&rec, mg_health_safe(), lost));
		if (!go) {
			/* Inside SAFE: bounded; the button route and SWD
			 * still work. It keeps watching, so 10 min reachable
			 * still clears it and a reach-SAFE still goes back. */
			static bool capped_logged;

			if (!capped_logged) {
				capped_logged = true;
				LOG_ERR("unreachable in SAFE mode after %d restarts in a row: "
					"staying up", MG_BOOT_SAFE_REACH_MAX);
			}
			mg_reach_init(&reach, now);
			reach.reached = true;
			k_work_schedule(&reach_work, K_SECONDS(1));
			return;
		}
		REC_LOCKED(mg_boot_crash(&rec, MG_BOOT_REASON_UNREACHABLE, 0,
					 (uint32_t)(now / 1000),
					 lost ? "unreachable 30 s" : "not reachable at boot"));
		LOG_ERR("%s: restarting as a failed boot",
			lost ? "no advertising and no link for 30 s"
			     : "neither advertising nor a link 30 s after boot");
		LOG_PANIC();
		sys_reboot(SYS_REBOOT_COLD);
		return;
	}
	k_work_schedule(&reach_work, K_SECONDS(1));
}

void mg_health_reach_start(void)
{
	mg_reach_init(&reach, k_uptime_get());
	k_work_init_delayable(&reach_work, reach_fn);
	k_work_schedule(&reach_work, K_SECONDS(1));
}

void mg_health_reach_exempt(bool on)
{
	atomic_set(&reach_exempt, on ? 1 : 0);
}
#else
void mg_health_att_exchange(void)
{
	REC_LOCKED(mg_boot_exchange(&rec));
}


bool mg_health_link_ended(bool att_ok, bool own_failed)
{
	ARG_UNUSED(att_ok);
	ARG_UNUSED(own_failed);
	return false;
}

unsigned int mg_health_att_dead_run(void)
{
	return 0;
}

void mg_health_reach_start(void)
{
}

void mg_health_reach_exempt(bool on)
{
	ARG_UNUSED(on);
}
#endif

void mg_health_advertising(void)
{
	k_work_reschedule_for_queue(mg_app_wq(), &healthy_work,
				    K_MSEC(CONFIG_MG_HEALTHY_AFTER_MS));
}

static int crash_load_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
			 void *param)
{
	ARG_UNUSED(key);
	ARG_UNUSED(param);
	if (len > 0 && len < sizeof(last_crash)) {
		ssize_t n = read_cb(cb_arg, last_crash, len);

		last_crash[n > 0 ? n : 0] = '\0';
	}
	return 0;
}

void mg_health_persist(void)
{
	if (have_new_crash) {
		int err = settings_save_one(CRASH_KEY, last_crash, strlen(last_crash));

		if (err) {
			LOG_WRN("crash breadcrumb not kept (%d)", err);
		}
	} else {
		(void)settings_load_subtree_direct(CRASH_KEY, crash_load_cb, NULL);
		if (last_crash[0]) {
			LOG_INF("earlier crash: %s", last_crash);
		}
	}
}

/* The boot is counted at the first moments of a boot, before the early
 * watchdog can fire: a confirmed image that hangs before main()
 * counts toward SAFE like any boot that never gets healthy. (SAFE itself
 * starts in main(), so it can't escape a hang before it: MCUboot's revert
 * covers an unconfirmed image; a confirmed one needs SWD.) */
static uint8_t bench_req; /* this boot's bench request (MG_BOOT_F_BENCH_*) */

static int boot_count_early(void)
{
	REC_LOCKED({
		unhealthy_before =
			mg_boot_valid(&rec) ? rec.unhealthy : 0;
		boot_mode = mg_boot_start(&rec, CONFIG_MG_SAFE_MODE_BOOTS, &new_crash,
					  &have_new_crash);
		bench_req = mg_boot_bench_take(&rec, IS_ENABLED(CONFIG_MG_BENCH_MODE));
	});
	return 0;
}

uint8_t mg_health_bench_taken(void)
{
	return bench_req;
}

#if defined(CONFIG_MG_BENCH_MODE)
void mg_health_bench_request(uint8_t req)
{
	REC_LOCKED(mg_boot_bench_set(&rec, req));
	LOG_WRN("bench: %s at the next boot (a scripted request, bench builds only)",
		(req & MG_BOOT_F_BENCH_SAFE) ? "SAFE mode" : "the setup reset");
}
#endif
SYS_INIT(boot_count_early, PRE_KERNEL_2, 0);

void mg_health_init(void)
{
	char cause[64];
	enum mg_boot_mode mode;

#if defined(CONFIG_HWINFO)
	if (hwinfo_get_reset_cause(&reset_cause) == 0) {
		(void)hwinfo_clear_reset_cause();
	}
#endif
	reset_cause = mg_board_reset_cause(reset_cause);
	uint8_t before = unhealthy_before;

	mode = boot_mode;
	mg_health_set_mode(mode);
	k_work_init_delayable(&healthy_work, healthy_fn);
	LOG_INF("boot %u since power-on; reset: %s; %u boot(s) before never got healthy",
		rec.boots, cause_text(reset_cause, cause, sizeof(cause)), before);
	if (have_new_crash) {
		mg_boot_crash_text(&new_crash, last_crash, sizeof(last_crash));
		LOG_ERR("last boot crashed: %s", last_crash);
	}
	if (mode == MG_BOOT_SAFE) {
		LOG_ERR("SAFE mode: %u boots in a row never got healthy; Bluetooth and firmware "
			"updates only (no audio, push-to-talk, clips or Link setup) until a "
			"healthy boot",
			before);
	}
#if defined(CONFIG_MG_WATCHDOG)
	watchdog_start();
#endif
}

uint32_t mg_health_reset_cause(void)
{
	return reset_cause;
}
