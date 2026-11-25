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
/* mg_rng: see include/mg_rng.h. */
#include "mg_rng.h"

#if MGRNG_LEAN
#error "MGRNG_LEAN builds src/mg_rng_lean.c"
#endif

#include <string.h>

#include "mgls_crypto.h"

#define F_LOADED     0x01u /* the record was read (or found absent) */
#define F_REC_SEEDED MGRNG_FLAG_REC_SEEDED
#define F_SEEDED     MGRNG_FLAG_SEEDED
#define F_FULL       MGRNG_FLAG_FULL
#define F_DIRTY      MGRNG_FLAG_WRITE_OWED
#define F_STORED     0x20u /* last_store is valid */
#define F_BOOT_W     0x40u /* the boot's first seed wrote the pool: the next tick starts the write gap from it */

#define REC_SEEDED   0x01u /* the record's flags byte */
#define DEF_H_MILLI  500u  /* the placeholder: 0.5 bit per raw sample */
#define DEF_RCT      41u   /* tests/cutoffs.py at H = 0.5, alpha 2^-20 */
#define DEF_APT      410u
#define DEF_SEED     192u
#define DEF_PULL     64u
#define DEF_GAP_MS   3600000u

static const uint8_t L_SEED[] = "mgrng seed v1";

static int hmac(const uint8_t *key, size_t kl, const uint8_t *const parts[], const size_t lens[], int n,
		uint8_t out[32])
{
	return mglsc_hmac_sha256(key, kl, parts, lens, n, out) == 0 ? MGRNG_OK : MGRNG_E_CRYPTO;
}

/* ---- HMAC-DRBG (SP 800-90A 10.1.2, SHA-256) ---- */

/* K = HMAC(K, V | 0x00 | data); V = HMAC(K, V); and again with 0x01 if data. */
static int drbg_update(mgrng_t *r, const uint8_t *const data[], const size_t dl[], int dn)
{
	const uint8_t zo[2] = {0x00, 0x01};   /* on the stack: a small static constant is RAM (.srodata) on some boards */
	const uint8_t *parts[6];
	size_t lens[6];
	size_t total = 0;
	int e = MGRNG_OK;

	for (int i = 0; i < dn; i++) {
		total += dl[i];
	}
	for (int round = 0; round < (total ? 2 : 1) && e == MGRNG_OK; round++) {
		parts[0] = r->v;
		lens[0] = 32;
		parts[1] = &zo[round];
		lens[1] = 1;
		for (int i = 0; i < dn; i++) {
			parts[2 + i] = data[i];
			lens[2 + i] = dl[i];
		}
		e = hmac(r->k, 32, parts, lens, 2 + dn, r->k);
		if (e == MGRNG_OK) {
			parts[0] = r->v;
			lens[0] = 32;
			e = hmac(r->k, 32, parts, lens, 1, r->v);
		}
	}
	return e;
}

/* Instantiate: the seed material is H(pool | samples | extra | nonce), then the personalization (SP 800-90A 10.1.2.3:
 * entropy | nonce | personalization; the nonce went into the conditioning). */
static int drbg_instantiate(mgrng_t *r, const uint8_t seed_material[32])
{
	const mgrng_port_t *p = r->p;
	const uint8_t *data[2] = {seed_material, p->pers};
	size_t dl[2] = {32, p->pers ? p->pers_len : 0};

	memset(r->k, 0x00, sizeof(r->k));
	memset(r->v, 0x01, sizeof(r->v));
	r->gen_count = 0;
	return drbg_update(r, data, dl, 2);
}

static int drbg_generate(mgrng_t *r, uint8_t *out, size_t n)
{
	const uint8_t *parts[1] = {r->v};
	size_t lens[1] = {32};
	int e = MGRNG_OK;

	while (n > 0 && e == MGRNG_OK) {
		size_t m = n < 32u ? n : 32u;

		e = hmac(r->k, 32, parts, lens, 1, r->v);
		if (e == MGRNG_OK) {
			memcpy(out, r->v, m);
			out += m;
			n -= m;
		}
	}
	if (e == MGRNG_OK) {
		e = drbg_update(r, NULL, NULL, 0);
	}
	r->gen_count++;
	return e;
}

