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

#include "ghash32.h"

#include <string.h>

// Reduction terms for the bits a 4- or 8-bit right shift drops off the low
// end: shifting a block right by n is (block >> n) ^ red_n[dropped bits],
// and only the top word is touched. Built once from the 1-bit rule.
static uint32_t s_red4[16];
static uint32_t s_red8[256];
static int s_red_ready;

static void shift1(uint32_t v[4]) {
    uint32_t lsb = v[3] & 1;
    v[3] = (v[3] >> 1) | (v[2] << 31);
    v[2] = (v[2] >> 1) | (v[1] << 31);
    v[1] = (v[1] >> 1) | (v[0] << 31);
    v[0] = (v[0] >> 1) ^ (lsb ? 0xE1000000u : 0);
}

static void build_reductions(void) {
    if (s_red_ready) return;
    for (uint32_t r = 0; r < 256; r++) {
        uint32_t v[4] = {0, 0, 0, r};
        for (int i = 0; i < 8; i++) {
            shift1(v);
            if (i == 3 && r < 16) s_red4[r] = v[0];
        }
        s_red8[r] = v[0];
    }
    s_red_ready = 1;
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// Multiples of H by single bits: m[top] = H, each lower bit one more x.
// Sums of bits are XORs of those.
static void fill_table(uint32_t (*t)[4], int n, const uint8_t h[16]) {
    uint32_t v[4] = {be32(h), be32(h + 4), be32(h + 8), be32(h + 12)};
    memset(t, 0, sizeof(uint32_t) * 4 * (size_t)n);
    for (int i = n / 2; i >= 1; i /= 2) {
        memcpy(t[i], v, sizeof(v));
        shift1(v);
    }
    for (int i = 2; i < n; i *= 2) {
        for (int j = 1; j < i; j++) {
            for (int w = 0; w < 4; w++) t[i + j][w] = t[i][w] ^ t[j][w];
        }
    }
}

void ghash4_init(ghash4_key_t *k, const uint8_t h[16]) {
    build_reductions();
    fill_table(k->t, 16, h);
}

void ghash8_init(ghash8_key_t *k, const uint8_t h[16]) {
    build_reductions();
    fill_table(k->t, 256, h);
}

#define SHIFT(z0, z1, z2, z3, n, red) do {                 \
        uint32_t r_ = (z3) & ((1u << (n)) - 1);             \
        (z3) = ((z3) >> (n)) | ((z2) << (32 - (n)));         \
        (z2) = ((z2) >> (n)) | ((z1) << (32 - (n)));         \
        (z1) = ((z1) >> (n)) | ((z0) << (32 - (n)));         \
        (z0) = ((z0) >> (n)) ^ (red)[r_];                    \
    } while (0)

#define ADD(z0, z1, z2, z3, e) do {                          \
        (z0) ^= (e)[0]; (z1) ^= (e)[1]; (z2) ^= (e)[2]; (z3) ^= (e)[3]; \
    } while (0)

// y = (y ^ block) * H
static void mult4(const ghash4_key_t *k, uint8_t x[16]) {
    uint32_t z0 = 0, z1 = 0, z2 = 0, z3 = 0;
    for (int i = 15; i >= 0; i--) {
        SHIFT(z0, z1, z2, z3, 4, s_red4);
        ADD(z0, z1, z2, z3, k->t[x[i] & 0xf]);
        SHIFT(z0, z1, z2, z3, 4, s_red4);
        ADD(z0, z1, z2, z3, k->t[x[i] >> 4]);
    }
    put_be32(x, z0);
    put_be32(x + 4, z1);
    put_be32(x + 8, z2);
    put_be32(x + 12, z3);
}

static void mult8(const ghash8_key_t *k, uint8_t x[16]) {
    uint32_t z0 = 0, z1 = 0, z2 = 0, z3 = 0;
    for (int i = 15; i >= 0; i--) {
        SHIFT(z0, z1, z2, z3, 8, s_red8);
        ADD(z0, z1, z2, z3, k->t[x[i]]);
    }
    put_be32(x, z0);
    put_be32(x + 4, z1);
    put_be32(x + 8, z2);
    put_be32(x + 12, z3);
}

#define UPDATE(mult)                                         \
    while (len >= 16) {                                      \
        for (int i = 0; i < 16; i++) y[i] ^= x[i];           \
        mult(k, y);                                          \
        x += 16;                                             \
        len -= 16;                                           \
    }                                                        \
    if (len) {                                               \
        for (size_t i = 0; i < len; i++) y[i] ^= x[i];       \
        mult(k, y);                                          \
    }

void ghash4_update(const ghash4_key_t *k, uint8_t y[16], const uint8_t *x, size_t len) {
    UPDATE(mult4)
}

void ghash8_update(const ghash8_key_t *k, uint8_t y[16], const uint8_t *x, size_t len) {
    UPDATE(mult8)
}
