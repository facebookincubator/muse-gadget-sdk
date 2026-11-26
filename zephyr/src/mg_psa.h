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

#ifndef MG_PSA_H_
#define MG_PSA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Hashing, HMAC and HKDF on the PSA Crypto API (mbedTLS / TF-PSA-Crypto), for
 * Muse Link setup and the token proof. Portable: the host tests link the
 * same file against the host's PSA library. Every function returns 0 or a
 * negative errno.
 */

#define MG_PSA_HASH_LEN 32

int mg_psa_init(void);
int mg_psa_random(uint8_t *buf, size_t len);
/* Constant time in len. */
bool mg_psa_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);
void mg_psa_wipe(void *buf, size_t len);

int mg_psa_sha256(const uint8_t *data, size_t len, uint8_t out[MG_PSA_HASH_LEN]);
/* HMAC-SHA256 over the concatenation of n parts. key_len is 1..64. */
int mg_psa_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *const parts[],
		       const size_t lens[], int n, uint8_t out[MG_PSA_HASH_LEN]);
/* RFC 5869 HKDF-SHA256, one output block (32 bytes). salt_len 1..64. */
int mg_psa_hkdf32(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
		  const uint8_t *info, size_t info_len, uint8_t out[MG_PSA_HASH_LEN]);
/* HKDF-Expand, one block: HMAC(prk, info || 0x01). */
int mg_psa_hkdf_expand32(const uint8_t prk[MG_PSA_HASH_LEN], const uint8_t *info,
			 size_t info_len, uint8_t out[MG_PSA_HASH_LEN]);

#endif
