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

#include <string.h>

#include "mgcommands.h"
#include "mgls_crypto.h"

#define NONCE MG_TOKEN_PROOF_NONCE_LEN
#define MAC   MG_TOKEN_PROOF_MAC_LEN

#define TF_PENDING 0x01u /* a response went out; confirm may follow */
#define TF_MATCHED 0x02u /* the last confirm matched */
#define TF_TAKEN   0x04u /* the match's nonces went to a sealed session */

typedef char mgtp_check_lens[(NONCE == 16 && MAC == 32) ? 1 : -1];

void mgtp_init(mgtp_t *t, const mgtp_platform_t *p)
{
	memset(t, 0, sizeof(*t));
	t->p = p;
}

void mgtp_reset(mgtp_t *t)
{
	t->flags = 0;
	mglsc_wipe(t->nonces, sizeof(t->nonces));
}

bool mgtp_matched(const mgtp_t *t)
{
	return (t->flags & TF_MATCHED) != 0;
}

bool mgtp_take_nonces(mgtp_t *t, uint8_t client_nonce[NONCE], uint8_t device_nonce[NONCE])
{
	if (!(t->flags & TF_MATCHED) || (t->flags & TF_TAKEN)) {
		return false;
	}
	memcpy(client_nonce, t->nonces, NONCE);
	memcpy(device_nonce, &t->nonces[NONCE], NONCE);
	mglsc_wipe(t->nonces, sizeof(t->nonces));
	t->flags |= TF_TAKEN;
	return true;
}

int mgtp_derive_key(const char *access, size_t access_len, const char *node_id,
		    size_t node_id_len, uint8_t k[32])
{
	return mglsc_hkdf32((const uint8_t *)MGTP_SALT, sizeof(MGTP_SALT) - 1,
			    (const uint8_t *)access, access_len, (const uint8_t *)node_id,
			    node_id_len, k);
}

int mgtp_mac(const uint8_t k[32], bool device, const uint8_t client_nonce[NONCE],
	     const uint8_t device_nonce[NONCE], uint8_t mac[MAC])
{
	const char *label = device ? MGTP_DEVICE_LABEL : MGTP_CLIENT_LABEL;
	const uint8_t *parts[3];
	size_t lens[3];

	parts[0] = (const uint8_t *)label;
	lens[0] = sizeof(MGTP_DEVICE_LABEL) - 1; /* both labels are 24 bytes */
	parts[1] = client_nonce;
	lens[1] = NONCE;
	parts[2] = device_nonce;
	lens[2] = NONCE;
	return mglsc_hmac_sha256(k, 32, parts, lens, 3, mac);
}

static void send(mgtp_t *t, const uint8_t *m, size_t n)
{
	if (t->p->send) {
		(void)t->p->send(t->p->ctx, m, n);
	}
}

/* [mg_command_error, command, mg_error_data_code, code (LE16)] */
static void send_error(mgtp_t *t, uint16_t code)
{
	const uint8_t m[] = {mg_command_error, mg_command_token_proof, mg_error_data_code,
			     (uint8_t)code, (uint8_t)(code >> 8)};

	send(t, m, sizeof(m));
}

static void send_result(mgtp_t *t, uint8_t matched)
{
	const uint8_t m[] = {mg_command_token_proof, mg_token_proof_result, matched};

	send(t, m, sizeof(m));
}

