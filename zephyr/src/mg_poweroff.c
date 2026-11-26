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

/* device_action power_off: System OFF, woken by the
 * talk button (GPIO SENSE wake on the nRF54L15; the chip resets on wake),
 * and on boards that can tell USB power (mg_usb.h), by plugging in.
 * Also the boot's power gate: a power off while plugged in stays
 * off until a key press or an unplug and replug, and a low cell charges
 * dark on USB (no radio) or stays off without it. */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/poweroff.h>
#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

#include "mg_app.h"
#include "mg_audio.h"
#include "mg_button.h"
#include "mg_clock.h"
#include "mg_health.h"
#include "mg_led.h"
#include "mg_power.h"
#include "mg_settings.h"
#include "mg_transport.h"
#include "mg_usb.h"
#if defined(CONFIG_MG_BATTERY_ADC)
#include "mg_battery.h"
#include "mg_button.h"
#endif

LOG_MODULE_REGISTER(mg_poweroff, CONFIG_MG_LOG_LEVEL);

static const struct gpio_dt_spec sw = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const char *reason;
static struct k_work_delayable off_work;

#define OFF_KEY "mg/off"

static int off_load(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		    void *param)
{
	uint8_t *v = param;

	if (key == NULL && len == 1 && read_cb(cb_arg, v, 1) == 1) {
		return 0;
	}
	*v = MG_OFF_NONE;
	return 0;
}

static enum mg_off_kind off_kind_load(void)
{
	uint8_t v = MG_OFF_NONE;

	(void)settings_load_subtree_direct(OFF_KEY, off_load, &v);
	return v <= MG_OFF_ON_USB ? (enum mg_off_kind)v : MG_OFF_NONE;
}

static void off_kind_store(enum mg_off_kind k)
{
	uint8_t v = k;

	if (k == MG_OFF_NONE) {
		(void)settings_delete(OFF_KEY);
	} else if (settings_save_one(OFF_KEY, &v, 1)) {
		LOG_WRN("power off: can't store its kind");
	}
}

/* System OFF now: the key wakes it, and plugging in (unplugging, while
 * plugged in now) where the board can tell. -EBUSY with the key held. */
static int off_now(const char *why)
{
	int usb = mg_usb_present();

	/* Held, the level wake would fire at once. */
	if (gpio_pin_get_dt(&sw) == 1) {
		return -EBUSY;
	}
	int err = gpio_pin_configure_dt(&sw, GPIO_INPUT);

	err = err ?: gpio_pin_interrupt_configure_dt(&sw, GPIO_INT_LEVEL_ACTIVE);
	if (err) {
		LOG_ERR("power off: can't arm the button wake (%d); staying on", err);
		return err;
	}
	off_kind_store(usb > 0 ? MG_OFF_ON_USB : MG_OFF_PLAIN);
	mg_settings_flush(); /* a planned power off: a waiting change goes now */
	/* An unset gadget's clock offset/flags too (mgcommands.h, Clock: once at
	 * a planned power off); a set-up gadget's unproven change never waits to
	 * be flushed (mg_clock.c). */
	mg_clock_flush();
	if (usb >= 0) {
		/* Plugged in: wake at the unplug, only to arm the plug. */
		mg_usb_arm_wake(usb == 0);
	}
	mg_health_stop_watchdog();
	LOG_INF("power off (%s): %s", why,
		usb > 0 ? "press the button, or unplug and plug in again, to wake"
		: usb == 0 ? "press the button or plug in to wake" : "press the button to wake");
	LOG_PANIC();
#if defined(CONFIG_HWINFO)
	(void)hwinfo_clear_reset_cause();
#endif
	sys_poweroff();
	return 0;
}

static bool off_pending; /* mg_app_power_off() until System OFF, or until it fails */

static void off_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	int err = off_now(reason);

	if (err == -EBUSY) {
		k_work_reschedule_for_queue(mg_app_wq(), &off_work, K_MSEC(100));
	} else if (err) {
		/* Staying on: the button's events (gpio-keys interrupts on both
		 * edges) and the LED as before, the reach check counts again, and
		 * a later power off can try again. */
		(void)gpio_pin_interrupt_configure_dt(&sw, GPIO_INT_EDGE_BOTH);
		mg_led_resume();
		mg_health_reach_exempt(false);
		off_pending = false;
	}
}

#if defined(CONFIG_MG_BATTERY_ADC)
#define BATTERY_MV() mg_battery_read_mv()
#else
#define BATTERY_MV() (-1)
#endif

void mg_power_boot_gate(void)
{
	bool woke = (mg_health_reset_cause() & RESET_LOW_POWER_WAKE) != 0;

	if (!MG_SW0_GPIO_KEYS) {
		(void)gpio_pin_configure_dt(&sw, GPIO_INPUT); /* gpio-keys owns its pin */
	}
	enum mg_off_kind off = off_kind_load();
	int usb = mg_usb_present();
	int mv = BATTERY_MV();
	enum mg_boot_power d =
		mg_power_boot_decision(woke, off, usb, mv, CONFIG_MG_BATTERY_RESUME_MV);

	if (d == MG_BOOT_OFF) {
		/* The unplug after a power off on USB, or a low cell off USB. */
		LOG_INF("power gate: %s; staying off", off == MG_OFF_ON_USB && usb == 0
							       ? "unplugged while off"
							       : "battery too low without USB");
		while (off_now(woke ? "unplugged" : "battery low") == -EBUSY) {
			k_sleep(K_MSEC(100));
		}
		return;
	}
	if (d == MG_BOOT_CHARGE_DARK) {
		LOG_INF("power gate: battery %d mV (< %d mV) on USB: charging dark", mv,
			CONFIG_MG_BATTERY_RESUME_MV);
		for (;;) {
			for (int i = 0; i < 100; i++) {
				k_sleep(K_MSEC(100));
				/* A key press starts it anyway. */
				if (gpio_pin_get_dt(&sw) == 1) {
					LOG_INF("power gate: key pressed: starting");
					goto run;
				}
			}
			usb = mg_usb_present();
			mv = BATTERY_MV();
			d = mg_power_boot_decision(false, MG_OFF_NONE, usb, mv,
						   CONFIG_MG_BATTERY_RESUME_MV);
			if (d == MG_BOOT_OFF) {
				LOG_INF("power gate: unplugged at %d mV: off", mv);
				while (off_now("unplugged while charging") == -EBUSY) {
					k_sleep(K_MSEC(100));
				}
			}
			if (d == MG_BOOT_RUN) {
				LOG_INF("power gate: battery %d mV: starting", mv);
				break;
			}
		}
	}
run:
	if (off != MG_OFF_NONE) {
		off_kind_store(MG_OFF_NONE);
	}
}

void mg_app_power_off(const char *why)
{
	if (off_pending) {
		return;
	}
	off_pending = true;
	reason = why;
	LOG_INF("powering off (%s)", why);
	mg_health_reach_exempt(true); /* on its way to System OFF: not a loss */
	mg_audio_stop();
	mg_led_shutdown();
	mg_transport_disconnect();
	k_work_init_delayable(&off_work, off_fn);
	/* Time for the disconnect to reach the phone. */
	k_work_reschedule_for_queue(mg_app_wq(), &off_work, K_MSEC(300));
}
