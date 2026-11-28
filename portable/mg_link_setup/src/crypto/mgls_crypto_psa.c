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
 * The PSA Crypto backend (mbedTLS 3.x, TF-PSA-Crypto / mbedTLS 4.x), for
 * SDKs that already link it (Zephyr, ESP-IDF). Build with
 * -DMGLS_CRYPTO_PSA and compile this file instead of
 * mgls_crypto_builtin.c + mgls_uecc.c. PSA brings its own RNG for keys;
 * the platform RNG still provides the protocol nonces.
 */

#include <string.h>

#include "mgls_crypto.h"

#if !defined(MGLS_CRYPTO_PSA)
#error "build mgls_crypto_psa.c with -DMGLS_CRYPTO_PSA"
#endif

void mglsc_wipe(void *buf, size_t len)
{
	volatile uint8_t *p = buf;

	while (len--) {
		*p++ = 0;
	}
}

bool mglsc_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
	volatile uint8_t d = 0;

	for (size_t i = 0; i < len; i++) {
		d |= a[i] ^ b[i];
	}
	return d == 0;
}

int mglsc_init(int (*rng)(void *ctx, uint8_t *out, size_t len), void *ctx)
{
	(void)rng;
	(void)ctx;
	return psa_crypto_init() == PSA_SUCCESS ? 0 : -1;
}

int mglsc_sha256(const uint8_t *data, size_t len, uint8_t out[MGLSC_HASH_LEN])
{
	size_t n = 0;

	return psa_hash_compute(PSA_ALG_SHA_256, data, len, out, MGLSC_HASH_LEN, &n) ==
				       PSA_SUCCESS &&
			       n == MGLSC_HASH_LEN
		       ? 0
		       : -1;
}

int mglsc_sha256_init(mglsc_sha256_ctx_t *ctx)
{
	ctx->op = psa_hash_operation_init();
	return psa_hash_setup(&ctx->op, PSA_ALG_SHA_256) == PSA_SUCCESS ? 0 : -1;
}

int mglsc_sha256_update(mglsc_sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
	return psa_hash_update(&ctx->op, data, len) == PSA_SUCCESS ? 0 : -1;
}

int mglsc_sha256_final(mglsc_sha256_ctx_t *ctx, uint8_t out[MGLSC_HASH_LEN])
{
	size_t n = 0;

	if (psa_hash_finish(&ctx->op, out, MGLSC_HASH_LEN, &n) == PSA_SUCCESS && n == MGLSC_HASH_LEN) {
		return 0;
	}
	(void)psa_hash_abort(&ctx->op); /* a failed finish leaves the operation to release */
	mglsc_wipe(out, MGLSC_HASH_LEN);
	return -1;
}

int mglsc_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *const parts[],
		      const size_t lens[], int n, uint8_t out[MGLSC_HASH_LEN])
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
	psa_key_id_t id = PSA_KEY_ID_NULL;
	uint8_t kb[MGLSC_HASH_LEN];
	size_t olen = 0;
	psa_status_t st;

	/* PSA refuses zero-length HMAC keys; hash long ones as HMAC does. */
	if (key_len > 64) {
		if (mglsc_sha256(key, key_len, kb)) {
			return -1;
		}
		key = kb;
		key_len = sizeof(kb);
	}
	if (key_len == 0) {
		/* HMAC pads the key with zeros: an empty key is a zero byte. */
		memset(kb, 0, sizeof(kb));
		key = kb;
		key_len = 1;
	}
	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_bits(&attr, PSA_BYTES_TO_BITS(key_len));
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
	st = psa_import_key(&attr, key, key_len, &id);
	mglsc_wipe(kb, sizeof(kb));
	if (st != PSA_SUCCESS) {
		return -1;
	}
	st = psa_mac_sign_setup(&op, id, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	for (int i = 0; i < n && st == PSA_SUCCESS; i++) {
		if (lens[i]) {
			st = psa_mac_update(&op, parts[i], lens[i]);
		}
	}
	if (st == PSA_SUCCESS) {
		st = psa_mac_sign_finish(&op, out, MGLSC_HASH_LEN, &olen);
	} else {
		(void)psa_mac_abort(&op);
	}
	(void)psa_destroy_key(id);
	return st == PSA_SUCCESS && olen == MGLSC_HASH_LEN ? 0 : -1;
}

