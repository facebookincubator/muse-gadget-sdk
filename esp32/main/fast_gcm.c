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

#include "sdkconfig.h"

#if CONFIG_HOMEHUB_FAST_GCM

#include "fast_gcm.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "aes/esp_aes.h"
#include "aes/esp_aes_gcm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/platform_util.h"
#include "psa/crypto.h"

#include "ghash32.h"

int __real_esp_aes_gcm_crypt_and_tag(esp_gcm_context *ctx, int mode, size_t length,
                                     const unsigned char *iv, size_t iv_len,
                                     const unsigned char *aad, size_t aad_len,
                                     const unsigned char *input, unsigned char *output,
                                     size_t tag_len, unsigned char *tag);
int __real_esp_aes_gcm_auth_decrypt(esp_gcm_context *ctx, size_t length,
                                    const unsigned char *iv, size_t iv_len,
                                    const unsigned char *aad, size_t aad_len,
                                    const unsigned char *tag, size_t tag_len,
                                    const unsigned char *input, unsigned char *output);

static fast_gcm_mode_t s_mode = (fast_gcm_mode_t)CONFIG_HOMEHUB_FAST_GCM_DEFAULT_MODE;
static atomic_uint s_fast, s_fallback;

static const char *TAG = "link.fast_gcm";

// Per-key state: the AES key schedule lives in the engine, so only H and its
// tables are worth keeping. A TLS connection and the Noise session use two
// keys each; four entries cover both. 8-bit tables are allocated on first use,
// up to CONFIG_HOMEHUB_FAST_GCM_G8_TABLES; entries without one use 4-bit
// tables.
//
// Entries are found by H = AES_K(0^128), which every operation computes
// anyway, so the cache never holds an AES key: a RAM dump after a TLS client
// closes shows at most H and its tables (GHASH keys, which can't decrypt).
// They are wiped on eviction and by fast_gcm_forget_all().
#define CACHE_ENTRIES 4
typedef struct {
    bool used;
    uint32_t last_use;
    uint8_t h[16];
    ghash4_key_t g4;
    ghash8_key_t *g8;     // internal RAM, NULL until the G8 mode needs it
} key_entry_t;

static key_entry_t s_cache[CACHE_ENTRIES];
// Internal-RAM staging for PSRAM data (see fast_gcm()). Whole AES blocks;
// guarded by s_lock.
#if CONFIG_SPIRAM
// Whole AES blocks, so the CTR counter and GHASH state carry across chunks.
_Static_assert(CONFIG_HOMEHUB_FAST_GCM_BOUNCE_BYTES % 16 == 0,
               "CONFIG_HOMEHUB_FAST_GCM_BOUNCE_BYTES must be a multiple of 16");
static uint8_t s_bounce[CONFIG_HOMEHUB_FAST_GCM_BOUNCE_BYTES] __attribute__((aligned(32)));
#endif
static uint32_t s_g8_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
static int s_g8_count;   // 8-bit tables allocated (they stay with their entry)
static uint32_t s_tick;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

void fast_gcm_set_mode(fast_gcm_mode_t mode) { s_mode = mode; }
fast_gcm_mode_t fast_gcm_get_mode(void) { return s_mode; }
void fast_gcm_set_table_psram(bool psram) {
    s_g8_caps = MALLOC_CAP_8BIT | (psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        heap_caps_free(s_cache[i].g8);
        s_cache[i].g8 = NULL;
    }
    s_g8_count = 0;
}

// Wipe one entry's key, H and tables (its 8-bit table stays allocated).
static void entry_wipe(key_entry_t *e) {
    ghash8_key_t *g8 = e->g8;
    if (g8) mbedtls_platform_zeroize(g8, sizeof(*g8));
    mbedtls_platform_zeroize(e, sizeof(*e));
    e->g8 = g8;
}

void fast_gcm_forget_all(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CACHE_ENTRIES; i++) entry_wipe(&s_cache[i]);
    xSemaphoreGive(s_lock);
}

void fast_gcm_counts(unsigned *fast, unsigned *fallback) {
    if (fast) *fast = atomic_load(&s_fast);
    if (fallback) *fallback = atomic_load(&s_fallback);
}

