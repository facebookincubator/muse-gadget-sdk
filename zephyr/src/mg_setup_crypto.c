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

#include "mg_setup_crypto.h"

#include <errno.h>
#include <string.h>

#include "mg_psa.h"

static const char B64URL[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static const char B64STD[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const char *alphabet, bool pad, const uint8_t *in, size_t len, char *out,
		      size_t cap)
{
	size_t need = pad ? (len + 2) / 3 * 4 : len / 3 * 4 + (len % 3 ? len % 3 + 1 : 0);
	size_t o = 0;

	if (need + 1 > cap) {
		return -1;
	}
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		size_t rem = len - i;

		if (rem > 1) {
			v |= (uint32_t)in[i + 1] << 8;
		}
		if (rem > 2) {
			v |= in[i + 2];
		}
		out[o++] = alphabet[(v >> 18) & 63];
		out[o++] = alphabet[(v >> 12) & 63];
		if (rem > 1) {
			out[o++] = alphabet[(v >> 6) & 63];
		}
		if (rem > 2) {
			out[o++] = alphabet[v & 63];
		}
	}
	while (pad && o < need) {
		out[o++] = '=';
	}
	out[o] = '\0';
	return (int)o;
}

int mg_b64url_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
	return b64_encode(B64URL, false, in, len, out, cap);
}

int mg_b64_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
	return b64_encode(B64STD, true, in, len, out, cap);
}

/* A base64url digit, or with std the standard alphabet's ('+', '/'). */
static int b64val(char c, bool std)
{
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	if (c == (std ? '+' : '-')) {
		return 62;
	}
	if (c == (std ? '/' : '_')) {
		return 63;
	}
	return -1;
}

/* Safe in place (out == in): each output byte is written behind the input
 * it came from. */
static int b64_decode(bool std, const char *in, size_t n, uint8_t *out, size_t cap,
		      size_t *out_len)
{
	size_t need = n / 4 * 3 + (n % 4 ? n % 4 - 1 : 0);
	size_t o = 0;
	uint32_t acc = 0;
	int bits = 0;

	if (n == 0 || n > 4096 || n % 4 == 1 || need > cap) {
		return -EINVAL;
	}
	for (size_t i = 0; i < n; i++) {
		int v = b64val(in[i], std);

		if (v < 0) {
			return -EINVAL;
		}
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (uint8_t)(acc >> bits);
		}
	}
	/* The standard encoding is canonical: no bits left over past the last
	 * byte. */
	if (std && (acc & ((1u << bits) - 1u)) != 0) {
		return -EINVAL;
	}
	*out_len = o;
	return 0;
}

int mg_b64url_decode(const char *in, size_t n, uint8_t *out, size_t cap, size_t *out_len)
{
	return b64_decode(false, in, n, out, cap, out_len);
}

