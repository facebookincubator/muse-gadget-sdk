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

#include "mg_token_proof.h"

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include "mg_core.h"
#include "mg_psa.h"
#include "mg_session.h"
#include "mg_setup.h"
#include "mg_setup_store.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_VM_SESSION)
#include "mg_vm_session.h"
#endif

LOG_MODULE_REGISTER(mg_proof, CONFIG_MG_LOG_LEVEL);

#define NONCE MG_TOKEN_PROOF_NONCE_LEN
#define MAC   MG_TOKEN_PROOF_MAC_LEN

static struct {
	bool pending; /* a response went out; confirm may follow */
	bool matched; /* the last confirm matched */
	bool taken;   /* that match's nonces went to a sealed session */
	uint32_t gen; /* the connection the challenge, and so the match, was made on
		       * (mg_transport_conn_gen) */
	uint8_t client_nonce[NONCE];
	uint8_t device_nonce[NONCE];
} st;

#if defined(CONFIG_ZTEST) || defined(MG_HOST_TEST)
/* Tests only: a fixed device nonce, never in firmware. */
static bool test_nonce_set;
static uint8_t test_nonce[NONCE];

void mg_token_proof_test_set_nonce(const uint8_t nonce[NONCE])
{
	memcpy(test_nonce, nonce, NONCE);
	test_nonce_set = true;
}
#endif

/* Counts resets: output released under one match stops at the next.
 * Written on the app work queue only; other threads read the word. */
static volatile uint32_t epoch;

uint32_t mg_token_proof_epoch(void)
{
	return epoch;
}

void mg_token_proof_connection_reset(void)
{
	bool was_matched = st.matched;

	epoch++;
	mg_psa_wipe(&st, sizeof(st));
	if (was_matched) {
		/* Output started under that match stops with it. */
		mg_core_output_revoked();
	}
}

bool mg_token_proof_pending(void)
{
	return st.pending && st.gen == mg_transport_conn_gen();
}

/* A match: mg_health.c starts its peer restart bound over (weak here, so
 * builds without it link). */
__attribute__((weak)) void mg_token_proof_on_match(void)
{
}

bool mg_token_proof_matched(void)
{
	/* Only on the connection it was made on: SMP asks from its own
	 * thread, and the app work queue may not have seen a disconnect or a new
	 * connection yet (or may have dropped the event). */
	return st.matched && st.gen == mg_transport_conn_gen();
}

bool mg_token_proof_take_nonces(uint8_t client_nonce[NONCE], uint8_t device_nonce[NONCE])
{
	if (!mg_token_proof_matched() || st.taken) {
		return false;
	}
	memcpy(client_nonce, st.client_nonce, NONCE);
	memcpy(device_nonce, st.device_nonce, NONCE);
	/* One sealed session per proof : a second
	 * sealed hello would reuse the first one's keystream. */
	mg_psa_wipe(st.client_nonce, NONCE);
	mg_psa_wipe(st.device_nonce, NONCE);
	st.taken = true;
	return true;
}

int mg_token_proof_derive_key(const char *access, size_t access_len, const char *node_id,
			      uint8_t k[32])
{
	return mg_psa_hkdf32((const uint8_t *)MG_TOKEN_PROOF_SALT, sizeof(MG_TOKEN_PROOF_SALT) - 1,
			     (const uint8_t *)access, access_len, (const uint8_t *)node_id,
			     strlen(node_id), k);
}

int mg_token_proof_mac(const uint8_t k[32], bool device, const uint8_t client_nonce[NONCE],
		       const uint8_t device_nonce[NONCE], uint8_t mac[MAC])
{
	const char *label = device ? MG_TOKEN_PROOF_DEVICE_LABEL : MG_TOKEN_PROOF_CLIENT_LABEL;
	const uint8_t *parts[] = {(const uint8_t *)label, client_nonce, device_nonce};
	const size_t lens[] = {strlen(label), NONCE, NONCE};

	return mg_psa_hmac_sha256(k, 32, parts, lens, 3, mac);
}

/* Every answer goes at once, ahead of queued replies (clip data, a
 * capture), and busy rather than silence when it can't go:
 * a client that hears nothing waits out its timeout. */
static void proof_error(uint16_t code, const char *log)
{
	if (mg_core_send_error_urgent(mg_command_token_proof, code, log)) {
		LOG_WRN("token proof: error %u not sent", code);
	}
}

static void proof_send(const uint8_t *m, size_t len)
{
	int err = mg_session_send_urgent(m, len);

	if (err && err != -ENOTCONN) {
		LOG_WRN("token proof: answer not sent (%d): busy", err);
		mg_token_proof_connection_reset();
		proof_error(mg_error_code_busy, "proof not sent");
	}
}

static void send_result(uint8_t matched)
{
	const uint8_t m[] = {mg_command_token_proof, mg_token_proof_result, matched};

	proof_send(m, sizeof(m));
}

