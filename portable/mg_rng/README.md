<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# mg_rng: session random numbers from a counted noise source and a forward pool

A gadget's security random numbers (Link setup's and the token proof's device nonces,
the P-256 ephemeral key on the builtin crypto backend (the PSA backend makes its key with its own generator), a
first-boot identity) come from a hardware TRNG, or from a CSPRNG seeded with counted
noise plus a persistent pool written forward at every boot before the first output, with health tests on the raw
samples. This is that CSPRNG, written once for every board (vendored unchanged, like `mg_device_session`).

**Experimental:** no board in this repository uses it (each has a hardware RNG); it is tested on the host only
(`tests/run.sh`).

- **Generator:** HMAC-DRBG, SHA-256 (SP 800-90A 10.1.2), on `mglsc_hmac_sha256` (`portable/mg_link_setup`'s crypto
  backend, builtin or PSA). Checked against NIST CAVP's HMAC_DRBG SHA-256 vector. At most `MGRNG_RESEED_MAX` DRBG
  requests between reseeds (2^19 calls: each `mgrng_generate()` makes two, its output and the pool's forward); past
  that it refuses until the next full reseed.
- **Noise:** the board's raw samples, pushed (`mgrng_feed`, main loop) or pulled (`sample()`), each through SP 800-90B's
  repetition count and adaptive proportion tests (window 512; cutoffs from the measured H, `tests/cutoffs.py`), then
  conditioned (HMAC). A sample counts only after the startup test (1,024 samples) and after one clean window following
  a failure; a failure discards the counted samples since the last (re)seed. **Only a source that crosses clock
  domains counts** (a crystal-vs-RC beat, a TRNG, external timing), with H from the bench (`ea_non_iid` and the 1,000 x
  1,000 restart test); everything else (an address, a BT PRNG, ADC LSBs) goes in `extra()`, never counted.
- **The pool record** (44 bytes: `MGR1`, a flags byte, the 32-byte pool, a 4-byte check), the board's storage
  (`pool_load` / `pool_store`, which writes and reads back):
  - a record flagged **seeded** (written by a boot that reached a full counted seed): the first `mgrng_generate()`
    seeds the DRBG from H(pool | samples so far | extra | nonce), stores the next pool (DRBG bytes no one sees) and
    reads it back, then outputs. A full reseed follows once enough counted samples arrive. So every boot starts from
    the pool the one before it wrote: no two boots share a seed, even with no noise at all;
  - **no record, a corrupt or an unseeded one** (a factory part): `MGRNG_E_NOT_READY` until the startup test and
    `seed_bits / H` counted samples (default 192 bits; 384 samples at the placeholder H = 0.5), then the same, the
    record flagged seeded;
  - a failed store: when this boot's seed is a full counted one, output rests on it: the
    failure is counted (`st.store_fails`), the write is owed (`MGRNG_FLAG_WRITE_OWED`, retried by `mgrng_tick`) and
    output goes on; when output would rest on the pool (a seeded record's fast start before the count, or no counted
    source) it refuses (`MGRNG_E_POOL`), nothing output;
  - after outputs the pool moves forward again (DRBG bytes); `mgrng_tick()` writes it at most once every
    `write_gap_ms` (an hour by default, to spare the flash; coalescing is fine: the boot write is the safety property).
- **No static RAM**: `mgrng_t` (272 bytes on rv32). Code 2.2 KB (rv32imc, -Os). Stack: about 0.2 KB plus one HMAC.
- **The record's check** is the first 4 bytes of HMAC(pool, `c` | `MGR1` | flags | 0 0 0) (both configurations).

## The lean configuration (`MGRNG_LEAN=1`, build `src/mg_rng_lean.c`)

For boards short of RAM: pull-only, **44 bytes resident on rv32** (a 32-byte key, a counter,
flags and the last seed's findings), everything else on the stack of the call that seeds (384 B plus one HMAC). Each
boot's first `mgrng_generate()` pulls the startup test and the counted samples (at most `pull_max`, default 4 x what it
needs), health-tests and conditions them, seeds from H(pool | samples | extra | pers | pulled), stores and reads back
the next pool (H(seed, `p`)), and only then makes the key (H(seed, `k`)). The generator is a key-erasure one on the
same HMAC, in place of the HMAC-DRBG's 64 bytes of K and V: block i of call ctr is HMAC(key, `o` | ctr | i), then
key = HMAC(key, `n` | ctr), so earlier outputs can't be recomputed from the state. A record flagged seeded may output
without the full count only with `warm_without_noise` (the port's choice); a factory part never does. One record write
per boot that generates; no push path, no tick. A failed write with the full count is owed: output goes on and the next
`mgrng_generate` retries it (the pool HMAC(key, `p` | ctr), before that call's blocks); without the full count it
refuses. A fresh seed (and write) every `MGRNG_RESEED_MAX` calls (2^20). Code 1.7 KB
(rv32imc -Os). Its known answers come from `tests/lean_model.py`, a Python model of the seed and the generator.

## Use

```c
static const mgrng_port_t port = {
	.ctx = NULL, .sample = NULL,                    /* push board: mgrng_feed() from its sampler */
	.pool_load = pool_load, .pool_store = pool_store, /* store: write, read back, 0 only if equal */
	.extra = extra,                                 /* optional, never counted */
	.h_milli = 500, .rct_cutoff = 41, .apt_cutoff = 410, /* tests/cutoffs.py 0.5: replace with the bench's H */
};
static mgrng_t rng;
mgrng_init(&rng, &port);
/* main loop */  mgrng_feed(&rng, raw);  mgrng_tick(&rng, now_ms);
/* a consumer */ if (mgrng_generate(&rng, nonce, 16) != MGRNG_OK) { /* answer busy */ }
```

Link setup can't answer busy: its `random()` failing ends the hello with `error_invalid_hello`. A board finishes the
first seed (`mgrng_ready()`) before it advertises the setup service.

## Tests

`tests/run.sh`: a strict build (-Werror), the exported-symbol prefix check (`mgrng_`), and
`tests/test_rng.c` at -O2, under ASan + UBSan and on PSA when mbedcrypto is installed: the CAVP vector; the health
tests' cutoffs and trips (repetition, adaptive proportion, custom cutoffs), the startup test and the clean window after
a failure (a failure during the startup test restarts it whole); the cold seed (nothing before the startup test and the
counted samples; a failed conditioning's samples never count; a failed store or a failed read-back refuses and
outputs nothing; a crypto failure after seeding outputs nothing until a fresh full counted seed); the warm boot (output with no samples right after the forward write; each boot
from the pool the one before it wrote; a replayed record replays the output, which is why a failed boot write refuses);
the coalesced forward writes and their retry; a corrupt record; a pull port; the reseed (by `mgrng_generate` and by the
tick); wipe. `tests/test_rng_lean.c` (the lean build, ASan + UBSan and PSA): the known answers (the stored record and
two calls' outputs), the startup test and the count, a repetition failure and its clean window, a failed store, two
boots from one record, `warm_without_noise` (and a factory part never seeding without the count), the pull budget, a
corrupt record, key erasure, the reseed interval, the resident size.
