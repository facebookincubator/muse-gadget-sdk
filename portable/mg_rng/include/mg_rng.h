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
 * mg_rng: a gadget's random bytes for session security (the P-256 ephemeral
 * key, Link setup's and the token proof's device nonces): an HMAC-DRBG
 * (SP 800-90A, SHA-256) seeded from a board's noise source plus a
 * persistent 32-byte pool written forward at every boot before the first
 * output, with SP 800-90B health tests on the raw samples.
 *
 * The board gives (mgrng_port_t):
 *  - raw samples, pushed (mgrng_feed, from the main loop) or pulled (sample()):
 *    only a source that crosses clock domains (a crystal-vs-RC beat, a TRNG,
 *    external timing) may be counted, with its min-entropy per sample
 *    (h_milli) measured on the bench (ea_non_iid and the restart test);
 *  - the pool record's storage (pool_load / pool_store; store writes and
 *    reads back, 0 only if the read-back matches);
 *  - optionally, extra input that is mixed in and never counted (extra()).
 *
 * The seed:
 *  - a pool record flagged seeded (a boot that reached a full counted seed
 *    wrote it): the first mgrng_generate() forwards the pool with what has
 *    arrived (pool' = H(pool | samples | extra), stored and read back first),
 *    seeds the DRBG from it and outputs; a full reseed follows once enough
 *    counted samples arrive (mgrng_generate does it, or mgrng_tick);
 *  - otherwise (a factory part, an unreadable or unseeded record): output
 *    waits (MGRNG_E_NOT_READY) for the startup test (1,024 samples through
 *    both health tests) and the counted samples (seed_bits / H), then the
 *    record is written flagged seeded and read back before anything is
 *    output.
 * A failed store: when this boot's seed is a full
 * counted one (seed_bits at H, the health tests passed), output rests on it:
 * the failure is counted (st.store_fails), the write is owed
 * (MGRNG_FLAG_WRITE_OWED; mgrng_tick retries it) and output goes on. When
 * output would rest on the pool (a seeded record's fast start before the
 * counted samples, or no counted source), it refuses (MGRNG_E_POOL) and
 * nothing is output. After every
 * output the pool is forwarded again from DRBG bytes that are never output;
 * that write goes out from mgrng_tick() at most once every write_gap_ms (the
 * boot write before the first output is the safety property: no two boots
 * start from one pool, so coalescing the later writes is fine).
 *
 * Health tests (SP 800-90B 4.4), on every raw sample, before anything else:
 * the repetition count test (cutoff rct_cutoff) and the adaptive proportion
 * test (window 512, cutoff apt_cutoff); tests/cutoffs.py gives both from H
 * (alpha 2^-20). A failure discards every counted sample since the last
 * (re)seed and starts counting again only after one clean window; the DRBG,
 * once seeded, goes on (its seed came from samples that passed).
 *
 * No static RAM: everything is in mgrng_t (rv32: 272 bytes). Uses
 * mglsc_hmac_sha256 (portable/mg_link_setup's crypto backend). Main loop
 * only; not reentrant.
 */
#ifndef MG_RNG_H
#define MG_RNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MGRNG_POOL_LEN   32u
#define MGRNG_REC_LEN    44u   /* "MGR1" | flags | 3 reserved | pool | check (4) */
#define MGRNG_APT_WINDOW 512u
#define MGRNG_STARTUP    1024u /* SP 800-90B 4.3: samples tested before the first use */
#define MGRNG_RESEED_MAX (1ul << 20) /* between reseeds at most: HMAC-DRBG requests, two per mgrng_generate() (its
                                     * output and the pool's forward), so 2^19 calls; the lean generator counts
                                     * calls */
#ifndef MGRNG_LEAN
#define MGRNG_LEAN 0     /* 1: the lean pull-only configuration (build src/mg_rng_lean.c instead of src/mg_rng.c) */
#endif

/* mgrng_flags() bits (both configurations; the lean one sets SEEDED and WRITE_OWED only) */
#define MGRNG_FLAG_REC_SEEDED 0x02u /* the record loaded, or the one owed or written since, is flagged seeded */
#define MGRNG_FLAG_SEEDED     0x04u /* seeded: output possible */
#define MGRNG_FLAG_FULL       0x08u /* a full counted seed this boot */
#define MGRNG_FLAG_WRITE_OWED 0x10u /* a pool write is owed (full: mgrng_tick writes it; lean: the next mgrng_generate) */

enum {
	MGRNG_OK = 0,
	MGRNG_E_NOT_READY = -1, /* not seeded yet: too few counted samples (or the startup test) */
	MGRNG_E_POOL = -2,      /* the pool record could not be stored and read back: nothing output */
	MGRNG_E_ARG = -3,
	MGRNG_E_CRYPTO = -4,
};

typedef struct mgrng_port {
	void *ctx;
	/* Optional (pull boards): one raw sample, 0 if one was taken. */
	int (*sample)(void *ctx, uint32_t *raw);
	/* The record: load 0 if read (any content; the core checks it), < 0 none. */
	int (*pool_load)(void *ctx, uint8_t rec[MGRNG_REC_LEN]);
	/* Write, then read back: 0 only if the read-back is the record. */
	int (*pool_store)(void *ctx, const uint8_t rec[MGRNG_REC_LEN]);
	/* Optional: 32 bytes mixed in, never counted (an address, a PRNG, ADC LSBs). */
	void (*extra)(void *ctx, uint8_t out[32]);
	uint16_t h_milli;     /* min-entropy per raw sample x 1000 (0: 500, the placeholder until the bench) */
	uint16_t seed_bits;   /* counted bits per seed (0: 192; at least 128) */
	uint16_t rct_cutoff;  /* 0: H = 0.5's (41); otherwise tests/cutoffs.py's for h_milli */
	uint16_t apt_cutoff;  /* 0: H = 0.5's (410) */
	/* Pull boards: samples pulled per call at most (0: 64). MGRNG_LEAN: the whole seed's budget instead (0: 4 x
	 * (1,024 + seed_bits x 1000 / h_milli)); below 1,024 + seed_bits x 1000 / h_milli no seed completes. */
	uint16_t pull_max;
	uint32_t write_gap_ms; /* the forward write after outputs, at most this often (0: an hour, to spare the flash) */
	const uint8_t *pers;  /* the DRBG's personalization (a board id); may be NULL */
	size_t pers_len;
	/* MGRNG_LEAN only: 1 lets a boot whose record is flagged seeded output after its forward write when the source
	 * gives too few clean samples (it stopped, or failed its tests); 0: MGRNG_E_NOT_READY then. */
	uint8_t warm_without_noise;
} mgrng_port_t;

#if MGRNG_LEAN
/* The lean configuration (src/mg_rng_lean.c, pull-only, for boards short of RAM): 44 bytes resident (rv32). Each boot's
 * first mgrng_generate() pulls the startup test and the counted samples and does the health tests, the conditioning and
 * the record's forward write with everything on its stack (about 0.3 KB plus one HMAC); the generator is a key-erasure
 * one on the same HMAC (out = HMAC(key, 'o' | ctr | block); then key = HMAC(key, 'n' | ctr)), forward-secure, in place
 * of the HMAC-DRBG's 64 bytes of K and V. No push path, no tick: one record write per boot that generates; a failed
 * write with a full counted seed is retried at the next mgrng_generate (the pool then HMAC(key, 'p' | ctr), before
 * that call's blocks). */
typedef struct mgrng {
	const mgrng_port_t *p;
	uint8_t key[32];
	uint32_t ctr;
	uint8_t flags;
	uint8_t last;           /* the last seed's findings (MGRNG_L_*), for a console */
} mgrng_t;
#define MGRNG_L_RCT     0x01u /* a repetition count failure */
#define MGRNG_L_APT     0x02u /* an adaptive proportion failure */
#define MGRNG_L_STOPPED 0x04u /* the source stopped (or the pull budget ran out) before the count */
#define MGRNG_L_STORE   0x08u /* the record's store or read-back failed */
#define MGRNG_L_WARM    0x10u /* seeded from a seeded record without the full count (warm_without_noise) */
#else
typedef struct mgrng_stats {
	uint32_t fed, counted, rct_fails, apt_fails, seeds, reseeds, stores, store_fails;
} mgrng_stats_t;

typedef struct mgrng {
	const mgrng_port_t *p;
	uint8_t k[32], v[32];   /* HMAC-DRBG */
	uint8_t pool[MGRNG_POOL_LEN];
	uint8_t acc[32];        /* the counted samples since the last (re)seed, conditioned (HMAC) */
	uint8_t buf[64];        /* samples waiting for the conditioning */
	uint32_t rct_last, apt_ref, gen_count, last_store;
	uint32_t need, cnt;     /* counted samples a seed needs; counted since the last (re)seed */
	uint16_t rct_run, apt_i, apt_n, rct_cut, apt_cut;
	uint16_t clean, need_clean; /* passing samples in a row; how many before counting (startup, after a failure) */
	uint8_t buf_n, state, flags;
	mgrng_stats_t st;
} mgrng_t;
#endif

/* Sets up (nothing read or written yet). */
void mgrng_init(mgrng_t *r, const mgrng_port_t *p);
#if !MGRNG_LEAN
/* One raw sample from the board's noise source (push boards; pull boards may
 * call it too). Main loop only. */
void mgrng_feed(mgrng_t *r, uint32_t raw);
#endif
/* n random bytes: MGRNG_OK, MGRNG_E_NOT_READY (nothing output), MGRNG_E_POOL. */
int mgrng_generate(mgrng_t *r, uint8_t *out, size_t n);
#if !MGRNG_LEAN
/* Main loop: the coalesced forward write, and a full reseed once enough
 * counted samples arrived (now: the port's clock, ms). */
void mgrng_tick(mgrng_t *r, uint32_t now);
#endif
/* Seeded (output possible without waiting). */
bool mgrng_ready(const mgrng_t *r);
/* The state's flags (MGRNG_FLAG_*): for a console and the board's log (a
 * write owed after a failed store). */
uint8_t mgrng_flags(const mgrng_t *r);
/* Everything wiped (the board's reset). */
void mgrng_wipe(mgrng_t *r);

#ifdef __cplusplus
}
#endif
#endif /* MG_RNG_H */
