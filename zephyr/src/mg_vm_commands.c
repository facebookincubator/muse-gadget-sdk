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
 * The gadget's device commands, registered on its VM session with
 * link.register (mgcommands.h, Device session), and the identity it
 * registers with.
 *
 *   device.health  as a Wi-Fi gadget's (esp32/main/noise_control.cpp):
 *                  overall, metrics (uptime, battery), version
 *   device.led     (CONFIG_MG_VM_COMMAND_LED) the XIAO nRF54L15's user LED
 *                  (led0), one single-colour LED on a GPIO: any colour turns
 *                  it on, all 0 turns it off, for duration_ms, then it shows
 *                  the link again
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "mg_app.h"
#include "mg_identity.h"
#include "mg_led.h"
#include "mg_vm_link.h"

LOG_MODULE_REGISTER(mg_vm_commands, CONFIG_MG_LOG_LEVEL);

#define LED_DEFAULT_MS 5000
#define LED_MAX_MS     60000

static int health_handler(const struct mg_json_obj *params, struct mg_jw *payload, char *err,
			  size_t err_len)
{
	int mv = mg_app_battery_mv();

	ARG_UNUSED(params);
	ARG_UNUSED(err);
	ARG_UNUSED(err_len);
	mg_jw_str(payload, "overall", "ok");
	mg_jw_obj(payload, "levels");
	mg_jw_close(payload);
	mg_jw_arr(payload, "critical");
	mg_jw_close(payload);
	mg_jw_arr(payload, "warnings");
	mg_jw_close(payload);
	mg_jw_obj(payload, "metrics");
	mg_jw_int(payload, "uptime_seconds", k_uptime_get() / 1000);
	if (mv >= 0) {
		mg_jw_int(payload, "battery_pct", mg_app_battery_percent());
		mg_jw_int(payload, "battery_mv", mv);
	} else {
		mg_jw_raw(payload, "battery_pct", "null", 4);
		mg_jw_raw(payload, "battery_mv", "null", 4);
	}
	/* The XIAO can't tell charging or USB power from its battery pin. */
	mg_jw_raw(payload, "charging", "null", 4);
	mg_jw_raw(payload, "usb_power", "null", 4);
	mg_jw_close(payload);
	mg_jw_str(payload, "version", mg_app_version());
	return 0;
}

static const struct mg_vm_command health_command = {
	.name = "device.health",
	.description = "Report basic gadget health, including battery level (percent) and "
		       "voltage, and whether it is charging or on USB power; each is null when "
		       "the gadget has no battery or can't measure it",
	.handler = health_handler,
};

#if defined(CONFIG_MG_VM_COMMAND_LED)
/* An optional integer parameter (types are checked already), range-checked. */
static int get_int(const struct mg_json_obj *params, const char *key, int64_t lo, int64_t hi,
		   int64_t dflt, int64_t *out, char *err, size_t err_len)
{
	const struct mg_json_field *f = mg_json_get(params, key);

	if (f == NULL) {
		*out = dflt;
		return 0;
	}
	if (f->num < lo || f->num > hi) {
		return mg_vm_fail(err, err_len, "parameter %s must be %lld-%lld", key,
				  (long long)lo, (long long)hi);
	}
	*out = f->num;
	return 0;
}

static int led_handler(const struct mg_json_obj *params, struct mg_jw *payload, char *err,
		       size_t err_len)
{
	int64_t r, g, b, ms;

	if (get_int(params, "r", 0, 255, 0, &r, err, err_len) ||
	    get_int(params, "g", 0, 255, 0, &g, err, err_len) ||
	    get_int(params, "b", 0, 255, 0, &b, err, err_len) ||
	    get_int(params, "duration_ms", 1, LED_MAX_MS, LED_DEFAULT_MS, &ms, err, err_len)) {
		return -1;
	}
	bool on = (r | g | b) != 0;

	LOG_INF("mg.link: device.led %d,%d,%d -> %s for %d ms", (int)r, (int)g, (int)b,
		on ? "on" : "off", (int)ms);
	mg_led_command(on ? MG_LED_COMMAND_ON : MG_LED_COMMAND_OFF, (uint32_t)ms);
	mg_jw_bool(payload, "on", on);
	mg_jw_int(payload, "duration_ms", ms);
	return 0;
}

