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
/* Host tests for portable/mg_rng (tests/run.sh): the HMAC-DRBG against NIST CAVP's HMAC_DRBG SHA-256 vector, the
 * health tests' cutoffs and trips, the startup test, the cold (factory) seed, the warm (seeded-record) boot, the
 * forward writes (before the first output; coalesced after outputs), a failed store at every step, a failed
 * conditioning, a corrupt record, pull ports, and the reseed. The core is included so its static DRBG can be driven
 * directly. */
#include "mgls_crypto.h"

/* The core's HMAC, failing on demand (a crypto backend error). */
static int hmac_fail;
static int t_hmac(const uint8_t *key, size_t kl, const uint8_t *const parts[], const size_t lens[], int n,
		  uint8_t out[32]);
#define mglsc_hmac_sha256 t_hmac
#include "../src/mg_rng.c"
#undef mglsc_hmac_sha256

static int t_hmac(const uint8_t *key, size_t kl, const uint8_t *const parts[], const size_t lens[], int n,
		  uint8_t out[32])
{
	if (hmac_fail > 0) {
		hmac_fail--;
		return -1;
	}
	return mglsc_hmac_sha256(key, kl, parts, lens, n, out);
}

#include <stdio.h>
#include <stdlib.h>

static int checks, fails;
#define CHECK(c)                                                                     \
	do {                                                                          \
		checks++;                                                             \
		if (!(c)) {                                                           \
			fails++;                                                      \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
		}                                                                     \
	} while (0)

static void hex(const char *s, uint8_t *out, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned v;
		sscanf(s + 2 * i, "%2x", &v);
		out[i] = (uint8_t)v;
	}
}

/* ---- the board: a RAM record, a failing store on demand, a noise source ---- */
typedef struct {
	uint8_t rec[MGRNG_REC_LEN];
	int have, stores, fail_store, corrupt_readback;
	uint32_t x;                     /* xorshift state: the "noise" */
	int pull_left;
} board_t;

static int b_load(void *ctx, uint8_t rec[MGRNG_REC_LEN])
{
	board_t *b = ctx;
	if (!b->have) {
		return -1;
	}
	memcpy(rec, b->rec, MGRNG_REC_LEN);
	return 0;
}

static int b_store(void *ctx, const uint8_t rec[MGRNG_REC_LEN])
{
	board_t *b = ctx;
	b->stores++;
	if (b->fail_store) {
		return -1;
	}
	memcpy(b->rec, rec, MGRNG_REC_LEN);
	b->have = 1;
	if (b->corrupt_readback) {
		return -1;                  /* the read-back did not match */
	}
	return 0;
}

static uint32_t noise(board_t *b)
{
	b->x ^= b->x << 13;
	b->x ^= b->x >> 17;
	b->x ^= b->x << 5;
	return b->x;
}

static int b_sample(void *ctx, uint32_t *raw)
{
	board_t *b = ctx;
	if (b->pull_left <= 0) {
		return -1;
	}
	b->pull_left--;
	*raw = noise(b);
	return 0;
}

static void feed(mgrng_t *r, board_t *b, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		mgrng_feed(r, noise(b));
	}
}

static mgrng_port_t port(board_t *b)
{
	mgrng_port_t p;
	memset(&p, 0, sizeof(p));
	p.ctx = b;
	p.pool_load = b_load;
	p.pool_store = b_store;
	p.write_gap_ms = 10000; /* the timings below; the default is an hour */
	return p;
}

/* NIST CAVP HMAC_DRBG.rsp, [SHA-256] [PredictionResistance = False] [EntropyInputLen = 256] [NonceLen = 128]
 * [PersonalizationStringLen = 0] [AdditionalInputLen = 0], COUNT = 0: instantiate, generate 1024 bits twice, the second
 * is ReturnedBits. */
