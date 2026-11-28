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

#ifndef MGLS_CRYPTO_H_
#define MGLS_CRYPTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The crypto backend of the setup core and the token proof: SHA-256 (one-shot
 * and incremental), HMAC-SHA256, HKDF-SHA256 (one 32-byte block), AES-256-GCM in place, and
 * P-256 ECDH. Pick one backend at build time by compiling exactly one of:
 *
 *   src/crypto/mgls_crypto_builtin.c + src/crypto/mgls_uecc.c
 *       self-contained: micro-ecc P-256 (BSD-2-Clause), table-light
 *       SHA-256 / AES-256 / GHASH, constant-time tag comparison.
 *   src/crypto/mgls_crypto_psa.c   (define MGLS_CRYPTO_PSA)
 *       PSA Crypto (mbedTLS 3.x / TF-PSA-Crypto), for SDKs that link it.
 *
 * Every function returns 0 on success, nonzero on failure.
 */

#define MGLSC_HASH_LEN 32

#if defined(MGLS_CRYPTO_PSA)
#include <psa/crypto.h>
typedef struct {
	psa_key_id_t id;
} mglsc_p256_t;
typedef struct {
	psa_hash_operation_t op;
} mglsc_sha256_ctx_t;
#else
typedef struct {
	uint8_t d[32]; /* the private scalar, big-endian */
} mglsc_p256_t;
typedef struct {
	uint32_t h[8];
	uint32_t len_lo, len_hi; /* bytes */
	uint8_t buf[64];
	uint32_t n;
} mglsc_sha256_ctx_t;
#endif

/* Installs the platform RNG (the builtin backend's P-256 uses it; PSA uses
 * its own). Idempotent. */
int mglsc_init(int (*rng)(void *ctx, uint8_t *out, size_t len), void *ctx);

int mglsc_sha256(const uint8_t *data, size_t len, uint8_t out[MGLSC_HASH_LEN]);
/* The same incrementally (a streaming HMAC: portable/mg_device_session's
 * sealed tag with MGSEAL_EXT_SHA): init, any number of updates, then final,
 * which writes the digest and wipes (or releases) the context, on failure
 * too. After a failed update, final still releases it and fails. */
int mglsc_sha256_init(mglsc_sha256_ctx_t *ctx);
int mglsc_sha256_update(mglsc_sha256_ctx_t *ctx, const uint8_t *data, size_t len);
int mglsc_sha256_final(mglsc_sha256_ctx_t *ctx, uint8_t out[MGLSC_HASH_LEN]);
/* HMAC-SHA256 over the concatenation of n parts (empty parts allowed). */
int mglsc_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *const parts[],
		      const size_t lens[], int n, uint8_t out[MGLSC_HASH_LEN]);
/* RFC 5869 HKDF-SHA256 with L = 32: extract(salt, ikm), expand(info). */
int mglsc_hkdf32(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
		 const uint8_t *info, size_t info_len, uint8_t out[MGLSC_HASH_LEN]);
/* HKDF-Expand, one block: HMAC(prk, info || 0x01). */
int mglsc_hkdf_expand32(const uint8_t prk[MGLSC_HASH_LEN], const uint8_t *info, size_t info_len,
			uint8_t out[MGLSC_HASH_LEN]);

/* AES-256-GCM with a 12-byte nonce, in place. seal: buf[0..len) plaintext
 * -> ciphertext, and the 16-byte tag written to buf[len..len+16). open:
 * buf[0..len) ciphertext with its tag at buf[len..len+16) -> plaintext; the
 * tag is checked in constant time (the builtin backend checks it before
 * decrypting anything). On failure buf's content is unspecified: wipe it. */
int mglsc_gcm_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
		   size_t aad_len, uint8_t *buf, size_t len);
int mglsc_gcm_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
		   size_t aad_len, uint8_t *buf, size_t len);

/* A fresh ephemeral key pair; pub is the uncompressed point 04 || X || Y. */
int mglsc_p256_keygen(mglsc_p256_t *key, uint8_t pub[65]);
/* A key pair from a known private scalar (tests). */
int mglsc_p256_import(mglsc_p256_t *key, const uint8_t priv[32], uint8_t pub[65]);
/* ss = X(priv * peer). Fails on a peer point that is not uncompressed or
 * not on the curve. */
int mglsc_p256_ecdh(mglsc_p256_t *key, const uint8_t peer[65], uint8_t ss[32]);
/* Wipes / destroys the private key. */
void mglsc_p256_free(mglsc_p256_t *key);

/* Constant time in len. */
bool mglsc_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);
/* A wipe the compiler can't drop. */
void mglsc_wipe(void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
