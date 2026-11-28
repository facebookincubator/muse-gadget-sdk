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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * mg_command_token_proof (34; protocols/mgcommands.h, Token proof), for an
 * mgcommands handler: the client checks that the gadget holds the proof key
 * K this phone set up, without either side sending the token.
 *
 *   client -> challenge [client_nonce 16]
 *   device -> response  [device_nonce 16, HMAC(K, "mg token proof v1 device"
 *                        || client_nonce || device_nonce)]
 *   client -> confirm   [HMAC(K, "mg token proof v1 client" || nonces)]
 *   device -> result    [1 match, 0 mismatch]
 *   client -> clear     (after result 1) -> result 1, then the set is erased
 *
 * No token: challenge -> error not_found. confirm with nothing pending ->
 * invalid_value. clear before a match -> proof_required; built with
 * MGTP_SEALED_CLEAR 1 (a device declaring mg_session_cap_sealed_clear),
 * nothing erased here: no token -> not_found, before a match ->
 * proof_required, after a match -> insecure (the clear comes sealed, as
 * mg_session_clear in the device session). A new challenge
 * restarts the exchange. On a gadget that holds a token the integration
 * gates the commands mgcommands.h's Token proof section lists on
 * mgtp_matched() for the connection (mic and camera output, start_mic, clip
 * reads and queue erase, power off, clear, the session hello), answering
 * [25, cmd, 1, proof_required] before a match; this core gates clear.
 * Bluetooth updates over SMP need no proof: that section admits them with a
 * confirm press on the gadget (rc 10 while it asks, rc 11 after a timeout or
 * cancel). State is per
 * connection (mgtp_t, 40 bytes on rv32); call mgtp_reset() at every connect, disconnect and
 * provisioning commit (MGLS_EV_PROVISIONED). Separate from the setup core:
 * it needs only the crypto backend and these callbacks.
 */

/* The sealed clear is one switch for both libraries. 1: the gadget declares mg_session_cap_sealed_clear (the device
 * session's MGDS_CAP_SEALED_CLEAR advertises it) and this core erases nothing on an unsealed clear (above). 0
 * (default, until both Muse apps send the sealed clear): the unsealed clear is also accepted. Set either name:
 * MGTP_SEALED_CLEAR follows MGDS_CAP_SEALED_CLEAR when only that one is defined, and a build that sets them
 * differently fails. That happens in the preprocessor: define it for every unit of both libraries (a global define),
 * since a define only one library's units see leaves the other at 0. */
#ifndef MGTP_SEALED_CLEAR
#ifdef MGDS_CAP_SEALED_CLEAR
#define MGTP_SEALED_CLEAR MGDS_CAP_SEALED_CLEAR
#else
#define MGTP_SEALED_CLEAR 0
#endif
#endif
#if defined(MGDS_CAP_SEALED_CLEAR) && (MGDS_CAP_SEALED_CLEAR) != (MGTP_SEALED_CLEAR)
#error "MGTP_SEALED_CLEAR and MGDS_CAP_SEALED_CLEAR differ: the sealed clear is one switch"
#endif

#define MGTP_SALT         "mg token proof v1"
#define MGTP_DEVICE_LABEL "mg token proof v1 device"
#define MGTP_CLIENT_LABEL "mg token proof v1 client"

/* get_k() result when no token is held. */
#define MGTP_NO_TOKEN 1

typedef struct mgtp_platform {
	void *ctx;
	/* K from storage: 0, MGTP_NO_TOKEN without a complete setup, or < 0 on
	 * a read error. */
	int (*get_k)(void *ctx, uint8_t k[32]);
	/* Cryptographically secure random bytes. 0 on success. */
	int (*random)(void *ctx, uint8_t *out, size_t len);
	/* Sends one mgcommands Control frame (command byte first). */
	int (*send)(void *ctx, const uint8_t *frame, size_t len);
	/* Erases the tokens, K and the setup markers and returns the gadget to
	 * Link setup (advertising flags 0x02). 0 on success. Called after the
	 * result 1 went out. */
	int (*clear)(void *ctx);
} mgtp_platform_t;

typedef struct mgtp {
	const mgtp_platform_t *p;
	uint8_t flags;
	/* client_nonce || device_nonce of the pending challenge, then of the
	 * matching proof (the confirm's MAC is computed from them when it
	 * arrives); wiped by a mismatch, a new challenge, mgtp_reset() and
	 * mgtp_take_nonces(). */
	uint8_t nonces[32];
} mgtp_t;

void mgtp_init(mgtp_t *t, const mgtp_platform_t *p);
/* Forgets nonces and the result. */
void mgtp_reset(mgtp_t *t);
/* One token_proof command: the bytes after the command byte (34). */
void mgtp_rx(mgtp_t *t, const uint8_t *payload, size_t len);
/* A matching proof ran on this connection. */
bool mgtp_matched(const mgtp_t *t);
/* The matching proof's nonces, ONCE: the device session's seal derives its
 * keys from them (portable/mg_device_session), and one proof may key one
 * sealed session only (a second session under the same proof would reuse
 * its keystream). The first call after a match copies them out and wipes
 * them; every later call returns false until a new challenge matches. Also
 * false before a match. mgtp_matched() is unaffected. */
bool mgtp_take_nonces(mgtp_t *t, uint8_t client_nonce[16], uint8_t device_nonce[16]);

/* K = HKDF-SHA256(salt "mg token proof v1", IKM access token, info node_id,
 * 32). The setup core derives it at provisioning; exported for tests. */
int mgtp_derive_key(const char *access, size_t access_len, const char *node_id,
		    size_t node_id_len, uint8_t k[32]);
/* HMAC-SHA256(K, device or client label || client_nonce || device_nonce). */
int mgtp_mac(const uint8_t k[32], bool device, const uint8_t client_nonce[16],
	     const uint8_t device_nonce[16], uint8_t mac[32]);

#ifdef __cplusplus
}
#endif

#endif
