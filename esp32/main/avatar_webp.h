/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t *pixels;
    int width, height;
    size_t peak_bytes;
    const char *error;
} avatar_webp_result_t;

/* Internal avatar allocation reserve, shared by admission and the ESP-IDF
 * allocator's checks. Not a new setting or an increase to the peak cap. */
#define AVATAR_WEBP_RESERVE_BYTES (1024u * 1024u)

/* One decoder at a time. Live outputs continue to count against the budget.
 * Large allocations use PSRAM exclusively on ESP-IDF. */
bool avatar_webp_decode(const uint8_t *bytes, size_t len, int max_edge,
                        size_t live_budget, avatar_webp_result_t *out);
void *muse_webp_malloc(size_t size);
void *muse_webp_calloc(size_t n, size_t size);
void muse_webp_free(void *ptr);
size_t avatar_webp_live_bytes(void);
void avatar_webp_set_budget(size_t bytes);