static void cavp(void)
{
	mgrng_t r;
	mgrng_port_t p;
	board_t b;
	uint8_t ent[32], nonce[16], out[128], want[128];
	const uint8_t *data[2] = {ent, nonce};
	size_t dl[2] = {32, 16};

	memset(&b, 0, sizeof(b));
	p = port(&b);
	mgrng_init(&r, &p);
	hex("ca851911349384bffe89de1cbdc46e6831e44d34a4fb935ee285dd14b71a7488", ent, 32);
	hex("659ba96c601dc69fc902940805ec0ca8", nonce, 16);
	hex("e528e9abf2dece54d47c7e75e5fe302149f817ea9fb4bee6f4199697d04d5b89d54fbb978a15b5c443c9ec21036d2460b6f73eba"
	    "d0dc2aba6e624abf07745bc107694bb7547bb0995f70de25d6b29e2d3011bb19d27676c07162c8b5ccde0668961df86803482cb37ed"
	    "6d5c0bb8d50cf1f50d476aa0458bdaba806f48be9dcb8", want, 128);
	memset(r.k, 0x00, 32);
	memset(r.v, 0x01, 32);
	CHECK(drbg_update(&r, data, dl, 2) == MGRNG_OK);
	CHECK(drbg_generate(&r, out, sizeof(out)) == MGRNG_OK);
	CHECK(drbg_generate(&r, out, sizeof(out)) == MGRNG_OK);
	CHECK(!memcmp(out, want, sizeof(want)));
}

static void health_tests(void)
{
	board_t b;
	mgrng_port_t p;
	mgrng_t r;

	memset(&b, 0, sizeof(b));
	b.x = 1;
	p = port(&b);
	mgrng_init(&r, &p);
	CHECK(r.rct_cut == 41 && r.apt_cut == 410 && r.need == 384); /* H 0.5 (the placeholder), 192 bits */
	/* the repetition count: 40 equal in a row pass, the 41st trips and discards the counted samples */
	feed(&r, &b, MGRNG_STARTUP + 50);
	CHECK(r.cnt == 50 && r.st.rct_fails == 0);
	for (int i = 0; i < 40; i++) {
		mgrng_feed(&r, 7);
	}
	CHECK(r.st.rct_fails == 0 && r.cnt == 90);
	mgrng_feed(&r, 7);
	CHECK(r.st.rct_fails == 1 && r.cnt == 0 && r.need_clean == MGRNG_APT_WINDOW && r.clean == 0);
	feed(&r, &b, MGRNG_APT_WINDOW);                 /* one clean window, then counting again */
	CHECK(r.cnt == 0);
	feed(&r, &b, 3);
	CHECK(r.cnt == 3);
	/* a failure during the startup test restarts the whole startup test */
	mgrng_init(&r, &p);
	feed(&r, &b, 100);
	for (int i = 0; i < 41; i++) {
		mgrng_feed(&r, 7);
	}
	CHECK(r.st.rct_fails == 1 && r.need_clean == MGRNG_STARTUP && r.clean == 0);
	/* the adaptive proportion: 410 of a 512-sample window equal to its first trips */
	mgrng_init(&r, &p);
	for (int i = 0; i < 512; i++) {
		mgrng_feed(&r, (i % 5 == 4) ? noise(&b) : 9u);   /* the repetition test never trips: runs of 4 */
	}
	CHECK(r.st.apt_fails == 1 && r.st.rct_fails == 0);   /* 410 of 512 equal to the window's first: the cutoff */
	mgrng_init(&r, &p);
	int tripped_at = -1;
	for (int i = 0; i < 512 && tripped_at < 0; i++) {
		mgrng_feed(&r, (i % 2) ? noise(&b) : 9u);
		if (r.st.apt_fails) {
			tripped_at = i;
		}
	}
	CHECK(tripped_at < 0);                          /* 256 of 512: under the cutoff */
	mgrng_init(&r, &p);
	for (int i = 0; i < 512; i++) {
		mgrng_feed(&r, (i % 10 < 7) ? 9u : noise(&b)); /* 7 of every 10: 359 of 512, under the cutoff */
	}
	CHECK(r.st.apt_fails == 0 && r.st.rct_fails == 0);
	/* custom cutoffs (tests/cutoffs.py for H = 4: 6 and 62) */
	p.h_milli = 4000;
	p.rct_cutoff = 6;
	p.apt_cutoff = 62;
	mgrng_init(&r, &p);
	CHECK(r.need == 48);
	for (int i = 0; i < 6; i++) {
		mgrng_feed(&r, 3);
	}
	CHECK(r.st.rct_fails == 1);
}

