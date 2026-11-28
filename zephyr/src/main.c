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

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "mg_app.h"
#include "mg_audio.h"
#include "mg_button.h"
#include "mg_clock.h"
#include "mg_boot.h"
#include "mg_core.h"
#include "mg_health.h"
#include "mg_led.h"
#include "mg_reset_hold.h"
#include "mg_power.h"
#if defined(CONFIG_MG_POWER_OFF)
#include "mg_usb.h"
#endif
#if defined(CONFIG_MG_DFU)
#include "mg_ota.h"
#endif
#include "mg_session.h"
#include "mg_settings.h"
#include "mg_resetup_policy.h"
#include "mg_setup.h"
#include "mg_setup_store.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_VM_SESSION)
#include "mg_vm_link.h"
#include "mg_vm_session.h"
#endif
#if defined(CONFIG_MG_AUDIO_QUEUE)
#include "mg_queue.h"
#endif
#if defined(CONFIG_MG_BATTERY_ADC)
#include "mg_battery.h"
#endif

LOG_MODULE_REGISTER(mg_main, CONFIG_MG_LOG_LEVEL);

/* Link setup runs P-256 ECDH (mbedTLS) on this queue. */
K_THREAD_STACK_DEFINE(app_stack, 6144);
static struct k_work_q app_wq;

struct k_work_q *mg_app_wq(void)
{
	return &app_wq;
}

const char *mg_app_version(void)
{
	return CONFIG_BT_DIS_FW_REV_STR;
}

static struct k_work_delayable battery_work;

uint8_t mg_app_battery_percent(void)
{
	return mg_power_battery()->percent;
}

int mg_app_battery_mv(void)
{
	const struct mg_battery_state *b = mg_power_battery();

	return b->measured ? b->mv : -ENODEV;
}

int mg_app_usb_present(void)
{
#if defined(CONFIG_MG_POWER_OFF)
	return mg_usb_present();
#else
	return -ENOTSUP;
#endif
}

/* Battery Level, report_battery_status and the low-battery rules all come
 * from mg_power.c, fed here. */
static void battery_fn(struct k_work *w)
{
	const struct mg_battery_state *b = mg_power_battery();

	ARG_UNUSED(w);
#if defined(CONFIG_MG_BATTERY_ADC)
	uint32_t ev = mg_power_reading(mg_battery_read_mv(), k_uptime_get_32());

	LOG_DBG("battery %d mV, %u%%", b->mv, b->percent);
	/* Low battery: warn at 15 %, flash at 10 %, power off cleanly at
	 * CONFIG_MG_BATTERY_SHUTDOWN_MV, all only with a cell. */
	if (ev & MG_POWER_EV_LOW) {
		LOG_WRN("battery low: %d mV, %u%%", b->mv, b->percent);
	}
#if defined(CONFIG_MG_BATTERY_STATUS)
	if (ev & MG_POWER_EV_REPORT) {
		/* Unsolicited report_battery_status. */
		LOG_INF("battery %s, %u%%: report_battery_status sent",
			b->present ? "present" : "gone", b->percent);
		mg_core_battery_report();
	}
#endif
	if (ev & MG_POWER_EV_CRITICAL) {
		LOG_WRN("battery critical: %d mV, %u%%; the LED flashes every 30 s", b->mv,
			b->percent);
		mg_led_set(MG_LED_LOW_BATTERY, true);
	}
	if (ev & MG_POWER_EV_RECOVERED) {
		LOG_INF("battery fine again: %d mV, %u%%", b->mv, b->percent);
		mg_led_set(MG_LED_LOW_BATTERY, false);
	}
	if (ev & MG_POWER_EV_SHUTDOWN) {
		LOG_ERR("battery empty: %d mV <= %d mV; powering off before a brownout", b->mv,
			CONFIG_MG_BATTERY_SHUTDOWN_MV);
#if defined(CONFIG_MG_POWER_OFF)
		mg_app_power_off("battery empty");
		/* Polled on: if powering off fails the battery is still watched
		 * (a pending power off isn't asked twice). */
		k_work_reschedule(&battery_work, K_SECONDS(10));
		return;
#endif
	}
#endif
	mg_ble_set_battery(b->percent);
	k_work_reschedule(&battery_work, K_SECONDS(b->critical ? 10 : 60));
}

