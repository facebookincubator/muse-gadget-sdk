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

/* Layer 1 host tests: protocols/test-vectors/mg-session-seal-v1.json byte for
 * byte (keys, sealed bytes, opening), the reference checker's rejections (a
 * flipped tag, a reflected direction, a replayed or skipped counter), every
 * truncation and single-bit flip, keys from other nonces or node_id, and the
 * streaming path (mgseal_tag_stream + mgseal_xor_at at every fragment size)
 * against the in-place one. Vectors arrive as seal_vectors.h
 * (gen_seal_vectors.py). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_session_seal.h"
#include "mgls_crypto.h"

typedef struct {
	const char *name;
	const char *k, *cn, *dn, *node, *enc, *mac, *sealed, *plaintext;
	unsigned dir, counter;
} vec_t;
typedef struct { /* sealed_data: one sealed message of the session */
	const char *name, *sealed;
	unsigned dir, counter;
	const char *plaintext;
	int refused; /* the receiver refused it: a replayed counter (a good seal), or a bad tag */
} data_t;
typedef struct { /* sealed_credentials_saved: one sealed answer */
	const char *name, *sealed, *refresh;
	int saved; /* the client counts it as saved (result 1, the hash of the update's refresh_token) */
} saved_t;

#include "seal_vectors.h"

#define CHECK(c)                                                                         \
	do {                                                                             \
		if (!(c)) {                                                              \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
			exit(1);                                                         \
		}                                                                        \
		checks++;                                                                \
	} while (0)

static long checks;

static size_t unhex(const char *h, uint8_t *out, size_t cap)
{
	size_t n = strlen(h) / 2;

	CHECK(n <= cap);
	for (size_t i = 0; i < n; i++) {
		unsigned v;

		CHECK(sscanf(h + 2 * i, "%2x", &v) == 1);
		out[i] = (uint8_t)v;
	}
	return n;
}

typedef struct {
	const uint8_t *pt;
	uint32_t len;
	int reads;
} src_t;

static int read_pt(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	src_t *s = ctx;

	if (n > 32 || off + n > s->len) {
		return -1;
	}
	memcpy(buf, s->pt + off, n);
	s->reads++;
	return 0;
}

static int read_fail(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	(void)ctx;
	(void)buf;
	(void)n;
	return off >= 64 ? -1 : 0;
}