/* ---- the conditioning: acc = HMAC(acc, samples) ---- */

static int flush(mgrng_t *r)
{
	const uint8_t *parts[1] = {r->buf};
	size_t lens[1] = {r->buf_n};
	int e = MGRNG_OK;

	if (r->buf_n) {
		e = hmac(r->acc, sizeof(r->acc), parts, lens, 1, r->acc);
		mglsc_wipe(r->buf, sizeof(r->buf));
		r->buf_n = 0;
		if (e != MGRNG_OK) {
			r->cnt = 0;             /* the samples are gone, not in acc: a full seed needs a new count */
		}
	}
	return e;
}

/* ---- health tests (SP 800-90B 4.4) ---- */

static void health_fail(mgrng_t *r)
{
	r->cnt = 0;                         /* the counted samples since the last (re)seed are discarded */
	/* one clean window before counting again; during the startup test, the whole startup test again */
	r->need_clean = r->need_clean == MGRNG_STARTUP && r->clean < MGRNG_STARTUP ? MGRNG_STARTUP : MGRNG_APT_WINDOW;
	r->clean = 0;
	r->rct_run = 0;
	r->apt_i = 0;
}

/* true: the sample passed both tests. */
static bool health(mgrng_t *r, uint32_t raw)
{
	bool ok = true;

	r->rct_run = (r->rct_run && raw == r->rct_last) ? (uint16_t)(r->rct_run + 1u) : 1u;
	r->rct_last = raw;
	if (r->rct_run >= r->rct_cut) {
		r->st.rct_fails++;
		ok = false;
	}
	if (r->apt_i == 0) {
		r->apt_ref = raw;
		r->apt_n = 1;
	} else if (raw == r->apt_ref) {
		r->apt_n++;
	}
	if (ok && r->apt_n >= r->apt_cut) {
		r->st.apt_fails++;
		ok = false;
	}
	if (++r->apt_i >= MGRNG_APT_WINDOW) {
		r->apt_i = 0;
	}
	if (!ok) {
		health_fail(r);
	}
	return ok;
}

void mgrng_init(mgrng_t *r, const mgrng_port_t *p)
{
	uint32_t h, bits;

	memset(r, 0, sizeof(*r));
	r->p = p;
	h = p->h_milli ? p->h_milli : DEF_H_MILLI;
	bits = p->seed_bits ? p->seed_bits : DEF_SEED;
	if (bits < 128u) {
		bits = 128u;
	}
	r->need = (bits * 1000u + h - 1u) / h;
	r->rct_cut = p->rct_cutoff ? p->rct_cutoff : DEF_RCT;
	r->apt_cut = p->apt_cutoff ? p->apt_cutoff : DEF_APT;
	r->need_clean = MGRNG_STARTUP;
}

void mgrng_feed(mgrng_t *r, uint32_t raw)
{
	bool lost;

	r->st.fed++;
	if (!health(r, raw)) {
		return;
	}
	r->buf[r->buf_n++] = (uint8_t)raw;
	r->buf[r->buf_n++] = (uint8_t)(raw >> 8);
	r->buf[r->buf_n++] = (uint8_t)(raw >> 16);
	r->buf[r->buf_n++] = (uint8_t)(raw >> 24);
	lost = r->buf_n == sizeof(r->buf) && flush(r) != MGRNG_OK;
	if (r->clean < r->need_clean) {
		r->clean++;                     /* the startup test, or the clean window after a failure */
		return;
	}
	if (lost) {
		return;                         /* this sample went with the failed conditioning */
	}
	r->cnt++;
	r->st.counted++;
}

/* ---- the record ---- */