/* A board's extra conditions for the setup reset gesture (e.g. only with
 * the charger present); none by default. */
__weak bool mg_board_reset_hold_allowed(void)
{
	return true;
}

/* Whether the hold passes a SAFE point first (recovery tier 3 by button:
 * every gadget has a button route into SAFE): CONFIG_MG_SAFE_HOLD; a board
 * can narrow it (e.g. only with the charger present). */
__weak bool mg_board_safe_hold_allowed(void)
{
	return IS_ENABLED(CONFIG_MG_SAFE_HOLD);
}

enum boot_hold {
	BOOT_HOLD_NONE,
	BOOT_HOLD_SETUP, /* released before the SAFE cue: setup mode */
	BOOT_HOLD_SAFE,
	BOOT_HOLD_RESET,
};

/* The boot hold (src/mg_reset_hold.h): on a cold boot with the key
 * down, held CONFIG_MG_RESET_HOLD_MS then released (after the LED's
 * "release now" cue) within CONFIG_MG_RESET_RELEASE_MS resets the setup;
 * where the board allows it, a release after the SAFE cue
 * (CONFIG_MG_SAFE_HOLD_MS) and before that boots once in SAFE mode. */
static enum boot_hold held_for_reset(void)
{
	uint32_t cause = mg_health_reset_cause();
	struct mg_rh h = {0};
	bool reset_ok, safe_ok;

	if (!mg_reset_hold_cold(cause)) {
		LOG_INF("button down at boot, but not a cold boot (reset cause 0x%x: a wake "
			"from power off, an update, a watchdog): not a reset gesture",
			cause);
		return BOOT_HOLD_NONE;
	}
	reset_ok = mg_board_reset_hold_allowed();
	safe_ok = mg_board_safe_hold_allowed();
	if (!reset_ok && !safe_ok) {
		LOG_INF("button held at power-up: the board doesn't allow a reset now");
		return BOOT_HOLD_NONE;
	}
	if (safe_ok) {
		LOG_INF("button held at power-up: release after %d s for SAFE mode, after %d s "
			"to reset setup",
			CONFIG_MG_SAFE_HOLD_MS / 1000, CONFIG_MG_RESET_HOLD_MS / 1000);
	} else {
		LOG_INF("button held at power-up: hold %d s, then release, to reset setup",
			CONFIG_MG_RESET_HOLD_MS / 1000);
	}
	/* Times from the uptime, not counted sleeps: a sleep can run long. */
	int64_t start = k_uptime_get();

	for (;;) {
		k_sleep(K_MSEC(50));
		/* Without the board's reset OK the reset point is never reached:
		 * the window starts past any hold. */
		switch (mg_reset_hold_step(&h, mg_button_held_at_boot(),
					   (uint32_t)(k_uptime_get() - start),
					   safe_ok ? CONFIG_MG_SAFE_HOLD_MS : 0,
					   reset_ok ? CONFIG_MG_RESET_HOLD_MS : UINT32_MAX / 2,
					   CONFIG_MG_RESET_RELEASE_MS)) {
		case MG_RH_SAFE_ARM:
			mg_led_set(MG_LED_SAFE_ARMED, true);
			LOG_INF("release now for SAFE mode%s", reset_ok ? " (keep holding to reset setup)" : "");
			break;
		case MG_RH_SAFE:
			mg_led_set(MG_LED_SAFE_ARMED, false);
			return BOOT_HOLD_SAFE;
		case MG_RH_ARM:
			mg_led_set(MG_LED_SAFE_ARMED, false);
			mg_led_set(MG_LED_RESET_ARMED, true);
			LOG_INF("release now to reset setup (within %d s)",
				CONFIG_MG_RESET_RELEASE_MS / 1000);
			break;
		case MG_RH_COMMIT:
			mg_led_set(MG_LED_RESET_ARMED, false);
			return BOOT_HOLD_RESET;
		case MG_RH_CANCEL:
			mg_led_set(MG_LED_RESET_ARMED, false);
			if (!h.armed) {
				/* Released before any cue: a normal boot in setup
				 * mode, Link setup open under the normal identity
				 * (the only way a set-up XIAO takes a re-setup). */
				LOG_INF("released early: setup mode (a re-setup can be confirmed)");
				return BOOT_HOLD_SETUP;
			}
			LOG_INF("held past the release window: no reset");
			return BOOT_HOLD_NONE;
		default:
			break;
		}
	}
}