/* A factory part: nothing until the startup test and the counted samples; then the record (flagged seeded) is stored
 * and read back before the first output; a failed store or read-back of that full counted seed owes the write and
 * output goes on; a crypto failure after seeding unseeds. */
static void cold(void)
{
	board_t b;
	mgrng_port_t p;
	mgrng_t r;
	uint8_t out[48], zero[48], before[48];

	memset(&b, 0, sizeof(b));
	memset(zero, 0, sizeof(zero));
	b.x = 0x1234567;
	p = port(&b);
	mgrng_init(&r, &p);
	memset(out, 0, sizeof(out));
	CHECK(mgrng_generate(&r, out, sizeof(out)) == MGRNG_E_NOT_READY && !memcmp(out, zero, sizeof(out)) && !b.stores);
	feed(&r, &b, MGRNG_STARTUP);                    /* the startup test: none counted */
	CHECK(r.cnt == 0 && mgrng_generate(&r, out, sizeof(out)) == MGRNG_E_NOT_READY);
	feed(&r, &b, 100);
	hmac_fail = 1;                                  /* a conditioning fails: its samples never reach the seed */
	feed(&r, &b, r.need - 100);                     /* a full count, the lost samples included */
	CHECK(!hmac_fail && mgrng_generate(&r, out, sizeof(out)) == MGRNG_E_NOT_READY && !b.stores);
	feed(&r, &b, r.need - 1 - r.cnt);
	CHECK(mgrng_generate(&r, out, sizeof(out)) == MGRNG_E_NOT_READY && !mgrng_ready(&r));
	feed(&r, &b, 1);
	/* the store fails, but this boot has its full counted seed: output goes on, the write is owed */
	b.fail_store = 1;
	CHECK(mgrng_generate(&r, out, sizeof(out)) == MGRNG_OK && memcmp(out, zero, sizeof(out)) && mgrng_ready(&r));
	CHECK(r.st.store_fails == 1 && (mgrng_flags(&r) & MGRNG_FLAG_WRITE_OWED) && !b.have);
	mgrng_tick(&r, 1000);                           /* retried at the next tick: fails again */
	CHECK(r.st.store_fails == 2 && (mgrng_flags(&r) & MGRNG_FLAG_WRITE_OWED) && !b.have);
	b.fail_store = 0;
	b.corrupt_readback = 1;
	mgrng_tick(&r, 5000);                           /* within the gap: not yet */
	CHECK(r.st.stores == 2);
	mgrng_tick(&r, 11000);                          /* written, but the read-back differs: still owed */
	CHECK(r.st.store_fails == 3 && (mgrng_flags(&r) & MGRNG_FLAG_WRITE_OWED));
	b.corrupt_readback = 0;
	mgrng_tick(&r, 21000);
	CHECK(!(mgrng_flags(&r) & MGRNG_FLAG_WRITE_OWED) && b.have && !memcmp(b.rec, "MGR1", 4) && (b.rec[4] & 1));
	CHECK((mgrng_flags(&r) & F_FULL) && r.st.seeds == 1);
	memcpy(before, out, sizeof(out));
	CHECK(mgrng_generate(&r, out, sizeof(out)) == MGRNG_OK && memcmp(out, before, sizeof(out)));
	/* the forward write after outputs: from the tick, at most once per write_gap_ms (10 s) */
	int s0 = b.stores;
	mgrng_tick(&r, 31000);
	CHECK(b.stores == s0 + 1);
	CHECK(mgrng_generate(&r, out, sizeof(out)) == MGRNG_OK);
	mgrng_tick(&r, 35000);
	CHECK(b.stores == s0 + 1);                      /* within the gap: coalesced */
	mgrng_tick(&r, 41000);
	CHECK(b.stores == s0 + 2);
	mgrng_tick(&r, 60000);
	CHECK(b.stores == s0 + 2);                      /* nothing owed */
	/* a failed forward write is retried after the gap */
	CHECK(mgrng_generate(&r, out, sizeof(out)) == MGRNG_OK);
	b.fail_store = 1;
	mgrng_tick(&r, 80000);
	CHECK(b.stores == s0 + 3 && (r.flags & F_DIRTY));
	b.fail_store = 0;
	mgrng_tick(&r, 85000);
	CHECK(b.stores == s0 + 3);
	mgrng_tick(&r, 90000);
	CHECK(b.stores == s0 + 4 && !(r.flags & F_DIRTY));
	/* a crypto failure after seeding: the output wiped, and none until a fresh full counted seed */
	hmac_fail = 1;
	CHECK(mgrng_generate(&r, out, sizeof(out)) == MGRNG_E_CRYPTO && !memcmp(out, zero, sizeof(out)) &&
	      !mgrng_ready(&r));
	CHECK(mgrng_generate(&r, out, sizeof(out)) == MGRNG_E_NOT_READY);
	feed(&r, &b, r.need);
	CHECK(mgrng_generate(&r, out, sizeof(out)) == MGRNG_OK && mgrng_ready(&r) && (mgrng_flags(&r) & F_FULL));
}

