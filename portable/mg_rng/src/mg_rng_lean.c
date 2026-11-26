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
/* mg_rng, the lean pull-only configuration (MGRNG_LEAN): see include/mg_rng.h. */
#include "mg_rng.h"

#include <string.h>

#include "mgls_crypto.h"

#if !MGRNG_LEAN
#error "src/mg_rng_lean.c is the MGRNG_LEAN build"
#endif

#define F_SEEDED     MGRNG_FLAG_SEEDED
#define F_OWED       MGRNG_FLAG_WRITE_OWED
#define REC_SEEDED   0x01u
#define DEF_H_MILLI  500u
#define DEF_RCT      41u
#define DEF_APT      410u
#define DEF_SEED     192u

static const uint8_t L_SEED[] = "mgrng lean seed v1";

typedef struct {                /* one seed's work, on the stack */
	uint8_t pool[32], acc[32], buf[64], rec[MGRNG_REC_LEN], ex[32];
	uint32_t rct_last, apt_ref, cnt, need;
	uint16_t rct_run, apt_i, apt_n, clean, need_clean;
	uint8_t buf_n;
} work_t;

/* HMAC(key, parts...): 0 or MGRNG_E_CRYPTO. */
static int hm(const uint8_t *key, size_t kl, const uint8_t *const parts[], const size_t lens[], int n, uint8_t out[32])
{
	return mglsc_hmac_sha256(key, kl, parts, lens, n, out) == 0 ? MGRNG_OK : MGRNG_E_CRYPTO;
}

/* HMAC(key, dom | ctr | i): the generator's blocks ('o'), its next key ('n'), the seed's pool ('p') and key ('k'). */
static int kdf(const uint8_t key[32], uint8_t dom, uint32_t ctr, uint32_t i, uint8_t out[32])
{
	uint8_t m[9];
	const uint8_t *parts[1] = {m};
	size_t lens[1] = {sizeof(m)};

	m[0] = dom;
	for (int b = 0; b < 4; b++) {
		m[1 + b] = (uint8_t)(ctr >> (8 * b));
		m[5 + b] = (uint8_t)(i >> (8 * b));
	}
	return hm(key, 32, parts, lens, 1, out);
}

static int cond(work_t *w)
{
	const uint8_t *parts[1] = {w->buf};
	size_t lens[1] = {w->buf_n};
	int e = w->buf_n ? hm(w->acc, 32, parts, lens, 1, w->acc) : MGRNG_OK;

	w->buf_n = 0;
	return e;
}

/* The record's check: the first 4 bytes of HMAC(pool, 'c' | "MGR1" | flags), as src/mg_rng.c. */
static int rec_check(const uint8_t *rec, uint8_t out[4])
{
	uint8_t d[32];
	int e = kdf(&rec[8], 'c', 0x3152474Du, rec[4], d);

	memcpy(out, d, 4);
	mglsc_wipe(d, sizeof(d));
	return e;
}

/* The seed: pull the startup test and the counted samples (health-tested, conditioned), then
 * seed = H(pool | samples | extra | pers | nonce); the next pool H(seed, 'p') stored and read back BEFORE the key
 * H(seed, 'k') exists. */
