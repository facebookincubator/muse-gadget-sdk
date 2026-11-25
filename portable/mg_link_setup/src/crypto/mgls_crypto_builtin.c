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
 * The self-contained crypto backend: SHA-256 (FIPS 180-4), HMAC (RFC 2104),
 * HKDF (RFC 5869, one block), AES-256 encryption (FIPS 197) with GCM (SP
 * 800-38D, 96-bit nonces), and P-256 ECDH through micro-ecc. Sized for
 * small MCUs: no table over 256 bytes (the AES S-box and SHA-256's round
 * constants), GHASH bit-serial and branch-free, tags compared in constant
 * time. MGLS_AES_CT_SBOX computes the S-box instead of reading the table
 * (constant time on parts with a data cache; ~10x slower AES).
 */

#include <string.h>

#include "mgls_crypto.h"
#include "mgls_uecc_config.h"

#include "../../third_party/micro-ecc/uECC.h"

/* ---- Utilities ------------------------------------------------------------ */

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

static int (*g_rng)(void *ctx, uint8_t *out, size_t len);
static void *g_rng_ctx;

static int uecc_rng(uint8_t *dest, unsigned size)
{
	return g_rng != NULL && g_rng(g_rng_ctx, dest, size) == 0;
}

int mglsc_init(int (*rng)(void *ctx, uint8_t *out, size_t len), void *ctx)
{
	g_rng = rng;
	g_rng_ctx = ctx;
	uECC_set_rng(uecc_rng);
	return rng != NULL ? 0 : -1;
}

/* ---- SHA-256 ---------------------------------------------------------------- */

typedef mglsc_sha256_ctx_t sha256_ctx; /* mgls_crypto.h: the incremental API's context */

static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
	0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
	0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
	0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
	0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
	0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
	0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
	uint32_t w[16], v[8];

	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
		       ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
	}
	memcpy(v, c->h, sizeof(v));
	for (int t = 0; t < 64; t++) {
		uint32_t wt;

		if (t < 16) {
			wt = w[t];
		} else {
			uint32_t a = w[(t - 15) & 15], b = w[(t - 2) & 15];
			uint32_t s0 = ROR(a, 7) ^ ROR(a, 18) ^ (a >> 3);
			uint32_t s1 = ROR(b, 17) ^ ROR(b, 19) ^ (b >> 10);

			wt = w[t & 15] += s0 + w[(t - 7) & 15] + s1;
		}
		uint32_t e = v[4];
		uint32_t t1 = v[7] + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) +
			      ((e & v[5]) ^ (~e & v[6])) + K256[t] + wt;
		uint32_t a = v[0];
		uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) +
			      ((a & v[1]) ^ (a & v[2]) ^ (v[1] & v[2]));

		/* the rotation by hand: a memmove call per round was most of the hash's time */
		v[7] = v[6];
		v[6] = v[5];
		v[5] = v[4];
		v[4] = v[3];
		v[3] = v[2];
		v[2] = v[1];
		v[1] = v[0];
		v[4] += t1;
		v[0] = t1 + t2;
	}
	for (int i = 0; i < 8; i++) {
		c->h[i] += v[i];
	}
	mglsc_wipe(w, sizeof(w));
	mglsc_wipe(v, sizeof(v));
}

static void sha256_init(sha256_ctx *c)
{
	static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
				       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

	memcpy(c->h, iv, sizeof(iv));
	c->len_lo = c->len_hi = 0;
	c->n = 0;
}

static void sha256_update(sha256_ctx *c, const uint8_t *p, size_t len)
{
	uint32_t lo = c->len_lo + (uint32_t)len;

	c->len_hi += (lo < c->len_lo) + (uint32_t)((uint64_t)len >> 32);
	c->len_lo = lo;
	while (len > 0) {
		if (c->n == 0 && len >= 64) {
			sha256_block(c, p);
			p += 64;
			len -= 64;
			continue;
		}
		size_t take = 64 - c->n;

		if (take > len) {
			take = len;
		}
		memcpy(&c->buf[c->n], p, take);
		c->n += (uint32_t)take;
		p += take;
		len -= take;
		if (c->n == 64) {
			sha256_block(c, c->buf);
			c->n = 0;
		}
	}
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
	uint32_t hi = (c->len_hi << 3) | (c->len_lo >> 29), lo = c->len_lo << 3;
	static const uint8_t pad = 0x80;
	uint8_t zero[8] = {0};

	/* 0x80, zeros up to 56 mod 64, then the bit length (big-endian). */
	sha256_update(c, &pad, 1);
	while (c->n != 56) {
		sha256_update(c, zero, 1);
	}
	put_be32(zero, hi);
	put_be32(&zero[4], lo);
	sha256_update(c, zero, 8);
	for (int i = 0; i < 8; i++) {
		put_be32(&out[4 * i], c->h[i]);
	}
	mglsc_wipe(c, sizeof(*c));
}