// The entry for H, made from the least recently used one on a miss.
static key_entry_t *lookup(const uint8_t h[16]) {
    key_entry_t *victim = &s_cache[0];
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        key_entry_t *e = &s_cache[i];
        if (e->used && memcmp(e->h, h, 16) == 0) {
            e->last_use = ++s_tick;
            return e;
        }
        if (!e->used || (victim->used && e->last_use < victim->last_use)) victim = e;
    }
    entry_wipe(victim);
    memcpy(victim->h, h, 16);
    victim->used = true;
    victim->last_use = ++s_tick;
    ghash4_init(&victim->g4, victim->h);
    if (victim->g8) ghash8_init(victim->g8, victim->h);
    return victim;
}

static void ghash(const key_entry_t *e, bool g8, uint8_t y[16], const uint8_t *x, size_t len) {
    if (g8) ghash8_update(e->g8, y, x, len);
    else ghash4_update(&e->g4, y, x, len);
}

// fast_gcm() failed before writing to output: the caller can run IDF's GCM
// on the untouched buffers instead. Any other non-zero result is final.
#define FAST_GCM_NOT_STARTED 1

// One-shot GCM with a 96-bit IV. Encrypt: output = CTR(input), tag over
// output. Decrypt: tag over input (before an in-place CTR overwrites it).
//
// Once any of output has been written, a failure must not fall back: for an
// in-place seal (TLS records and Noise both seal in place) the blocks already
// encrypted would be encrypted again by the fallback, turning them back into
// plaintext under a valid tag. So such a failure wipes output and is
// returned as is.
static int fast_gcm(esp_gcm_context *ctx, int mode, size_t length,
                    const uint8_t *iv, const uint8_t *aad, size_t aad_len,
                    const uint8_t *input, uint8_t *output, uint8_t tag[16]) {
    if (!s_lock) return FAST_GCM_NOT_STARTED;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_aes_context aes;
    esp_aes_init(&aes);
    static const uint8_t zero[16];
    uint8_t h[16];
    int rc = esp_aes_setkey(&aes, ctx->aes_ctx.key, ctx->aes_ctx.key_bytes * 8);
    if (rc == 0) rc = esp_aes_crypt_ecb(&aes, ESP_AES_ENCRYPT, zero, h);   // H = AES_K(0^128)
    if (rc != 0) {
        esp_aes_free(&aes);
        mbedtls_platform_zeroize(h, sizeof(h));
        xSemaphoreGive(s_lock);
        return FAST_GCM_NOT_STARTED;
    }
    key_entry_t *e = lookup(h);
    mbedtls_platform_zeroize(h, sizeof(h));
    bool g8 = s_mode == FAST_GCM_G8;
    if (g8 && !e->g8 && s_g8_count < CONFIG_HOMEHUB_FAST_GCM_G8_TABLES) {
        e->g8 = heap_caps_malloc(sizeof(ghash8_key_t), s_g8_caps);
        if (e->g8) {
            s_g8_count++;
            ghash8_init(e->g8, e->h);
        }
    }
    if (g8 && !e->g8) g8 = false;   // no room: 4-bit tables

    uint8_t j0[16], ctr[16], stream[16], y[16] = {0}, s[16];
    memcpy(j0, iv, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    memcpy(ctr, j0, 16);
    ctr[15] = 2;

    ghash(e, g8, y, aad, aad_len);

    size_t nc_off = 0;
    bool decrypt = mode == ESP_AES_DECRYPT;
    bool touched = false;   // output written: no fallback from here on
#if CONFIG_SPIRAM
    bool staged = esp_ptr_external_ram(input) || esp_ptr_external_ram(output);
#else
    bool staged = false;
#endif
    if (rc == 0 && length && !staged) {
        // Internal RAM: DMA straight from and to the caller's buffers.
        if (decrypt) ghash(e, g8, y, input, length);
        touched = true;   // the DMA may have written part of output on failure
        rc = esp_aes_crypt_ctr(&aes, length, &nc_off, ctr, stream, input, output);
        if (rc == 0 && !decrypt) ghash(e, g8, y, output, length);
    }
#if CONFIG_SPIRAM
    else if (rc == 0 && length) {
        // PSRAM: run each chunk through an internal buffer. The AES DMA to
        // and from PSRAM is ~6x slower than to internal RAM plus two CPU
        // copies, and GHASH reads the chunk while it is in internal RAM.
        // Chunks are whole blocks, so the CTR and GHASH state carry over.
        for (size_t off = 0; off < length && rc == 0; off += sizeof(s_bounce)) {
            size_t n = length - off;
            if (n > sizeof(s_bounce)) n = sizeof(s_bounce);
            memcpy(s_bounce, input + off, n);
            if (decrypt) ghash(e, g8, y, s_bounce, n);
            rc = esp_aes_crypt_ctr(&aes, n, &nc_off, ctr, stream, s_bounce, s_bounce);
            if (rc != 0) break;
            if (!decrypt) ghash(e, g8, y, s_bounce, n);
            touched = true;
            memcpy(output + off, s_bounce, n);
        }
        mbedtls_platform_zeroize(s_bounce, sizeof(s_bounce));
    }
#endif
    if (rc == 0) rc = esp_aes_crypt_ecb(&aes, ESP_AES_ENCRYPT, j0, s);
    esp_aes_free(&aes);
    if (rc != 0) {
        xSemaphoreGive(s_lock);
        mbedtls_platform_zeroize(stream, sizeof(stream));
        if (!touched) return FAST_GCM_NOT_STARTED;
        if (length) mbedtls_platform_zeroize(output, length);
        return rc < 0 ? rc : -1;
    }

    uint8_t lens[16];
    uint64_t abits = (uint64_t)aad_len * 8, cbits = (uint64_t)length * 8;
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(abits >> (56 - 8 * i));
        lens[8 + i] = (uint8_t)(cbits >> (56 - 8 * i));
    }
    ghash(e, g8, y, lens, 16);
    // Held to here so no other operation evicts this entry's tables mid-use.
    // The core is single and the AES engine serialized anyway.
    xSemaphoreGive(s_lock);
    for (int i = 0; i < 16; i++) tag[i] = s[i] ^ y[i];
    mbedtls_platform_zeroize(stream, sizeof(stream));
    mbedtls_platform_zeroize(s, sizeof(s));
    return 0;
}

