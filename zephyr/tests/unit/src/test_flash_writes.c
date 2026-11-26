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

/* No flash write per event a peer starts: connecting, subscribing, the
 * client's per-connection settings, request_status and a link drop, fifty
 * times over, write nothing; and a stranger's stored settings reach flash at
 * most once per 10 min. */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/ztest.h>

#include "fakes.h"
#include "mg_settings.h"
#include "mg_setup_store.h"
#include "mg_core.h"
#include "mg_session.h"
#include "mgcommands.h"

static atomic_t writes;

int __real_settings_save_one(const char *name, const void *value, size_t val_len);
int __wrap_settings_save_one(const char *name, const void *value, size_t val_len)
{
	atomic_inc(&writes);
	return __real_settings_save_one(name, value, val_len);
}

int __real_settings_delete(const char *name);
int __wrap_settings_delete(const char *name)
{
	atomic_inc(&writes);
	return __real_settings_delete(name);
}

static void rx(const uint8_t *m, size_t n)
{
	mg_session_rx(MG_ATT_CONTROL, m, n);
}

ZTEST(flash_writes, test_no_write_per_connection)
{
	static const uint8_t status[] = {mg_command_request_status};
	static const uint8_t ptt[] = {mg_command_set_settings,
				      mg_setting_parameter_push_to_talk_enabled, 1};
	/* client_app: the app's name, then its version (empty here). */
	static const uint8_t app[] = {mg_command_set_settings, mg_setting_parameter_client_app,
				      5, 'M', 'u', 's', 'e', ' ', 0};

	mg_session_disconnected();
	atomic_set(&writes, 0);
	for (int i = 0; i < 50; i++) {
		mg_session_connected();
		mg_session_subscribed(MG_ATT_CONTROL, true);
		mg_session_subscribed(MG_ATT_DATA, true);
		rx(status, sizeof(status));
		fake_reset();
		rx(ptt, sizeof(ptt));
		rx(app, sizeof(app));
		/* Both taken (a malformed app would be refused before it reached
		 * the per-connection path). */
		zassert_is_null(fake_find(MG_ATT_CONTROL, mg_command_error, 0), "a setting refused");
		zassert_equal(mg_settings_get()->push_to_talk_enabled, 1);
		mg_session_disconnected();
	}
	zassert_equal(atomic_get(&writes), 0, "%ld flash writes across 50 links",
		      (long)atomic_get(&writes));
	fake_connect();
}

static void *setup(void)
{
	fake_init_once();
	return NULL;
}

/* Not set up: a stranger's setting writes reach flash at most once per
 * 10 min; the change that waits goes at the end of the window. */
ZTEST(flash_writes, test_stranger_settings_coalesced)
{
	static const uint8_t lc3[] = {mg_command_set_settings, mg_setting_parameter_audio_codec,
				      mg_data_type_audio_lc3};
	static const uint8_t sbc[] = {mg_command_set_settings, mg_setting_parameter_audio_codec,
				      mg_data_type_audio_sbc};

	Z_TEST_SKIP_IFNDEF(CONFIG_MG_LC3);
	if (mg_setup_store_complete()) {
		zassert_equal(mg_setup_store_erase(), 0);
	}
	mg_settings_flush();
	k_sleep(K_MINUTES(11)); /* past any earlier save's window */
	atomic_set(&writes, 0);
	for (int i = 0; i < 40; i++) {
		rx(i & 1 ? sbc : lc3, sizeof(lc3));
		k_sleep(K_SECONDS(10));
	}
	zassert_equal(atomic_get(&writes), 1, "%ld writes for 40 changes in ~7 min",
		      (long)atomic_get(&writes));
	k_sleep(K_MINUTES(4));
	zassert_equal(atomic_get(&writes), 2, "the waiting change at the window's end");
	rx(lc3, sizeof(lc3));
	mg_settings_flush(); /* a planned power off */
	zassert_equal(atomic_get(&writes), 3, "the waiting change goes at the flush");
	rx(sbc, sizeof(sbc));
	mg_settings_flush();
}

ZTEST_SUITE(flash_writes, NULL, setup, NULL, NULL, NULL);
