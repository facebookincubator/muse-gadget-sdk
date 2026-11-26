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

/* Link setup crypto against esp32/tests/vectors/link_pairing_v5.json: keys
 * from the private scalars, ECDH, the transcript (built by the ESP32's own
 * pairing_transcript.c), its hash, the session secret, record keys, session
 * id, AAD, and the client_finished record both ways. */

#include <stdlib.h>

#include "check.h"
#include "mg_psa.h"
#include "mg_setup_crypto.h"
#include "pairing_transcript.h"
#include "vectors.h"

static void test_b64url(void)
{
	char out[64];
	uint8_t bin[64];
	size_t n;

	CHECK(mg_b64url_encode((const uint8_t *)"", 0, out, sizeof(out)) == 0 && out[0] == 0);
	CHECK(mg_b64url_encode((const uint8_t *)"f", 1, out, sizeof(out)) == 2);
	CHECK_STR(out, "Zg");
	CHECK(mg_b64url_encode((const uint8_t *)"fo", 2, out, sizeof(out)) == 3);
	CHECK_STR(out, "Zm8");
	CHECK(mg_b64url_encode((const uint8_t *)"foo", 3, out, sizeof(out)) == 4);
	CHECK_STR(out, "Zm9v");
	CHECK(mg_b64url_encode((const uint8_t *)"\xfb\xff", 2, out, sizeof(out)) == 3);
	CHECK_STR(out, "-_8");
	CHECK(mg_b64url_encode((const uint8_t *)"foo", 3, out, 4) == -1);
	CHECK(mg_b64url_decode("-_8", 3, bin, sizeof(bin), &n) == 0 && n == 2 && bin[0] == 0xfb &&
	      bin[1] == 0xff);
	CHECK(mg_b64url_decode("Zm9v", 4, bin, sizeof(bin), &n) == 0 && n == 3);
	CHECK(mg_b64url_decode("Zm9v", 4, bin, 2, &n) != 0);  /* too small */
	CHECK(mg_b64url_decode("", 0, bin, sizeof(bin), &n) != 0);
	CHECK(mg_b64url_decode("Z", 1, bin, sizeof(bin), &n) != 0);
	CHECK(mg_b64url_decode("Zg==", 4, bin, sizeof(bin), &n) != 0); /* padding */
	CHECK(mg_b64url_decode("Zm+v", 4, bin, sizeof(bin), &n) != 0); /* standard alphabet */
	char big[4100];

	memset(big, 'A', sizeof(big));
	CHECK(mg_b64url_decode(big, 4096, (uint8_t *)big, sizeof(big), &n) == 0 && n == 3072);
	CHECK(mg_b64url_decode(big, 4100, (uint8_t *)big, sizeof(big), &n) != 0);
	/* In place. */
	strcpy(out, "aGVsbG8gd29ybGQ");
	CHECK(mg_b64url_decode(out, strlen(out), (uint8_t *)out, sizeof(out), &n) == 0 && n == 11 &&
	      memcmp(out, "hello world", 11) == 0);
}

/* provision_v2's device_key and device_key_id: canonical padded base64 of
 * exactly the length asked for, nothing else. */
