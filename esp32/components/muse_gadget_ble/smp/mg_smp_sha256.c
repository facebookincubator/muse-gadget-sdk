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
 * mg_smp_sha256.c: the core's built-in SHA-256 (FIPS 180-4), small and
 * portable. Leave this file out and build the core with
 * -DMG_SMP_SHA256_BUILTIN=0 to hook the chip's own SHA-256 instead
 * (mg_smp_backend.h). From the ESP32 boards' SMP server.
 */
#include "mg_smp_backend.h"

#include <string.h>

typedef struct {
    uint32_t h[8];
    uint32_t len_lo, len_hi;   /* bytes */
    uint8_t buf[64];
    uint32_t n;
} sha_t;

typedef char mg_sha_ctx_fits[(sizeof(sha_t) <= MG_SMP_SHA256_CTX_SIZE) ? 1 : -1];

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(sha_t *s, const uint8_t *b)
{
    uint32_t w[16];
    uint32_t v[8];
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 |
               (uint32_t)b[4 * i + 3];
    }
    memcpy(v, s->h, sizeof(v));
    for (i = 0; i < 64; i++) {
        uint32_t wi;
        if (i < 16) {
            wi = w[i];
        } else {
            uint32_t a = w[(i + 1) & 15], c = w[(i + 14) & 15];
            uint32_t s0 = ROR(a, 7) ^ ROR(a, 18) ^ (a >> 3);
            uint32_t s1 = ROR(c, 17) ^ ROR(c, 19) ^ (c >> 10);
            wi = w[i & 15] = w[i & 15] + s0 + w[(i + 9) & 15] + s1;
        }
        uint32_t e = v[4];
        uint32_t t1 = v[7] + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & v[5]) ^ (~e & v[6])) + K[i] + wi;
        uint32_t a = v[0];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & v[1]) ^ (a & v[2]) ^ (v[1] & v[2]));
        v[7] = v[6];
        v[6] = v[5];
        v[5] = v[4];
        v[4] = v[3] + t1;
        v[3] = v[2];
        v[2] = v[1];
        v[1] = v[0];
        v[0] = t1 + t2;
    }
    for (i = 0; i < 8; i++) {
        s->h[i] += v[i];
    }
}

void mg_smp_sha256_init(void *ctx)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    sha_t *s = (sha_t *)ctx;
    memcpy(s->h, iv, sizeof(iv));
    s->len_lo = s->len_hi = 0;
    s->n = 0;
}

void mg_smp_sha256_update(void *ctx, const uint8_t *p, uint32_t n)
{
    sha_t *s = (sha_t *)ctx;
    uint32_t lo = s->len_lo + n;
    s->len_hi += lo < s->len_lo;
    s->len_lo = lo;
    while (n) {
        uint32_t k = 64 - s->n < n ? 64 - s->n : n;
        if (s->n == 0 && n >= 64) {
            block(s, p);
            k = 64;
        } else {
            memcpy(s->buf + s->n, p, k);
            s->n += k;
            if (s->n == 64) {
                block(s, s->buf);
                s->n = 0;
            }
        }
        p += k;
        n -= k;
    }
}

void mg_smp_sha256_final(void *ctx, uint8_t out[32])
{
    sha_t *s = (sha_t *)ctx;
    uint32_t hi = s->len_hi << 3 | s->len_lo >> 29, lo = s->len_lo << 3;
    int i;
    s->buf[s->n++] = 0x80;
    if (s->n > 56) {
        memset(s->buf + s->n, 0, 64 - s->n);
        block(s, s->buf);
        s->n = 0;
    }
    memset(s->buf + s->n, 0, 56 - s->n);
    for (i = 0; i < 4; i++) {
        s->buf[56 + i] = (uint8_t)(hi >> (24 - 8 * i));
        s->buf[60 + i] = (uint8_t)(lo >> (24 - 8 * i));
    }
    block(s, s->buf);
    for (i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}