#if defined(CONFIG_MG_BENCH_MODE)
/* A bench-mode image says so in its symbols; tools/check_mic_gate.py
 * --release refuses it, so it never ships. */
__attribute__((used)) const char mg_bench_mode_image[] = "bench mode: never ships";
#endif

int main(void)
{
	bool held;

	LOG_INF("Muse Gadget %s starting (mg%d)", mg_app_version(), MG_SPEC_VERSION);
	if (strstr(mg_app_version(), ".devkey")) {
		LOG_WRN("signed with a PUBLIC development key (MCUboot's sample key): a development "
			"build, never a release");
	}
#if defined(CONFIG_MG_BENCH_MODE)
	LOG_WRN("%s: every human step scriptable", mg_bench_mode_image);
#endif
	k_work_queue_start(&app_wq, app_stack, K_THREAD_STACK_SIZEOF(app_stack),
			   K_PRIO_PREEMPT(5), NULL);
	k_thread_name_set(&app_wq.thread, "mg_app");
	/* Before any other app code: the boot count, SAFE mode, the
	 * watchdog, and the last reset's reason and breadcrumb. */
	mg_health_init();

	mg_led_init();
	held = mg_button_held_at_boot();

	bool settings_ok = settings_subsys_init() == 0;

	if (!settings_ok) {
		LOG_ERR("settings unavailable; using defaults (this boot won't count healthy)");
	}
	mg_health_persist();
	/* SAFE mode: Bluetooth, the mg service's status and SMP firmware
	 * updates only; no audio, clips, push-to-talk, Link setup or bench. */
	bool safe = mg_health_safe();
	bool setup_hold = false; /* the button held at boot, released early */

#if defined(CONFIG_MG_POWER_OFF)
	/* Plugging in turns it on: an unplug after a power off on USB, or
	 * a low cell, keeps it off or charging dark before the radio starts.
	 * Never in SAFE mode (the recovery tiers come first). */
	if (!safe) {
		mg_power_boot_gate();
	}
#endif
	/* From here a boot must become reachable within 30 s and stay
	 * reachable (after the power gate: charging dark isn't an app boot). */
	mg_health_reach_start();
	(void)mg_settings_load();
	mg_clock_load();
	(void)mg_setup_store_load();

	/* A bench-mode image's scripted SAFE / setup-reset request, taken as
	 * the boot hold would be (board hooks don't apply: the request is the
	 * bench rig's, not a hand's). A SAFE the gadget is already in by itself
	 * becomes the bench's. */
	uint8_t bench = mg_health_bench_taken();

	if (bench & MG_BOOT_F_BENCH_SAFE) {
		mg_health_set_safe_bench();
		mg_health_peer_bound_clear("SAFE asked by the bench");
		safe = true;
		held = false;
		LOG_ERR("SAFE mode (bench request): Bluetooth and firmware updates only until the "
			"next reset; recovery reason bench");
	} else if ((bench & MG_BOOT_F_BENCH_RESET) && !safe) {
		mg_health_peer_bound_clear("reset asked by the bench");
		(void)mg_setup_factory_reset("bench request (scripted setup reset hold)");
		held = false;
	}
	if (safe && held) {
		/* A SAFE the gadget entered by itself still reads the boot hold
		 * (otherwise a set-up gadget's own SAFE has no physical way out).
		 * Either release point makes it the button's SAFE, which takes
		 * Bluetooth updates; the setup reset waits for a normal boot (the
		 * clip store isn't open in SAFE). */
		enum boot_hold h = held_for_reset();

		if ((h == BOOT_HOLD_SAFE || h == BOOT_HOLD_RESET) && !mg_health_safe_by_button()) {
			mg_health_set_safe_by_button();
			mg_health_peer_bound_clear("SAFE taken over by the button");
			LOG_ERR("SAFE mode, now the button's: Bluetooth updates are open until the "
				"next reset%s", h == BOOT_HOLD_RESET ? " (reset setup from a normal boot)" : "");
		}
	} else if (held) {
		switch (held_for_reset()) {
		case BOOT_HOLD_RESET:
			mg_health_peer_bound_clear("reset asked by the button");
			(void)mg_setup_factory_reset("button held at power-up");
			break;
		case BOOT_HOLD_SAFE:
			/* This boot runs as SAFE mode; the boot record's count and the
			 * healthy mark apply as to any SAFE boot. */
			mg_health_set_safe_by_button();
			mg_health_peer_bound_clear("SAFE asked by the button");
			safe = true;
			LOG_ERR("SAFE mode (asked by the button at power-up): Bluetooth and "
				"firmware updates only until the next reset");
			break;
		case BOOT_HOLD_SETUP:
			setup_hold = true;
			break;
		default:
			break;
		}
	}
	/* Board-ready: what this boot needs up before it counts healthy. SAFE
	 * mode runs no audio or clips, so it needs the core only. */
	mg_health_ready_need(MG_READY_CORE |
			     (safe ? 0
				   : MG_READY_AUDIO | MG_READY_BOARD |
					     (IS_ENABLED(CONFIG_MG_AUDIO_QUEUE) ? MG_READY_QUEUE : 0)));
#if defined(CONFIG_MG_AUDIO_QUEUE)
	if (!safe) {
		if (mg_queue_init() == 0) {
			mg_health_ready(MG_READY_QUEUE);
		} else {
			LOG_ERR("clip store not mounted: this boot won't count healthy");
		}
	}
#endif
#if defined(CONFIG_MG_DFU)
	mg_ota_init();
#endif
	if (!safe) {
		/* An owner change's erase, owed from before, once the update's
		 * slot check has run; the queue stays closed until it ends. */
		mg_setup_clips_resume();
	}
	mg_core_init();
	if (!safe) {
		mg_audio_init();
		if (mg_audio_src_probe() == 0) {
			mg_health_ready(MG_READY_AUDIO);
		} else {
			LOG_ERR("mic source not ready: this boot won't count healthy");
		}
	}

	if (mg_ble_init()) {
		return 0;
	}
	/* After BLE: the identity comes from the address. */
	if (!safe) {
		mg_setup_init();
		if (mg_setup_mode_begins(safe, setup_hold)) {
			mg_setup_mode_begin(); /* the button held at boot */
		}
	}
	mg_session_init();
#if defined(CONFIG_MG_VM_SESSION)
	mg_vm_session_init();
	(void)mg_vm_commands_init();
#endif
	mg_core_link_refresh();
#if defined(CONFIG_MG_NUS)
	mg_nus_init();
#endif
#if defined(CONFIG_MG_BATTERY_ADC)
	mg_power_set_shutdown_mv(CONFIG_MG_BATTERY_SHUTDOWN_MV);
#endif
	k_work_init_delayable(&battery_work, battery_fn);
	k_work_reschedule(&battery_work, K_NO_WAIT);
	if (settings_ok) {
		mg_health_ready(MG_READY_CORE); /* settings, core, session and setup up */
	}
	/* Button events count from here: gpio-keys reports edges from boot, and
	 * one before the core, the session and Link setup are up is dropped
	 * (the boot hold above reads the pin itself). */
	mg_button_init();
	int err = mg_ble_start_advertising();

	if (!err) {
		/* Healthy CONFIG_MG_HEALTHY_AFTER_MS from now if the threads keep
		 * up: then the boot count starts over and a test image is kept. */
		mg_health_advertising();
	}
#if defined(CONFIG_MG_BENCH)
	if (!safe) {
		mg_bench_init();
	}
#endif
	return 0;
}