static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static bool ensure_lock(void) {
    if (!s_lock) {
        // First use can race between tasks: create the mutex exactly once.
        taskENTER_CRITICAL(&s_init_mux);
        if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        taskEXIT_CRITICAL(&s_init_mux);
    }
    return s_lock != NULL;
}

// Self-test against IDF's GCM before the first operation goes through the
// fast path: encrypt and decrypt in internal RAM and, with PSRAM, across the
// staging buffer's chunk boundaries, comparing ciphertext and tag with
// __real_esp_aes_gcm_crypt_and_tag. Any difference turns the fast path off
// for good (IDF's GCM serves everything) and is logged.
enum { SELFTEST_NOT_RUN, SELFTEST_RUNNING, SELFTEST_PASSED, SELFTEST_FAILED };
static int s_selftest = SELFTEST_NOT_RUN;

static bool self_test_case(esp_gcm_context *ctx, uint8_t *buf, size_t n) {
    static const uint8_t iv[12] = {0xca, 0xfe, 0xba, 0xbe, 0xfa, 0xce, 0xdb, 0xad,
                                   0xde, 0xca, 0xf8, 0x88};
    uint8_t aad[20];
    for (size_t i = 0; i < sizeof(aad); i++) aad[i] = (uint8_t)(0xfe - i);
    uint8_t *plain = buf, *fast = buf + n, *ref = buf + 2 * n;
    for (size_t i = 0; i < n; i++) plain[i] = (uint8_t)(i * 7 + 3);
    uint8_t tag_fast[16], tag_ref[16], tag_dec[16];
    if (fast_gcm(ctx, ESP_AES_ENCRYPT, n, iv, aad, sizeof(aad), plain, fast, tag_fast) != 0
        || __real_esp_aes_gcm_crypt_and_tag(ctx, ESP_AES_ENCRYPT, n, iv, sizeof(iv), aad,
                                            sizeof(aad), plain, ref, 16, tag_ref) != 0) {
        return false;
    }
    bool ok = memcmp(fast, ref, n) == 0 && memcmp(tag_fast, tag_ref, 16) == 0;
    // Decrypt IDF's ciphertext with the fast path: the plaintext and tag again.
    ok = ok && fast_gcm(ctx, ESP_AES_DECRYPT, n, iv, aad, sizeof(aad), ref, fast, tag_dec) == 0
         && memcmp(fast, plain, n) == 0 && memcmp(tag_dec, tag_ref, 16) == 0;
    return ok;
}