static void on_challenge(const uint8_t *p, size_t len)
{
	uint8_t k[32];
	uint8_t m[2 + NONCE + MAC];
	int err;

	if (len != NONCE) {
		proof_error(mg_error_code_invalid_length, NULL);
		return;
	}
	/* A new challenge restarts the exchange, and ends a device session
	 * the old match let open: its hello needs a match again. It also clears
	 * the client's ready state (an iOS relaunch can keep the
	 * link up): push-to-talk is off until the client enables it again. */
	mg_token_proof_connection_reset();
	mg_core_proof_challenge();
#if defined(CONFIG_MG_VM_SESSION)
	mg_vm_session_connection_reset();
#endif
	if (!mg_setup_store_complete()) {
		proof_error(mg_error_code_not_found, NULL);
		return;
	}
	memcpy(st.client_nonce, p, NONCE);
#if defined(CONFIG_ZTEST) || defined(MG_HOST_TEST)
	if (test_nonce_set) {
		memcpy(st.device_nonce, test_nonce, NONCE);
		test_nonce_set = false;
		err = 0;
	} else
#endif
	{
		err = mg_psa_random(st.device_nonce, NONCE);
	}
	err = err ?: mg_setup_store_get_k(k);
	err = err ?: mg_token_proof_mac(k, true, st.client_nonce, st.device_nonce, &m[2 + NONCE]);
	mg_psa_wipe(k, sizeof(k));
	if (err) {
		mg_token_proof_connection_reset();
		proof_error(mg_error_code_busy, "proof failed");
		return;
	}
	m[0] = mg_command_token_proof;
	m[1] = mg_token_proof_response;
	memcpy(&m[2], st.device_nonce, NONCE);
	st.pending = true;
	st.gen = mg_transport_conn_gen();
	proof_send(m, sizeof(m));
}

static void on_confirm(const uint8_t *p, size_t len)
{
	uint8_t k[32], want[MAC];
	bool ok;
	int err;

	/* A confirm counts only on the connection its challenge was made on: one
	 * queued before a reconnect never binds a match to the next link. */
	if (!mg_token_proof_pending()) {
		proof_error(mg_error_code_invalid_value, NULL);
		return;
	}
	if (len != MAC) {
		proof_error(mg_error_code_invalid_length, NULL);
		return;
	}
	st.pending = false;
	err = mg_setup_store_get_k(k);
	err = err ?: mg_token_proof_mac(k, false, st.client_nonce, st.device_nonce, want);
	ok = err == 0 && mg_psa_ct_equal(want, p, MAC);
	mg_psa_wipe(k, sizeof(k));
	mg_psa_wipe(want, sizeof(want));
	if (err) {
		/* The key or the HMAC failed: that says nothing about the client's
		 * proof, so it is never a mismatch (result 0 tells the app the
		 * gadget doesn't hold its token). No match, nothing held is
		 * released or discarded, and busy asks for a new challenge. */
		LOG_WRN("token proof: confirm not checked (%d): busy", err);
		mg_token_proof_connection_reset();
		proof_error(mg_error_code_busy, "proof failed");
		return;
	}
	st.matched = ok;
	if (ok) {
		mg_token_proof_on_match();
	}
	LOG_INF("token proof %s", ok ? "matched" : "did not match");
	send_result(ok ? 1 : 0);
	/* A held capture goes right after the result, or is discarded. */
	mg_core_proof_result(ok);
}

static void on_clear(void)
{
#if defined(CONFIG_MG_SESSION_SEALED_CLEAR)
	/* Forget's clear is taken only sealed (mg_session_clear): a
	 * relayed proof must not let a proxy erase the gadget. Refused in the
	 * hello's order: no token not_found, before a match proof_required,
	 * then insecure. */
	if (!mg_setup_store_complete()) {
		proof_error(mg_error_code_not_found, NULL);
	} else if (!mg_token_proof_matched()) {
		proof_error(mg_error_code_proof_required, NULL);
	} else {
		proof_error(mg_error_code_insecure, NULL);
	}
	return;
#endif
	if (!mg_token_proof_matched()) {
		proof_error(mg_error_code_proof_required, NULL);
		return;
	}
	/* Answer first: the client waits for it before forgetting its record. */
	send_result(1);
	mg_token_proof_connection_reset();
	if (mg_setup_factory_reset("token proof clear")) {
		proof_error(mg_error_code_busy, "erase failed");
	}
}

void mg_token_proof_rx(const uint8_t *p, size_t len)
{
	if (len < 1) {
		proof_error(mg_error_code_invalid_length, NULL);
		return;
	}
	switch (p[0]) {
	case mg_token_proof_challenge:
		on_challenge(&p[1], len - 1);
		break;
	case mg_token_proof_confirm:
		on_confirm(&p[1], len - 1);
		break;
	case mg_token_proof_clear:
		on_clear();
		break;
	default:
		proof_error(mg_error_code_unsupported, NULL);
		break;
	}
}
