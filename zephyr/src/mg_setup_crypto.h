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

#ifndef MG_SETUP_CRYPTO_H_
#define MG_SETUP_CRYPTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <psa/crypto.h>

/*
 * The crypto of Muse Link community pairing v5 (suite
 * p256-hkdf-sha256-aes-gcm-v1), byte for byte as esp32/main/link_pairing.c:
 * P-256 ECDH, the session secret, the two record keys and the session id,
 * and AES-256-GCM records with their nonce and AAD. Checked against
 * esp32/tests/vectors/link_pairing_v5.json by tests/host. PSA only.
 */

#define MG_SETUP_P256_PUB_LEN 65
#define MG_SETUP_NONCE_LEN    16
#define MG_SETUP_KEY_LEN      32
#define MG_SETUP_SID_LEN      16
#define MG_SETUP_TAG_LEN      16
#define MG_SETUP_RECORD_LABEL "hatch-link ble setup v1"
#define MG_SETUP_SID_LABEL    "hatch-link session id v1"

/* Record directions (the nonce's first byte, and "m2d" / "d2m" in the AAD). */
#define MG_SETUP_M2D 0
#define MG_SETUP_D2M 1

struct mg_setup_keys {
	uint8_t rx[MG_SETUP_KEY_LEN]; /* mobile -> device */
	uint8_t tx[MG_SETUP_KEY_LEN]; /* device -> mobile */
	uint8_t session_id[MG_SETUP_SID_LEN];
	char session_id_b64[24];
};

/* base64url without padding. encode: the length written (NUL added), or -1
 * if it doesn't fit. decode: refuses padding, other characters, an empty or
 * impossible length and more than 4096 characters (as link_pairing.c). */
int mg_b64url_encode(const uint8_t *in, size_t len, char *out, size_t cap);
int mg_b64url_decode(const char *in, size_t in_len, uint8_t *out, size_t cap, size_t *out_len);
/* base64 with padding (RFC 4648, 4), as the device session's credentials
 * carry device_key and key_id. The length written (NUL added), or -1. */
int mg_b64_encode(const uint8_t *in, size_t len, char *out, size_t cap);
/* provision_v2's device_key and device_key_id: 0 only if in[0..in_len) is
 * exactly the canonical base64 with padding of len bytes, decoded into out;
 * anything else (base64url, missing padding, another length) is -EINVAL. */
int mg_b64_decode_exact(const char *in, size_t in_len, uint8_t *out, size_t len);

/* A fresh P-256 key pair (or one from a private scalar, for tests); pub is
 * the uncompressed point. */
int mg_setup_p256_generate(psa_key_id_t *key, uint8_t pub[MG_SETUP_P256_PUB_LEN]);
int mg_setup_p256_import(psa_key_id_t *key, const uint8_t priv[32],
			 uint8_t pub[MG_SETUP_P256_PUB_LEN]);
/* x of priv * peer; fails on a point not on the curve. */
int mg_setup_ecdh(psa_key_id_t key, const uint8_t peer[MG_SETUP_P256_PUB_LEN], uint8_t ss[32]);

/* session_secret = HKDF(ikm ss, salt SHA256(mobile_nonce || device_nonce ||
 * th), info MG_SETUP_RECORD_LABEL); rx/tx = HKDF-Expand(session_secret,
 * "mobile->device" / "device->mobile"); session_id = SHA256(MG_SETUP_SID_LABEL
 * || th || ss)[0..16). session_secret is copied out if not NULL (tests). */
int mg_setup_derive(const uint8_t ss[32], const uint8_t mobile_nonce[MG_SETUP_NONCE_LEN],
		    const uint8_t device_nonce[MG_SETUP_NONCE_LEN], const uint8_t th[32],
		    struct mg_setup_keys *k, uint8_t session_secret[32]);

/* "label|session_id|m2d|counter". The length, or -1. */
int mg_setup_aad(uint8_t dir, uint64_t counter, const char *sid_b64, char *out, size_t cap);
/* [dir, 0, 0, 0, counter big-endian (8)]. */
void mg_setup_nonce(uint8_t dir, uint64_t counter, uint8_t out[12]);

/* out gets ciphertext || tag (len + MG_SETUP_TAG_LEN bytes). */
int mg_setup_seal(const uint8_t key[MG_SETUP_KEY_LEN], uint8_t dir, uint64_t counter,
		  const char *sid_b64, const uint8_t *pt, size_t len, uint8_t *out);
/* in is ciphertext || tag (len includes the tag); pt gets len - tag bytes. */
int mg_setup_open(const uint8_t key[MG_SETUP_KEY_LEN], uint8_t dir, uint64_t counter,
		  const char *sid_b64, const uint8_t *in, size_t len, uint8_t *pt);

#endif
