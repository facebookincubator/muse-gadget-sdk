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
 * Host unit tests: crypto KATs (FIPS 180-4, RFC 4231, RFC 5869, the GCM
 * spec's AES-256 cases), P-256 against link_pairing_v5.json and invalid
 * points, the v5 key schedule and records, the token proof against
 * mg-token-proof-v1.json and its rules, the JSON reader and writer,
 * base64url, the advertising helpers and the two-slot store under torn
 * writes and power loss.
 */

/* arc4random_buf: glibc 2.36 and later declare it only with _DEFAULT_SOURCE,
 * which -std=c99 leaves off (older glibc gets the stand-in below). */
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_link_setup.h"
#include "mg_token_proof.h"
#include "mgls_crypto.h"
#include "mgls_store.h"
#include "mgls_util.h"
#include "mgcommands.h"
#include "vectors_gen.h"

/* Setup refuses a pair the device session couldn't hand back: the same limit. */
typedef char check_credentials_max[(MGLS_CREDENTIALS_MAX == MG_SESSION_MAX_CREDENTIALS) ? 1 : -1];

static int failures, checks;

#define CHECK(c)                                                                                   \
	do {                                                                                       \
		checks++;                                                                          \
		if (!(c)) {                                                                        \
			failures++;                                                                \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);       \
		}                                                                                  \
	} while (0)

static size_t unhex(const char *h, uint8_t *out, size_t cap)
{
	size_t n = strlen(h) / 2;

	if (n > cap) {
		abort();
	}
	for (size_t i = 0; i < n; i++) {
		unsigned int b;

		if (sscanf(&h[2 * i], "%2x", &b) != 1) {
			abort();
		}
		out[i] = (uint8_t)b;
	}
	return n;
}

static bool eq_hex(const uint8_t *b, size_t n, const char *h)
{
	uint8_t want[512];
	size_t wn = unhex(h, want, sizeof(want));

	return wn == n && memcmp(b, want, n) == 0;
}

static size_t unb64(const char *s, uint8_t *out, size_t cap)
{
	size_t n = 0;

	if (mgls_b64_decode(s, strlen(s), out, cap, &n)) {
		abort();
	}
	return n;
}

/* ---- crypto --------------------------------------------------------------- */

