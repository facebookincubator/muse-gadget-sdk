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

#pragma once

/*
 * The token proof's crypto (protocols/mgcommands.h, Token proof): the proof
 * key K that Link setup derives from the provisioned access token, the two
 * HMACs, and a constant-time compare. PSA Crypto on the device; the host
 * tests link a reference implementation and check both against
 * protocols/test-vectors/mg-token-proof-v1.json.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mgcommands.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MG_TOKEN_PROOF_KEY_LEN 32

/* K = HKDF-SHA256(salt "mg token proof v1", IKM access_token, info node_id, 32 bytes). */
bool mg_token_proof_key(const char *access_token, const char *node_id, uint8_t k[MG_TOKEN_PROOF_KEY_LEN]);

/*
 * HMAC-SHA256(K, "mg token proof v1 device" || client_nonce || device_nonce)
 * when device, else with the "... client" label.
 */
bool mg_token_proof_mac(const uint8_t k[MG_TOKEN_PROOF_KEY_LEN], bool device,
                        const uint8_t client_nonce[MG_TOKEN_PROOF_NONCE_LEN],
                        const uint8_t device_nonce[MG_TOKEN_PROOF_NONCE_LEN], uint8_t mac[MG_TOKEN_PROOF_MAC_LEN]);

/* Constant-time comparison of n bytes. */
bool mg_token_proof_equal(const uint8_t *a, const uint8_t *b, size_t n);

/* Zeroes secrets so the compiler can't drop it. */
void mg_token_proof_wipe(void *p, size_t n);
/* HMAC-SHA256(key, a || b || c), any piece may be empty (PSA Crypto). */
bool mg_hmac_sha256_3(const uint8_t *key, size_t klen, const void *a, size_t alen, const void *b, size_t blen,
                      const void *c, size_t clen, uint8_t out[32]);
/* HMAC-SHA256(key, parts[0] || ... || parts[n - 1]), any part may be empty:
 * the sealed device session's primitive (mg_seal_crypto.c). */
bool mg_hmac_sha256_v(const uint8_t *key, size_t klen, const uint8_t *const parts[], const size_t lens[], int n,
                      uint8_t out[32]);

#ifdef __cplusplus
}
#endif