static int seed(mgrng_t *r)
{
	const mgrng_port_t *p = r->p;
	work_t w;
	uint8_t s[32], c[4];
	uint32_t raw = 0, h = p->h_milli ? p->h_milli : DEF_H_MILLI, bits = p->seed_bits ? p->seed_bits : DEF_SEED;
	uint32_t budget, pulled = 0;
	uint16_t rct_cut = p->rct_cutoff ? p->rct_cutoff : DEF_RCT, apt_cut = p->apt_cutoff ? p->apt_cutoff : DEF_APT;
	bool rec_seeded = false, full, owed = false;
	int e = MGRNG_OK;

	memset(&w, 0, sizeof(w));
	r->last = 0;
	w.need = ((bits < 128u ? 128u : bits) * 1000u + h - 1u) / h;
	w.need_clean = MGRNG_STARTUP;
	budget = p->pull_max ? p->pull_max : 4u * (MGRNG_STARTUP + w.need);
	if (p->pool_load(p->ctx, w.rec) == 0 && !memcmp(w.rec, "MGR1", 4) && rec_check(w.rec, c) == MGRNG_OK &&
	    mglsc_ct_equal(c, &w.rec[40], 4)) {
		memcpy(w.pool, &w.rec[8], 32);
		rec_seeded = (w.rec[4] & REC_SEEDED) != 0;
	}
	while (w.cnt < w.need && e == MGRNG_OK) {
		bool ok = true;

		if (pulled >= budget || !p->sample || p->sample(p->ctx, &raw) != 0) {
			r->last |= MGRNG_L_STOPPED;
			break;
		}
		pulled++;
		/* SP 800-90B 4.4: the repetition count and the adaptive proportion (window 512) */
		w.rct_run = (w.rct_run && raw == w.rct_last) ? (uint16_t)(w.rct_run + 1u) : 1u;
		w.rct_last = raw;
		if (w.rct_run >= rct_cut) {
			r->last |= MGRNG_L_RCT;
			ok = false;
		}
		if (w.apt_i == 0) {
			w.apt_ref = raw;
			w.apt_n = 1;
		} else if (raw == w.apt_ref) {
			w.apt_n++;
		}
		if (ok && w.apt_n >= apt_cut) {
			r->last |= MGRNG_L_APT;
			ok = false;
		}
		w.apt_i = (uint16_t)((w.apt_i + 1u) % MGRNG_APT_WINDOW);
		if (!ok) {                      /* the counted samples go; one clean window before counting again */
			w.cnt = 0;
			/* during the startup test, the whole startup test again */
			w.need_clean = w.need_clean == MGRNG_STARTUP && w.clean < MGRNG_STARTUP ? MGRNG_STARTUP : MGRNG_APT_WINDOW;
			w.clean = 0;
			w.rct_run = 0;
			w.apt_i = 0;
			continue;
		}
		for (int b = 0; b < 4; b++) {
			w.buf[w.buf_n++] = (uint8_t)(raw >> (8 * b));
		}
		if (w.buf_n == sizeof(w.buf)) {
			e = cond(&w);
		}
		if (w.clean < w.need_clean) {
			w.clean++;
		} else {
			w.cnt++;
		}
	}
	raw = 0;
	full = w.cnt >= w.need;
	if (e == MGRNG_OK && !full && !(rec_seeded && p->warm_without_noise)) {
		e = MGRNG_E_NOT_READY;
	}
	if (e == MGRNG_OK) {
		const uint8_t *parts[5] = {w.pool, w.acc, w.ex, p->pers, (const uint8_t *)&pulled};
		size_t lens[5] = {32, 32, 32, p->pers ? p->pers_len : 0, sizeof(pulled)};

		if (!full) {
			r->last |= MGRNG_L_WARM;
		}
		if (p->extra) {
			p->extra(p->ctx, w.ex);
		}
		e = cond(&w);
		if (e == MGRNG_OK) {
			e = hm(L_SEED, sizeof(L_SEED) - 1, parts, lens, 5, s);
		}
	}
	if (e == MGRNG_OK) {
		memset(w.rec, 0, sizeof(w.rec));
		memcpy(w.rec, "MGR1", 4);
		w.rec[4] = (full || rec_seeded) ? REC_SEEDED : 0u;
		e = kdf(s, 'p', 0, 0, &w.rec[8]);
		if (e == MGRNG_OK) {
			e = rec_check(w.rec, &w.rec[40]);
		}
		if (e == MGRNG_OK && p->pool_store(p->ctx, w.rec) != 0) {
			r->last |= MGRNG_L_STORE;
			if (full) {
				owed = true;    /* output rests on this boot's fresh counted seed: retried at the next call */
			} else {
				e = MGRNG_E_POOL; /* output would rest on the pool: nothing out */
			}
		}
	}
	if (e == MGRNG_OK) {
		e = kdf(s, 'k', 0, 0, r->key);
	}
	if (e == MGRNG_OK) {
		r->ctr = 0;
		r->flags = (uint8_t)((r->flags & ~F_OWED) | F_SEEDED | (owed ? F_OWED : 0u));
	} else {
		mglsc_wipe(r->key, sizeof(r->key));
	}
	mglsc_wipe(&w, sizeof(w));
	mglsc_wipe(s, sizeof(s));
	return e;
}

