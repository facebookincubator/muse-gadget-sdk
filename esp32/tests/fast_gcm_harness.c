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

// main/fast_gcm.c against a scripted AES driver (tests/fast_gcm_fakes):
// - a failure before anything is written falls back to IDF's GCM; a failure
//   after part of the output is written never does (an in-place seal would
//   otherwise go out as plaintext under a valid tag) and wipes the output;
// - the self-test against IDF's GCM runs before the first fast operation and
//   turns the fast path off when they disagree (built with
//   -DTEST_SELFTEST_FAIL);
// - the key cache holds no AES key, reuses tables per key, and forgets on
//   fast_gcm_forget_all().
// fast_gcm.c is included so its cache can be inspected. The GHASH arithmetic
// itself is checked by test_ghash32.py.

#include <assert.h>
#include <stdio.h>

#include "fast_gcm.c"

// ---- scripted driver ----
static int g_ctr_calls, g_fail_ctr_at = -1, g_ecb_calls, g_fail_setkey;
static int g_real_calls;
static bool g_external;   // whether buffers count as PSRAM
static bool g_real_broken;   // IDF's GCM disagrees (flips a tag bit)

// The fake block cipher: "encryption" is a fixed XOR, keyed on nothing but
// key byte 0, so different keys give different H.
static void fake_ecb(const uint8_t key0, const unsigned char in[16], unsigned char out[16]) {
    for (int i = 0; i < 16; i++) out[i] = (unsigned char)(in[i] ^ 0x5c ^ i ^ key0);
}

void esp_aes_gcm_init(esp_gcm_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }
int esp_aes_gcm_setkey(esp_gcm_context *ctx, int cipher, const unsigned char *key, unsigned int bits) {
    (void)cipher;
    ctx->aes_ctx.key_bytes = (uint8_t)(bits / 8);
    memcpy(ctx->aes_ctx.key, key, ctx->aes_ctx.key_bytes);
    return 0;
}
void esp_aes_gcm_free(esp_gcm_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }

void esp_aes_init(esp_aes_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }
void esp_aes_free(esp_aes_context *ctx) { (void)ctx; }
int esp_aes_setkey(esp_aes_context *ctx, const unsigned char *key, unsigned int bits) {
    if (g_fail_setkey) return -0x20;
    ctx->key_bytes = (uint8_t)(bits / 8);
    memcpy(ctx->key, key, ctx->key_bytes);
    return 0;
}
int esp_aes_crypt_ecb(esp_aes_context *ctx, int mode, const unsigned char in[16], unsigned char out[16]) {
    (void)mode;
    g_ecb_calls++;
    fake_ecb(ctx->key[0], in, out);
    return 0;
}
int esp_aes_crypt_ctr(esp_aes_context *ctx, size_t length, size_t *nc_off, unsigned char ctr[16],
                      unsigned char stream[16], const unsigned char *in, unsigned char *out) {
    (void)ctx; (void)nc_off; (void)ctr; (void)stream;
    int call = g_ctr_calls++;
    if (call == g_fail_ctr_at) {
        // A DMA failure part-way: half the data processed.
        for (size_t i = 0; i < length / 2; i++) out[i] = in[i] ^ 0xAA;
        return -0x21;
    }
    for (size_t i = 0; i < length; i++) out[i] = in[i] ^ 0xAA;
    return 0;
}
bool esp_ptr_external_ram(const void *p) { (void)p; return g_external; }
void mbedtls_platform_zeroize(void *buf, size_t len) { memset(buf, 0, len); }

// IDF's GCM, as a reference over the same fake cipher: CTR is the fixed XOR,
// H = E(0), the tag is E(J0) ^ GHASH(aad, ciphertext, lengths).
static void ref_gcm(const esp_gcm_context *ctx, int mode, size_t length, const unsigned char *iv,
                    const unsigned char *aad, size_t aad_len, const unsigned char *input,
                    unsigned char *output, unsigned char tag[16]) {
    uint8_t key0 = ctx->aes_ctx.key[0];
    static const uint8_t zero[16];
    uint8_t h[16], j0[16], s[16], y[16] = {0}, lens[16];
    fake_ecb(key0, zero, h);
    memcpy(j0, iv, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    fake_ecb(key0, j0, s);
    ghash4_key_t k;
    ghash4_init(&k, h);
    ghash4_update(&k, y, aad, aad_len);
    if (mode == ESP_AES_DECRYPT) ghash4_update(&k, y, input, length);
    for (size_t i = 0; i < length; i++) output[i] = input[i] ^ 0xAA;
    if (mode != ESP_AES_DECRYPT) ghash4_update(&k, y, output, length);
    uint64_t abits = (uint64_t)aad_len * 8, cbits = (uint64_t)length * 8;
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(abits >> (56 - 8 * i));
        lens[8 + i] = (uint8_t)(cbits >> (56 - 8 * i));
    }
    ghash4_update(&k, y, lens, 16);
    for (int i = 0; i < 16; i++) tag[i] = s[i] ^ y[i];
    if (g_real_broken) tag[0] ^= 1;
}

