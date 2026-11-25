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
/* Host tests for mg_rng's lean configuration (MGRNG_LEAN, tests/run.sh): known answers from tests/lean_model.py (the
 * stored record and the outputs for a record and a sample stream), the startup test and the count, health failures, a
 * failed store or read-back (nothing output), warm_without_noise, a corrupt record, the pull budget, key erasure, the
 * reseed interval, 44 bytes resident. */
#include "mg_rng.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mgls_crypto.h"
#include "lean_ka.h"

static int checks, fails;
#define CHECK(c)                                                                     \
	do {                                                                          \
		checks++;                                                             \
		if (!(c)) {                                                           \
			fails++;                                                      \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
		}                                                                     \
	} while (0)

typedef struct {
	uint8_t rec[MGRNG_REC_LEN];
	int have, stores, fail_store;
	uint32_t x, left, stuck_at, stuck_val;
	unsigned pulled;
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
	return 0;
}

static int b_sample(void *ctx, uint32_t *raw)
{
	board_t *b = ctx;
	if (!b->left) {
		return -1;
	}
	b->left--;
	b->pulled++;
	if (b->stuck_at && b->pulled >= b->stuck_at && b->pulled < b->stuck_at + 50) {
		*raw = b->stuck_val;            /* 50 equal in a row: the repetition test trips */
		return 0;
	}
	b->x ^= b->x << 13;
	b->x ^= b->x >> 17;
	b->x ^= b->x << 5;
	*raw = b->x;
	return 0;
}

static mgrng_port_t port(board_t *b)
{
	mgrng_port_t p;
	memset(&p, 0, sizeof(p));
	p.ctx = b;
	p.sample = b_sample;
	p.pool_load = b_load;
	p.pool_store = b_store;
	return p;
}