/* An owed record write (a failed store with a full counted seed): the pool HMAC(key, 'p' | ctr), flagged seeded,
 * stored and read back; the write stays owed if it fails again. The key is erased after this call's blocks, so the pool
 * can't be recomputed from a later state, and the blocks ('o') aren't derivable from it. */
static int retry_store(mgrng_t *r)
{
	uint8_t rec[MGRNG_REC_LEN];
	int e;

	memset(rec, 0, sizeof(rec));
	memcpy(rec, "MGR1", 4);
	rec[4] = REC_SEEDED;
	e = kdf(r->key, 'p', r->ctr, 0, &rec[8]);
	if (e == MGRNG_OK) {
		e = rec_check(rec, &rec[40]);
	}
	if (e == MGRNG_OK) {
		if (r->p->pool_store(r->p->ctx, rec) == 0) {
			r->flags &= (uint8_t)~F_OWED;
		} else {
			r->last |= MGRNG_L_STORE;
		}
	}
	mglsc_wipe(rec, sizeof(rec));
	return e;
}

void mgrng_init(mgrng_t *r, const mgrng_port_t *p)
{
	memset(r, 0, sizeof(*r));
	r->p = p;
}

int mgrng_generate(mgrng_t *r, uint8_t *out, size_t n)
{
	uint8_t blk[32];
	uint32_t i = 0;
	int e = MGRNG_OK;

	if (!out && n) {
		return MGRNG_E_ARG;
	}
	if ((r->flags & (F_SEEDED | F_OWED)) == (F_SEEDED | F_OWED) && r->ctr < MGRNG_RESEED_MAX) {
		e = retry_store(r);                         /* the write a failed store owes (never in the seeding call) */
	}
	if (e == MGRNG_OK && (!(r->flags & F_SEEDED) || r->ctr >= MGRNG_RESEED_MAX)) {
		r->flags &= (uint8_t)~F_SEEDED;
		e = seed(r);
	}
	while (e == MGRNG_OK && n > 0) {
		size_t m = n < 32u ? n : 32u;

		e = kdf(r->key, 'o', r->ctr, i++, blk);
		if (e == MGRNG_OK) {
			memcpy(out, blk, m);
			out += m;
			n -= m;
		}
	}
	if (e == MGRNG_OK) {
		e = kdf(r->key, 'n', r->ctr, 0, r->key);  /* the old key is gone: earlier outputs can't be recomputed */
		r->ctr++;
	}
	if (e != MGRNG_OK && (r->flags & F_SEEDED)) {
		r->flags &= (uint8_t)~F_SEEDED;           /* a crypto failure mid-way: reseed next time */
		mglsc_wipe(r->key, sizeof(r->key));
	}
	mglsc_wipe(blk, sizeof(blk));
	return e;
}

bool mgrng_ready(const mgrng_t *r)
{
	return (r->flags & F_SEEDED) != 0;
}

uint8_t mgrng_flags(const mgrng_t *r)
{
	return r->flags;
}

void mgrng_wipe(mgrng_t *r)
{
	const mgrng_port_t *p = r->p;

	mglsc_wipe(r, sizeof(*r));
	mgrng_init(r, p);
}