int mg_b64_decode_exact(const char *in, size_t n, uint8_t *out, size_t len)
{
	size_t pad = (3 - len % 3) % 3;
	size_t got = 0;

	if (len == 0 || n != (len + 2) / 3 * 4 || (pad >= 1 && in[n - 1] != '=') ||
	    (pad == 2 && in[n - 2] != '=')) {
		return -EINVAL;
	}
	if (b64_decode(true, in, n - pad, out, len, &got) || got != len) {
		memset(out, 0, len);
		return -EINVAL;
	}
	return 0;
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

static int export_pub(psa_key_id_t *key, uint8_t pub[MG_SETUP_P256_PUB_LEN])
{
	size_t n = 0;

	if (psa_export_public_key(*key, pub, MG_SETUP_P256_PUB_LEN, &n) != PSA_SUCCESS ||
	    n != MG_SETUP_P256_PUB_LEN) {
		(void)psa_destroy_key(*key);
		*key = PSA_KEY_ID_NULL;
		return -EIO;
	}
	return 0;
}

int mg_setup_p256_generate(psa_key_id_t *key, uint8_t pub[MG_SETUP_P256_PUB_LEN])
{
	psa_key_attributes_t attr;

	p256_attrs(&attr);
	if (psa_generate_key(&attr, key) != PSA_SUCCESS) {
		*key = PSA_KEY_ID_NULL;
		return -EIO;
	}
	return export_pub(key, pub);
}

int mg_setup_p256_import(psa_key_id_t *key, const uint8_t priv[32],
			 uint8_t pub[MG_SETUP_P256_PUB_LEN])
{
	psa_key_attributes_t attr;

	p256_attrs(&attr);
	if (psa_import_key(&attr, priv, 32, key) != PSA_SUCCESS) {
		*key = PSA_KEY_ID_NULL;
		return -EIO;
	}
	return export_pub(key, pub);
}

int mg_setup_ecdh(psa_key_id_t key, const uint8_t peer[MG_SETUP_P256_PUB_LEN], uint8_t ss[32])
{
	size_t n = 0;

	if (peer[0] != 0x04) {
		return -EINVAL;
	}
	if (psa_raw_key_agreement(PSA_ALG_ECDH, key, peer, MG_SETUP_P256_PUB_LEN, ss, 32, &n) !=
		    PSA_SUCCESS ||
	    n != 32) {
		mg_psa_wipe(ss, 32);
		return -EINVAL;
	}
	return 0;
}

int mg_setup_derive(const uint8_t ss[32], const uint8_t mobile_nonce[MG_SETUP_NONCE_LEN],
		    const uint8_t device_nonce[MG_SETUP_NONCE_LEN], const uint8_t th[32],
		    struct mg_setup_keys *k, uint8_t session_secret[32])
{
	uint8_t salt_in[MG_SETUP_NONCE_LEN * 2 + 32];
	uint8_t salt[32], secret[32], sid[32];
	uint8_t sid_in[sizeof(MG_SETUP_SID_LABEL) - 1 + 32 + 32];
	const size_t ll = sizeof(MG_SETUP_SID_LABEL) - 1;
	int err;

	memcpy(salt_in, mobile_nonce, MG_SETUP_NONCE_LEN);
	memcpy(&salt_in[MG_SETUP_NONCE_LEN], device_nonce, MG_SETUP_NONCE_LEN);
	memcpy(&salt_in[MG_SETUP_NONCE_LEN * 2], th, 32);
	memcpy(sid_in, MG_SETUP_SID_LABEL, ll);
	memcpy(&sid_in[ll], th, 32);
	memcpy(&sid_in[ll + 32], ss, 32);

	err = mg_psa_sha256(salt_in, sizeof(salt_in), salt);
	err = err ?: mg_psa_hkdf32(salt, sizeof(salt), ss, 32,
				   (const uint8_t *)MG_SETUP_RECORD_LABEL,
				   sizeof(MG_SETUP_RECORD_LABEL) - 1, secret);
	err = err ?: mg_psa_hkdf_expand32(secret, (const uint8_t *)"mobile->device", 14, k->rx);
	err = err ?: mg_psa_hkdf_expand32(secret, (const uint8_t *)"device->mobile", 14, k->tx);
	err = err ?: mg_psa_sha256(sid_in, sizeof(sid_in), sid);
	if (!err) {
		memcpy(k->session_id, sid, MG_SETUP_SID_LEN);
		if (mg_b64url_encode(k->session_id, MG_SETUP_SID_LEN, k->session_id_b64,
				     sizeof(k->session_id_b64)) < 0) {
			err = -EIO;
		}
	}
	if (!err && session_secret) {
		memcpy(session_secret, secret, 32);
	}
	mg_psa_wipe(salt_in, sizeof(salt_in));
	mg_psa_wipe(salt, sizeof(salt));
	mg_psa_wipe(secret, sizeof(secret));
	mg_psa_wipe(sid, sizeof(sid));
	mg_psa_wipe(sid_in, sizeof(sid_in));
	if (err) {
		mg_psa_wipe(k, sizeof(*k));
	}
	return err;
}

int mg_setup_aad(uint8_t dir, uint64_t counter, const char *sid_b64, char *out, size_t cap)
{
	char num[21];
	size_t i = sizeof(num);
	size_t ll = sizeof(MG_SETUP_RECORD_LABEL) - 1, sl = strlen(sid_b64);

	do {
		num[--i] = (char)('0' + counter % 10);
		counter /= 10;
	} while (counter);
	size_t nl = sizeof(num) - i;
	size_t total = ll + 1 + sl + 1 + 3 + 1 + nl;

	if (total + 1 > cap) {
		return -1;
	}
	char *p = out;

	memcpy(p, MG_SETUP_RECORD_LABEL, ll);
	p += ll;
	*p++ = '|';
	memcpy(p, sid_b64, sl);
	p += sl;
	*p++ = '|';
	memcpy(p, dir == MG_SETUP_M2D ? "m2d" : "d2m", 3);
	p += 3;
	*p++ = '|';
	memcpy(p, &num[i], nl);
	p += nl;
	*p = '\0';
	return (int)total;
}

void mg_setup_nonce(uint8_t dir, uint64_t counter, uint8_t out[12])
{
	memset(out, 0, 12);
	out[0] = dir;
	for (int i = 0; i < 8; i++) {
		out[11 - i] = (uint8_t)(counter & 0xff);
		counter >>= 8;
	}
}

static int aes_key(const uint8_t key[MG_SETUP_KEY_LEN], psa_key_id_t *id)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attr, PSA_ALG_GCM);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
	return psa_import_key(&attr, key, MG_SETUP_KEY_LEN, id) == PSA_SUCCESS ? 0 : -EIO;
}