int mglsc_sha256_init(mglsc_sha256_ctx_t *ctx)
{
	sha256_init(ctx);
	return 0;
}

int mglsc_sha256_update(mglsc_sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
	sha256_update(ctx, data, len);
	return 0;
}

int mglsc_sha256_final(mglsc_sha256_ctx_t *ctx, uint8_t out[MGLSC_HASH_LEN])
{
	sha256_final(ctx, out);
	return 0;
}

int mglsc_sha256(const uint8_t *data, size_t len, uint8_t out[MGLSC_HASH_LEN])
{
	sha256_ctx c;

	sha256_init(&c);
	sha256_update(&c, data, len);
	sha256_final(&c, out);
	return 0;
}

/* ---- HMAC / HKDF ------------------------------------------------------------ */

int mglsc_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *const parts[],
		      const size_t lens[], int n, uint8_t out[MGLSC_HASH_LEN])
{
	uint8_t kb[64];
	sha256_ctx c;

	memset(kb, 0, sizeof(kb));
	if (key_len > 64) {
		(void)mglsc_sha256(key, key_len, kb);
	} else if (key_len > 0) {
		memcpy(kb, key, key_len);
	}
	for (int i = 0; i < 64; i++) {
		kb[i] ^= 0x36;
	}
	sha256_init(&c);
	sha256_update(&c, kb, 64);
	for (int i = 0; i < n; i++) {
		if (lens[i]) {
			sha256_update(&c, parts[i], lens[i]);
		}
	}
	sha256_final(&c, out);
	for (int i = 0; i < 64; i++) {
		kb[i] ^= 0x36 ^ 0x5c;
	}
	sha256_init(&c);
	sha256_update(&c, kb, 64);
	sha256_update(&c, out, 32);
	sha256_final(&c, out);
	mglsc_wipe(kb, sizeof(kb));
	return 0;
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

/* ---- AES-256 (encryption only: GCM needs nothing else) ---------------------- */

static uint8_t xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ (0x1b & (0u - (x >> 7))));
}

#if defined(MGLS_AES_CT_SBOX)
static uint8_t gmul(uint8_t a, uint8_t b)
{
	uint8_t p = 0;

	for (int i = 0; i < 8; i++) {
		p ^= (uint8_t)(a & (0u - (b & 1u)));
		b >>= 1;
		a = xtime(a);
	}
	return p;
}

/* S(x) = affine(x^254), no table, no secret-dependent branch or index. */
static uint8_t sbox(uint8_t x)
{
	uint8_t x2 = gmul(x, x), x3 = gmul(x2, x), x6 = gmul(x3, x3), x12 = gmul(x6, x6);
	uint8_t x15 = gmul(x12, x3), x30 = gmul(x15, x15), x60 = gmul(x30, x30);
	uint8_t x120 = gmul(x60, x60), x126 = gmul(x120, x6), x252 = gmul(x126, x126);
	uint8_t b = gmul(x252, x2);

	return (uint8_t)(b ^ ((b << 1) | (b >> 7)) ^ ((b << 2) | (b >> 6)) ^
			 ((b << 3) | (b >> 5)) ^ ((b << 4) | (b >> 4)) ^ 0x63);
}
#else
static const uint8_t SBOX[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab,
	0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4,
	0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71,
	0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
	0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6,
	0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb,
	0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45,
	0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
	0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44,
	0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a,
	0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49,
	0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
	0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25,
	0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e,
	0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1,
	0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb,
	0x16,
};

static uint8_t sbox(uint8_t x)
{
	return SBOX[x];
}
#endif