static void one(const vec_t *v)
{
	uint8_t k[32], cn[16], dn[16], enc[32], mac[32], tag[16];
	static uint8_t want[4096], buf[4096], work[4096];
	size_t sl, pl, n;
	mgseal_keys_t keys, other;

	CHECK(unhex(v->k, k, 32) == 32 && unhex(v->cn, cn, 16) == 16 && unhex(v->dn, dn, 16) == 16);
	CHECK(unhex(v->enc, enc, 32) == 32 && unhex(v->mac, mac, 32) == 32);
	sl = unhex(v->sealed, want, sizeof(want));
	pl = strlen(v->plaintext);
	CHECK(sl == pl + MGSEAL_OVERHEAD);

	CHECK(mgseal_keys(&keys, k, cn, dn, v->node, strlen(v->node)) == MGSEAL_OK);
	CHECK(!memcmp(keys.enc, enc, 32) && !memcmp(keys.mac, mac, 32));

	memcpy(buf + 4, v->plaintext, pl);
	CHECK(mgseal_seal(&keys, (uint8_t)v->dir, v->counter, buf, pl) == MGSEAL_OK);
	CHECK(!memcmp(buf, want, sl));

	memcpy(work, want, sl);
	CHECK(mgseal_open(&keys, (uint8_t)v->dir, v->counter, work, sl, &n) == MGSEAL_OK && n == pl);
	CHECK(!memcmp(work + 4, v->plaintext, pl));

	/* The reference checker's rejections; a refused message is never decrypted. */
	memcpy(work, want, sl);
	work[sl - 1] ^= 1;
	CHECK(mgseal_open(&keys, (uint8_t)v->dir, v->counter, work, sl, &n) == MGSEAL_E_TAG && n == 0);
	memcpy(work, want, sl);
	CHECK(mgseal_open(&keys, (uint8_t)(3 - v->dir), v->counter, work, sl, &n) == MGSEAL_E_TAG);
	CHECK(mgseal_open(&keys, (uint8_t)v->dir, v->counter + 1, work, sl, &n) == MGSEAL_E_COUNTER);
	if (v->counter) {
		CHECK(mgseal_open(&keys, (uint8_t)v->dir, v->counter - 1, work, sl, &n) ==
		      MGSEAL_E_COUNTER);
	}
	CHECK(!memcmp(work, want, sl));
	for (size_t cut = 0; cut < sl; cut++) {
		memcpy(work, want, sl);
		CHECK(mgseal_open(&keys, (uint8_t)v->dir, v->counter, work, cut, &n) != MGSEAL_OK &&
		      n == 0);
	}
	for (size_t i = 0; i < sl; i++) {
		for (unsigned b = 0; b < 8; b++) {
			memcpy(work, want, sl);
			work[i] ^= (uint8_t)(1u << b);
			CHECK(mgseal_open(&keys, (uint8_t)v->dir, v->counter, work, sl, &n) != MGSEAL_OK);
		}
	}
	cn[0] ^= 1;
	CHECK(mgseal_keys(&other, k, cn, dn, v->node, strlen(v->node)) == MGSEAL_OK);
	memcpy(work, want, sl);
	CHECK(mgseal_open(&other, (uint8_t)v->dir, v->counter, work, sl, &n) == MGSEAL_E_TAG);
	cn[0] ^= 1;
	CHECK(mgseal_keys(&other, k, cn, dn, v->node, strlen(v->node) - 1) == MGSEAL_OK);
	CHECK(mgseal_open(&other, (uint8_t)v->dir, v->counter, work, sl, &n) == MGSEAL_E_TAG);

#if MGSEAL_STREAM
	/* Streaming: the same tag, and fragments of every size give the same bytes. */
	src_t s = {(const uint8_t *)v->plaintext, (uint32_t)pl, 0};

	CHECK(mgseal_tag_stream(&keys, (uint8_t)v->dir, v->counter, (uint32_t)pl, read_pt, &s, tag) ==
	      MGSEAL_OK);
	CHECK(!memcmp(tag, want + 4 + pl, 16) && s.reads == (int)((pl + 31) / 32));
	for (size_t frag = 1; frag <= 300; frag += (frag < 40 ? 1 : 37)) {
		for (size_t off = 0; off < pl; off += frag) {
			size_t m = pl - off < frag ? pl - off : frag;

			memcpy(work, v->plaintext + off, m);
			CHECK(mgseal_xor_at(&keys, (uint8_t)v->dir, v->counter, (uint32_t)off, work, m) ==
			      MGSEAL_OK);
			CHECK(!memcmp(work, want + 4 + off, m));
		}
	}
	CHECK(mgseal_tag_stream(&keys, (uint8_t)v->dir, v->counter, (uint32_t)pl, read_fail, NULL,
				tag) == (pl > 64 ? MGSEAL_E_CRYPTO : MGSEAL_OK));
#else
	(void)tag;
	(void)read_pt;
	(void)read_fail;
#endif
}

#if defined(MGLS_CRYPTO_PSA)
/* mg_link_setup's PSA backend starts PSA in mglsc_init. A board's own seal
 * glue (the ESP32's, Zephyr's) provides only what the seal calls, so this
 * test asks nothing more of it. */
static int no_rng(void *ctx, uint8_t *out, size_t len)
{
	(void)ctx;
	memset(out, 0, len); /* the seal draws nothing */
	return 0;
}
#endif

/* sealed_data: every sealed message of the session (credentials, credentials_update and data,
 * both directions, one counter per direction) seals and opens to the vector's bytes, and the incremental tag a
 * receiver makes as fragments arrive matches it in any piece sizes; a flipped byte, another counter or the other
 * direction don't. A refused step with a bad tag: the ciphertext is the seal's, the tag isn't, and nothing opens. */