/* The record's check: the first 4 bytes of HMAC(pool, 'c' | "MGR1" | flags | 0 0 0) (the lean build's too). */
static int rec_check(const uint8_t rec[MGRNG_REC_LEN], uint8_t out[4])
{
	uint8_t m[9] = {'c', 'M', 'G', 'R', '1', rec[4], 0, 0, 0}, d[32];
	const uint8_t *parts[1] = {m};
	size_t lens[1] = {sizeof(m)};
	int e = hmac(&rec[8], 32, parts, lens, 1, d);

	memcpy(out, d, 4);
	mglsc_wipe(d, sizeof(d));
	return e;
}

static void load_once(mgrng_t *r)
{
	uint8_t rec[MGRNG_REC_LEN], c[4];

	if (r->flags & F_LOADED) {
		return;
	}
	r->flags |= F_LOADED;
	if (r->p->pool_load(r->p->ctx, rec) == 0 && !memcmp(rec, "MGR1", 4) && rec_check(rec, c) == MGRNG_OK &&
	    mglsc_ct_equal(c, &rec[40], 4)) {
		memcpy(r->pool, &rec[8], MGRNG_POOL_LEN);
		if (rec[4] & REC_SEEDED) {
			r->flags |= F_REC_SEEDED;
		}
	}
	mglsc_wipe(rec, sizeof(rec));
}

/* Writes the record (the pool now, flagged seeded once a full seed happened) and reads it back. */
static int store(mgrng_t *r, const uint8_t pool[MGRNG_POOL_LEN], bool seeded)
{
	uint8_t rec[MGRNG_REC_LEN];
	int e;

	memset(rec, 0, sizeof(rec));
	memcpy(rec, "MGR1", 4);
	rec[4] = seeded ? REC_SEEDED : 0u;
	memcpy(&rec[8], pool, MGRNG_POOL_LEN);
	e = rec_check(rec, &rec[40]);
	if (e == MGRNG_OK) {
		r->st.stores++;
		e = r->p->pool_store(r->p->ctx, rec) == 0 ? MGRNG_OK : MGRNG_E_POOL;
		if (e != MGRNG_OK) {
			r->st.store_fails++;
		}
	}
	mglsc_wipe(rec, sizeof(rec));
	return e;
}

static void pull(mgrng_t *r)
{
	uint32_t raw;
	uint16_t n = r->p->pull_max ? r->p->pull_max : DEF_PULL;

	if (!r->p->sample) {
		return;
	}
	for (uint16_t i = 0; i < n; i++) {
		if (r->p->sample(r->p->ctx, &raw) != 0) {
			break;
		}
		mgrng_feed(r, raw);
	}
	raw = 0;
}

/* After a failure that may have touched K and V: no output until a fresh full counted seed. */
static void unseed(mgrng_t *r)
{
	mglsc_wipe(r->k, sizeof(r->k));
	mglsc_wipe(r->v, sizeof(r->v));
	if (r->flags & F_SEEDED) {
		r->flags &= (uint8_t)~(F_SEEDED | F_FULL | F_REC_SEEDED);
	}
}

/* Seeds (first: instantiate from H(pool | samples | extra | nonce); else reseed with samples | extra) and forwards the
 * pool from DRBG bytes no one sees. The boot's first seed stores that pool (flagged seeded when this seed is a full one
 * or the record already was) and reads it back BEFORE anything is output; a later one leaves the write to the tick. */