/* 15 round keys. */
static void aes256_expand(const uint8_t key[32], uint8_t rk[240])
{
	uint8_t rcon = 1;

	memcpy(rk, key, 32);
	for (int i = 32; i < 240; i += 4) {
		uint8_t t0 = rk[i - 4], t1 = rk[i - 3], t2 = rk[i - 2], t3 = rk[i - 1];

		if (i % 32 == 0) {
			uint8_t u = t0;

			t0 = (uint8_t)(sbox(t1) ^ rcon);
			t1 = sbox(t2);
			t2 = sbox(t3);
			t3 = sbox(u);
			rcon = xtime(rcon);
		} else if (i % 32 == 16) {
			t0 = sbox(t0);
			t1 = sbox(t1);
			t2 = sbox(t2);
			t3 = sbox(t3);
		}
		rk[i] = rk[i - 32] ^ t0;
		rk[i + 1] = rk[i - 31] ^ t1;
		rk[i + 2] = rk[i - 30] ^ t2;
		rk[i + 3] = rk[i - 29] ^ t3;
	}
}

static void aes256_encrypt(const uint8_t rk[240], const uint8_t in[16], uint8_t out[16])
{
	uint8_t s[16], t[16];

	for (int i = 0; i < 16; i++) {
		s[i] = in[i] ^ rk[i];
	}
	for (int r = 1; r <= 14; r++) {
		/* SubBytes and ShiftRows: row j of column c comes from column
		 * c + j. */
		for (int c = 0; c < 4; c++) {
			for (int j = 0; j < 4; j++) {
				t[c * 4 + j] = sbox(s[((c + j) & 3) * 4 + j]);
			}
		}
		if (r < 14) {
			for (int c = 0; c < 4; c++) {
				uint8_t *a = &t[c * 4];
				uint8_t all = a[0] ^ a[1] ^ a[2] ^ a[3], a0 = a[0];

				a[0] ^= all ^ xtime(a[0] ^ a[1]);
				a[1] ^= all ^ xtime(a[1] ^ a[2]);
				a[2] ^= all ^ xtime(a[2] ^ a[3]);
				a[3] ^= all ^ xtime(a[3] ^ a0);
			}
		}
		for (int i = 0; i < 16; i++) {
			s[i] = t[i] ^ rk[r * 16 + i];
		}
	}
	memcpy(out, s, 16);
	mglsc_wipe(s, sizeof(s));
	mglsc_wipe(t, sizeof(t));
}

/* ---- GCM -------------------------------------------------------------------- */

typedef struct {
	uint8_t rk[240];
	uint8_t h[16];
	uint8_t y[16];  /* GHASH accumulator */
	uint8_t j0[16]; /* the pre-counter block */
} gcm_ctx;

/* x = x * h in GF(2^128) (SP 800-38D algorithm 1), bit-serial, branch-free. */
static void gf_mult(uint8_t x[16], const uint8_t h[16])
{
	uint32_t z[4] = {0, 0, 0, 0}, v[4];

	for (int i = 0; i < 4; i++) {
		v[i] = ((uint32_t)h[4 * i] << 24) | ((uint32_t)h[4 * i + 1] << 16) |
		       ((uint32_t)h[4 * i + 2] << 8) | h[4 * i + 3];
	}
	for (int i = 0; i < 128; i++) {
		uint32_t m = 0u - (uint32_t)((x[i >> 3] >> (7 - (i & 7))) & 1u);
		uint32_t lsb = 0u - (v[3] & 1u);

		z[0] ^= v[0] & m;
		z[1] ^= v[1] & m;
		z[2] ^= v[2] & m;
		z[3] ^= v[3] & m;
		v[3] = (v[3] >> 1) | (v[2] << 31);
		v[2] = (v[2] >> 1) | (v[1] << 31);
		v[1] = (v[1] >> 1) | (v[0] << 31);
		v[0] = (v[0] >> 1) ^ (0xE1000000u & lsb);
	}
	for (int i = 0; i < 4; i++) {
		x[4 * i] = (uint8_t)(z[i] >> 24);
		x[4 * i + 1] = (uint8_t)(z[i] >> 16);
		x[4 * i + 2] = (uint8_t)(z[i] >> 8);
		x[4 * i + 3] = (uint8_t)z[i];
	}
	mglsc_wipe(z, sizeof(z));
	mglsc_wipe(v, sizeof(v));
}

static void ghash(gcm_ctx *g, const uint8_t *p, size_t len)
{
	while (len > 0) {
		size_t n = len < 16 ? len : 16;

		for (size_t i = 0; i < n; i++) {
			g->y[i] ^= p[i];
		}
		gf_mult(g->y, g->h);
		p += n;
		len -= n;
	}
}

