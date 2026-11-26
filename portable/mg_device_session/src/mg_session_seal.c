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

#include "mg_session_seal.h"

#include <string.h>

#include "mgls_crypto.h"

static const char SALT[] = "mg session seal v1";
static const char STREAM[] = "mg session seal v1 stream";
static const char TAGL[] = "mg session seal v1 tag";

void mgseal_wipe(void *p, size_t n)
{
	volatile uint8_t *v = (volatile uint8_t *)p;

	while (n--) {
		*v++ = 0;
	}
}

static void be32(uint8_t b[4], uint32_t v)
{
	b[0] = (uint8_t)(v >> 24);
	b[1] = (uint8_t)(v >> 16);
	b[2] = (uint8_t)(v >> 8);
	b[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t b[4])
{
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

int mgseal_keys(mgseal_keys_t *keys, const uint8_t k[MGSEAL_KEY_LEN],
		const uint8_t cn[MGSEAL_NONCE_LEN], const uint8_t dn[MGSEAL_NONCE_LEN],
		const char *node_id, size_t node_len)
{
	/* RFC 5869, L = 64: PRK = HMAC(salt, K); T1 = HMAC(PRK, info || 1);
	 * T2 = HMAC(PRK, T1 || info || 2). */
	uint8_t prk[MGLSC_HASH_LEN];
	const uint8_t one = 1, two = 2;
	const uint8_t *p[5];
	size_t l[5];
	int r;

	p[0] = k;
	l[0] = MGSEAL_KEY_LEN;
	r = mglsc_hmac_sha256((const uint8_t *)SALT, sizeof(SALT) - 1, p, l, 1, prk);
	p[0] = cn;
	l[0] = MGSEAL_NONCE_LEN;
	p[1] = dn;
	l[1] = MGSEAL_NONCE_LEN;
	p[2] = (const uint8_t *)node_id;
	l[2] = node_len;
	p[3] = &one;
	l[3] = 1;
	if (r == 0) {
		r = mglsc_hmac_sha256(prk, sizeof(prk), p, l, 4, keys->enc);
	}
	p[0] = keys->enc;
	l[0] = MGSEAL_KEY_LEN;
	p[1] = cn;
	l[1] = MGSEAL_NONCE_LEN;
	p[2] = dn;
	l[2] = MGSEAL_NONCE_LEN;
	p[3] = (const uint8_t *)node_id;
	l[3] = node_len;
	p[4] = &two;
	l[4] = 1;
	if (r == 0) {
		r = mglsc_hmac_sha256(prk, sizeof(prk), p, l, 5, keys->mac);
	}
	mgseal_wipe(prk, sizeof(prk));
	if (r != 0) {
		mgseal_wipe(keys, sizeof(*keys));
		return MGSEAL_E_CRYPTO;
	}
	return MGSEAL_OK;
}

int mgseal_xor_at(const mgseal_keys_t *keys, uint8_t dir, uint32_t counter, uint32_t off,
		  uint8_t *data, size_t n)
{
	uint8_t ctr[4], idx[4], block[32];
	const uint8_t *p[4];
	size_t l[4];
	int r = 0;

	p[0] = (const uint8_t *)STREAM;
	l[0] = sizeof(STREAM) - 1;
	p[1] = &dir;
	l[1] = 1;
	p[2] = ctr;
	l[2] = 4;
	p[3] = idx;
	l[3] = 4;
	be32(ctr, counter);
	while (n && r == 0) {
		uint32_t i = off / 32u, at = off % 32u;
		size_t take = 32u - at;

		if (take > n) {
			take = n;
		}
		be32(idx, i);
		r = mglsc_hmac_sha256(keys->enc, MGSEAL_KEY_LEN, p, l, 4, block);
		for (size_t j = 0; j < take; j++) {
			data[j] ^= block[at + j];
		}
		data += take;
		off += (uint32_t)take;
		n -= take;
	}
	mgseal_wipe(block, sizeof(block));
	return r != 0 ? MGSEAL_E_CRYPTO : MGSEAL_OK;
}

static int tag_of(const mgseal_keys_t *keys, uint8_t dir, uint32_t counter, const uint8_t *ct,
		  size_t len, uint8_t out[32])
{
	uint8_t ctr[4], ln[4];
	const uint8_t *p[5];
	size_t l[5];

	be32(ctr, counter);
	be32(ln, (uint32_t)len);
	p[0] = (const uint8_t *)TAGL;
	l[0] = sizeof(TAGL) - 1;
	p[1] = &dir;
	l[1] = 1;
	p[2] = ctr;
	l[2] = 4;
	p[3] = ln;
	l[3] = 4;
	p[4] = ct;
	l[4] = len;
	return mglsc_hmac_sha256(keys->mac, MGSEAL_KEY_LEN, p, l, 5, out) != 0 ? MGSEAL_E_CRYPTO
									    : MGSEAL_OK;
}

int mgseal_seal(const mgseal_keys_t *keys, uint8_t dir, uint32_t counter, uint8_t *buf,
		size_t len)
{
	uint8_t t[32];
	int r;

	if (len > 0xFFFFFFFFu - MGSEAL_OVERHEAD) {
		return MGSEAL_E_LEN;
	}
	be32(buf, counter);
	r = mgseal_xor_at(keys, dir, counter, 0, buf + 4, len);
	if (r == MGSEAL_OK) {
		r = tag_of(keys, dir, counter, buf + 4, len, t);
	}
	if (r == MGSEAL_OK) {
		memcpy(buf + 4 + len, t, MGSEAL_TAG_LEN);
	}
	mgseal_wipe(t, sizeof(t));
	return r;
}

int mgseal_open(const mgseal_keys_t *keys, uint8_t dir, uint32_t expected, uint8_t *buf,
		size_t sealed_len, size_t *len)
{
	uint8_t t[32];
	size_t n;
	int r;

	*len = 0;
	if (sealed_len < MGSEAL_OVERHEAD) {
		return MGSEAL_E_LEN;
	}
	n = sealed_len - MGSEAL_OVERHEAD;
	if (get_be32(buf) != expected) {
		return MGSEAL_E_COUNTER;
	}
	r = tag_of(keys, dir, expected, buf + 4, n, t);
	if (r == MGSEAL_OK && !mglsc_ct_equal(t, buf + 4 + n, MGSEAL_TAG_LEN)) {
		r = MGSEAL_E_TAG;
	}
	mgseal_wipe(t, sizeof(t));
	if (r == MGSEAL_OK) {
		r = mgseal_xor_at(keys, dir, expected, 0, buf + 4, n);
	}
	if (r == MGSEAL_OK) {
		*len = n;
	}
	return r;
}

#if MGSEAL_STREAM
/* ---- an incremental SHA-256 / HMAC for the streaming tag ------------------ */

#if MGSEAL_EXT_SHA
/* The backend's (mgls_crypto.h). err holds the first failure (PSA can fail):
 * the tag is then refused (MGSEAL_E_CRYPTO); final always releases the
 * context. */
typedef mgseal_sha_t sha_t; /* mg_session_seal.h */

static void sha_init(sha_t *c)
{
	c->err = mglsc_sha256_init(&c->s);
}

static void sha_update(sha_t *c, const uint8_t *p, size_t n)
{
	if (c->err == 0) {
		c->err = mglsc_sha256_update(&c->s, p, n);
	}
}

static void sha_final(sha_t *c, uint8_t out[32])
{
	int e = mglsc_sha256_final(&c->s, out);

	if (c->err == 0) {
		c->err = e;
	}
}
#define SHA_FAILED(c) ((c).err != 0)
#else
#define SHA_FAILED(c) 0

typedef mgseal_sha_t sha_t; /* mg_session_seal.h */

static const uint32_t KTAB[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
	0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
	0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
	0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
	0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
	0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
	0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
	0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
	0xc67178f2u,
};

static uint32_t ror(uint32_t x, unsigned n)
{
	return x >> n | x << (32u - n);
}

static void sha_block(sha_t *c, const uint8_t *p)
{
	uint32_t w[16], s[8];

	for (unsigned i = 0; i < 16; i++) {
		w[i] = get_be32(p + 4 * i);
	}
	memcpy(s, c->h, sizeof(s));
	for (unsigned i = 0; i < 64; i++) {
		uint32_t wi;

		if (i < 16) {
			wi = w[i];
		} else {
			uint32_t a = w[(i + 1) & 15], b = w[(i + 14) & 15];
			uint32_t s0 = ror(a, 7) ^ ror(a, 18) ^ (a >> 3);
			uint32_t s1 = ror(b, 17) ^ ror(b, 19) ^ (b >> 10);

			wi = w[i & 15] = w[i & 15] + s0 + w[(i + 9) & 15] + s1;
		}
		uint32_t t1 = s[7] + (ror(s[4], 6) ^ ror(s[4], 11) ^ ror(s[4], 25)) +
			      ((s[4] & s[5]) ^ (~s[4] & s[6])) + KTAB[i] + wi;
		uint32_t t2 = (ror(s[0], 2) ^ ror(s[0], 13) ^ ror(s[0], 22)) +
			      ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));

		/* the rotation by hand: a memmove call per round was most of the hash's time */
		s[7] = s[6];
		s[6] = s[5];
		s[5] = s[4];
		s[4] = s[3];
		s[3] = s[2];
		s[2] = s[1];
		s[1] = s[0];
		s[4] += t1;
		s[0] = t1 + t2;
	}
	for (unsigned i = 0; i < 8; i++) {
		c->h[i] += s[i];
	}
	mgseal_wipe(w, sizeof(w));
	mgseal_wipe(s, sizeof(s));
}