int main(void)
{
	board_t b;
	mgrng_port_t p;
	mgrng_t r;
	uint8_t out[48], zero[48], k0[32];

#if defined(MGLS_CRYPTO_PSA)
	CHECK(mglsc_init(NULL, NULL) == 0);
#endif
	memset(zero, 0, sizeof(zero));
	/* known answers: a seeded record and a varied stream */
	memset(&b, 0, sizeof(b));
	memcpy(b.rec, KA_REC_IN, sizeof(b.rec));
	b.have = 1;
	b.x = KA_SEED;
	b.left = 100000;
	p = port(&b);
	mgrng_init(&r, &p);
	CHECK(mgrng_generate(&r, out, 48) == MGRNG_OK && !memcmp(out, KA_OUT1, 48));
	CHECK(b.stores == 1 && !memcmp(b.rec, KA_REC_OUT, sizeof(b.rec)) && b.pulled == 1024 + 384 && r.last == 0);
	memcpy(k0, r.key, 32);
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_OK && !memcmp(out, KA_OUT2, 16));
	CHECK(memcmp(k0, r.key, 32) && b.stores == 1 && b.pulled == 1024 + 384);   /* key erased; one store a boot */
	/* a factory part: nothing until the startup test and the count; then the record flagged seeded */
	memset(&b, 0, sizeof(b));
	b.x = 77;
	b.left = 1024 + 383;
	p = port(&b);
	mgrng_init(&r, &p);
	memset(out, 0, sizeof(out));
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_E_NOT_READY && !memcmp(out, zero, 16) && !b.stores &&
	      (r.last & MGRNG_L_STOPPED) && !mgrng_ready(&r));
	b.left = 100000;
	b.pulled = 0;
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_OK && b.have && (b.rec[4] & 1) && mgrng_ready(&r));
	/* a failed store with this boot's full counted seed: output goes on, the write is owed and retried at the next
	 * call */
	mgrng_init(&r, &p);
	b.fail_store = 1;
	int s1 = b.stores;
	uint8_t recb[MGRNG_REC_LEN];
	memcpy(recb, b.rec, sizeof(recb));
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_OK && memcmp(out, zero, 16) && (r.last & MGRNG_L_STORE) &&
	      (mgrng_flags(&r) & MGRNG_FLAG_WRITE_OWED) && b.stores == s1 + 1);
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_OK && b.stores == s1 + 2 && (mgrng_flags(&r) & MGRNG_FLAG_WRITE_OWED));
	b.fail_store = 0;
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_OK && b.stores == s1 + 3 && !(mgrng_flags(&r) & MGRNG_FLAG_WRITE_OWED));
	CHECK(memcmp(b.rec, recb, sizeof(recb)) && (b.rec[4] & 1));     /* a new pool, flagged seeded */
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_OK && b.stores == s1 + 3);   /* nothing owed */
	/* output that would rest on the pool (a seeded record, no count, warm_without_noise): a failed store refuses */
	p.warm_without_noise = 1;
	b.left = 10;
	b.fail_store = 1;
	mgrng_init(&r, &p);
	memset(out, 0, sizeof(out));
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_E_POOL && !memcmp(out, zero, 16) && !mgrng_ready(&r) &&
	      !(mgrng_flags(&r) & MGRNG_FLAG_WRITE_OWED));
	b.fail_store = 0;
	p.warm_without_noise = 0;
	b.left = 100000;
	/* two boots from one record differ: each stores the next pool first */
	uint8_t a1[16], a2[16], rec1[MGRNG_REC_LEN];
	memcpy(rec1, b.rec, sizeof(rec1));
	mgrng_init(&r, &p);
	CHECK(mgrng_generate(&r, a1, 16) == MGRNG_OK && memcmp(b.rec, rec1, sizeof(rec1)));
	mgrng_init(&r, &p);
	CHECK(mgrng_generate(&r, a2, 16) == MGRNG_OK && memcmp(a1, a2, 16));
	/* a source that stops: a seeded record outputs only with warm_without_noise */
	b.left = 10;
	mgrng_init(&r, &p);
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_E_NOT_READY);
	p.warm_without_noise = 1;
	b.left = 10;
	mgrng_init(&r, &p);
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_OK && (r.last & MGRNG_L_WARM) && (b.rec[4] & 1));
	p.warm_without_noise = 0;
	/* a factory part never seeds without the count, warm flag or not */
	board_t f;
	memset(&f, 0, sizeof(f));
	f.x = 9;
	f.left = 500;
	mgrng_port_t pf = port(&f);
	pf.warm_without_noise = 1;
	mgrng_init(&r, &pf);
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_E_NOT_READY && !f.stores);
	/* a repetition failure discards the count: one clean window more */
	memset(&f, 0, sizeof(f));
	f.x = 11;
	f.left = 100000;
	f.stuck_at = 1200;
	f.stuck_val = 0x55;
	pf = port(&f);
	mgrng_init(&r, &pf);
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_OK && (r.last & MGRNG_L_RCT));
	CHECK(f.pulled == 1240 + 512 + 384);       /* trips at the 41st equal (1240), then a clean window, the count */
	/* the pull budget */
	memset(&f, 0, sizeof(f));
	f.x = 12;
	f.left = 100000;
	pf = port(&f);
	pf.pull_max = 1000;
	mgrng_init(&r, &pf);
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_E_NOT_READY && f.pulled == 1000);
	/* a corrupt record is a factory part's */
	b.rec[20] ^= 1;
	b.left = 10;
	mgrng_init(&r, &p);
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_E_NOT_READY);
	/* the reseed interval: a fresh seed (and store) at MGRNG_RESEED_MAX */
	b.rec[20] ^= 1;
	b.left = 100000;
	mgrng_init(&r, &p);
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_OK);
	int s0 = b.stores;
	r.ctr = MGRNG_RESEED_MAX;
	CHECK(mgrng_generate(&r, out, 16) == MGRNG_OK && b.stores == s0 + 1 && r.ctr == 1);
	CHECK(mgrng_generate(&r, NULL, 4) == MGRNG_E_ARG);
	mgrng_wipe(&r);
	CHECK(!mgrng_ready(&r) && r.p == &p);
	printf("mg_rng lean: %d checks, %d failures (mgrng_t %u bytes)\n", checks, fails, (unsigned)sizeof(mgrng_t));
	return fails ? 1 : 0;
}
