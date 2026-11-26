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
 * The token proof through the protocol engine (mg_session -> mg_core) with
 * Zephyr's own PSA Crypto and settings (ZMS on the flash simulator), against
 * protocols/test-vectors/mg-token-proof-v1.json, and the commands that need
 * it. tests/host covers the proof's rules and Link setup on any host.
 */

#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "fakes.h"
#include "mg_clock.h"
#include "mg_core.h"
#include "mg_queue.h"
#include "mg_session.h"
#include "mg_settings.h"
#include "mg_setup.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "vectors.h"

static void *setup(void)
{
	fake_init_once();
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	mg_settings_reset();
	(void)mg_setup_store_erase();
	fake_reset();
	fake_connect();
}

static void after(void *f)
{
	ARG_UNUSED(f);
	mg_session_disconnected();
	(void)mg_setup_store_erase();
}

static void rx(const struct proof_frame *fr)
{
	mg_session_rx(MG_ATT_CONTROL, fr->b, fr->n);
}

static const struct fake_note *last(void)
{
	return fake_nnotes ? &fake_notes[fake_nnotes - 1] : NULL;
}

static void assert_error(uint16_t code)
{
	const struct fake_note *n = last();

	zassert_not_null(n);
	zassert_equal(n->data[0], mg_command_error);
	zassert_equal(n->data[1], mg_command_token_proof);
	zassert_equal(sys_get_le16(&n->data[3]), code, "code %u", sys_get_le16(&n->data[3]));
}

ZTEST(proof, test_vectors_frame_by_frame)
{
	for (int i = 0; i < PROOF_VEC_COUNT; i++) {
		const struct proof_vec *v = &proof_vecs[i];
		uint8_t k[32];

		zassert_ok(mg_token_proof_derive_key(v->access_token, strlen(v->access_token),
						     v->node_id, k));
		zassert_mem_equal(k, v->K, 32, "K, vector %s", v->name);
		zassert_ok(mg_setup_store_commit(v->access_token, strlen(v->access_token), "r", 1, k));
		zassert_true(mg_setup_store_complete());

		fake_reset();
		mg_token_proof_test_set_nonce(v->device_nonce);
		rx(&v->challenge);
		zassert_equal(fake_nnotes, 1);
		zassert_equal(last()->ch, MG_ATT_CONTROL);
		zassert_equal(last()->len, v->response.n);
		zassert_mem_equal(last()->data, v->response.b, v->response.n, "response, %s",
				  v->name);
		rx(&v->confirm);
		zassert_equal(last()->len, v->result.n);
		zassert_mem_equal(last()->data, v->result.b, v->result.n);
		/* clear: result 1, then setup is erased. */
		rx(&v->clear);
		zassert_equal(last()->len, 3);
		zassert_equal(last()->data[2], 1);
		zassert_false(mg_setup_store_complete());
		zassert_true(mg_setup_advertise_link());
		uint8_t got[32];

		zassert_equal(mg_setup_store_get_k(got), -ENOENT);
	}
}

/* Every token proof answer goes ahead of
 * queued replies (the urgent path), and busy rather than silence when it
 * can't go. */
ZTEST(proof, test_answers_urgent)
{
	const struct proof_vec *v = &proof_vecs[0];

	zassert_ok(mg_setup_store_commit(v->access_token, strlen(v->access_token), "r", 1, v->K));
	fake_reset();
	mg_token_proof_test_set_nonce(v->device_nonce);
	rx(&v->challenge);
	zassert_equal(fake_urgent_notes, 1, "the response, ahead of the queue");
	zassert_mem_equal(last()->data, v->response.b, v->response.n);
	rx(&v->confirm);
	zassert_equal(fake_urgent_notes, 2, "the result too");
	/* The answer can't go: busy instead, and the exchange starts over. */
	fake_reset();
	fake_urgent_fail = 1;
	mg_token_proof_test_set_nonce(v->device_nonce);
	rx(&v->challenge);
	assert_error(mg_error_code_busy);
	zassert_equal(fake_urgent_notes, 1, "busy goes the same way");
	rx(&v->confirm);
	assert_error(mg_error_code_invalid_value); /* nothing pending */
	/* Errors go ahead too. */
	(void)mg_setup_store_erase();
	fake_reset();
	rx(&v->challenge);
	assert_error(mg_error_code_not_found);
	zassert_equal(fake_urgent_notes, 1);
}