static void test_sha_hmac_hkdf(void)
{
	uint8_t out[32];
	static char big[1000000];

	mglsc_sha256((const uint8_t *)"abc", 3, out);
	CHECK(eq_hex(out, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	mglsc_sha256((const uint8_t *)"", 0, out);
	CHECK(eq_hex(out, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

	mglsc_sha256((const uint8_t *)m448, strlen(m448), out);
	CHECK(eq_hex(out, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
	memset(big, 'a', sizeof(big));
	mglsc_sha256((const uint8_t *)big, sizeof(big), out);
	CHECK(eq_hex(out, 32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

	/* The incremental API: the same digests in pieces of every size around the block edges, and against the one-shot
	 * over every length 0..300 split in two at every point. */
	{
		mglsc_sha256_ctx_t c;
		uint8_t want[32], msg[300];
		static const size_t steps[] = {1, 3, 55, 56, 63, 64, 65, 127, 1000};

		for (size_t k = 0; k < sizeof(steps) / sizeof(steps[0]); k++) {
			CHECK(mglsc_sha256_init(&c) == 0);
			for (size_t off = 0; off < sizeof(big); off += steps[k]) {
				size_t n = sizeof(big) - off < steps[k] ? sizeof(big) - off : steps[k];

				CHECK(mglsc_sha256_update(&c, (const uint8_t *)big + off, n) == 0);
			}
			CHECK(mglsc_sha256_final(&c, out) == 0);
			CHECK(eq_hex(out, 32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
		}
		CHECK(mglsc_sha256_init(&c) == 0 && mglsc_sha256_update(&c, NULL, 0) == 0 &&
		      mglsc_sha256_final(&c, out) == 0);
		CHECK(eq_hex(out, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
		for (size_t i = 0; i < sizeof(msg); i++) {
			msg[i] = (uint8_t)(i * 37u + 11u);
		}
		for (size_t len = 0; len <= sizeof(msg); len++) {
			mglsc_sha256(msg, len, want);
			for (size_t cut = 0; cut <= len; cut += (len > 140 ? 7 : 1)) {
				CHECK(mglsc_sha256_init(&c) == 0);
				CHECK(mglsc_sha256_update(&c, msg, cut) == 0);
				CHECK(mglsc_sha256_update(&c, msg + cut, len - cut) == 0);
				CHECK(mglsc_sha256_final(&c, out) == 0);
				CHECK(memcmp(out, want, 32) == 0);
			}
		}
	}

	/* RFC 4231 cases 1 and 6 (a 131-byte key is hashed first). */
	uint8_t key[131];
	const uint8_t *parts[2];
	size_t lens[2];

	memset(key, 0x0b, 20);
	parts[0] = (const uint8_t *)"Hi ";
	lens[0] = 3;
	parts[1] = (const uint8_t *)"There";
	lens[1] = 5;
	mglsc_hmac_sha256(key, 20, parts, lens, 2, out);
	CHECK(eq_hex(out, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
	memset(key, 0xaa, sizeof(key));
	parts[0] = (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First";
	lens[0] = strlen((const char *)parts[0]);
	mglsc_hmac_sha256(key, sizeof(key), parts, lens, 1, out);
	CHECK(eq_hex(out, 32, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));

	/* RFC 5869 cases 1 and 3: T(1), the first 32 bytes of OKM. */
	uint8_t salt[13], ikm[22], info[10];

	unhex("000102030405060708090a0b0c", salt, sizeof(salt));
	memset(ikm, 0x0b, sizeof(ikm));
	unhex("f0f1f2f3f4f5f6f7f8f9", info, sizeof(info));
	mglsc_hkdf32(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), out);
	CHECK(eq_hex(out, 32, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"));
	mglsc_hkdf32(NULL, 0, ikm, sizeof(ikm), NULL, 0, out);
	CHECK(eq_hex(out, 32, "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"));
}

static void test_gcm(void)
{
	uint8_t k[32], iv[12], p[64 + 16], a[20], z[32 + 16];

	/* GCM spec test cases 13 and 14 (AES-256). */
	memset(k, 0, sizeof(k));
	memset(iv, 0, sizeof(iv));
	CHECK(mglsc_gcm_seal(k, iv, NULL, 0, z, 0) == 0);
	CHECK(eq_hex(z, 16, "530f8afbc74536b9a963b4f1c4cb738b"));
	memset(z, 0, 16);
	CHECK(mglsc_gcm_seal(k, iv, NULL, 0, z, 16) == 0);
	CHECK(eq_hex(z, 32, "cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919"));
	CHECK(mglsc_gcm_open(k, iv, NULL, 0, z, 16) == 0);
	CHECK(eq_hex(z, 16, "00000000000000000000000000000000"));

	/* Cases 15 and 16. */
	const char *P = "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
			"1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255";

	unhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", k, 32);
	unhex("cafebabefacedbaddecaf888", iv, 12);
	unhex(P, p, 64);
	CHECK(mglsc_gcm_seal(k, iv, NULL, 0, p, 64) == 0);
	CHECK(eq_hex(p, 80, "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
			    "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad"
			    "b094dac5d93471bdec1a502270e3cc6c"));
	unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", a, 20);
	unhex(P, p, 64);
	CHECK(mglsc_gcm_seal(k, iv, a, 20, p, 60) == 0);
	CHECK(eq_hex(p, 76, "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
			    "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662"
			    "76fc6ece0f4e1768cddf8853bb2d551b"));
	CHECK(mglsc_gcm_open(k, iv, a, 20, p, 60) == 0);
	uint8_t want[64];

	unhex(P, want, 64);
	CHECK(memcmp(p, want, 60) == 0);

	/* Tampering: tag, ciphertext, AAD, nonce. */
	CHECK(mglsc_gcm_seal(k, iv, a, 20, p, 60) == 0);
	uint8_t saved[76];

	memcpy(saved, p, 76);
	p[60] ^= 1;
	CHECK(mglsc_gcm_open(k, iv, a, 20, p, 60) != 0);
#if !defined(MGLS_CRYPTO_PSA)
	CHECK(memcmp(p, saved, 60) == 0); /* the builtin checks before decrypting */
#endif
	memcpy(p, saved, 76);
	p[3] ^= 0x80;
	CHECK(mglsc_gcm_open(k, iv, a, 20, p, 60) != 0);
	memcpy(p, saved, 76);
	a[0] ^= 1;
	CHECK(mglsc_gcm_open(k, iv, a, 20, p, 60) != 0);
	a[0] ^= 1;
	memcpy(p, saved, 76);
	iv[11] ^= 1;
	CHECK(mglsc_gcm_open(k, iv, a, 20, p, 60) != 0);
	iv[11] ^= 1;
	memcpy(p, saved, 76);
	CHECK(mglsc_gcm_open(k, iv, a, 20, p, 60) == 0);
}

static void test_p256(void)
{
	uint8_t d[32], pub[65], want[65], ss[32], peer[65];
	mglsc_p256_t key;

#if defined(MGLS_CRYPTO_PSA)
	/* micro-ecc's co-Z ladder refuses the scalar 1 (the vector's mobile
	 * key); PSA takes it. The device's keys are random 256-bit scalars. */
	unhex(V5_MOBILE_PRIVATE_SCALAR_HEX, d, 32);
	CHECK(mglsc_p256_import(&key, d, pub) == 0);
	CHECK(unb64(V5_MOBILE_PUB, want, sizeof(want)) == 65 && memcmp(pub, want, 65) == 0);
	mglsc_p256_free(&key);
#endif
	unhex(V5_DEVICE_PRIVATE_SCALAR_HEX, d, 32);
	CHECK(mglsc_p256_import(&key, d, pub) == 0);
	CHECK(unb64(V5_DEVICE_PUB, want, sizeof(want)) == 65 && memcmp(pub, want, 65) == 0);
	unb64(V5_MOBILE_PUB, peer, sizeof(peer));
	CHECK(mglsc_p256_ecdh(&key, peer, ss) == 0);
	CHECK(eq_hex(ss, 32, V5_ECDH_SECRET_HEX));

	/* Points the agreement must refuse. */
	uint8_t bad[65];

	memcpy(bad, peer, 65);
	bad[0] = 0x02;
	CHECK(mglsc_p256_ecdh(&key, bad, ss) != 0);
	memcpy(bad, peer, 65);
	bad[64] ^= 1; /* off the curve */
	CHECK(mglsc_p256_ecdh(&key, bad, ss) != 0);
	memset(bad, 0, 65);
	bad[0] = 0x04; /* (0, 0) */
	CHECK(mglsc_p256_ecdh(&key, bad, ss) != 0);
	memset(bad, 0xff, 65);
	bad[0] = 0x04; /* coordinates >= p */
	CHECK(mglsc_p256_ecdh(&key, bad, ss) != 0);
	mglsc_p256_free(&key);

	/* Fresh keys agree. */
	mglsc_p256_t a, b;
	uint8_t pa[65], pb[65], s1[32], s2[32];

	CHECK(mglsc_p256_keygen(&a, pa) == 0 && mglsc_p256_keygen(&b, pb) == 0);
	CHECK(mglsc_p256_ecdh(&a, pb, s1) == 0 && mglsc_p256_ecdh(&b, pa, s2) == 0);
	CHECK(memcmp(s1, s2, 32) == 0 && memcmp(pa, pb, 65) != 0);
	mglsc_p256_free(&a);
	mglsc_p256_free(&b);
}

/* ---- token proof ------------------------------------------------------------ */

static struct {
	uint8_t k[32];
	int k_result;
	uint8_t nonce[16];
	uint8_t frames[8][64];
	size_t lens[8];
	int nframes;
	int clears;
	int clear_result;
} tp;

static int tp_get_k(void *ctx, uint8_t k[32])
{
	(void)ctx;
	memcpy(k, tp.k, 32);
	return tp.k_result;
}

static int tp_random(void *ctx, uint8_t *out, size_t len)
{
	(void)ctx;
	if (len != 16) {
		abort();
	}
	memcpy(out, tp.nonce, 16);
	return 0;
}

static int tp_send(void *ctx, const uint8_t *frame, size_t len)
{
	(void)ctx;
	if (tp.nframes < 8 && len <= 64) {
		memcpy(tp.frames[tp.nframes], frame, len);
		tp.lens[tp.nframes++] = len;
	}
	return 0;
}

static int tp_clear(void *ctx)
{
	(void)ctx;
	tp.clears++;
	return tp.clear_result;
}

static const mgtp_platform_t TP_PLAT = {NULL, tp_get_k, tp_random, tp_send, tp_clear};

static void tp_cmd(mgtp_t *t, const char *hex)
{
	uint8_t f[64];
	size_t n = unhex(hex, f, sizeof(f));

	/* The handler takes the bytes after the command byte (34 = 0x22). */
	CHECK(n >= 1 && f[0] == 0x22);
	tp.nframes = 0;
	mgtp_rx(t, &f[1], n - 1);
}

static bool last_frame_is(const char *hex)
{
	return tp.nframes == 1 && eq_hex(tp.frames[0], tp.lens[0], hex);
}

static void test_token_proof(void)
{
	mgtp_t t;
	uint8_t k[32], cn[16], dn[16], mac[32];

	for (size_t i = 0; i < sizeof(TP_VECS) / sizeof(TP_VECS[0]); i++) {
		const struct tp_vec *v = &TP_VECS[i];

		CHECK(mgtp_derive_key(v->access, strlen(v->access), v->node, strlen(v->node), k) == 0);
		CHECK(eq_hex(k, 32, v->k));
		unhex(v->cn, cn, 16);
		unhex(v->dn, dn, 16);
		mgtp_mac(k, true, cn, dn, mac);
		CHECK(eq_hex(mac, 32, v->dmac));
		mgtp_mac(k, false, cn, dn, mac);
		CHECK(eq_hex(mac, 32, v->cmac));

		/* Frame by frame. */
		memset(&tp, 0, sizeof(tp));
		memcpy(tp.k, k, 32);
		memcpy(tp.nonce, dn, 16);
		mgtp_init(&t, &TP_PLAT);
		tp_cmd(&t, v->challenge);
		CHECK(last_frame_is(v->response));
		CHECK(!mgtp_matched(&t));
		uint8_t gcn[16], gdn[16], z[32] = {0};
		CHECK(!mgtp_take_nonces(&t, gcn, gdn)); /* pending: not a match yet */
		tp_cmd(&t, v->confirm);
		CHECK(last_frame_is(v->result));
		CHECK(mgtp_matched(&t));
		/* The matching proof's nonces, once (one sealed session per proof):
		 * the second take fails and nothing is left to read. */
		CHECK(mgtp_take_nonces(&t, gcn, gdn) && !memcmp(gcn, cn, 16) && !memcmp(gdn, dn, 16));
		CHECK(!memcmp(t.nonces, z, 32) && mgtp_matched(&t));
		memset(gcn, 0xAA, 16);
		CHECK(!mgtp_take_nonces(&t, gcn, gdn) && gcn[0] == 0xAA);
		/* A new matching challenge gives them again (the same vector's
		 * nonces here: the device nonce comes from the test RNG). */
		tp_cmd(&t, v->challenge);
		CHECK(!mgtp_take_nonces(&t, gcn, gdn));
		tp_cmd(&t, v->confirm);
		CHECK(mgtp_take_nonces(&t, gcn, gdn) && !memcmp(gcn, cn, 16));
		CHECK(!mgtp_take_nonces(&t, gcn, gdn));
		tp_cmd(&t, v->clear);
		CHECK(!mgtp_take_nonces(&t, gcn, gdn));
#if MGTP_SEALED_CLEAR
		/* The clear only sealed (mg_session_clear): this one gets insecure and erases nothing. */
		CHECK(last_frame_is("1922010700") && tp.clears == 0);
		CHECK(mgtp_matched(&t));
#else
		CHECK(last_frame_is("220301") && tp.clears == 1);
		CHECK(!mgtp_matched(&t));
#endif
	}

	/* The rules (mgcommands.h, Token proof). Error frames are
	 * [25, 34, 1, code LE16]. */
	const struct tp_vec *v = &TP_VECS[0];

	memset(&tp, 0, sizeof(tp));
	unhex(v->k, tp.k, 32);
	unhex(v->dn, tp.nonce, 16);
	mgtp_init(&t, &TP_PLAT);
	tp.k_result = MGTP_NO_TOKEN;
	tp_cmd(&t, v->challenge);
	CHECK(last_frame_is("1922010500")); /* not_found */
	tp.k_result = 0;
	tp_cmd(&t, v->confirm);
	CHECK(last_frame_is("1922010300")); /* invalid_value: nothing pending */
	tp_cmd(&t, "2204");
	CHECK(last_frame_is("1922010600") && tp.clears == 0); /* proof_required (both builds) */
	tp_cmd(&t, "2200");
	CHECK(last_frame_is("1922010200")); /* invalid_length */
	tp_cmd(&t, "22");
	CHECK(last_frame_is("1922010200"));
	tp_cmd(&t, "2209");
	CHECK(last_frame_is("1922010100")); /* unsupported */
	tp_cmd(&t, v->challenge);
	CHECK(last_frame_is(v->response));
	tp_cmd(&t, "2202ff");
	CHECK(last_frame_is("1922010200")); /* confirm of the wrong length */
	/* A wrong MAC: result 0 (the nonces wiped, none to read); and one
	 * confirm per challenge. */
	tp_cmd(&t, "22020000000000000000000000000000000000000000000000000000000000000000");
	CHECK(last_frame_is("220300") && !mgtp_matched(&t));
	{
		uint8_t z[32] = {0}, a[16], b[16];
		CHECK(!memcmp(t.nonces, z, 32) && !mgtp_take_nonces(&t, a, b));
	}
	tp_cmd(&t, v->confirm);
	CHECK(last_frame_is("1922010300"));
	/* A new challenge restarts and drops an earlier match. */
	tp_cmd(&t, v->challenge);
	tp_cmd(&t, v->confirm);
	CHECK(mgtp_matched(&t));
	tp_cmd(&t, v->challenge);
	CHECK(!mgtp_matched(&t));
	tp_cmd(&t, "2204");
	CHECK(last_frame_is("1922010600"));
	/* reset (connect, disconnect, provisioning) forgets the match. */
	tp_cmd(&t, v->confirm);
	CHECK(mgtp_matched(&t));
	mgtp_reset(&t);
	CHECK(!mgtp_matched(&t));
	{
		uint8_t z[32] = {0}, a[16], b[16];
		CHECK(!memcmp(t.nonces, z, 32) && !mgtp_take_nonces(&t, a, b));
	}
	/* K gone or unreadable between challenge and confirm: not_found / busy,
	 * no result, nothing matched. */
	tp_cmd(&t, v->challenge);
	tp.k_result = MGTP_NO_TOKEN;
	tp_cmd(&t, v->confirm);
	CHECK(last_frame_is("1922010500") && !mgtp_matched(&t));
	tp.k_result = 0;
	tp_cmd(&t, v->challenge);
	tp.k_result = -5;
	tp_cmd(&t, v->confirm);
	CHECK(last_frame_is("1922010400") && !mgtp_matched(&t));
	tp.k_result = 0;
	tp_cmd(&t, v->confirm);
	CHECK(last_frame_is("1922010300")); /* nothing pending after either */
	tp_cmd(&t, v->confirm);
	CHECK(last_frame_is("1922010300"));
	/* A key read error: busy. */
	tp.k_result = -5;
	tp_cmd(&t, v->challenge);
	CHECK(last_frame_is("1922010400"));
#if MGTP_SEALED_CLEAR
	/* No token: not_found, before the match check (the hello's order). */
	tp.k_result = MGTP_NO_TOKEN;
	tp_cmd(&t, "2204");
	CHECK(last_frame_is("1922010500") && tp.clears == 0);
	tp.k_result = 0;
#else
	/* A failed erase after clear: result 1, then busy. */
	tp.k_result = 0;
	tp.clear_result = -1;
	tp_cmd(&t, v->challenge);
	tp_cmd(&t, v->confirm);
	tp_cmd(&t, "2204");
	CHECK(tp.nframes == 2 && eq_hex(tp.frames[0], tp.lens[0], "220301") &&
	      eq_hex(tp.frames[1], tp.lens[1], "1922010400"));
#endif
}

/* ---- JSON, base64url -------------------------------------------------------- */

static int parse(const char *s, mgls_jfield_t *f, uint8_t *n, char *buf)
{
	size_t len = strlen(s);

	memcpy(buf, s, len + 1);
	return mgj_parse(buf, len, f, MGLS_JSON_MAX_FIELDS, n);
}

static void test_json(void)
{
	mgls_jfield_t f[MGLS_JSON_MAX_FIELDS];
	char buf[2048];
	uint8_t n;
	int32_t v;

	CHECK(parse(" {\"a\" : \"x\\\"y\\u00e9\\ud83d\\ude00\\/\", \"n\":5, \"m\":-12, \"t\":true,"
		    "\"z\":null,\"o\":{\"k\":[1,2,{\"q\":\"r\"}]},\"e\":[]} ",
		    f, &n, buf) == 0);
	CHECK(n == 7);
	CHECK(strcmp(mgj_str(buf, f, n, "a"), "x\"y\xc3\xa9\xf0\x9f\x98\x80/") == 0);
	CHECK(mgj_int(buf, mgj_get(buf, f, n, "n"), &v) && v == 5);
	CHECK(mgj_int(buf, mgj_get(buf, f, n, "m"), &v) && v == -12);
	CHECK(mgj_get(buf, f, n, "t")->type == MGJ_TRUE);
	CHECK(mgj_get(buf, f, n, "z")->type == MGJ_NULL);
	CHECK(mgj_get(buf, f, n, "o")->type == MGJ_NESTED);
	CHECK(mgj_get(buf, f, n, "e")->type == MGJ_NESTED);
	CHECK(mgj_str(buf, f, n, "n") == NULL && mgj_get(buf, f, n, "nope") == NULL);
	CHECK(parse("{}", f, &n, buf) == 0 && n == 0);
	CHECK(parse("{\"v\":5.0}", f, &n, buf) == 0 && !mgj_int(buf, &f[0], &v));
	CHECK(parse("{\"v\":5e0}", f, &n, buf) == 0 && !mgj_int(buf, &f[0], &v));
	CHECK(parse("{\"v\":2147483648}", f, &n, buf) == 0 && !mgj_int(buf, &f[0], &v));
	CHECK(parse("{\"v\":2147483647}", f, &n, buf) == 0 && mgj_int(buf, &f[0], &v) &&
	      v == 2147483647);

	static const char *const bad[] = {
		"",
		"[]",
		"{",
		"{\"a\":1,}",
		"{\"a\":1}x",
		"{\"a\":1}{}",
		"{\"a\" 1}",
		"{a:1}",
		"{\"a\":01}",
		"{\"a\":-}",
		"{\"a\":1.}",
		"{\"a\":tru}",
		"{\"a\":\"\\u0000\"}",
		"{\"a\":\"\\ud800\"}",
		"{\"a\":\"\\udc00\"}",
		"{\"a\":\"\\x\"}",
		"{\"a\":\"\x01\"}",
		"{\"a\":\"unterminated}",
		"{\"a\":1,\"a\":2}",
		"{\"a\":[1,]}",
		"{\"a\":{\"b\"}}",
		"{\"a\":[}",
		"{\"a\":{]}",
	};

	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		int r = parse(bad[i], f, &n, buf);

		if (r == 0) {
			fprintf(stderr, "accepted: %s\n", bad[i]);
		}
		CHECK(r != 0);
	}

	/* Depth: 31 nested levels inside the top object pass, 32 don't. */
	for (int depth = 31; depth <= 32; depth++) {
		size_t p = 0;

		p += (size_t)snprintf(&buf[p], sizeof(buf) - p, "{\"a\":");
		for (int i = 0; i < depth; i++) {
			buf[p++] = '[';
		}
		for (int i = 0; i < depth; i++) {
			buf[p++] = ']';
		}
		buf[p++] = '}';
		CHECK((mgj_parse(buf, p, f, MGLS_JSON_MAX_FIELDS, &n) == 0) == (depth == 31));
	}
	/* Too many members. */
	{
		size_t p = 0;

		buf[p++] = '{';
		for (int i = 0; i <= MGLS_JSON_MAX_FIELDS; i++) {
			p += (size_t)snprintf(&buf[p], sizeof(buf) - p, "%s\"k%d\":%d", i ? "," : "", i, i);
		}
		buf[p++] = '}';
		CHECK(mgj_parse(buf, p, f, MGLS_JSON_MAX_FIELDS, &n) != 0);
	}

	/* Writer. */
	mgj_w w;
	char out[64];

	mgj_begin(&w, out, sizeof(out));
	mgj_kstr(&w, "s", "a\"b\\c\n");
	mgj_kint(&w, "n", 42);
	CHECK(mgj_end(&w) > 0 && strcmp(out, "{\"s\":\"a\\\"b\\\\c\\u000a\",\"n\":42}") == 0);
	mgj_begin(&w, out, 8);
	mgj_kstr(&w, "long", "value");
	CHECK(mgj_end(&w) < 0 && out[0] == '\0');
	mgj_begin(&w, out, sizeof(out));
	CHECK(mgj_end(&w) == 2 && strcmp(out, "{}") == 0);
}

static void test_b64(void)
{
	uint8_t raw[8] = {0xfb, 0xff, 0xbf, 0x00, 0x01, 0x02, 0x03, 0x04}, back[8];
	char s[16];
	size_t n;

	for (size_t len = 0; len <= 8; len++) {
		int el = mgls_b64_encode(raw, len, s, sizeof(s));

		CHECK(el == (int)(len / 3 * 4 + (len % 3 ? len % 3 + 1 : 0)));
		if (len > 0) {
			CHECK(mgls_b64_decode(s, (size_t)el, back, sizeof(back), &n) == 0 && n == len &&
			      memcmp(back, raw, len) == 0);
		}
	}
	mgls_b64_encode(raw, 3, s, sizeof(s));
	CHECK(strcmp(s, "-_-_") == 0);
	CHECK(mgls_b64_decode("", 0, back, 8, &n) != 0);
	CHECK(mgls_b64_decode("A", 1, back, 8, &n) != 0);
	CHECK(mgls_b64_decode("AA==", 4, back, 8, &n) != 0);
	CHECK(mgls_b64_decode("A+A/", 4, back, 8, &n) != 0);
	CHECK(mgls_b64_decode("AAAAAAAA", 8, back, 5, &n) != 0); /* doesn't fit */
	CHECK(mgls_b64_encode(raw, 3, s, 4) < 0);
	/* In place. */
	char inplace[16] = "AAECAwQFBgc";

	CHECK(mgls_b64_decode(inplace, strlen(inplace), (uint8_t *)inplace, 16, &n) == 0 && n == 8);
	CHECK(memcmp(inplace, "\x00\x01\x02\x03\x04\x05\x06\x07", 8) == 0);

	uint32_t u;

	CHECK(mgls_parse_u32("0", &u) && u == 0);
	CHECK(mgls_parse_u32("007", &u) && u == 7);
	CHECK(mgls_parse_u32("4294967295", &u) && u == UINT32_MAX);
	CHECK(!mgls_parse_u32("4294967296", &u) && !mgls_parse_u32("18446744073709551616", &u));
	CHECK(!mgls_parse_u32("", &u) && !mgls_parse_u32("-1", &u) && !mgls_parse_u32("1a", &u));
}

static void test_adv(void)
{
	uint8_t adv[31];
	char name[32];
	static const uint8_t addr[6] = {0x41, 0x42, 0xfd, 0x1a, 0x2b, 0x3c};

	CHECK(mgls_adv_flags(false) == 0x02 && mgls_adv_flags(true) == 0x03);
	CHECK(mgls_adv_build(adv, sizeof(adv), false) == 26);
	CHECK(eq_hex(adv, 26, "020106" "1107" "0c245fcf4e31468bcf46ea381c3ddd7f" "04ffffff02"));
	CHECK(mgls_adv_build(adv, sizeof(adv), true) == 26 && adv[25] == 0x03);
	CHECK(mgls_adv_build(adv, 25, true) == 0);
	CHECK(mgls_format_name(name, sizeof(name), "XN54", addr) == 22 &&
	      strcmp(name, "MuseGadget-XN54-1A2B3C") == 0);
	CHECK(mgls_format_name(name, 23, "XN54", addr) == 22); /* exactly fits with the NUL */
	CHECK(mgls_format_name(name, 22, "XN54", addr) == 0);
	CHECK(mgls_format_name(name, sizeof(name), "XYZ", addr) == 21 &&
	      strcmp(name, "MuseGadget-XYZ-1A2B3C") == 0);
	/* The longest model a legacy scan response takes: a 29-char name. */
	CHECK(mgls_format_name(name, sizeof(name), "ABCDEFGHIJK", addr) == 29);
	printf("  workspace %zu B (RX %u, TX %u), mgls_t %zu B, mgtp_t %zu B\n",
	       sizeof(mgls_workspace_t), (unsigned)MGLS_RX_CAP, (unsigned)MGLS_TX_CAP,
	       sizeof(mgls_t), sizeof(mgtp_t));
}

/* ---- the two-slot store under faults ------------------------------------- */

#define SLOT  4096
#define FLASH (2 * SLOT)

static struct {
	uint8_t mem[FLASH];
	int ops_left;   /* < 0: no fault; the fault hits when it reaches 0 */
	bool power_off; /* after the fault every operation fails */
	bool dead;
	bool erase_refused; /* every erase fails, nothing touched (reads and writes still work) */
	int erase_fail_at;  /* 0: off; n: the n-th erase from now fails once, nothing touched */
	bool erase_fail_tears; /* that failing erase tears half the range instead */
	int logs;           /* lines the store logged */
	int misaligned, reprogrammed;
	int reads_left; /* < 0: reads work; the read that reaches 0 fails, once */
	int erase_fail_from; /* 0: off; n: the n-th erase (counted in erases) and every later one fail, nothing touched */
	int read_fail_from;  /* 0: off; n: every read fails once erases has reached n */
	int erases;
} fl;

/* 0: the operation runs; 1: it is torn now; 2: no power, nothing happens. */
static int fault(void)
{
	if (fl.dead) {
		return 2;
	}
	if (fl.ops_left < 0) {
		return 0;
	}
	if (fl.ops_left-- == 0) {
		if (fl.power_off) {
			fl.dead = true;
		}
		return 1;
	}
	return 0;
}

static int fl_erase(void *ctx, uint32_t addr, uint32_t len)
{
	(void)ctx;
	if (addr + len > FLASH) {
		abort();
	}
	fl.erases++;
	if (fl.erase_fail_from > 0 && fl.erases >= fl.erase_fail_from) {
		return -1;
	}
	if (fl.erase_refused || (fl.erase_fail_at > 0 && --fl.erase_fail_at == 0)) {
		if (fl.erase_fail_tears && !fl.erase_refused) {
			memset(&fl.mem[addr], 0xFF, len / 2);
		}
		return -1;
	}
	switch (fault()) {
	case 1:
		memset(&fl.mem[addr], 0xFF, len / 2); /* torn */
		return -1;
	case 2:
		return -1;
	default:
		break;
	}
	memset(&fl.mem[addr], 0xFF, len);
	return 0;
}

static int fl_write(void *ctx, uint32_t addr, const void *data, uint32_t len)
{
	const uint8_t *p = data;

	(void)ctx;
	if (addr % MGLSS_UNIT || len != MGLSS_UNIT) {
		fl.misaligned++;
	}
	for (uint32_t i = 0; i < len; i++) {
		if (fl.mem[addr + i] != 0xFF) {
			fl.reprogrammed++;
		}
	}
	switch (fault()) {
	case 1:
		for (uint32_t i = 0; i < len / 2; i++) {
			fl.mem[addr + i] &= p[i]; /* torn */
		}
		return -1;
	case 2:
		return -1;
	default:
		break;
	}
	for (uint32_t i = 0; i < len; i++) {
		fl.mem[addr + i] &= p[i];
	}
	return 0;
}

static int fl_read(void *ctx, uint32_t addr, void *data, uint32_t len)
{
	(void)ctx;
	if (fl.dead || (fl.reads_left >= 0 && fl.reads_left-- == 0) ||
	    (fl.read_fail_from > 0 && fl.erases >= fl.read_fail_from)) {
		return -1;
	}
	memcpy(data, &fl.mem[addr], len);
	return 0;
}

static void fl_log(void *ctx, const char *line)
{
	(void)ctx;
	(void)line;
	fl.logs++;
}

static const mglss_flash_t FL = {NULL, {0, SLOT}, SLOT, fl_erase, fl_write, fl_read, fl_log};

static mgls_credentials_t make_set(char *a, size_t al, char *r, size_t rl, uint8_t k[32], char tag)
{
	mgls_credentials_t c;

	for (size_t i = 0; i < al; i++) {
		a[i] = (char)('a' + (i + (size_t)tag) % 26);
	}
	for (size_t i = 0; i < rl; i++) {
		r[i] = (char)('A' + (i * 7 + (size_t)tag) % 26);
	}
	memset(k, tag, 32);
	c.access_token = a;
	c.access_len = al;
	c.refresh_token = r;
	c.refresh_len = rl;
	c.k = k;
	c.flags = MGLS_CRED_SETUP_COMPLETE | MGLS_CRED_WIFI_SKIPPED;
	return c;
}

static bool holds(const mglss_t *st, const mgls_credentials_t *c)
{
	char buf[2048];
	uint8_t k[32];
	size_t n;

	return mglss_set_up(st) && mglss_get_k(st, k) == 0 && memcmp(k, c->k, 32) == 0 &&
	       mglss_read_token(st, false, buf, sizeof(buf), &n) == 0 && n == c->access_len &&
	       memcmp(buf, c->access_token, n) == 0 &&
	       mglss_read_token(st, true, buf, sizeof(buf), &n) == 0 && n == c->refresh_len &&
	       memcmp(buf, c->refresh_token, n) == 0 &&
	       (st->flags & MGLS_CRED_WIFI_SKIPPED);
}

static bool other_slot_blank(const mglss_t *st)
{
	int other = st->cur == 0 ? 1 : 0;

	for (int i = 0; i < SLOT; i++) {
		if (fl.mem[other * SLOT + i] != 0xFF) {
			return false;
		}
	}
	return true;
}

static void test_store(void)
{
	static char a1[403], r1[1396], a2[600], r2[1400];
	uint8_t k1[32], k2[32], k[32];
	mgls_credentials_t s1 = make_set(a1, sizeof(a1), r1, sizeof(r1), k1, 1);
	mgls_credentials_t s2 = make_set(a2, sizeof(a2), r2, sizeof(r2), k2, 2);
	mglss_t st;
	int total = -1;

	CHECK(MGLSS_SLOT_NEED <= SLOT);
	memset(fl.mem, 0xFF, sizeof(fl.mem));
	fl.ops_left = -1;
	fl.reads_left = -1;
	CHECK(mglss_load(&st, &FL) == 0 && !mglss_set_up(&st));
	CHECK(mglss_get_k(&st, k) == MGTP_NO_TOKEN);
	CHECK(mglss_commit(&st, &s1) == 0 && holds(&st, &s1));
	CHECK(mglss_load(&st, &FL) == 0 && holds(&st, &s1) && other_slot_blank(&st));
	CHECK(mglss_commit(&st, &s2) == 0 && holds(&st, &s2) && other_slot_blank(&st));
	CHECK(mglss_load(&st, &FL) == 0 && holds(&st, &s2) && st.seq == 2);
	CHECK(fl.misaligned == 0 && fl.reprogrammed == 0);
	CHECK(mglss_erase(&st) == 0 && !mglss_set_up(&st));
	CHECK(mglss_load(&st, &FL) == 0 && !mglss_set_up(&st));

	/* Count a commit's operations, then cut it at every one of them. */
	for (int power_off = 0; power_off <= 1; power_off++) {
		for (int cut = 0;; cut++) {
			memset(fl.mem, 0xFF, sizeof(fl.mem));
			fl.ops_left = -1;
			fl.dead = false;
			fl.power_off = false;
			CHECK(mglss_load(&st, &FL) == 0 && mglss_commit(&st, &s1) == 0);
			fl.ops_left = cut;
			fl.power_off = power_off != 0;
			int r = mglss_commit(&st, &s2);
			bool hit = fl.ops_left < 0 || fl.dead;

			/* Power back on and boot. */
			fl.ops_left = -1;
			fl.dead = false;
			CHECK(mglss_load(&st, &FL) == 0);
			if (!power_off) {
				/* A failed commit keeps the old set; a done one the
				 * new set. (After a power loss either may stand:
				 * nothing was reported.) */
				CHECK(holds(&st, r == 0 ? &s2 : &s1));
			}
			CHECK(holds(&st, &s1) || holds(&st, &s2)); /* never a mix or nothing */
			CHECK(other_slot_blank(&st));            /* never two sets */
			if (!hit) {
				total = cut;
				break;
			}
		}
	}
	CHECK(total > 10);
	printf("  store: %d flash operations per commit, each cut (torn, power loss) recovers\n",
	       total);

	/* A failing erase of the target slot: nothing written, old set kept. */
	memset(fl.mem, 0xFF, sizeof(fl.mem));
	fl.ops_left = -1;
	fl.power_off = false;
	CHECK(mglss_load(&st, &FL) == 0 && mglss_commit(&st, &s1) == 0);
	fl.ops_left = 0;
	CHECK(mglss_commit(&st, &s2) != 0 && holds(&st, &s1));
	fl.ops_left = -1;

	/* A read failing once at boot, at each of load's reads in turn, with two
	 * valid slots (the old one kept by a power loss): nothing erased, nothing
	 * chosen; the next boot keeps the newer set. A commit after such a boot
	 * leaves only its own set. */
	static uint8_t old_slot[SLOT];
	int fail_at;

	memset(fl.mem, 0xFF, sizeof(fl.mem));
	CHECK(mglss_load(&st, &FL) == 0 && mglss_commit(&st, &s1) == 0);
	memcpy(old_slot, fl.mem, SLOT);
	CHECK(mglss_commit(&st, &s2) == 0 && st.cur == 1);
	for (fail_at = 0;; fail_at++) {
		memcpy(fl.mem, old_slot, SLOT);
		fl.erases = 0;
		fl.reads_left = fail_at;
		int r = mglss_load(&st, &FL);
		bool hit = fl.reads_left < 0;

		fl.reads_left = -1;
		if (r == 0) {
			/* past both checks: only the old slot's erase was left */
			CHECK(holds(&st, &s2) && other_slot_blank(&st));
			if (!hit) {
				break;
			}
			continue;
		}
		/* unreadable is not empty: set up (fail closed), nothing readable */
		CHECK(r == -2 && fl.erases == 0 && mglss_set_up(&st) && mglss_get_k(&st, k) < 0 &&
		      memcmp(fl.mem, old_slot, SLOT) == 0);
		CHECK(mglss_load(&st, &FL) == 0 && holds(&st, &s2) && other_slot_blank(&st));
	}
	CHECK(fail_at >= 8);
	memcpy(fl.mem, old_slot, SLOT);
	fl.reads_left = 0;
	CHECK(mglss_load(&st, &FL) == -2 && mglss_set_up(&st) && mglss_get_k(&st, k) < 0);
	fl.reads_left = -1;
	CHECK(mglss_commit(&st, &s1) == 0 && mglss_load(&st, &FL) == 0 && holds(&st, &s1) && other_slot_blank(&st));

	/* Two valid sets again (the old one kept by a power loss) behind a boot that couldn't read. The commit tells
	 * an unreadable slot from an empty one first: still unreadable, it fails with nothing erased or written (that
	 * slot may hold a set newer than any seq the commit could choose); readable, the new set goes above the set
	 * the boot missed. */
	static uint8_t both[FLASH];

	memset(fl.mem, 0xFF, sizeof(fl.mem));
	CHECK(mglss_load(&st, &FL) == 0 && mglss_commit(&st, &s1) == 0);
	memcpy(old_slot, fl.mem, SLOT);
	CHECK(mglss_commit(&st, &s2) == 0 && st.cur == 1);
	memcpy(fl.mem, old_slot, SLOT);
	memcpy(both, fl.mem, FLASH);
	fl.reads_left = 0;
	CHECK(mglss_load(&st, &FL) == -2 && mglss_set_up(&st));
	fl.erases = 0;
	fl.reads_left = 0;
	CHECK(mglss_commit(&st, &s1) == -2 && fl.erases == 0 && memcmp(fl.mem, both, FLASH) == 0 && mglss_set_up(&st));
	fl.reads_left = -1;
	CHECK(mglss_commit(&st, &s1) == 0 && st.seq == 3 && holds(&st, &s1) && other_slot_blank(&st));
	CHECK(mglss_load(&st, &FL) == 0 && holds(&st, &s1) && st.seq == 3 && other_slot_blank(&st));

	/* The old slot's erase fails with its set intact: the new set is taken back and the commit fails, the old
	 * set kept and never two sets, now and at the next boot. */
	fl.erase_fail_at = 2; /* the target's erase works, the old slot's fails */
	CHECK(mglss_commit(&st, &s2) != 0 && fl.erase_fail_at == 0 && holds(&st, &s1) && other_slot_blank(&st));
	CHECK(mglss_load(&st, &FL) == 0 && holds(&st, &s1) && other_slot_blank(&st));
	CHECK(mglss_commit(&st, &s2) == 0 && mglss_commit(&st, &s1) == 0 && mglss_load(&st, &FL) == 0 &&
	      holds(&st, &s1) && other_slot_blank(&st));
	/* The old slot's erase fails after tearing its set: the new set stands, the store logs one line, and the
	 * next boot clears what's left. */
	fl.logs = 0;
	fl.erase_fail_at = 2;
	fl.erase_fail_tears = true;
	CHECK(mglss_commit(&st, &s2) == 0 && fl.logs == 1 && holds(&st, &s2));
	fl.erase_fail_tears = false;
	CHECK(mglss_load(&st, &FL) == 0 && holds(&st, &s2) && other_slot_blank(&st) && fl.logs == 1);
	CHECK(mglss_commit(&st, &s1) == 0 && holds(&st, &s1));
	/* An erase that fails leaves the set it couldn't erase, and the state says so, as the next boot would. */
	fl.erase_refused = true;
	CHECK(mglss_erase(&st) != 0 && holds(&st, &s1));
	fl.erase_refused = false;
	CHECK(mglss_load(&st, &FL) == 0 && holds(&st, &s1));
	/* The reloads inside an erase and a commit keep an unreadable slot (-2): mglss_set_up() stays true (fail
	 * closed), nothing is readable, and a load that reads both slots again finds what they hold. */
	fl.erases = 0;
	fl.erase_refused = true;
	fl.read_fail_from = 1; /* the erase's reload can't read */
	CHECK(mglss_erase(&st) == -2 && mglss_set_up(&st) && mglss_get_k(&st, k) < 0);
	fl.erase_refused = false;
	fl.read_fail_from = 0;
	CHECK(mglss_load(&st, &FL) == 0 && holds(&st, &s1));
	fl.erases = 0;
	fl.erase_fail_from = 2; /* the target's erase works; the old slot's and the take-back fail */
	fl.read_fail_from = 3;  /* from the take-back on, reads fail: the reload can't read */
	CHECK(mglss_commit(&st, &s2) == -2 && fl.erases == 3 && mglss_set_up(&st) && mglss_get_k(&st, k) < 0);
	fl.erase_fail_from = 0;
	fl.read_fail_from = 0;
	CHECK(mglss_load(&st, &FL) == 0 && holds(&st, &s2) && other_slot_blank(&st)); /* the newer set wins */
	CHECK(mglss_commit(&st, &s1) == 0 && holds(&st, &s1));

	/* Tokens over the limits are refused before touching flash. */
	mgls_credentials_t big = s1;

	big.access_len = MGLS_ACCESS_TOKEN_MAX + 1;
	CHECK(mglss_commit(&st, &big) != 0 && holds(&st, &s1));
}

#if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 36)) /* no arc4random_buf before glibc 2.36 */
#include <sys/random.h>
static void arc4random_buf(void *buf, size_t n)
{
	for (uint8_t *p = buf; n;) {
		size_t k = n < 256 ? n : 256;
		if (getentropy(p, k) != 0)
			abort();
		p += k;
		n -= k;
	}
}
#endif

static int host_rng(void *ctx, uint8_t *out, size_t len)
{
	(void)ctx;
	arc4random_buf(out, len);
	return 0;
}

/* mgls_init() fails closed without the required hooks (set_up always; may_prompt in a release build). */
static bool hook_set_up(void *ctx)
{
	(void)ctx;
	return true;
}

static bool hook_may_prompt(void *ctx, uint32_t since_ms)
{
	(void)ctx;
	return since_ms >= MGLS_RESETUP_PROMPT_MS;
}

MGLS_STATIC_HOOKS(hook_set_up, hook_may_prompt);

static void test_init_hooks(void)
{
	mgls_t s;
	mgls_config_t c;

	memset(&c, 0, sizeof(c));
	c.random = host_rng;
	CHECK(mgls_init(&s, &c) == MGLS_E_CONFIG); /* no set_up, no may_prompt */
	c.may_prompt = hook_may_prompt;
	CHECK(mgls_init(&s, &c) == MGLS_E_CONFIG); /* no set_up */
	c.set_up = hook_set_up;
	CHECK(mgls_init(&s, &c) == MGLS_OK);
	c.may_prompt = NULL;
#if MGLS_RELEASE
	CHECK(mgls_init(&s, &c) == MGLS_E_CONFIG); /* a release build needs the rate */
#else
	CHECK(mgls_init(&s, &c) == MGLS_OK);
	c.set_up = MGLS_HOOK_TEST_DEFAULT(set_up);
	c.may_prompt = MGLS_HOOK_TEST_DEFAULT(may_prompt);
	CHECK(mgls_init(&s, &c) == MGLS_OK);
	CHECK(!c.set_up(NULL) && c.may_prompt(NULL, 0));
#endif
	CHECK(sizeof(mgls_t) <= 48); /* 36 B on 32-bit targets: refused took padding */
}

int main(void)
{
	printf("test_unit\n");
	CHECK(mglsc_init(host_rng, NULL) == 0);
	test_sha_hmac_hkdf();
	test_gcm();
	test_p256();
	test_token_proof();
	test_json();
	test_b64();
	test_adv();
	test_store();
	test_init_hooks();
	printf("  %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
