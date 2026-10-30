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

// ws_mask_inplace() against the byte-at-a-time RFC 6455 loop, at every
// start alignment and many lengths, with guard bytes on both sides.

#include <assert.h>
#include <stdio.h>

#include "ws_mask.h"

int main(void) {
    static uint8_t buf[9000 + 16], ref[9000 + 16];
    const uint8_t keys[3][4] = {{0x12, 0x34, 0x56, 0x78}, {0xff, 0x00, 0xa5, 0x5a}, {1, 2, 3, 4}};
    const size_t lens[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 63, 64, 65, 511, 4096, 8193, 9000};
    unsigned cases = 0;
    for (int k = 0; k < 3; k++) {
        for (size_t a = 0; a < 8; a++) {
            for (size_t l = 0; l < sizeof(lens) / sizeof(lens[0]); l++) {
                size_t len = lens[l];
                for (size_t i = 0; i < sizeof(buf); i++) buf[i] = ref[i] = (uint8_t)(i * 13 + a);
                ws_mask_inplace(buf + 8 + a, len, keys[k]);
                uint8_t *r = ref + 8 + a;
                for (size_t i = 0; i < len; i++) r[i] ^= keys[k][i & 3];
                for (size_t i = 0; i < sizeof(buf); i++) assert(buf[i] == ref[i]);
                // Masking twice restores the payload.
                ws_mask_inplace(buf + 8 + a, len, keys[k]);
                for (size_t i = 0; i < len; i++) assert(buf[8 + a + i] == (uint8_t)((8 + a + i) * 13 + a));
                cases++;
            }
        }
    }
    printf("ws_mask ok (%u cases)\n", cases);
    return 0;
}