/* On a set-up gadget, clearing the clip queue, power off
 * and a stored setting need this connection's token proof (the bench switch
 * doesn't count); per-connection settings stay open; a gadget that isn't set
 * up stays open too. */
static void ctl(const uint8_t *b, size_t n)
{
	mg_session_rx(MG_ATT_CONTROL, b, n);
}

static bool refused(uint8_t cmd)
{
	const struct fake_note *n = fake_find(MG_ATT_CONTROL, mg_command_error, 0);

	return n && n->len >= 5 && n->data[1] == cmd &&
	       sys_get_le16(&n->data[3]) == mg_error_code_proof_required;
}

ZTEST(proof, test_privileged_need_proof)
{
	const struct proof_vec *v = &proof_vecs[0];
	static const uint8_t store_q[] = {mg_command_set_settings,
					  mg_setting_parameter_audio_queue_enabled, 1};
	static const uint8_t ptt[] = {mg_command_set_settings,
				      mg_setting_parameter_push_to_talk_enabled, 1};
	static const uint8_t clear[] = {mg_command_audio_queue, mg_audio_queue_command_clear};
	static const uint8_t off[] = {mg_command_device_action, mg_device_action_power_off};
	uint32_t refusals = mg_core_privileged_refused();
	uint8_t k[32];

	/* Not set up: open, as before. */
	fake_reset();
	ctl(store_q, sizeof(store_q));
	zassert_false(refused(mg_command_set_settings));
	zassert_equal(mg_settings_get()->audio_queue_enabled, 1);
	mg_settings_reset();

	/* Set up, no proof on this connection (the suite's central has the bench
	 * switch on: it doesn't count). */
	zassert_ok(mg_token_proof_derive_key(v->access_token, strlen(v->access_token), v->node_id,
					     k));
	zassert_ok(mg_setup_store_commit(v->access_token, strlen(v->access_token), "r", 1, k));
	zassert_true(mg_core_bench());
	fake_reset();
	ctl(store_q, sizeof(store_q));
	zassert_true(refused(mg_command_set_settings));
	zassert_equal(mg_settings_get()->audio_queue_enabled, 0, "nothing stored");
	fake_reset();
	ctl(ptt, sizeof(ptt)); /* per connection: open */
	zassert_false(refused(mg_command_set_settings));
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 1);
	/* One write with both (as the apps send at connect): push-to-talk
	 * applied, the stored pair skipped, proof_required once. */
	static const uint8_t mixed[] = {mg_command_set_settings,
					mg_setting_parameter_push_to_talk_enabled, 0,
					mg_setting_parameter_audio_queue_enabled, 1};
	fake_reset();
	ctl(mixed, sizeof(mixed));
	zassert_true(refused(mg_command_set_settings));
	zassert_is_null(fake_find(MG_ATT_CONTROL, mg_command_error, 1), "one notice");
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 0, "per connection applied");
	zassert_equal(mg_settings_get()->audio_queue_enabled, 0, "stored one skipped");
	fake_reset();
	ctl(ptt, sizeof(ptt));
	fake_reset();
	fake_power_offs = 0;
	ctl(off, sizeof(off));
	zassert_true(refused(mg_command_device_action));
	zassert_equal(fake_power_offs, 0);
	fake_reset();
	ctl(clear, sizeof(clear));
	const struct fake_note *qe = fake_find(MG_ATT_CONTROL, mg_command_error, 0);

	if (qe && qe->len >= 5 && qe->data[1] == mg_command_audio_queue &&
	    sys_get_le16(&qe->data[3]) == mg_error_code_busy) {
		/* This configuration's clip store isn't mounted ("no storage": busy,
		 * nothing else; the build lists it). */
	} else {
		zassert_true(refused(mg_command_audio_queue));
	}
	zassert_true(mg_core_privileged_refused() >= refusals + 2);
	/* The throughput test stays open unproven on a set-up gadget. */
	static const uint8_t tp[] = {mg_command_device_action, mg_device_action_throughput_test,
				     mg_throughput_test_stop};
	fake_reset();
	ctl(tp, sizeof(tp));
	zassert_false(refused(mg_command_device_action), "throughput test refused unproven");
	/* The proof gates on hid_key, a legacy skill invoke and haptic_play:
	 * this app implements none of them, so even unproven on a set-up gadget
	 * each gets unsupported, before any proof check. */
	static const uint8_t hid[] = {mg_command_ui_input, mg_ui_input_hid_key, 0x24, 0x02};
	static const uint8_t skill[] = {mg_command_skill, mg_skill_invoke, 1, 0, 0};
	static const uint8_t haptic[] = {mg_command_device_action, mg_device_action_haptic_play, 0};
	const struct {
		const uint8_t *m;
		size_t n;
	} gated[] = {{hid, sizeof(hid)}, {skill, sizeof(skill)}, {haptic, sizeof(haptic)}};

	for (size_t i = 0; i < ARRAY_SIZE(gated); i++) {
		fake_reset();
		ctl(gated[i].m, gated[i].n);
		const struct fake_note *e = fake_find(MG_ATT_CONTROL, mg_command_error, 0);

		zassert_not_null(e, "case %u answered", (unsigned int)i);
		zassert_equal(e->data[1], gated[i].m[0]);
		zassert_equal(sys_get_le16(&e->data[3]), mg_error_code_unsupported, "case %u",
			      (unsigned int)i);
	}

	/* Clip metadata: zero clips and bytes, clip_info refused (bench switch
	 * on). */
	static const uint8_t qstat[] = {mg_command_audio_queue, mg_audio_queue_command_status};
	static const uint8_t qinfo[] = {mg_command_audio_queue,
					mg_audio_queue_command_clip_info, 0, 0};
	bool queue = mg_core_clips_ready(); /* a clip store this configuration can use */
	const struct fake_note *n;

	if (queue) {
		zassert_ok(mg_queue_begin(mg_data_type_audio_sbc, 1));
		zassert_ok(mg_queue_append("0123456789", 10));
		zassert_ok(mg_queue_end());
		fake_reset();
		ctl(qstat, sizeof(qstat));
		n = fake_find(MG_ATT_CONTROL, mg_command_audio_queue, 0);
		zassert_true(n && n->len == 13);
		zassert_equal(sys_get_le32(&n->data[3]), 0, "no bytes used");
		zassert_equal(sys_get_le16(&n->data[11]), 0, "no clips");
		fake_reset();
		ctl(qinfo, sizeof(qinfo));
		zassert_true(refused(mg_command_audio_queue));
	}

	/* The token proof on this connection: allowed. */
	fake_reset();
	mg_token_proof_test_set_nonce(v->device_nonce);
	rx(&v->challenge);
	rx(&v->confirm);
	zassert_mem_equal(last()->data, v->result.b, v->result.n);
	zassert_true(mg_token_proof_matched());
	fake_reset();
	ctl(store_q, sizeof(store_q));
	zassert_false(refused(mg_command_set_settings));
	zassert_equal(mg_settings_get()->audio_queue_enabled, 1);
	if (queue) {
		fake_reset();
		ctl(qstat, sizeof(qstat));
		n = fake_find(MG_ATT_CONTROL, mg_command_audio_queue, 0);
		zassert_true(n && sys_get_le16(&n->data[11]) >= 1 && sys_get_le32(&n->data[3]) > 0);
		fake_reset();
		ctl(qinfo, sizeof(qinfo));
		n = fake_find(MG_ATT_CONTROL, mg_command_audio_queue, 0);
		zassert_true(n && n->data[1] == mg_audio_queue_command_clip_info);
	}
	fake_reset();
	ctl(off, sizeof(off));
	zassert_equal(fake_power_offs, 1);

	/* Output started under the match ends when the match is reset (a new
	 * challenge), with the bench switch off. */
	static const uint8_t mic[] = {mg_command_start_mic, mg_data_type_audio_sbc};

	zassert_ok(mg_core_set_bench(false, "test", true));
	if (!IS_ENABLED(CONFIG_MG_AUTOMATION)) {
		/* A release build of a set-up gadget also needs a press on the
		 * gadget within 5 s (mg_mic_policy.h); an injected press (a
		 * console word, a test) never counts, outside bench mode. */
		if (!IS_ENABLED(CONFIG_MG_BENCH_MODE)) {
			mg_core_button_from(true, k_uptime_get(), false);
			mg_core_button_from(false, k_uptime_get(), false);
		}
		fake_reset();
		ctl(mic, sizeof(mic));
		n = fake_find(MG_ATT_CONTROL, mg_command_error, 0);
		zassert_true(n && sys_get_le16(&n->data[3]) == mg_error_code_press_required);
		zassert_not_equal(fake_audio_state, MG_AUDIO_LIVE);
		mg_core_wake_word(); /* a wake word the gadget heard counts as a press */
	}
	ctl(mic, sizeof(mic));
	zassert_equal(fake_audio_state, MG_AUDIO_LIVE);
	int stops = fake_audio_stops;

	rx(&v->challenge);
	zassert_equal(fake_audio_stops, stops + 1, "the capture ends with the match");
	zassert_equal(fake_audio_state, MG_AUDIO_IDLE);
	mg_settings_reset();
}