static void test_b64_exact(void)
{
	uint8_t bin[32];

	CHECK(mg_b64_decode_exact("Zm9v", 4, bin, 3) == 0 && memcmp(bin, "foo", 3) == 0);
	CHECK(mg_b64_decode_exact("Zm8=", 4, bin, 2) == 0 && memcmp(bin, "fo", 2) == 0);
	CHECK(mg_b64_decode_exact("Zg==", 4, bin, 1) == 0 && bin[0] == 'f');
	CHECK(mg_b64_decode_exact("+/8=", 4, bin, 2) == 0 && bin[0] == 0xfb && bin[1] == 0xff);
	CHECK(mg_b64_decode_exact("-_8=", 4, bin, 2) != 0); /* base64url */
	CHECK(mg_b64_decode_exact("Zm8", 3, bin, 2) != 0);  /* unpadded */
	CHECK(mg_b64_decode_exact("Zm9=", 4, bin, 2) != 0); /* bits past the last byte */
	CHECK(mg_b64_decode_exact("Zh==", 4, bin, 1) != 0);
	CHECK(mg_b64_decode_exact("Zm9v", 4, bin, 2) != 0); /* another length */
	CHECK(mg_b64_decode_exact("Zm8=", 4, bin, 3) != 0);
	CHECK(mg_b64_decode_exact("Z===", 4, bin, 1) != 0);
	CHECK(mg_b64_decode_exact("Zm=v", 4, bin, 2) != 0);
	CHECK(mg_b64_decode_exact("Zm 8", 4, bin, 2) != 0);
	CHECK(mg_b64_decode_exact("", 0, bin, 0) != 0);
	CHECK(mg_b64_decode_exact("oKGio6SlpqeoqaqrrK2urw==", 24, bin, 16) == 0 && bin[0] == 0xa0 &&
	      bin[15] == 0xaf);
}

