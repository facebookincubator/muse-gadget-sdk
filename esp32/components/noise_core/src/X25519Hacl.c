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
 * X25519 from HACL* Curve25519 (CONFIG_NOISE_CORE_X25519_HACL): the
 * formally verified, constant-time implementation mbedTLS ships as its
 * Everest driver (tf-psa-crypto/drivers/everest, Apache-2.0), compiled from
 * ESP-IDF's own copy. Always its 32-bit build (radix 2^51 with the verified
 * software uint128), so the host test runs the same code as every chip.
 */

#include <stdint.h>
#include <string.h>

#define KRML_VERIFIED_UINT128
#include "kremlib/FStar_UInt128_extracted.c"
#include "legacy/Hacl_Curve25519.c"
#include "kremlib/FStar_UInt64_FStar_UInt32_FStar_UInt16_FStar_UInt8.c"

/* out = X25519(scalar, u): the scalar clamped and u's top bit masked as RFC
 * 7748 says (HACL* does both). */
void noise_core_x25519_hacl(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32])
{
    uint8_t k[32], p[32];
    memcpy(k, scalar, sizeof(k));
    memcpy(p, u, sizeof(p));
    Hacl_Curve25519_crypto_scalarmult(out, k, p);
    volatile uint8_t *z = k;
    for (size_t i = 0; i < sizeof(k); i++) {
        z[i] = 0;
    }
}