static void sealed_steps(const char *kh, const char *cnh, const char *dnh, const char *node, const data_t *steps,
			 size_t nsteps)
{
	uint8_t k[32], cn[16], dn[16];
	static uint8_t want[4096], buf[4096], pt[4096];
	mgseal_keys_t keys;

	if (nsteps == 0) {
		return;
	}
	CHECK(unhex(kh, k, 32) == 32 && unhex(cnh, cn, 16) == 16 && unhex(dnh, dn, 16) == 16);
	CHECK(mgseal_keys(&keys, k, cn, dn, node, strlen(node)) == MGSEAL_OK);
	for (size_t i = 0; i < nsteps; i++) {
		const data_t *s = &steps[i];
		size_t sl = unhex(s->sealed, want, sizeof(want)), pl = unhex(s->plaintext, pt, sizeof(pt)), n;

		CHECK(sl == pl + MGSEAL_OVERHEAD);
		memcpy(buf + 4, pt, pl);
		CHECK(mgseal_seal(&keys, (uint8_t)s->dir, s->counter, buf, pl) == MGSEAL_OK);
		if (s->refused && memcmp(buf, want, sl) != 0) {
			/* a bad tag: the same counter and ciphertext, another tag; it doesn't open, in place or in pieces */
			CHECK(!memcmp(buf, want, sl - MGSEAL_TAG_LEN));
			memcpy(buf, want, sl);
			CHECK(mgseal_open(&keys, (uint8_t)s->dir, s->counter, buf, sl, &n) == MGSEAL_E_TAG && n == 0);
#if MGSEAL_STREAM
			{
				mgseal_tag_t t;
				uint32_t ct = (uint32_t)(sl - MGSEAL_OVERHEAD);

				CHECK(mgseal_tag_begin(&t, &keys, (uint8_t)s->dir, s->counter, ct) == MGSEAL_OK);
				mgseal_tag_update(&t, want + 4, ct);
				CHECK(mgseal_tag_check(&t, &keys, want + 4 + ct) == MGSEAL_E_TAG);
			}
#endif
			continue;
		}
		CHECK(!memcmp(buf, want, sl));
		CHECK(mgseal_open(&keys, (uint8_t)s->dir, s->counter, buf, sl, &n) == MGSEAL_OK && n == pl);
		CHECK(!memcmp(buf + 4, pt, pl));
#if MGSEAL_STREAM
		{
			static const size_t pieces[] = {1, 5, 31, 32, 33, 90, 4096};
			uint32_t ct = (uint32_t)(sl - MGSEAL_OVERHEAD);

			for (size_t j = 0; j < sizeof(pieces) / sizeof(pieces[0]); j++) {
				mgseal_tag_t t;

				CHECK(mgseal_tag_begin(&t, &keys, (uint8_t)s->dir, s->counter, ct) == MGSEAL_OK);
				for (uint32_t off = 0; off < ct; off += (uint32_t)pieces[j]) {
					size_t m = ct - off < pieces[j] ? ct - off : pieces[j];

					mgseal_tag_update(&t, want + 4 + off, m);
				}
				CHECK(mgseal_tag_check(&t, &keys, want + 4 + ct) == MGSEAL_OK);
			}
			for (int bad = 0; bad < 3; bad++) {
				mgseal_tag_t t;
				uint8_t dir = (uint8_t)(bad == 2 ? 3u - s->dir : s->dir);

				CHECK(mgseal_tag_begin(&t, &keys, dir, s->counter + (bad == 1), ct) == MGSEAL_OK);
				memcpy(buf, want, sl);
				if (bad == 0) {
					buf[4 + ct / 2] ^= 0x40;
				}
				mgseal_tag_update(&t, buf + 4, ct);
				CHECK(mgseal_tag_check(&t, &keys, want + 4 + ct) == MGSEAL_E_TAG);
			}
		}
#endif
	}
}

static void sealed_data(void)
{
	sealed_steps(DATA_K, DATA_CN, DATA_DN, DATA_NODE, DATA_STEPS, N_DATA_STEPS);
	/* sealed_interleaved: the messages of two in flight together, the earlier refused at its
	 * tag, the real one and its replay */
	sealed_steps(IL_K, IL_CN, IL_DN, IL_NODE, IL_STEPS, N_IL_STEPS);
	/* sealed_data_large: with the device's sealed credentials_saved at counter 2 */
	sealed_steps(LARGE_K, LARGE_CN, LARGE_DN, LARGE_NODE, LARGE_STEPS, N_LARGE_STEPS);
}

#if MGSEAL_STREAM
/* sealed_credentials_saved: every sealed answer opens under its counter (device -> client);
 * mgseal_saved_plaintext() of the update's refresh_token gives exactly the plaintext of an answer the client counts as
 * saved, and sealing it gives the vector's bytes; a not-saved answer is [0] and zeros, as mgseal_saved_plaintext(false)
 * gives, or [1] with another pair's hash. */
