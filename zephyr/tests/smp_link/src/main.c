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

/*
 * Boot like the XIAO: MCUboot (boot_sim.c), then the app's update path
 * (mg_ota_init(), the SMP service on the link), healthy
 * --healthy-ms after it is up (CONFIG_MG_HEALTHY_AFTER_MS on the board,
 * 30 s). --crash-ms restarts before that, as a watchdog would (a test image
 * that never gets healthy). An SMP reset (or that crash) ends the process
 * with status 77; run.py starts it again, which is the next boot.
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "cmdline.h"
#include "posix_board_if.h"
#include "soc.h"

#include "mg_ota.h"
#include "sim.h"
#include "mg_zephyr_version.h"

MG_ZEPHYR_WRAPS_CHECK(); /* __wrap_sys_arch_reboot */

#define EXIT_REBOOT 77

static uint32_t opt_port = 47000;
static uint32_t opt_healthy_ms = 30000;
static uint32_t opt_crash_ms;
static bool opt_never_healthy;
static bool opt_ready_at_healthy;
static uint32_t opt_hold_kb;
bool sim_healthy;
uint32_t sim_boot;

static void add_options(void)
{
	static struct args_struct_t opts[] = {
		{.option = "port", .name = "port", .type = 'u', .dest = &opt_port,
		 .descript = "UDP port of the fake GATT link (47000)"},
		{.option = "healthy-ms", .name = "ms", .type = 'u', .dest = &opt_healthy_ms,
		 .descript = "healthy this long after start (30000, the XIAO's)"},
		{.option = "crash-ms", .name = "ms", .type = 'u', .dest = &opt_crash_ms,
		 .descript = "restart this long after start, before healthy (a watchdog reset)"},
		{.is_switch = true, .option = "never-healthy", .type = 'b',
		 .dest = &opt_never_healthy, .descript = "never reach healthy"},
		{.is_switch = true, .option = "safe", .type = 'b', .dest = &sim_safe_mode,
		 .descript = "run in SAFE mode (from failed boots)"},
		{.is_switch = true, .option = "safe-button", .type = 'b', .dest = &sim_safe_button,
		 .descript = "with --safe: SAFE mode asked with the button"},
		{.is_switch = true, .option = "set-up", .type = 'b', .dest = &sim_set_up,
		 .descript = "set up (a proof key stored; on --release the update gate asks the "
			     "same either way)"},
		{.is_switch = true, .option = "release", .type = 'b', .dest = &sim_release,
		 .descript = "a release build's update rule (unproven writes ask for the press)"},
		{.is_switch = true, .option = "proven", .type = 'b', .dest = &sim_proven,
		 .descript = "the client passed the token proof"},
		{.option = "boot", .name = "n", .type = 'u', .dest = &sim_boot,
		 .descript = "this boot's number (run.py counts them)"},
		{.option = "hold-kb", .name = "kb", .type = 'u', .dest = &opt_hold_kb,
		 .descript = "take this much of the malloc arena at start and keep it (the "
			     "device session's block, which shares the arena with SMP)"},
		{.is_switch = true, .option = "ready-at-healthy", .type = 'b',
		 .dest = &opt_ready_at_healthy,
		 .descript = "answer connects with ready only once healthy"},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(opts);
}

NATIVE_TASK(add_options, PRE_BOOT_1, 10);

/* sys_reboot() (the SMP reset, a crash) ends here: hand back to run.py. */
void __wrap_sys_arch_reboot(int type)
{
	printk("SIM: reboot (%d)\n", type);
	posix_exit(EXIT_REBOOT);
}

static void healthy_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	printk("SIM: boot healthy\n");
	sim_healthy = true;
	if (opt_ready_at_healthy) {
		link_set_ready(true);
	}
	mg_ota_healthy();
}

static K_WORK_DELAYABLE_DEFINE(healthy_work, healthy_fn);

static void crash_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	printk("SIM: crash before healthy (watchdog)\n");
	posix_exit(EXIT_REBOOT);
}

static K_WORK_DELAYABLE_DEFINE(crash_work, crash_fn);

/* The native simulator sets every 'u' option it wasn't given to UINT32_MAX. */
static void option_defaults(void)
{
	if (opt_port == UINT32_MAX) {
		opt_port = 47000;
	}
	if (opt_healthy_ms == UINT32_MAX) {
		opt_healthy_ms = 30000;
	}
	if (opt_crash_ms == UINT32_MAX) {
		opt_crash_ms = 0;
	}
	if (sim_boot == UINT32_MAX) {
		sim_boot = 0;
	}
	if (opt_hold_kb == UINT32_MAX) {
		opt_hold_kb = 0;
	}
}

int main(void)
{
	option_defaults();
	sim_proven_at_connect = sim_proven;
	if (boot_sim_run() != 0) {
		posix_exit(2);
	}
	sim_app_wq_start();
	if (opt_hold_kb) {
		/* As the device session's block (from a hello to the connection's
		 * end): taken before SMP's, from the same arena, never given back. */
		void *held = malloc(opt_hold_kb * 1024U);

		printk("SIM: holding %u KB of the malloc arena%s\n", opt_hold_kb,
		       held ? "" : ": FAILED");
		if (held == NULL) {
			posix_exit(4);
		}
	}
	mg_ota_init();
	if (link_start((uint16_t)opt_port) != 0) {
		posix_exit(3);
	}
	link_set_ready(!opt_ready_at_healthy);
	printk("SIM: up (healthy in %u ms%s%s)\n", opt_healthy_ms,
	       opt_never_healthy ? ", never" : "", sim_safe_mode ? ", SAFE mode" : "");
	if (opt_crash_ms) {
		k_work_reschedule(&crash_work, K_MSEC(opt_crash_ms));
	}
	if (!opt_never_healthy && !opt_crash_ms) {
		k_work_reschedule(&healthy_work, K_MSEC(opt_healthy_ms));
	}
	return 0;
}
