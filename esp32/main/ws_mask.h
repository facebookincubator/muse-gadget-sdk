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

// WebSocket client masking (RFC 6455 5.3) in place: payload[i] ^= key[i % 4],
// where key is the 4 mask bytes in the order they go on the wire.
//
// A word at a time: bytes up to the first word boundary, then whole words
// with the mask rotated to that offset, then the tail, so it works for a
// payload at any alignment. tests/test_ws_mask.py checks it against the byte
// loop at every alignment.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline void ws_mask_inplace(uint8_t *payload, size_t len, const uint8_t key[4]) {
    size_t i = 0;
    for (; i < len && ((uintptr_t)(payload + i) & 3); i++) {
        payload[i] ^= key[i & 3];
    }
    uint32_t word_mask;
    uint8_t wm[4];
    for (int k = 0; k < 4; k++) wm[k] = key[(i + (size_t)k) & 3];
    memcpy(&word_mask, wm, 4);
    // Word-aligned from here: memcpy keeps it legal C, and the alignment
    // hint lets the compiler use plain word loads and stores.
    uint8_t *words = (uint8_t *)__builtin_assume_aligned(payload + i, 4);
    size_t nwords = (len - i) / 4;
    for (size_t k = 0; k < nwords; k++) {
        uint32_t w;
        memcpy(&w, words + 4 * k, 4);
        w ^= word_mask;
        memcpy(words + 4 * k, &w, 4);
    }
    i += 4 * nwords;
    for (; i < len; i++) payload[i] ^= key[i & 3];
}
