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

#ifndef MG_TOKEN_PROOF_H_
#define MG_TOKEN_PROOF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mgcommands.h"

/*
 * mg_command_token_proof (mgcommands.h, Token proof): the client checks that
 * the gadget holds the proof key K this phone set up, without either side
 * sending the token. State is per connection; a match gates the device
 * session's hello, mic output and, on a set-up gadget, the commands that
 * destroy or store something and update writes over SMP.
 * Runs on the app thread, behind mg_session.
 */

#define MG_TOKEN_PROOF_SALT         "mg token proof v1"
#define MG_TOKEN_PROOF_DEVICE_LABEL "mg token proof v1 device"
#define MG_TOKEN_PROOF_CLIENT_LABEL "mg token proof v1 client"

/* A token_proof command (the bytes after the command byte). */
void mg_token_proof_rx(const uint8_t *p, size_t len);

/* Forgets nonces and the result: at every connection and disconnection. */
void mg_token_proof_connection_reset(void);

/* A matching proof ran on this connection. */
bool mg_token_proof_matched(void);
/* Changes at every reset of the exchange (a new challenge, a connection's
 * start or end, a setup change). */
uint32_t mg_token_proof_epoch(void);
/* A challenge was answered and its confirm may follow (a client's
 * start_mic then opens the mic, its output held). */
bool mg_token_proof_pending(void);

/* The latest matching proof's nonces on this connection, once: they seal one
 * device session (mgcommands.h, Sealed credentials) and are wiped as they
 * are taken. false before a match, after a new challenge or a reset, and
 * once taken. */
bool mg_token_proof_take_nonces(uint8_t client_nonce[MG_TOKEN_PROOF_NONCE_LEN],
				uint8_t device_nonce[MG_TOKEN_PROOF_NONCE_LEN]);

/* K = HKDF-SHA256(salt MG_TOKEN_PROOF_SALT, IKM access token, info node_id). */
int mg_token_proof_derive_key(const char *access, size_t access_len, const char *node_id,
			      uint8_t k[32]);
/* HMAC-SHA256(K, label || client_nonce || device_nonce), label the device's
 * or the client's. */
int mg_token_proof_mac(const uint8_t k[32], bool device,
		       const uint8_t client_nonce[MG_TOKEN_PROOF_NONCE_LEN],
		       const uint8_t device_nonce[MG_TOKEN_PROOF_NONCE_LEN],
		       uint8_t mac[MG_TOKEN_PROOF_MAC_LEN]);

#if defined(CONFIG_ZTEST) || defined(MG_HOST_TEST)
/* Test hook (host and unit tests only): the next challenge's device nonce. */
void mg_token_proof_test_set_nonce(const uint8_t nonce[MG_TOKEN_PROOF_NONCE_LEN]);
#endif

#endif