/* A seeded record: output right after the boot's forward write, no samples needed; each boot starts from the pool the
 * one before it wrote, so two boots with no noise at all still differ; a failed boot write refuses. */
static void warm(void)
{
	board_t b;
	mgrng_port_t p;
	mgrng_t r;
	uint8_t a[32], c[32], rec0[MGRNG_REC_LEN];

	memset(&b, 0, sizeof(b));
	b.x = 99;
	p = port(&b);
	mgrng_init(&r, &p);
	feed(&r, &b, MGRNG_STARTUP + r.need);
	CHECK(mgrng_generate(&r, a, sizeof(a)) == MGRNG_OK);    /* the factory seed */
	memcpy(rec0, b.rec, sizeof(rec0));
	/* boot 2: no samples at all */
	mgrng_init(&r, &p);
	int s0 = b.stores;
	CHECK(mgrng_generate(&r, a, sizeof(a)) == MGRNG_OK && b.stores == s0 + 1);
	CHECK(memcmp(b.rec, rec0, sizeof(rec0)) && (b.rec[4] & 1) && !(mgrng_flags(&r) & F_FULL));
	/* that boot write starts the write gap: the forward after the output waits for it, not for the first tick */
	mgrng_tick(&r, 1000);
	CHECK(b.stores == s0 + 1 && (mgrng_flags(&r) & MGRNG_FLAG_WRITE_OWED));
	mgrng_tick(&r, 11000);
	CHECK(b.stores == s0 + 2 && !(mgrng_flags(&r) & MGRNG_FLAG_WRITE_OWED));
	/* boot 3, also with no samples: a different pool, so a different output */
	mgrng_init(&r, &p);
	CHECK(mgrng_generate(&r, c, sizeof(c)) == MGRNG_OK && memcmp(a, c, sizeof(a)));
	/* replaying boot 2's record gives boot 2's output (the pool is the only input here) ... */
	memcpy(b.rec, rec0, sizeof(rec0));
	mgrng_init(&r, &p);
	CHECK(mgrng_generate(&r, c, sizeof(c)) == MGRNG_OK && !memcmp(a, c, sizeof(a)));
	/* ... which is why a boot whose output would rest on the pool (no counted seed yet) outputs nothing when its
	 * forward write fails */
	mgrng_init(&r, &p);
	b.fail_store = 1;
	memset(c, 0, sizeof(c));
	CHECK(mgrng_generate(&r, c, sizeof(c)) == MGRNG_E_POOL && !mgrng_ready(&r) && r.st.store_fails == 1);
	for (size_t i = 0; i < sizeof(c); i++) {
		CHECK(c[i] == 0);
	}
	/* a seeded record and the full count already in at the first call: output rests on the counted seed, so a failed
	 * write is owed, not refused */
	mgrng_init(&r, &p);
	feed(&r, &b, MGRNG_STARTUP + r.need);
	CHECK(mgrng_generate(&r, c, sizeof(c)) == MGRNG_OK && (mgrng_flags(&r) & MGRNG_FLAG_WRITE_OWED) &&
	      (mgrng_flags(&r) & F_FULL));
	b.fail_store = 0;
	mgrng_tick(&r, 0);
	CHECK(!(mgrng_flags(&r) & MGRNG_FLAG_WRITE_OWED) && (b.rec[4] & 1));
	/* the full reseed once the counted samples arrive (the startup test first) */
	mgrng_init(&r, &p);
	CHECK(mgrng_generate(&r, c, sizeof(c)) == MGRNG_OK && r.st.reseeds == 0);
	feed(&r, &b, MGRNG_STARTUP + r.need);
	CHECK(mgrng_generate(&r, c, sizeof(c)) == MGRNG_OK && r.st.reseeds == 1 && (mgrng_flags(&r) & F_FULL));
	feed(&r, &b, r.need);
	mgrng_tick(&r, 0);                              /* the tick reseeds too */
	CHECK(r.st.reseeds == 2 && r.cnt == 0);
	/* a warm seed takes the samples counted before it, so the next full reseed needs a whole new count */
	mgrng_init(&r, &p);
	feed(&r, &b, MGRNG_STARTUP + r.need / 2);
	CHECK(mgrng_generate(&r, c, sizeof(c)) == MGRNG_OK && r.cnt == 0 && !(mgrng_flags(&r) & F_FULL));
	feed(&r, &b, r.need - r.need / 2);
	CHECK(mgrng_generate(&r, c, sizeof(c)) == MGRNG_OK && r.st.reseeds == 0);
	feed(&r, &b, r.need / 2);
	CHECK(mgrng_generate(&r, c, sizeof(c)) == MGRNG_OK && r.st.reseeds == 1 && (mgrng_flags(&r) & F_FULL));
	/* a corrupt record is a factory part's: wait for the counted samples */
	b.rec[20] ^= 1;
	mgrng_init(&r, &p);
	CHECK(mgrng_generate(&r, c, sizeof(c)) == MGRNG_E_NOT_READY);
	b.rec[20] ^= 1;
	memcpy(b.rec, "XGR1", 4);
	mgrng_init(&r, &p);
	CHECK(mgrng_generate(&r, c, sizeof(c)) == MGRNG_E_NOT_READY);
}

