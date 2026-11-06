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

// GHASH (the GF(2^128) hash in AES-GCM) with 32-bit table arithmetic.
//
// ESP-IDF's GCM for chips without a GCM engine (the C5) does the CTR part on
// the AES DMA engine but GHASH in software, with mbedtls's 4-bit tables on
// 64-bit integers: ~60 cycles per byte on a 32-bit RISC-V core, more than half
// of an AES-256-GCM seal. These use 32-bit words only, and optionally Shoup's
// 8-bit table (4 KB per key) for half the iterations.
//
// Bit order follows mbedtls/NIST SP 800-38D: blocks are big-endian byte
// strings; multiplying by x shifts right with the 0xE1 reduction.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t t[16][4];      // nibble multiples of H, big-endian word order
} ghash4_key_t;

typedef struct {
    uint32_t t[256][4];     // byte multiples of H
} ghash8_key_t;

// Precompute tables for hash key H (= AES_K(0^128)).
void ghash4_init(ghash4_key_t *k, const uint8_t h[16]);
void ghash8_init(ghash8_key_t *k, const uint8_t h[16]);

// y = GHASH_H continued over `len` bytes of x, zero-padding a final partial
// block. y is the running 16-byte state (start from zeros).
void ghash4_update(const ghash4_key_t *k, uint8_t y[16], const uint8_t *x, size_t len);
void ghash8_update(const ghash8_key_t *k, uint8_t y[16], const uint8_t *x, size_t len);

#ifdef __cplusplus
}
#endif