int mglsc_hkdf_expand32(const uint8_t prk[MGLSC_HASH_LEN], const uint8_t *info, size_t info_len,
			uint8_t out[MGLSC_HASH_LEN])
{
	static const uint8_t one = 1;
	const uint8_t *parts[2];
	size_t lens[2];

	parts[0] = info;
	lens[0] = info_len;
	parts[1] = &one;
	lens[1] = 1;
	return mglsc_hmac_sha256(prk, MGLSC_HASH_LEN, parts, lens, 2, out);
}

int mglsc_hkdf32(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
		 const uint8_t *info, size_t info_len, uint8_t out[MGLSC_HASH_LEN])
{
	uint8_t prk[MGLSC_HASH_LEN];
	const uint8_t *parts[1];
	size_t lens[1];
	int err;

	parts[0] = ikm;
	lens[0] = ikm_len;
	err = mglsc_hmac_sha256(salt, salt_len, parts, lens, 1, prk);
	if (!err) {
		err = mglsc_hkdf_expand32(prk, info, info_len, out);
	}
	mglsc_wipe(prk, sizeof(prk));
	return err;
}

static int aes_key(const uint8_t key[32], psa_key_id_t *id)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attr, PSA_ALG_GCM);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
	return psa_import_key(&attr, key, 32, id) == PSA_SUCCESS ? 0 : -1;
}

int mglsc_gcm_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
		   size_t aad_len, uint8_t *buf, size_t len)
{
	psa_key_id_t id;
	size_t olen = 0;
	psa_status_t st;

	if (aes_key(key, &id)) {
		return -1;
	}
	st = psa_aead_encrypt(id, PSA_ALG_GCM, nonce, 12, aad, aad_len, buf, len, buf, len + 16,
			      &olen);
	(void)psa_destroy_key(id);
	return st == PSA_SUCCESS && olen == len + 16 ? 0 : -1;
}

int mglsc_gcm_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
		   size_t aad_len, uint8_t *buf, size_t len)
{
	psa_key_id_t id;
	size_t olen = 0;
	psa_status_t st;

	if (aes_key(key, &id)) {
		return -1;
	}
	st = psa_aead_decrypt(id, PSA_ALG_GCM, nonce, 12, aad, aad_len, buf, len + 16, buf, len,
			      &olen);
	(void)psa_destroy_key(id);
	return st == PSA_SUCCESS && olen == len ? 0 : -1;
}

static void p256_attrs(psa_key_attributes_t *attr)
{
	*attr = psa_key_attributes_init();
	psa_set_key_type(attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(attr, 256);
	psa_set_key_usage_flags(attr, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(attr, PSA_ALG_ECDH);
	psa_set_key_lifetime(attr, PSA_KEY_LIFETIME_VOLATILE);
}

static int export_pub(mglsc_p256_t *key, uint8_t pub[65])
{
	size_t n = 0;

	if (psa_export_public_key(key->id, pub, 65, &n) != PSA_SUCCESS || n != 65) {
		mglsc_p256_free(key);
		return -1;
	}
	return 0;
}

int mglsc_p256_keygen(mglsc_p256_t *key, uint8_t pub[65])
{
	psa_key_attributes_t attr;

	p256_attrs(&attr);
	if (psa_generate_key(&attr, &key->id) != PSA_SUCCESS) {
		key->id = PSA_KEY_ID_NULL;
		return -1;
	}
	return export_pub(key, pub);
}

int mglsc_p256_import(mglsc_p256_t *key, const uint8_t priv[32], uint8_t pub[65])
{
	psa_key_attributes_t attr;

	p256_attrs(&attr);
	if (psa_import_key(&attr, priv, 32, &key->id) != PSA_SUCCESS) {
		key->id = PSA_KEY_ID_NULL;
		return -1;
	}
	return export_pub(key, pub);
}

int mglsc_p256_ecdh(mglsc_p256_t *key, const uint8_t peer[65], uint8_t ss[32])
{
	size_t n = 0;

	/* PSA validates the peer point (on the curve, not the identity). */
	if (peer[0] != 0x04 ||
	    psa_raw_key_agreement(PSA_ALG_ECDH, key->id, peer, 65, ss, 32, &n) != PSA_SUCCESS ||
	    n != 32) {
		mglsc_wipe(ss, 32);
		return -1;
	}
	return 0;
}

void mglsc_p256_free(mglsc_p256_t *key)
{
	if (key->id != PSA_KEY_ID_NULL) {
		(void)psa_destroy_key(key->id);
	}
	key->id = PSA_KEY_ID_NULL;
}
