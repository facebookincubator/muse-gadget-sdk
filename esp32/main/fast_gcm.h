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

// Faster AES-GCM for chips whose AES engine has no GCM mode (the C5)
// (CONFIG_HOMEHUB_FAST_GCM). The link wraps ESP-IDF's
// esp_aes_gcm_crypt_and_tag() and esp_aes_gcm_auth_decrypt(), which its PSA
// driver calls for every one-shot AES-GCM operation, so both TLS records and
// Noise transport messages use it. CTR stays on the AES DMA engine; GHASH
// uses ghash32.c's tables, cached per key, instead of IDF's 64-bit 4-bit
// tables recomputed on every call. Anything other than a 96-bit IV goes to
// IDF's implementation.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FAST_GCM_OFF = 0,   // IDF's implementation
    FAST_GCM_G4 = 1,    // 4-bit tables, 256 B per key
    FAST_GCM_G8 = 2,    // 8-bit tables, 4 KB per key (internal RAM)
} fast_gcm_mode_t;

// Switch implementation at run time (bench A/B). Not thread-safe against
// operations in flight: call while no GCM operation runs.
void fast_gcm_set_mode(fast_gcm_mode_t mode);
fast_gcm_mode_t fast_gcm_get_mode(void);

// Where 8-bit tables go (default internal RAM). Drops the cached tables.
// Same threading rule as fast_gcm_set_mode().
void fast_gcm_set_table_psram(bool psram);

// Operations served by the fast path and by IDF's, since boot.
void fast_gcm_counts(unsigned *fast, unsigned *fallback);

#ifdef __cplusplus
}
#endif