int mg_setup_seal(const uint8_t key[MG_SETUP_KEY_LEN], uint8_t dir, uint64_t counter,
		  const char *sid_b64, const uint8_t *pt, size_t len, uint8_t *out)
{
	uint8_t nonce[12];
	char aad[96];
	int al = mg_setup_aad(dir, counter, sid_b64, aad, sizeof(aad));
	psa_key_id_t id;
	size_t olen = 0;
	psa_status_t st;

	if (al < 0 || aes_key(key, &id)) {
		return -EIO;
	}
	mg_setup_nonce(dir, counter, nonce);
	st = psa_aead_encrypt(id, PSA_ALG_GCM, nonce, sizeof(nonce), (const uint8_t *)aad,
			      (size_t)al, pt, len, out, len + MG_SETUP_TAG_LEN, &olen);
	(void)psa_destroy_key(id);
	return st == PSA_SUCCESS && olen == len + MG_SETUP_TAG_LEN ? 0 : -EIO;
}

int mg_setup_open(const uint8_t key[MG_SETUP_KEY_LEN], uint8_t dir, uint64_t counter,
		  const char *sid_b64, const uint8_t *in, size_t len, uint8_t *pt)
{
	uint8_t nonce[12];
	char aad[96];
	int al = mg_setup_aad(dir, counter, sid_b64, aad, sizeof(aad));
	psa_key_id_t id;
	size_t olen = 0;
	psa_status_t st;

	if (len < MG_SETUP_TAG_LEN || al < 0 || aes_key(key, &id)) {
		return -EIO;
	}
	mg_setup_nonce(dir, counter, nonce);
	st = psa_aead_decrypt(id, PSA_ALG_GCM, nonce, sizeof(nonce), (const uint8_t *)aad,
			      (size_t)al, in, len, pt, len - MG_SETUP_TAG_LEN, &olen);
	(void)psa_destroy_key(id);
	if (st != PSA_SUCCESS || olen != len - MG_SETUP_TAG_LEN) {
		mg_psa_wipe(pt, len - MG_SETUP_TAG_LEN);
		return -EBADMSG;
	}
	return 0;
}