int __real_esp_aes_gcm_crypt_and_tag(esp_gcm_context *ctx, int mode, size_t length,
                                     const unsigned char *iv, size_t iv_len,
                                     const unsigned char *aad, size_t aad_len,
                                     const unsigned char *input, unsigned char *output,
                                     size_t tag_len, unsigned char *tag) {
    (void)iv_len;
    g_real_calls++;
    uint8_t full[16];
    ref_gcm(ctx, mode, length, iv, aad, aad_len, input, output, full);
    memcpy(tag, full, tag_len);
    return 0;
}
int __real_esp_aes_gcm_auth_decrypt(esp_gcm_context *ctx, size_t length,
                                    const unsigned char *iv, size_t iv_len,
                                    const unsigned char *aad, size_t aad_len,
                                    const unsigned char *tag, size_t tag_len,
                                    const unsigned char *input, unsigned char *output) {
    (void)iv_len; (void)tag; (void)tag_len;
    g_real_calls++;
    uint8_t full[16];
    ref_gcm(ctx, ESP_AES_DECRYPT, length, iv, aad, aad_len, input, output, full);
    return 0;
}

// ---- helpers ----
static esp_gcm_context g_ctx;
static const uint8_t g_iv[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

static void reset(void) {
    g_ctr_calls = 0;
    g_fail_ctr_at = -1;
    g_fail_setkey = 0;
    g_real_calls = 0;
}

static int seal_in_place(uint8_t *buf, size_t len, uint8_t tag[16]) {
    return __wrap_esp_aes_gcm_crypt_and_tag(&g_ctx, ESP_AES_ENCRYPT, len, g_iv, 12, NULL, 0,
                                            buf, buf, 16, tag);
}

static bool all(const uint8_t *p, size_t n, uint8_t v) {
    for (size_t i = 0; i < n; i++) if (p[i] != v) return false;
    return true;
}

static bool cache_holds(const uint8_t *bytes, size_t n) {
    const uint8_t *c = (const uint8_t *)s_cache;
    for (size_t i = 0; i + n <= sizeof(s_cache); i++) {
        if (memcmp(c + i, bytes, n) == 0) return true;
    }
    return false;
}

int main(void) {
    g_ctx.aes_ctx.key_bytes = 32;
    for (int i = 0; i < 32; i++) g_ctx.aes_ctx.key[i] = (uint8_t)(0x40 + i);
    static uint8_t buf[3 * 2048 + 100];
    uint8_t tag[16];

#ifdef TEST_SELFTEST_FAIL
    // IDF's GCM disagrees with the fast path: the self-test, which runs on
    // the first operation, turns the fast path off and IDF serves it.
    g_real_broken = true;
    memset(buf, 0x11, 64);
    assert(seal_in_place(buf, 64, tag) == 0);
    assert(fast_gcm_get_mode() == FAST_GCM_OFF && s_selftest == SELFTEST_FAILED);
    reset();
    assert(seal_in_place(buf, 64, tag) == 0 && g_real_calls == 1 && g_ctr_calls == 0);
    puts("fast_gcm self-test failure ok");
    return 0;
#endif

    // The self-test runs on the first operation and passes (the reference
    // GCM agrees), across the staged path too.
    g_external = true;
    assert(fast_gcm_self_test() && s_selftest == SELFTEST_PASSED);
    assert(fast_gcm_get_mode() == FAST_GCM_G8);
    for (int i = 0; i < CACHE_ENTRIES; i++) assert(!s_cache[i].used);   // test key forgotten

    // The fast path and the reference agree on real operations.
    {
        uint8_t a[300], b[300], ta[16], tb[16];
        for (size_t i = 0; i < sizeof(a); i++) a[i] = b[i] = (uint8_t)i;
        assert(seal_in_place(a, sizeof(a), ta) == 0);
        ref_gcm(&g_ctx, ESP_AES_ENCRYPT, sizeof(b), g_iv, NULL, 0, b, b, tb);
        assert(memcmp(a, b, sizeof(a)) == 0 && memcmp(ta, tb, 16) == 0);
    }

    // Success, PSRAM path: every chunk encrypted once, no fallback.
    reset();
    g_external = true;
    memset(buf, 0x11, sizeof(buf));
    assert(seal_in_place(buf, sizeof(buf), tag) == 0);
    assert(all(buf, sizeof(buf), 0x11 ^ 0xAA) && g_real_calls == 0 && g_ctr_calls == 4);

    // PSRAM path, the first chunk's CTR fails: nothing written yet, so IDF's
    // GCM takes over on the untouched plaintext.
    reset();
    memset(buf, 0x11, sizeof(buf));
    g_fail_ctr_at = 0;
    assert(seal_in_place(buf, sizeof(buf), tag) == 0);
    assert(g_real_calls == 1 && all(buf, sizeof(buf), 0x11 ^ 0xAA));

    // PSRAM path, the second chunk fails after the first was written back:
    // no fallback (it would turn chunk one back into plaintext), an error,
    // and the buffer wiped.
    reset();
    memset(buf, 0x11, sizeof(buf));
    g_fail_ctr_at = 1;
    int rc = seal_in_place(buf, sizeof(buf), tag);
    assert(rc != 0 && g_real_calls == 0);
    assert(all(buf, sizeof(buf), 0));

    // Internal RAM path: the DMA works on the caller's buffer, so a CTR
    // failure there may have written part of it: same rule.
    reset();
    g_external = false;
    memset(buf, 0x11, sizeof(buf));
    g_fail_ctr_at = 0;
    rc = seal_in_place(buf, sizeof(buf), tag);
    assert(rc != 0 && g_real_calls == 0 && all(buf, sizeof(buf), 0));

    // Decrypt in place, failing after a chunk of plaintext was written:
    // the unauthenticated plaintext is wiped, no fallback.
    reset();
    g_external = true;
    memset(buf, 0x22, sizeof(buf));
    g_fail_ctr_at = 2;
    memset(tag, 0, sizeof(tag));
    rc = __wrap_esp_aes_gcm_auth_decrypt(&g_ctx, sizeof(buf), g_iv, 12, NULL, 0, tag, 16, buf, buf);
    assert(rc != 0 && g_real_calls == 0 && all(buf, sizeof(buf), 0));

    // A key the driver won't take: nothing written, fallback.
    reset();
    memset(buf, 0x11, sizeof(buf));
    g_fail_setkey = 1;
    assert(seal_in_place(buf, 64, tag) == 0 && g_real_calls == 1);

    // The cache is keyed by H: no AES key in it, tables reused per key, and
    // fast_gcm_forget_all() wipes it.
    reset();
    esp_gcm_context other;
    esp_aes_gcm_init(&other);
    uint8_t key2[32];
    for (int i = 0; i < 32; i++) key2[i] = (uint8_t)(0x90 + i);
    esp_aes_gcm_setkey(&other, 0, key2, 256);
    int tables = s_g8_count;
    uint8_t one[64] = {0};
    assert(__wrap_esp_aes_gcm_crypt_and_tag(&other, ESP_AES_ENCRYPT, 64, g_iv, 12, NULL, 0,
                                            one, one, 16, tag) == 0);
    int after_first = s_g8_count;
    assert(__wrap_esp_aes_gcm_crypt_and_tag(&other, ESP_AES_ENCRYPT, 64, g_iv, 12, NULL, 0,
                                            one, one, 16, tag) == 0);
    assert(s_g8_count == after_first && after_first <= tables + 1);   // reused
    assert(!cache_holds(key2, 16) && !cache_holds(g_ctx.aes_ctx.key, 16));
    fast_gcm_forget_all();
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        assert(!s_cache[i].used && s_cache[i].g4.t[1][0] == 0);
    }
    assert(seal_in_place(buf, 64, tag) == 0);   // still works afterwards

    puts("fast_gcm ok");
    return 0;
}