/* A pull board (a TRNG): generate pulls up to pull_max samples a call. */
static void pull_port(void)
{
	board_t b;
	mgrng_port_t p;
	mgrng_t r;
	uint8_t out[16];
	int calls = 0;

	memset(&b, 0, sizeof(b));
	b.x = 5;
	b.pull_left = 100000;
	p = port(&b);
	p.sample = b_sample;
	p.pull_max = 256;
	mgrng_init(&r, &p);
	while (mgrng_generate(&r, out, sizeof(out)) == MGRNG_E_NOT_READY && calls < 100) {
		calls++;
	}
	CHECK(calls == 5);                              /* 1,024 + 384 samples at 256 a call: the 6th call seeds */
	CHECK(mgrng_ready(&r) && r.st.fed >= MGRNG_STARTUP + 384);
	/* a source that stops: still seeded, generate goes on */
	b.pull_left = 0;
	CHECK(mgrng_generate(&r, out, sizeof(out)) == MGRNG_OK);
}

static void wipe_and_args(void)
{
	board_t b;
	mgrng_port_t p;
	mgrng_t r;

	memset(&b, 0, sizeof(b));
	b.x = 3;
	p = port(&b);
	mgrng_init(&r, &p);
	CHECK(mgrng_generate(&r, NULL, 4) == MGRNG_E_ARG);
	feed(&r, &b, MGRNG_STARTUP + r.need);
	uint8_t o[8];
	CHECK(mgrng_generate(&r, o, sizeof(o)) == MGRNG_OK);
	mgrng_wipe(&r);
	CHECK(!mgrng_ready(&r) && r.p == &p && r.st.fed == 0);
}

int main(void)
{
#if defined(MGLS_CRYPTO_PSA)
	CHECK(mglsc_init(NULL, NULL) == 0);             /* PSA: psa_crypto_init (the builtin backend needs no RNG here) */
#endif
	cavp();
	health_tests();
	cold();
	warm();
	pull_port();
	wipe_and_args();
	printf("mg_rng: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