bool fast_gcm_self_test(void) {
    taskENTER_CRITICAL(&s_init_mux);
    int state = s_selftest;
    if (state == SELFTEST_NOT_RUN) s_selftest = SELFTEST_RUNNING;
    taskEXIT_CRITICAL(&s_init_mux);
    if (state == SELFTEST_PASSED) return true;
    if (state != SELFTEST_NOT_RUN) return false;   // failed, or running elsewhere
    bool ok = ensure_lock();

    esp_gcm_context ctx;
    esp_aes_gcm_init(&ctx);
    uint8_t key[32];
    for (size_t i = 0; i < sizeof(key); i++) key[i] = (uint8_t)(0x10 + 3 * i);
    ok = ok && esp_aes_gcm_setkey(&ctx, 0, key, 256) == 0;

    // Internal RAM: one partial block past a few whole ones.
    const size_t small = 61;
    uint8_t *ibuf = heap_caps_malloc(3 * small, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ok = ok && ibuf && self_test_case(&ctx, ibuf, small);
    heap_caps_free(ibuf);
#if CONFIG_SPIRAM
    // PSRAM: through the staging buffer, over two chunk boundaries.
    const size_t big = 2 * sizeof(s_bounce) + 45;
    uint8_t *pbuf = heap_caps_malloc(3 * big, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pbuf) {   // no PSRAM at all: nothing will be staged either
        ok = ok && self_test_case(&ctx, pbuf, big);
        heap_caps_free(pbuf);
    }
#endif
    esp_aes_gcm_free(&ctx);
    mbedtls_platform_zeroize(key, sizeof(key));
    fast_gcm_forget_all();   // the test key's H and tables

    if (ok) {
        ESP_LOGI(TAG, "self-test against ESP-IDF's GCM passed");
    } else {
        s_mode = FAST_GCM_OFF;
        ESP_LOGE(TAG, "self-test against ESP-IDF's GCM FAILED: using ESP-IDF's GCM");
    }
    s_selftest = ok ? SELFTEST_PASSED : SELFTEST_FAILED;
    return ok;
}

static bool use_fast(size_t iv_len) {
    if (s_mode == FAST_GCM_OFF || iv_len != 12) return false;
    if (!ensure_lock()) return false;
    // Until the self-test has passed (it runs once, on the first operation,
    // which is at boot), operations take IDF's path.
    return fast_gcm_self_test() && s_mode != FAST_GCM_OFF;
}

int __wrap_esp_aes_gcm_crypt_and_tag(esp_gcm_context *ctx, int mode, size_t length,
                                     const unsigned char *iv, size_t iv_len,
                                     const unsigned char *aad, size_t aad_len,
                                     const unsigned char *input, unsigned char *output,
                                     size_t tag_len, unsigned char *tag) {
    if (ctx && iv && tag && tag_len >= 4 && tag_len <= 16 && use_fast(iv_len)
        && (aad_len == 0 || aad) && (length == 0 || (input && output))) {
        uint8_t full[16];
        int rc = fast_gcm(ctx, mode, length, iv, aad, aad_len, input, output, full);
        if (rc == 0) {
            memcpy(tag, full, tag_len);
            atomic_fetch_add(&s_fast, 1);
            return 0;
        }
        if (rc != FAST_GCM_NOT_STARTED) return rc;   // output written: see fast_gcm()
    }
    atomic_fetch_add(&s_fallback, 1);
    return __real_esp_aes_gcm_crypt_and_tag(ctx, mode, length, iv, iv_len, aad, aad_len,
                                            input, output, tag_len, tag);
}

int __wrap_esp_aes_gcm_auth_decrypt(esp_gcm_context *ctx, size_t length,
                                    const unsigned char *iv, size_t iv_len,
                                    const unsigned char *aad, size_t aad_len,
                                    const unsigned char *tag, size_t tag_len,
                                    const unsigned char *input, unsigned char *output) {
    if (ctx && iv && tag && tag_len >= 4 && tag_len <= 16 && use_fast(iv_len)
        && (aad_len == 0 || aad) && (length == 0 || (input && output))) {
        uint8_t check[16];
        int rc = fast_gcm(ctx, ESP_AES_DECRYPT, length, iv, aad, aad_len, input, output, check);
        if (rc == 0) {
            atomic_fetch_add(&s_fast, 1);
            uint8_t diff = 0;
            for (size_t i = 0; i < tag_len; i++) diff |= tag[i] ^ check[i];
            if (diff) {
                mbedtls_platform_zeroize(output, length);
                return PSA_ERROR_INVALID_SIGNATURE;
            }
            return 0;
        }
        if (rc != FAST_GCM_NOT_STARTED) return rc;   // output written: see fast_gcm()
    }
    atomic_fetch_add(&s_fallback, 1);
    return __real_esp_aes_gcm_auth_decrypt(ctx, length, iv, iv_len, aad, aad_len, tag, tag_len,
                                           input, output);
}

#endif  // CONFIG_HOMEHUB_FAST_GCM
