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

// GHASH tables (main/ghash32.c) against the GCM specification's test cases
// and a bit-at-a-time reference multiply over random inputs.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ghash32.h"

static size_t hex(const char *s, uint8_t *out) {
    size_t n = strlen(s) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

// SP 800-38D Algorithm 1, one bit at a time.
static void ref_mult(uint8_t x[16], const uint8_t h[16]) {
    uint8_t z[16] = {0}, v[16];
    memcpy(v, h, 16);
    for (int i = 0; i < 128; i++) {
        if (x[i / 8] & (0x80 >> (i % 8))) {
            for (int j = 0; j < 16; j++) z[j] ^= v[j];
        }
        int lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1;
    }
    memcpy(x, z, 16);
}

static void ref_ghash(const uint8_t h[16], uint8_t y[16], const uint8_t *x, size_t len) {
    while (len) {
        size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; i++) y[i] ^= x[i];
        ref_mult(y, h);
        x += n;
        len -= n;
    }
}

static ghash4_key_t k4;
static ghash8_key_t k8;

static void check_case(const char *h_hex, const char *a_hex, const char *c_hex, const char *want_hex) {
    uint8_t h[16], a[64], c[128], want[16], lens[16] = {0};
    hex(h_hex, h);
    size_t an = hex(a_hex, a), cn = hex(c_hex, c);
    hex(want_hex, want);
    uint64_t ab = (uint64_t)an * 8, cb = (uint64_t)cn * 8;
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(ab >> (56 - 8 * i));
        lens[8 + i] = (uint8_t)(cb >> (56 - 8 * i));
    }
    ghash4_init(&k4, h);
    ghash8_init(&k8, h);
    for (int which = 0; which < 2; which++) {
        uint8_t y[16] = {0};
        if (which == 0) {
            ghash4_update(&k4, y, a, an);
            ghash4_update(&k4, y, c, cn);
            ghash4_update(&k4, y, lens, 16);
        } else {
            ghash8_update(&k8, y, a, an);
            ghash8_update(&k8, y, c, cn);
            ghash8_update(&k8, y, lens, 16);
        }
        if (memcmp(y, want, 16) != 0) {
            fprintf(stderr, "ghash%d mismatch for H=%s\n", which ? 8 : 4, h_hex);
            exit(1);
        }
    }
}

int main(void) {
    // GCM spec (McGrew & Viega) test cases 2, 4 and 6: H, A, C, GHASH(H, A, C).
    check_case("66e94bd4ef8a2c3b884cfa59ca342b2e", "", "0388dace60b6a392f328c2b971b2fe78",
               "f38cbb1ad69223dcc3457ae5b6b0f885");
    check_case("b83b533708bf535d0aa6e52980d53b78", "feedfacedeadbeeffeedfacedeadbeefabaddad2",
               "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
               "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091",
               "698e57f70e6ecc7fd9463b7260a9ae5f");
    check_case("b83b533708bf535d0aa6e52980d53b78", "feedfacedeadbeeffeedfacedeadbeefabaddad2",
               "8ce24998625615b603a033aca13fb894be9112a5c3a211a8ba262a3cca7e2ca7"
               "01e4a9a4fba43c90ccdcb281d48c7c6fd62875d2aca417034c34aee5",
               "1c5afe9760d3932f3c9a878aac3dc3de");

    // Random keys and lengths (partial final blocks included) against the
    // bit-serial reference.
    srand(12345);
    static uint8_t buf[1000];
    for (int t = 0; t < 200; t++) {
        uint8_t h[16], y4[16] = {0}, y8[16] = {0}, yr[16] = {0};
        for (int i = 0; i < 16; i++) h[i] = (uint8_t)rand();
        size_t len = (size_t)(rand() % (int)sizeof(buf));
        for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)rand();
        ghash4_init(&k4, h);
        ghash8_init(&k8, h);
        ghash4_update(&k4, y4, buf, len);
        ghash8_update(&k8, y8, buf, len);
        // The reference pads each partial block the same way at the end.
        ref_ghash(h, yr, buf, len);
        if (memcmp(y4, yr, 16) || memcmp(y8, yr, 16)) {
            fprintf(stderr, "random case %d (len %zu) mismatch\n", t, len);
            return 1;
        }
    }
    puts("ok");
    return 0;
}
