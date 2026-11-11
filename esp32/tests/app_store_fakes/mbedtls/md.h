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

#pragma once
/* tests/test_app_store.py: SHA-256 alone, as mbedtls_md() computes it (FIPS 180-4). */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int mbedtls_md_info_t;
#define MBEDTLS_MD_SHA256 1
static const mbedtls_md_info_t fake_sha256_info = 1;

static inline const mbedtls_md_info_t *mbedtls_md_info_from_type(int type) {
    return type == MBEDTLS_MD_SHA256 ? &fake_sha256_info : NULL;
}

static inline int mbedtls_md(const mbedtls_md_info_t *info, const unsigned char *data, size_t len,
                             unsigned char *out) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    if (info != &fake_sha256_info) return -1;
    size_t total = ((len + 8) / 64 + 1) * 64;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t block[64];
        for (int i = 0; i < 64; i++) {
            size_t at = off + i;
            uint64_t bits = (uint64_t)len * 8;
            block[i] = at < len ? data[at] : at == len ? 0x80
                     : at >= total - 8 ? (uint8_t)(bits >> (8 * (total - 1 - at))) : 0;
        }
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
                   (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
        }
#define FAKE_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = FAKE_ROR(w[i - 15], 7) ^ FAKE_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = FAKE_ROR(w[i - 2], 17) ^ FAKE_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (FAKE_ROR(e, 6) ^ FAKE_ROR(e, 11) ^ FAKE_ROR(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (FAKE_ROR(a, 2) ^ FAKE_ROR(a, 13) ^ FAKE_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
#undef FAKE_ROR
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (int i = 0; i < 8; i++) {
        out[i * 4] = h[i] >> 24; out[i * 4 + 1] = h[i] >> 16; out[i * 4 + 2] = h[i] >> 8; out[i * 4 + 3] = h[i];
    }
    return 0;
}