static void gcm_start(gcm_ctx *g, const uint8_t key[32], const uint8_t nonce[12],
		      const uint8_t *aad, size_t aad_len)
{
	memset(g, 0, sizeof(*g));
	aes256_expand(key, g->rk);
	aes256_encrypt(g->rk, g->y, g->h); /* H = E(0^128); y is still zero */
	memcpy(g->j0, nonce, 12);
	g->j0[15] = 1;
	ghash(g, aad, aad_len);
}

/* CTR from inc32(J0). */
static void gcm_ctr(gcm_ctx *g, uint8_t *buf, size_t len)
{
	uint8_t ctr[16], ks[16];
	uint32_t c;

	memcpy(ctr, g->j0, 16);
	c = ((uint32_t)ctr[12] << 24) | ((uint32_t)ctr[13] << 16) | ((uint32_t)ctr[14] << 8) |
	    ctr[15];
	while (len > 0) {
		size_t n = len < 16 ? len : 16;

		c++;
		ctr[12] = (uint8_t)(c >> 24);
		ctr[13] = (uint8_t)(c >> 16);
		ctr[14] = (uint8_t)(c >> 8);
		ctr[15] = (uint8_t)c;
		aes256_encrypt(g->rk, ctr, ks);
		for (size_t i = 0; i < n; i++) {
			buf[i] ^= ks[i];
		}
		buf += n;
		len -= n;
	}
	mglsc_wipe(ks, sizeof(ks));
}

static void gcm_tag(gcm_ctx *g, size_t aad_len, size_t len, uint8_t tag[16])
{
	uint8_t lb[16];
	uint64_t ab = (uint64_t)aad_len * 8, cb = (uint64_t)len * 8;

	for (int i = 0; i < 8; i++) {
		lb[i] = (uint8_t)(ab >> (56 - 8 * i));
		lb[8 + i] = (uint8_t)(cb >> (56 - 8 * i));
	}
	ghash(g, lb, 16);
	aes256_encrypt(g->rk, g->j0, tag);
	for (int i = 0; i < 16; i++) {
		tag[i] ^= g->y[i];
	}
}

int mglsc_gcm_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
		   size_t aad_len, uint8_t *buf, size_t len)
{
	gcm_ctx g;

	gcm_start(&g, key, nonce, aad, aad_len);
	gcm_ctr(&g, buf, len);
	ghash(&g, buf, len);
	gcm_tag(&g, aad_len, len, &buf[len]);
	mglsc_wipe(&g, sizeof(g));
	return 0;
}

int mglsc_gcm_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
		   size_t aad_len, uint8_t *buf, size_t len)
{
	gcm_ctx g;
	uint8_t tag[16];
	bool ok;

	gcm_start(&g, key, nonce, aad, aad_len);
	ghash(&g, buf, len);
	gcm_tag(&g, aad_len, len, tag);
	ok = mglsc_ct_equal(tag, &buf[len], 16);
	if (ok) {
		gcm_ctr(&g, buf, len);
	}
	mglsc_wipe(&g, sizeof(g));
	mglsc_wipe(tag, sizeof(tag));
	return ok ? 0 : -1;
}

/* ---- P-256 (micro-ecc) ------------------------------------------------------ */

int mglsc_p256_keygen(mglsc_p256_t *key, uint8_t pub[65])
{
	if (!uECC_make_key(&pub[1], key->d, uECC_secp256r1())) {
		mglsc_wipe(key, sizeof(*key));
		return -1;
	}
	pub[0] = 0x04;
	return 0;
}

int mglsc_p256_import(mglsc_p256_t *key, const uint8_t priv[32], uint8_t pub[65])
{
	memcpy(key->d, priv, 32);
	if (!uECC_compute_public_key(key->d, &pub[1], uECC_secp256r1())) {
		mglsc_wipe(key, sizeof(*key));
		return -1;
	}
	pub[0] = 0x04;
	return 0;
}

int mglsc_p256_ecdh(mglsc_p256_t *key, const uint8_t peer[65], uint8_t ss[32])
{
	if (peer[0] != 0x04 || !uECC_valid_public_key(&peer[1], uECC_secp256r1()) ||
	    !uECC_shared_secret(&peer[1], key->d, ss, uECC_secp256r1())) {
		mglsc_wipe(ss, 32);
		return -1;
	}
	return 0;
}

void mglsc_p256_free(mglsc_p256_t *key)
{
	mglsc_wipe(key, sizeof(*key));
}