static void sealed_saved(void)
{
	uint8_t k[32], cn[16], dn[16], want[64], buf[64], pt[MGSEAL_SAVED_LEN], zero[MGSEAL_SAVED_LEN];
	mgseal_keys_t keys;

	if (N_SAVED == 0 || SAVED[0].sealed[0] == 0) {
		return;
	}
	CHECK(unhex(SV_K, k, 32) == 32 && unhex(SV_CN, cn, 16) == 16 && unhex(SV_DN, dn, 16) == 16);
	CHECK(mgseal_keys(&keys, k, cn, dn, SV_NODE, strlen(SV_NODE)) == MGSEAL_OK);
	CHECK(mgseal_saved_plaintext(zero, false, NULL, 0) == MGSEAL_OK);
	for (size_t i = 0; i < MGSEAL_SAVED_LEN; i++) {
		CHECK(zero[i] == 0);
	}
	for (size_t i = 0; i < N_SAVED; i++) {
		const saved_t *s = &SAVED[i];
		size_t sl = unhex(s->sealed, want, sizeof(want)), n = 0;
		uint32_t ctr;

		CHECK(sl == MGSEAL_SAVED_SEALED);
		ctr = (uint32_t)want[0] << 24 | (uint32_t)want[1] << 16 | (uint32_t)want[2] << 8 | want[3];
		memcpy(buf, want, sl);
		CHECK(mgseal_open(&keys, MGSEAL_DIR_DEVICE, ctr, buf, sl, &n) == MGSEAL_OK && n == MGSEAL_SAVED_LEN);
		CHECK(mgseal_saved_plaintext(pt, true, (const uint8_t *)s->refresh, strlen(s->refresh)) == MGSEAL_OK);
		CHECK(pt[0] == 1);
		if (s->saved) {
			CHECK(!memcmp(buf + 4, pt, MGSEAL_SAVED_LEN));
			memcpy(buf + 4, pt, MGSEAL_SAVED_LEN);
			CHECK(mgseal_seal(&keys, MGSEAL_DIR_DEVICE, ctr, buf, MGSEAL_SAVED_LEN) == MGSEAL_OK);
			CHECK(!memcmp(buf, want, sl));
		} else if (buf[4] == 0) {
			CHECK(!memcmp(buf + 4, zero, MGSEAL_SAVED_LEN));
			memcpy(buf + 4, zero, MGSEAL_SAVED_LEN);
			CHECK(mgseal_seal(&keys, MGSEAL_DIR_DEVICE, ctr, buf, MGSEAL_SAVED_LEN) == MGSEAL_OK);
			CHECK(!memcmp(buf, want, sl));
		} else {
			CHECK(buf[4] == 1 && memcmp(buf + 4, pt, MGSEAL_SAVED_LEN) != 0); /* another pair's hash */
		}
	}
}
#else
static void sealed_saved(void)
{
}
#endif

int main(void)
{
	mgseal_keys_t keys;
	uint8_t k[32] = {1}, cn[16] = {2}, dn[16] = {3}, buf[64], tag[16];
	size_t n = 99;

#if defined(MGLS_CRYPTO_PSA)
	if (mglsc_init(no_rng, NULL) != 0) { /* psa_crypto_init */
		fprintf(stderr, "mglsc_init\n");
		return 1;
	}
#endif

	for (size_t i = 0; i < sizeof(VECTORS) / sizeof(VECTORS[0]); i++) {
		one(&VECTORS[i]);
	}
	sealed_data();
	sealed_saved();
	/* An empty plaintext: the counter and the tag alone. */
	CHECK(mgseal_keys(&keys, k, cn, dn, "n", 1) == MGSEAL_OK);
	CHECK(mgseal_seal(&keys, MGSEAL_DIR_DEVICE, 5, buf, 0) == MGSEAL_OK);
	CHECK(mgseal_open(&keys, MGSEAL_DIR_DEVICE, 5, buf, MGSEAL_OVERHEAD, &n) == MGSEAL_OK && n == 0);
#if MGSEAL_STREAM
	CHECK(mgseal_tag_stream(&keys, MGSEAL_DIR_DEVICE, 5, 0, read_fail, NULL, tag) == MGSEAL_OK);
	CHECK(!memcmp(tag, buf + 4, 16));
#else
	(void)tag;
#endif
	printf("mg_session_seal: %zu vectors, %u sealed_data messages, %ld checks (%u sealed_interleaved messages, "
	       "%u sealed credentials_saved, %u sealed_data_large messages)\n",
	       sizeof(VECTORS) / sizeof(VECTORS[0]), (unsigned)N_DATA_STEPS, checks, (unsigned)N_IL_STEPS,
	       MGSEAL_STREAM ? (unsigned)N_SAVED : 0u, (unsigned)N_LARGE_STEPS);
	return 0;
}