static int clk_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		  void *param)
{
	ARG_UNUSED(key);
	return len == 3 && read_cb(cb_arg, param, 3) == 3 ? 0 : -EINVAL;
}

/* The UTC offset and flags mg/clk holds (0, 0 when nothing is stored). */
static int16_t stored_clock_off(uint8_t *flags)
{
	uint8_t b[3] = {0};

	(void)settings_load_subtree_direct("mg/clk", clk_cb, b);
	*flags = b[2];
	return (int16_t)sys_get_le16(b);
}

/* set_unix_time always applies; on a set-up gadget its offset and flags are
 * stored only on a token-proven connection, and the set is never refused. On
 * a gadget that isn't set up they wait in RAM for setup's commit. */
ZTEST(proof, test_clock_stores_only_proven)
{
	const struct proof_vec *v = &proof_vecs[0];
	/* 1793380800000 ms, UTC+60 min, 24 h; then UTC+120 min. */
	static const uint8_t set60[] = {mg_command_device_action, mg_device_action_set_unix_time,
					0x00, 0x56, 0xd3, 0x8d, 0xa1, 0x01, 0x00, 0x00, 60, 0, 1};
	static const uint8_t set120[] = {mg_command_device_action, mg_device_action_set_unix_time,
					 0x00, 0x56, 0xd3, 0x8d, 0xa1, 0x01, 0x00, 0x00, 120, 0, 1};
	int64_t ms;
	int16_t off;
	uint8_t flags, sflags;
	uint8_t k[32];

	(void)settings_delete("mg/clk");
	mg_clock_load();
	/* Not set up: kept in RAM, stored at setup's commit. */
	fake_reset();
	ctl(set60, sizeof(set60));
	zassert_is_null(fake_find(MG_ATT_CONTROL, mg_command_error, 0));
	zassert_equal(stored_clock_off(&sflags), 0);
	zassert_equal(sflags, 0);
	zassert_ok(mg_token_proof_derive_key(v->access_token, strlen(v->access_token), v->node_id,
					     k));
	zassert_ok(mg_setup_store_commit(v->access_token, strlen(v->access_token), "r", 1, k));
	zassert_equal(stored_clock_off(&sflags), 60);
	zassert_equal(sflags, 1);

	/* Set up, no proof: applied, answered as usual, not stored. */
	uint32_t refusals = mg_core_privileged_refused();

	fake_reset();
	ctl(set120, sizeof(set120));
	zassert_is_null(fake_find(MG_ATT_CONTROL, mg_command_error, 0));
	zassert_equal(mg_core_privileged_refused(), refusals);
	mg_clock_get(&ms, &off, &flags);
	zassert_equal(off, 120);
	zassert_true(ms >= 1793380800000LL);
	zassert_equal(stored_clock_off(&sflags), 60);

	/* Proven: the same set stores it. */
	mg_token_proof_test_set_nonce(v->device_nonce);
	rx(&v->challenge);
	rx(&v->confirm);
	zassert_true(mg_token_proof_matched());
	ctl(set120, sizeof(set120));
	zassert_equal(stored_clock_off(&sflags), 120);
	zassert_equal(sflags, 1);
	(void)settings_delete("mg/clk");
	mg_clock_load();
}