static void sha_init(sha_t *c)
{
	static const uint32_t IV[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
				       0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

	memcpy(c->h, IV, sizeof(IV));
	c->bytes = 0;
	c->fill = 0;
}

static void sha_update(sha_t *c, const uint8_t *p, size_t n)
{
	c->bytes += n;
	while (n) {
		size_t take = 64u - c->fill;

		if (take > n) {
			take = n;
		}
		memcpy(c->block + c->fill, p, take);
		c->fill += take;
		p += take;
		n -= take;
		if (c->fill == 64u) {
			sha_block(c, c->block);
			c->fill = 0;
		}
	}
}

static void sha_final(sha_t *c, uint8_t out[32])
{
	uint64_t bits = c->bytes * 8u;
	uint8_t pad = 0x80, len[8];

	sha_update(c, &pad, 1);
	pad = 0;
	while (c->fill != 56u) {
		sha_update(c, &pad, 1);
	}
	for (unsigned i = 0; i < 8; i++) {
		len[i] = (uint8_t)(bits >> (56u - 8u * i));
	}
	sha_update(c, len, 8);
	for (unsigned i = 0; i < 8; i++) {
		be32(out + 4 * i, c->h[i]);
	}
	mgseal_wipe(c, sizeof(*c));
}
#endif /* MGSEAL_EXT_SHA */

/* HMAC with a 32-byte key: inner = H((key ^ ipad) || msg), outer =
 * H((key ^ opad) || inner). */
static void key_pad(uint8_t pad[64], const mgseal_keys_t *keys, uint8_t x)
{
	for (unsigned i = 0; i < 64; i++) {
		pad[i] = (uint8_t)((i < MGSEAL_KEY_LEN ? keys->mac[i] : 0) ^ x);
	}
}

int mgseal_tag_begin(mgseal_tag_t *t, const mgseal_keys_t *keys, uint8_t dir, uint32_t counter,
		     uint32_t len)
{
	uint8_t pad[64], hdr[8];

	key_pad(pad, keys, 0x36);
	sha_init(&t->sha);
	sha_update(&t->sha, pad, sizeof(pad));
	sha_update(&t->sha, (const uint8_t *)TAGL, sizeof(TAGL) - 1);
	sha_update(&t->sha, &dir, 1);
	be32(hdr, counter);
	be32(hdr + 4, len);
	sha_update(&t->sha, hdr, sizeof(hdr));
	mgseal_wipe(pad, sizeof(pad));
	return SHA_FAILED(t->sha) ? MGSEAL_E_CRYPTO : MGSEAL_OK;
}

void mgseal_tag_update(mgseal_tag_t *t, const uint8_t *ct, size_t n)
{
	sha_update(&t->sha, ct, n);
}

/* The tag (first MGSEAL_TAG_LEN bytes of out) from the inner hash; t wiped. */
static int tag_final(mgseal_tag_t *t, const mgseal_keys_t *keys, uint8_t out[32])
{
	uint8_t pad[64];
	int r = MGSEAL_OK;

	sha_final(&t->sha, out);
	if (SHA_FAILED(t->sha)) {
		r = MGSEAL_E_CRYPTO; /* the outer hash's init starts err again */
	}
	key_pad(pad, keys, 0x5c);
	sha_init(&t->sha);
	sha_update(&t->sha, pad, sizeof(pad));
	sha_update(&t->sha, out, 32);
	sha_final(&t->sha, out);
	if (SHA_FAILED(t->sha)) {
		r = MGSEAL_E_CRYPTO;
	}
	mgseal_wipe(pad, sizeof(pad));
	mgseal_wipe(t, sizeof(*t));
	return r;
}

int mgseal_tag_check(mgseal_tag_t *t, const mgseal_keys_t *keys, const uint8_t tag[MGSEAL_TAG_LEN])
{
	uint8_t out[32];
	int r = tag_final(t, keys, out);

	if (r == MGSEAL_OK && !mglsc_ct_equal(out, tag, MGSEAL_TAG_LEN)) {
		r = MGSEAL_E_TAG;
	}
	mgseal_wipe(out, sizeof(out));
	return r;
}

void mgseal_tag_abort(mgseal_tag_t *t)
{
#if MGSEAL_EXT_SHA
	uint8_t out[32];

	sha_final(&t->sha, out); /* final releases the backend's operation, on failure too */
	mgseal_wipe(out, sizeof(out));
#endif
	mgseal_wipe(t, sizeof(*t));
}

int mgseal_tag_stream(const mgseal_keys_t *keys, uint8_t dir, uint32_t counter, uint32_t len,
		      mgseal_read_fn read, void *ctx, uint8_t tag[MGSEAL_TAG_LEN])
{
	mgseal_tag_t t;
	uint8_t chunk[32], out[32];
	int r = mgseal_tag_begin(&t, keys, dir, counter, len);

	for (uint32_t off = 0; off < len && r == 0; off += 32u) {
		size_t n = len - off < 32u ? len - off : 32u;

		r = read(ctx, off, chunk, n) != 0 ? MGSEAL_E_CRYPTO : MGSEAL_OK;
		if (r == MGSEAL_OK) {
			r = mgseal_xor_at(keys, dir, counter, off, chunk, n);
		}
		if (r == MGSEAL_OK) {
			mgseal_tag_update(&t, chunk, n);
		}
	}
	if (tag_final(&t, keys, out) != MGSEAL_OK && r == MGSEAL_OK) {
		r = MGSEAL_E_CRYPTO;
	}
	if (r == MGSEAL_OK) {
		memcpy(tag, out, MGSEAL_TAG_LEN);
	}
	mgseal_wipe(chunk, sizeof(chunk));
	mgseal_wipe(out, sizeof(out));
	return r;
}

int mgseal_saved_plaintext(uint8_t out[MGSEAL_SAVED_LEN], bool saved, const uint8_t *refresh,
			   size_t refresh_len)
{
	sha_t c;
	uint8_t h[32];
	int r = MGSEAL_OK;

	memset(out, 0, MGSEAL_SAVED_LEN);
	if (!saved) {
		return MGSEAL_OK;
	}
	sha_init(&c);
	sha_update(&c, refresh, refresh_len);
	sha_final(&c, h);
	if (SHA_FAILED(c)) {
		r = MGSEAL_E_CRYPTO; /* not saved, as far as the client can tell */
	} else {
		out[0] = 1;
		memcpy(&out[1], h, MGSEAL_SAVED_HASH_LEN);
	}
	mgseal_wipe(h, sizeof(h));
	mgseal_wipe(&c, sizeof(c));
	return r;
}
#endif
