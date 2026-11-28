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

#include "mg_psa.h"

#include <errno.h>
#include <string.h>

#include <psa/crypto.h>

int mg_psa_init(void)
{
	return psa_crypto_init() == PSA_SUCCESS ? 0 : -EIO;
}

int mg_psa_random(uint8_t *buf, size_t len)
{
	return psa_generate_random(buf, len) == PSA_SUCCESS ? 0 : -EIO;
}

bool mg_psa_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
	volatile uint8_t d = 0;

	for (size_t i = 0; i < len; i++) {
		d |= a[i] ^ b[i];
	}
	return d == 0;
}

void mg_psa_wipe(void *buf, size_t len)
{
	volatile uint8_t *p = buf;

	while (len--) {
		*p++ = 0;
	}
}

int mg_psa_sha256(const uint8_t *data, size_t len, uint8_t out[MG_PSA_HASH_LEN])
{
	size_t n = 0;

	return psa_hash_compute(PSA_ALG_SHA_256, data, len, out, MG_PSA_HASH_LEN, &n) ==
			       PSA_SUCCESS &&
		       n == MG_PSA_HASH_LEN
		       ? 0
		       : -EIO;
}

int mg_psa_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *const parts[],
		       const size_t lens[], int n, uint8_t out[MG_PSA_HASH_LEN])
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
	psa_key_id_t id = PSA_KEY_ID_NULL;
	size_t olen = 0;
	psa_status_t st;

	if (key_len == 0 || key_len > 64) {
		return -EINVAL;
	}
	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_bits(&attr, PSA_BYTES_TO_BITS(key_len));
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
	st = psa_import_key(&attr, key, key_len, &id);
	if (st != PSA_SUCCESS) {
		return -EIO;
	}
	st = psa_mac_sign_setup(&op, id, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	for (int i = 0; i < n && st == PSA_SUCCESS; i++) {
		if (lens[i]) {
			st = psa_mac_update(&op, parts[i], lens[i]);
		}
	}
	if (st == PSA_SUCCESS) {
		st = psa_mac_sign_finish(&op, out, MG_PSA_HASH_LEN, &olen);
	} else {
		(void)psa_mac_abort(&op);
	}
	(void)psa_destroy_key(id);
	return st == PSA_SUCCESS && olen == MG_PSA_HASH_LEN ? 0 : -EIO;
}

int mg_psa_hkdf_expand32(const uint8_t prk[MG_PSA_HASH_LEN], const uint8_t *info,
			 size_t info_len, uint8_t out[MG_PSA_HASH_LEN])
{
	static const uint8_t one = 1;
	const uint8_t *parts[] = {info, &one};
	const size_t lens[] = {info_len, 1};

	return mg_psa_hmac_sha256(prk, MG_PSA_HASH_LEN, parts, lens, 2, out);
}

int mg_psa_hkdf32(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
		  const uint8_t *info, size_t info_len, uint8_t out[MG_PSA_HASH_LEN])
{
	uint8_t prk[MG_PSA_HASH_LEN];
	const uint8_t *parts[] = {ikm};
	const size_t lens[] = {ikm_len};
	int err = mg_psa_hmac_sha256(salt, salt_len, parts, lens, 1, prk);

	if (!err) {
		err = mg_psa_hkdf_expand32(prk, info, info_len, out);
	}
	mg_psa_wipe(prk, sizeof(prk));
	return err;
}