ZTEST_SUITE(proof, NULL, setup, before, after, NULL);

/* Against mg-session-v1.json's settings exchange "the owner's content
 * without the proof, release build", on this
 * headless app (no haptics: link_mode stands in for the status field): on a
 * set-up release build get_settings answers its status fields and skips
 * client_app, then proof_required once; client_app alone is proof_required
 * only; get_unix_time is proof_required; writes are unchanged; after the
 * proof, everything answers. */
extern bool mg_test_owner_release;

ZTEST(proof, test_owner_reads_release)
{
	const struct proof_vec *v = &proof_vecs[0];
	static const uint8_t app[] = {mg_command_set_settings, mg_setting_parameter_client_app,
				      8,   'M', 'u', 's', 'e', ' ', 'a', 'p', 'p',
				      9,   '1', '6', '.', '0', '.', '0', '.', '8', '6'};
	static const uint8_t get2[] = {mg_command_get_settings, mg_setting_parameter_link_mode,
				       mg_setting_parameter_client_app};
	static const uint8_t get1[] = {mg_command_get_settings, mg_setting_parameter_client_app};
	static const uint8_t time_get[] = {mg_command_device_action, mg_device_action_get_unix_time};
	const struct fake_note *f;
	uint8_t k[32];

	zassert_ok(mg_token_proof_derive_key(v->access_token, strlen(v->access_token), v->node_id,
					     k));
	zassert_ok(mg_setup_store_commit(v->access_token, strlen(v->access_token), "r", 1, k));
	mg_test_owner_release = true;
	fake_reset();
	ctl(app, sizeof(app)); /* a write: taken */
	zassert_false(refused(mg_command_set_settings));
	fake_reset();
	ctl(get2, sizeof(get2));
	f = fake_find(MG_ATT_CONTROL, mg_command_get_settings, 0);
	zassert_not_null(f, "the status field answered");
	zassert_equal(f->len, 3, "link_mode only: client_app skipped");
	zassert_true(refused(mg_command_get_settings));
	fake_reset();
	ctl(get1, sizeof(get1));
	zassert_is_null(fake_find(MG_ATT_CONTROL, mg_command_get_settings, 0), "no answer");
	zassert_true(refused(mg_command_get_settings));
	fake_reset();
	ctl(time_get, sizeof(time_get));
	zassert_true(refused(mg_command_device_action));
	/* The proof matches: everything answers. */
	mg_token_proof_test_set_nonce(v->device_nonce);
	rx(&v->challenge);
	rx(&v->confirm);
	zassert_true(mg_token_proof_matched());
	fake_reset();
	ctl(get2, sizeof(get2));
	f = fake_find(MG_ATT_CONTROL, mg_command_get_settings, 0);
	zassert_not_null(f);
	zassert_true(f->len > 3, "client_app answered");
	zassert_false(refused(mg_command_get_settings));
	fake_reset();
	ctl(time_get, sizeof(time_get));
	zassert_false(refused(mg_command_device_action));
	/* Back to the build's rule. */
	mg_test_owner_release = !IS_ENABLED(CONFIG_MG_AUTOMATION) && !IS_ENABLED(CONFIG_MG_BENCH_MODE);
}