static const struct mg_vm_param led_params[] = {
	{"r", MG_VM_INTEGER, "Red 0-255", true},
	{"g", MG_VM_INTEGER, "Green 0-255", true},
	{"b", MG_VM_INTEGER, "Blue 0-255", true},
	{"duration_ms", MG_VM_INTEGER,
	 "How long to show it, in ms (1-60000, default 5000); then the LED shows the "
	 "connection again",
	 false},
};

static const struct mg_vm_command led_command = {
	.name = "device.led",
	.description = "Set the gadget's LED. It has one single-colour light, so any colour "
		       "turns it on and r, g and b all 0 turn it off. Returns on (whether it "
		       "lit) and duration_ms.",
	.params = led_params,
	.n_params = ARRAY_SIZE(led_params),
	.timeout_ms = 5000,
	.handler = led_handler,
};
#endif

static struct mg_vm_identity identity;

/* model_id: "muse-gadget-<board>" in DEVICE.md's [a-z0-9-], the board its
 * Zephyr board name ("muse-gadget-xiao-nrf54l15" on the XIAO, as the ESP32
 * boards send), never the uppercase catalog code. */
#if defined(CONFIG_BOARD)
#define MODEL_BOARD CONFIG_BOARD
#else
#define MODEL_BOARD "zephyr"
#endif
static char model_id[64];

static const char *make_model_id(void)
{
	static const char prefix[] = "muse-gadget-";
	size_t n = sizeof(prefix) - 1;

	memcpy(model_id, prefix, n);
	for (const char *b = MODEL_BOARD; *b && n < sizeof(model_id) - 1; b++) {
		char ch = *b;

		if (ch >= 'A' && ch <= 'Z') {
			ch = (char)(ch - 'A' + 'a');
		} else if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) {
			ch = '-';
		}
		model_id[n++] = ch;
	}
	model_id[n] = '\0';
	return model_id;
}
/* link.register metadata.model_name, the
 * product name ("XN54"): CONFIG_MG_DEVICE_MODEL_NAME, else the catalog code;
 * none on a board with neither. */
#if defined(CONFIG_MG_DEVICE_MODEL_NAME) && defined(CONFIG_MG_DEVICE_PRODUCT_CODE)
#define MODEL_NAME (sizeof(CONFIG_MG_DEVICE_MODEL_NAME) > 1 ? CONFIG_MG_DEVICE_MODEL_NAME \
						      : CONFIG_MG_DEVICE_PRODUCT_CODE)
#elif defined(CONFIG_MG_DEVICE_PRODUCT_CODE)
#define MODEL_NAME CONFIG_MG_DEVICE_PRODUCT_CODE
#else
#define MODEL_NAME NULL
#endif

int mg_vm_commands_init(void)
{
	int err;

	/* After BLE: the node id and name come from the identity address. */
	identity = (struct mg_vm_identity){
		.node_id = mg_identity_node_id(),
		.display_name = mg_identity_name(),
		.platform = "zephyr",
		.version = mg_app_version(),
		.device_family = "gadget",
		.model_id = make_model_id(),
		.model_name = MODEL_NAME,
	};
	mg_vm_link_set_identity(&identity);
	err = mg_vm_link_add_command(&health_command);
#if defined(CONFIG_MG_VM_COMMAND_LED)
	err = err ?: mg_vm_link_add_command(&led_command);
#endif
	if (err) {
		LOG_ERR("device command registration failed (%d)", err);
	}
	return err;
}
