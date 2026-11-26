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


#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "fakes.h"
#include "mg_session.h"
#include "mg_throughput.h"
#include "mgcommands.h"

/* Through mg_session. */

#define RX(...)                                                                                    \
	do {                                                                                       \
		const uint8_t _b[] = {__VA_ARGS__};                                                \
		mg_session_rx(MG_ATT_CONTROL, _b, sizeof(_b));                                     \
	} while (0)

static void *setup(void)
{
	fake_init_once();
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	fake_connect();
	fake_reset();
	fake_audio_state = MG_AUDIO_IDLE;
}

static void after(void *f)
{
	ARG_UNUSED(f);
	fake_notify_delay_us = 0;
	mg_session_disconnected();
}

static const struct fake_note *report_note(void)
{
	for (int i = fake_nnotes - 1; i >= 0; i--) {
		const struct fake_note *n = &fake_notes[i];

		if (n->ch == MG_ATT_CONTROL && n->len == 19 && n->data[0] == mg_command_device_action) {
			return n;
		}
	}
	return NULL;
}

ZTEST(throughput, test_packet_format_and_counting)
{
	uint8_t p[244];
	struct mg_tp_count c = {0};
	uint8_t r[19];

	mg_tp_fill(p, sizeof(p), 0x01020304);
	zassert_equal(sys_get_le32(p), 0x01020304);
	zassert_equal(p[4], (uint8_t)(0x04 + 4));
	zassert_equal(p[243], (uint8_t)(0x04 + 243));

	/* Packets 0, 1, 2, then 5 (two missing), then a late 3: two gaps. */
	for (uint32_t s = 0; s < 3; s++) {
		mg_tp_fill(p, sizeof(p), s);
		zassert_true(mg_tp_account(&c, p, sizeof(p), 1000 + s * 5));
	}
	mg_tp_fill(p, sizeof(p), 5);
	zassert_true(mg_tp_account(&c, p, sizeof(p), 1020));
	mg_tp_fill(p, sizeof(p), 3);
	zassert_true(mg_tp_account(&c, p, sizeof(p), 1025));
	zassert_false(mg_tp_account(&c, p, 3, 1030), "too short for a sequence");
	zassert_equal(c.packets, 5);
	zassert_equal(c.bytes, 5 * 244);
	zassert_equal(c.gaps, 2);
	zassert_equal(mg_tp_report(&c, r), 19);
	zassert_equal(r[0], mg_command_device_action);
	zassert_equal(r[1], mg_device_action_throughput_test);
	zassert_equal(r[2], mg_throughput_test_stop);
	zassert_equal(sys_get_le32(&r[3]), 5 * 244);
	zassert_equal(sys_get_le32(&r[7]), 5);
	zassert_equal(sys_get_le32(&r[11]), 25, "first to last packet");
	zassert_equal(sys_get_le32(&r[15]), 2);
}

ZTEST(throughput, test_refused)
{
	/* Busy while audio is live. */
	fake_audio_state = MG_AUDIO_LIVE;
	RX(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_send, 0,
	   0);
	zassert_equal(fake_notes[0].data[0], mg_command_error);
	zassert_equal(sys_get_le16(&fake_notes[0].data[3]), mg_error_code_busy);
	zassert_false(mg_tp_running());
	fake_audio_state = MG_AUDIO_IDLE;
	/* Unknown direction, unknown action, nothing at all. */
	fake_reset();
	RX(mg_command_device_action, mg_device_action_throughput_test, 7);
	zassert_equal(sys_get_le16(&fake_notes[0].data[3]), mg_error_code_invalid_value);
	fake_reset();
	RX(mg_command_device_action, 0);
	zassert_equal(sys_get_le16(&fake_notes[0].data[3]), mg_error_code_unsupported);
	fake_reset();
	RX(mg_command_device_action);
	zassert_equal(sys_get_le16(&fake_notes[0].data[3]), mg_error_code_invalid_length);
	/* A stop with nothing run reports zeros. */
	fake_reset();
	RX(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_stop);
	zassert_not_null(report_note());
	/* A disconnect ends a receive test silently. */
	RX(mg_command_device_action, mg_device_action_throughput_test, mg_throughput_test_receive,
	   0, 0);
	mg_session_disconnected();
	zassert_false(mg_tp_running());
	fake_connect();
}

ZTEST_SUITE(throughput, NULL, setup, before, after, NULL);
