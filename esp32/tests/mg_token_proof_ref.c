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
 * A host reference for include/mg_token_proof.h: SHA-256 (FIPS 180-4),
 * HMAC (RFC 2104) and one-block HKDF (RFC 5869) in plain C, so the protocol
 * and Link setup harnesses run without a crypto library. The device uses
 * mg_token_proof.c on PSA; test_mg_token_proof checks that one, and this,
 * against protocols/test-vectors/mg-token-proof-v1.json.
 */

#include "mg_token_proof.h"

#include <string.h>

typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    size_t n;
    uint64_t bits;
} sha256_t;

static const uint32_t K256[64] = {
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

static void sha256_block(sha256_t *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(sha256_t *s)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof(iv));
    s->n = 0;
    s->bits = 0;
}

static void sha256_update(sha256_t *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->bits += (uint64_t)len * 8;
    while (len--) {
        s->buf[s->n++] = *p++;
        if (s->n == 64) {
            sha256_block(s, s->buf);
            s->n = 0;
        }
    }
}

static void sha256_final(sha256_t *s, uint8_t out[32])
{
    uint64_t bits = s->bits;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    pad = 0;
    while (s->n != 56) {
        sha256_update(s, &pad, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha256_update(s, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

static void hmac3(const uint8_t *key, size_t klen, const void *a, size_t alen, const void *b, size_t blen,
                  const void *c, size_t clen, uint8_t out[32])
{
    uint8_t k[64] = { 0 }, pad[64], inner[32];
    sha256_t s;
    if (klen > 64) {
        sha256_init(&s);
        sha256_update(&s, key, klen);
        sha256_final(&s, k);
    } else {
        memcpy(k, key, klen);
    }
    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x36;
    }
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, a, alen);
    sha256_update(&s, b, blen);
    sha256_update(&s, c, clen);
    sha256_final(&s, inner);
    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x5c;
    }
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, inner, 32);
    sha256_final(&s, out);
}

/* HMAC-SHA256 over n pieces, as mg_token_proof.c's on PSA (the seal's glue). */
bool mg_hmac_sha256_v(const uint8_t *key, size_t klen, const uint8_t *const parts[], const size_t lens[], int n,
                      uint8_t out[32])
{
    uint8_t k[64] = { 0 }, pad[64], inner[32];
    sha256_t s;
    if (klen > 64) {
        sha256_init(&s);
        sha256_update(&s, key, klen);
        sha256_final(&s, k);
    } else {
        memcpy(k, key, klen);
    }
    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x36;
    }
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    for (int i = 0; i < n; i++) {
        sha256_update(&s, parts[i], lens[i]);
    }
    sha256_final(&s, inner);
    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x5c;
    }
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, inner, 32);
    sha256_final(&s, out);
    return true;
}

void mg_token_proof_wipe(void *p, size_t n)
{
    volatile uint8_t *b = p;
    while (n--) {
        *b++ = 0;
    }
}

bool mg_token_proof_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= a[i] ^ b[i];
    }
    return d == 0;
}

bool mg_token_proof_key(const char *access_token, const char *node_id, uint8_t k[MG_TOKEN_PROOF_KEY_LEN])
{
    if (!access_token || !*access_token || !node_id) {
        return false;
    }
    static const char salt[] = "mg token proof v1";
    static const uint8_t one = 1;
    uint8_t prk[32];
    hmac3((const uint8_t *)salt, strlen(salt), access_token, strlen(access_token), NULL, 0, NULL, 0, prk);
    hmac3(prk, sizeof(prk), node_id, strlen(node_id), &one, 1, NULL, 0, k);
    mg_token_proof_wipe(prk, sizeof(prk));
    return true;
}

bool mg_token_proof_mac(const uint8_t k[MG_TOKEN_PROOF_KEY_LEN], bool device,
                        const uint8_t client_nonce[MG_TOKEN_PROOF_NONCE_LEN],
                        const uint8_t device_nonce[MG_TOKEN_PROOF_NONCE_LEN], uint8_t mac[MG_TOKEN_PROOF_MAC_LEN])
{
    const char *label = device ? "mg token proof v1 device" : "mg token proof v1 client";
    hmac3(k, MG_TOKEN_PROOF_KEY_LEN, label, strlen(label), client_nonce, MG_TOKEN_PROOF_NONCE_LEN, device_nonce,
          MG_TOKEN_PROOF_NONCE_LEN, mac);
    return true;
}
