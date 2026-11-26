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
 * Test signals and hashes shared by the codec tools, in integer arithmetic
 * only: the same samples and the same hash on every host and compiler, so the
 * integer codecs' (lc3fx, libsbc) output can be compared bit for bit.
 */

#ifndef XPLAT_TESTS_SIGNAL_H
#define XPLAT_TESTS_SIGNAL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SIG_KINDS 8

static uint32_t sig_rng;

static uint32_t sig_rand(void)
{
    sig_rng = sig_rng * 1664525u + 1013904223u;
    return sig_rng;
}

/* sin(2 pi p / 2^16) * a, Bhaskara's approximation in integers */
static int32_t sig_sin(uint32_t p, int32_t a)
{
    int64_t u = p & 0x7fff, v = 0x8000 - u;
    int64_t s = (int64_t)a * 16 * u * v / (5 * (int64_t)0x8000 * 0x8000 - 4 * u * v);
    return (int32_t)(p & 0x8000 ? -s : s);
}

/* Sample `n` of signal `kind` at `sr` Hz; call with n = 0, 1, ... in turn */
static int16_t sig_sample(int kind, int n, int sr)
{
    static uint32_t ph1, ph2, fm;
    if (n == 0)
        ph1 = ph2 = fm = 0, sig_rng = 12345u + (uint32_t)kind;
    ph1 += (uint32_t)(440 * 65536 / sr);
    ph2 += (uint32_t)(1700 * 65536 / sr);
    int32_t v = 0;
    switch (kind) {
    case 0: v = 0; break;                                                   /* silence */
    case 1: v = (int32_t)(sig_rand() % 3) - 1; break;                      /* +-1 LSB */
    case 2: v = sig_sin(ph1, 3000) + sig_sin(ph2, 1500); break;            /* two tones */
    case 3: v = sig_sin(ph1, 32767); break;                                /* full scale */
    case 4: v = (int16_t)(sig_rand() >> 16); break;                        /* white noise */
    case 5: fm += (uint32_t)(100 * 65536 / sr) + (uint32_t)n / 4;          /* chirp */
            v = sig_sin(fm, 20000); break;
    case 6: v = n % 4000 == 2000 ? 30000 : 0; break;                       /* impulses */
    case 7: v = (n / 1600) % 2 ? sig_sin(ph1, 6000) : (int32_t)(sig_rand() % 3) - 1; break;
    }
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* FNV-1a, 64 bits */
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
#define FNV_INIT 0xcbf29ce484222325ull

static void *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    long len = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) { perror(path); exit(2); }
    void *p = malloc(len > 0 ? (size_t)len : 1);
    if (!p || fread(p, 1, (size_t)len, f) != (size_t)len) { perror(path); exit(2); }
    fclose(f);
    *n = (size_t)len;
    return p;
}

static void write_file(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n) { perror(path); exit(2); }
    fclose(f);
}

#endif /* XPLAT_TESTS_SIGNAL_H */
