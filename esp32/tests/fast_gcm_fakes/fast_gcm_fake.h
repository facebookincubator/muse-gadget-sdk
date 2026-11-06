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

// Just enough of ESP-IDF's AES driver, FreeRTOS and mbedtls to compile
// main/fast_gcm.c on a host (tests/fast_gcm_harness.c). The other headers in
// this directory include this one. The AES functions are scripted by the
// harness: CTR is a XOR with a fixed byte, and any call can be made to fail.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define ESP_AES_ENCRYPT 1
#define ESP_AES_DECRYPT 0

typedef struct {
    uint8_t key_bytes;
    uint8_t key[32];
} esp_aes_context;

typedef struct {
    esp_aes_context aes_ctx;
} esp_gcm_context;

void esp_aes_gcm_init(esp_gcm_context *ctx);
int esp_aes_gcm_setkey(esp_gcm_context *ctx, int cipher, const unsigned char *key, unsigned int keybits);
void esp_aes_gcm_free(esp_gcm_context *ctx);

void esp_aes_init(esp_aes_context *ctx);
void esp_aes_free(esp_aes_context *ctx);
int esp_aes_setkey(esp_aes_context *ctx, const unsigned char *key, unsigned int keybits);
int esp_aes_crypt_ecb(esp_aes_context *ctx, int mode, const unsigned char in[16], unsigned char out[16]);
int esp_aes_crypt_ctr(esp_aes_context *ctx, size_t length, size_t *nc_off, unsigned char nonce_counter[16],
                      unsigned char stream_block[16], const unsigned char *input, unsigned char *output);

#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define heap_caps_malloc(size, caps) malloc(size)
#define heap_caps_free(p) free(p)
bool esp_ptr_external_ram(const void *p);

typedef struct { int unused; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portMAX_DELAY 0xffffffffu
static inline void taskENTER_CRITICAL(portMUX_TYPE *m) { (void)m; }
static inline void taskEXIT_CRITICAL(portMUX_TYPE *m) { (void)m; }
// Functions, not comma macros: the callers ignore the result, which GCC
// would flag as a statement with no effect under -Werror.
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buf) { return buf; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, uint32_t ticks) { (void)s; (void)ticks; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }

void mbedtls_platform_zeroize(void *buf, size_t len);

#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define PSA_ERROR_INVALID_SIGNATURE (-149)