static int seed(mgrng_t *r, bool first, bool full)
{
	uint8_t ex[32], buf[32], nonce[8];
	const uint8_t *parts[4] = {r->pool, r->acc, ex, nonce};
	size_t lens[4] = {32, 32, 32, 8};
	int e = flush(r), e_store = MGRNG_E_POOL;

	memset(ex, 0, sizeof(ex));
	if (r->p->extra) {
		r->p->extra(r->p->ctx, ex);
	}
	for (int i = 0; i < 4; i++) {
		nonce[i] = (uint8_t)(r->st.fed >> (8 * i));
		nonce[4 + i] = (uint8_t)(r->st.seeds >> (8 * i));
	}
	if (e == MGRNG_OK && first) {
		e = hmac(L_SEED, sizeof(L_SEED) - 1, parts, lens, 4, buf);
		if (e == MGRNG_OK) {
			e = drbg_instantiate(r, buf);
		}
	} else if (e == MGRNG_OK) {
		e = drbg_update(r, &parts[1], &lens[1], 2);  /* SP 800-90A reseed: entropy | extra */
		r->gen_count = 0;
	}
	if (e == MGRNG_OK) {
		e = drbg_generate(r, buf, sizeof(buf));      /* the next pool */
	}
	if (e == MGRNG_OK && first) {
		e = e_store = store(r, buf, full || (r->flags & F_REC_SEEDED));
		if (e == MGRNG_E_POOL && full) {
			/* Output rests on this boot's fresh counted seed: the write is
			 * owed (counted in store(); mgrng_tick retries it), output goes
			 * on. */
			r->flags |= F_DIRTY;
			e = MGRNG_OK;
		}
	}
	if (e == MGRNG_OK) {
		memcpy(r->pool, buf, sizeof(buf));
		r->flags |= F_SEEDED;
		if (!first) {
			r->flags |= F_DIRTY;
			r->st.reseeds++;
		} else {
			r->st.seeds++;
		}
		if (full) {
			r->flags |= F_FULL | F_REC_SEEDED;
		}
		if (first && e_store == MGRNG_OK) {
			r->flags |= F_BOOT_W;
		}
		r->cnt = 0;                                   /* the samples counted so far are in this seed: the next
							       * full one needs a whole new count */
		mglsc_wipe(r->acc, sizeof(r->acc));
	} else {
		unseed(r);                                    /* no output resting on a pool that wasn't stored, or on a
							       * DRBG state a failed update may have damaged */
	}
	mglsc_wipe(ex, sizeof(ex));
	mglsc_wipe(buf, sizeof(buf));
	return e;
}

int mgrng_generate(mgrng_t *r, uint8_t *out, size_t n)
{
	int e;

	if (!out && n) {
		return MGRNG_E_ARG;
	}
	pull(r);
	if (!(r->flags & F_SEEDED)) {
		load_once(r);
		if (!(r->flags & F_REC_SEEDED) && r->cnt < r->need) {
			return MGRNG_E_NOT_READY;              /* cold: the startup test and a full counted seed first */
		}
		e = seed(r, true, r->cnt >= r->need);     /* warm: out right after the forward write */
	} else if (r->cnt >= r->need) {
		e = seed(r, false, true);
	} else {
		e = r->gen_count >= MGRNG_RESEED_MAX ? MGRNG_E_NOT_READY : MGRNG_OK; /* SP 800-90A's reseed interval */
	}
	if (e == MGRNG_OK) {
		e = drbg_generate(r, out, n);
	}
	if (e == MGRNG_OK) {
		e = drbg_generate(r, r->pool, sizeof(r->pool)); /* the pool forward, never output; stored by mgrng_tick */
		r->flags |= F_DIRTY;
	}
	if (e != MGRNG_OK && (r->flags & F_SEEDED)) {
		if (out) {
			mglsc_wipe(out, n);
		}
		if (e != MGRNG_E_NOT_READY) {
			unseed(r);                         /* the generate or the pool's forward failed part-way */
		}
	}
	return e;
}

void mgrng_tick(mgrng_t *r, uint32_t now)
{
	uint32_t gap = r->p->write_gap_ms ? r->p->write_gap_ms : DEF_GAP_MS;

	if ((r->flags & F_SEEDED) && r->cnt >= r->need) {
		(void)seed(r, false, true);
	}
	if (r->flags & F_BOOT_W) {
		/* The boot wrote the pool before its first output: the forward after it waits a gap, as any write. */
		r->flags = (uint8_t)((r->flags & ~F_BOOT_W) | F_STORED);
		r->last_store = now;
	}
	if ((r->flags & F_DIRTY) && (!(r->flags & F_STORED) || now - r->last_store >= gap)) {
		r->last_store = now;
		r->flags |= F_STORED;
		if (store(r, r->pool, (r->flags & F_REC_SEEDED) != 0) == MGRNG_OK) {
			r->flags &= (uint8_t)~F_DIRTY;
		}
	}
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