static void on_challenge(mgtp_t *t, const uint8_t *p, size_t len)
{
	uint8_t k[32];
	uint8_t m[2 + NONCE + MAC];
	uint8_t *dn = &m[2];
	int r;

	if (len != NONCE) {
		send_error(t, mg_error_code_invalid_length);
		return;
	}
	/* A new challenge restarts the exchange. */
	mgtp_reset(t);
	r = t->p->get_k ? t->p->get_k(t->p->ctx, k) : MGTP_NO_TOKEN;
	if (r == MGTP_NO_TOKEN) {
		mglsc_wipe(k, sizeof(k));
		send_error(t, mg_error_code_not_found);
		return;
	}
	if (r == 0) {
		r = t->p->random ? t->p->random(t->p->ctx, dn, NONCE) : -1;
	}
	/* The response's MAC; the nonces stay for the confirm (and, once it
	 * matches, for mgtp_take_nonces()). */
	if (r == 0) {
		r = mgtp_mac(k, true, p, dn, &m[2 + NONCE]);
	}
	mglsc_wipe(k, sizeof(k));
	if (r == 0) {
		memcpy(t->nonces, p, NONCE);
		memcpy(&t->nonces[NONCE], dn, NONCE);
	}
	if (r != 0) {
		mgtp_reset(t);
		send_error(t, mg_error_code_busy);
		return;
	}
	m[0] = mg_command_token_proof;
	m[1] = mg_token_proof_response;
	t->flags = TF_PENDING;
	send(t, m, sizeof(m));
}

static void on_confirm(mgtp_t *t, const uint8_t *p, size_t len)
{
	uint8_t k[32], expect[MAC];
	bool ok;
	int r;

	if (!(t->flags & TF_PENDING)) {
		send_error(t, mg_error_code_invalid_value);
		return;
	}
	if (len != MAC) {
		send_error(t, mg_error_code_invalid_length);
		return;
	}
	/* The client's confirm, from K and the pending nonces (K read again:
	 * the 32 bytes of the expected MAC aren't kept between the two). */
	r = t->p->get_k ? t->p->get_k(t->p->ctx, k) : MGTP_NO_TOKEN;
	if (r == 0) {
		r = mgtp_mac(k, false, t->nonces, &t->nonces[NONCE], expect);
	}
	mglsc_wipe(k, sizeof(k));
	if (r != 0) {
		mglsc_wipe(expect, sizeof(expect));
		mgtp_reset(t);
		send_error(t, r == MGTP_NO_TOKEN ? mg_error_code_not_found : mg_error_code_busy);
		return;
	}
	ok = mglsc_ct_equal(expect, p, MAC);
	mglsc_wipe(expect, sizeof(expect));
	if (!ok) {
		mglsc_wipe(t->nonces, sizeof(t->nonces));
	}
	t->flags = ok ? TF_MATCHED : 0;
	send_result(t, ok ? 1 : 0);
}

/* MGTP_SEALED_CLEAR 1 (mg_token_proof.h, mgcommands.h): a device that declares mg_session_cap_sealed_clear takes
 * the clear only sealed (portable/mg_device_session's mg_session_clear): this unsealed one erases nothing and
 * answers in the hello's order: no token not_found, before a match proof_required, after a match insecure (a
 * relayed proof can't let a proxy erase the gadget). 0 (default until both apps send the sealed clear): the
 * unsealed clear is also accepted. */

static void on_clear(mgtp_t *t)
{
	if (MGTP_SEALED_CLEAR) {
		uint8_t k[32];
		int r = t->p->get_k ? t->p->get_k(t->p->ctx, k) : MGTP_NO_TOKEN;

		mglsc_wipe(k, sizeof(k));
		send_error(t, r == MGTP_NO_TOKEN		 ? mg_error_code_not_found
			      : !(t->flags & TF_MATCHED) ? mg_error_code_proof_required
							 : mg_error_code_insecure);
		return;
	}
	if (!(t->flags & TF_MATCHED)) {
		send_error(t, mg_error_code_proof_required);
		return;
	}
	/* Answer first: the client waits for it before forgetting its record. */
	send_result(t, 1);
	mgtp_reset(t);
	if (t->p->clear == NULL || t->p->clear(t->p->ctx) != 0) {
		send_error(t, mg_error_code_busy);
	}
}

void mgtp_rx(mgtp_t *t, const uint8_t *p, size_t len)
{
	if (len < 1) {
		send_error(t, mg_error_code_invalid_length);
		return;
	}
	switch (p[0]) {
	case mg_token_proof_challenge:
		on_challenge(t, &p[1], len - 1);
		break;
	case mg_token_proof_confirm:
		on_confirm(t, &p[1], len - 1);
		break;
	case mg_token_proof_clear:
		on_clear(t);
		break;
	default:
		send_error(t, mg_error_code_unsupported);
		break;
	}
}