static void test_vector(const struct link_vec *v)
{
	psa_key_id_t dev = PSA_KEY_ID_NULL, mob = PSA_KEY_ID_NULL;
	uint8_t dpub[65], mpub[65], ss[32], ss2[32], th[32], secret[32];
	uint8_t mn[16], dn[16], mpub_ref[65], dpub_ref[65];
	char b64[96], th_b64[48];
	size_t n;

	printf("  vector %s\n", v->name);
	CHECK(mg_setup_p256_import(&dev, v->device_priv, dpub) == 0);
	CHECK(mg_setup_p256_import(&mob, v->mobile_priv, mpub) == 0);
	CHECK(mg_b64url_decode(v->device_pub, strlen(v->device_pub), dpub_ref, 65, &n) == 0 &&
	      n == 65);
	CHECK(mg_b64url_decode(v->mobile_pub, strlen(v->mobile_pub), mpub_ref, 65, &n) == 0 &&
	      n == 65);
	CHECK_MEM(dpub, dpub_ref, 65);
	CHECK_MEM(mpub, mpub_ref, 65);
	CHECK(mg_b64url_encode(dpub, 65, b64, sizeof(b64)) > 0);
	CHECK_STR(b64, v->device_pub);

	CHECK(mg_setup_ecdh(dev, mpub, ss) == 0);
	CHECK(mg_setup_ecdh(mob, dpub, ss2) == 0);
	CHECK_MEM(ss, v->ecdh, 32);
	CHECK_MEM(ss2, v->ecdh, 32);

	/* A point off the curve and a compressed point are refused. */
	uint8_t bad[65];

	memcpy(bad, mpub, 65);
	bad[64] ^= 1;
	CHECK(mg_setup_ecdh(dev, bad, ss2) != 0);
	bad[0] = 0x02;
	CHECK(mg_setup_ecdh(dev, bad, ss2) != 0);

	CHECK(mg_b64url_decode(v->mobile_nonce, strlen(v->mobile_nonce), mn, 16, &n) == 0 &&
	      n == 16);
	CHECK(mg_b64url_decode(v->device_nonce, strlen(v->device_nonce), dn, 16, &n) == 0 &&
	      n == 16);

	const pairing_transcript_fields_t f = {
		.device_id = v->device_id,
		.node_id = v->node_id,
		.mac = v->mac,
		.firmware_version = v->fw,
		.mobile_pub = v->mobile_pub,
		.device_pub = v->device_pub,
		.mobile_nonce = v->mobile_nonce,
		.device_nonce = v->device_nonce,
	};
	char *t = pairing_transcript_build(true, 0, v->policy, &f);

	CHECK_STR(t, v->transcript);
	CHECK(t && mg_psa_sha256((const uint8_t *)t, strlen(t), th) == 0);
	free(t);
	CHECK(mg_b64url_encode(th, 32, th_b64, sizeof(th_b64)) == 43);
	CHECK_STR(th_b64, v->transcript_hash);

	struct mg_setup_keys k;

	CHECK(mg_setup_derive(ss, mn, dn, th, &k, secret) == 0);
	CHECK_MEM(secret, v->session_secret, 32);
	CHECK_MEM(k.rx, v->mobile_tx_key, 32);
	CHECK_MEM(k.tx, v->mobile_rx_key, 32);
	CHECK_STR(k.session_id_b64, v->session_id);

	char aad[96];

	CHECK(mg_setup_aad(MG_SETUP_M2D, 0, k.session_id_b64, aad, sizeof(aad)) ==
	      (int)strlen(v->cf_aad));
	CHECK_STR(aad, v->cf_aad);

	/* client_finished: the app's record decrypts with our rx key ... */
	uint8_t rec[128], pt[128];
	size_t cl, tl;

	CHECK(mg_b64url_decode(v->cf_ct, strlen(v->cf_ct), rec, sizeof(rec), &cl) == 0);
	CHECK(mg_b64url_decode(v->cf_tag, strlen(v->cf_tag), &rec[cl], 16, &tl) == 0 && tl == 16);
	CHECK(mg_setup_open(k.rx, MG_SETUP_M2D, 0, k.session_id_b64, rec, cl + 16, pt) == 0);
	CHECK(cl == strlen(v->cf_plain) && memcmp(pt, v->cf_plain, cl) == 0);
	/* ... and sealing it again gives the same bytes. */
	uint8_t again[128];

	CHECK(mg_setup_seal(k.rx, MG_SETUP_M2D, 0, k.session_id_b64,
			    (const uint8_t *)v->cf_plain, cl, again) == 0);
	CHECK_MEM(again, rec, cl + 16);
	/* Wrong counter, direction, key, or a flipped bit: refused. */
	CHECK(mg_setup_open(k.rx, MG_SETUP_M2D, 1, k.session_id_b64, rec, cl + 16, pt) != 0);
	CHECK(mg_setup_open(k.rx, MG_SETUP_D2M, 0, k.session_id_b64, rec, cl + 16, pt) != 0);
	CHECK(mg_setup_open(k.tx, MG_SETUP_M2D, 0, k.session_id_b64, rec, cl + 16, pt) != 0);
	rec[3] ^= 0x10;
	CHECK(mg_setup_open(k.rx, MG_SETUP_M2D, 0, k.session_id_b64, rec, cl + 16, pt) != 0);

	/* Nonce layout. */
	uint8_t nonce[12];
	static const uint8_t want[12] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x02};

	mg_setup_nonce(MG_SETUP_D2M, 0x0102, nonce);
	CHECK_MEM(nonce, want, 12);
	CHECK(mg_setup_aad(MG_SETUP_D2M, 18446744073709551615ULL, "sid", aad, sizeof(aad)) > 0);
	CHECK_STR(aad, "hatch-link ble setup v1|sid|d2m|18446744073709551615");

	(void)psa_destroy_key(dev);
	(void)psa_destroy_key(mob);
}

static void test_vectors(void)
{
	CHECK(mg_psa_init() == 0);
	for (int i = 0; i < LINK_VEC_COUNT; i++) {
		test_vector(&link_vecs[i]);
	}
}

static void test_hkdf_rfc5869(void)
{
	/* RFC 5869 test case 1 (first 32 bytes of OKM). */
	uint8_t ikm[22], okm[32];
	static const uint8_t salt[13] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
	static const uint8_t info[10] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4,
					 0xf5, 0xf6, 0xf7, 0xf8, 0xf9};
	static const uint8_t want[32] = {0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a,
					 0x90, 0x43, 0x4f, 0x64, 0xd0, 0x36, 0x2f, 0x2a,
					 0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a, 0x5a, 0x4c,
					 0x5d, 0xb0, 0x2d, 0x56, 0xec, 0xc4, 0xc5, 0xbf};

	memset(ikm, 0x0b, sizeof(ikm));
	CHECK(mg_psa_hkdf32(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), okm) == 0);
	CHECK_MEM(okm, want, 32);
}

HOST_MAIN(test_b64url, test_b64_exact, test_vectors, test_hkdf_rfc5869)
