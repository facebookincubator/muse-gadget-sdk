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
#include "avatar_identity.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint8_t magic[8];
    uint32_t sequence, width, height, bytes, pixels_crc;
    uint8_t binding[32];
    avatar_identity_t identity;
    uint32_t header_crc;
} avatar_cache_header_t;

uint32_t avatar_cache_crc(const void *data, size_t len);
void avatar_cache_header_init(avatar_cache_header_t *h, uint32_t sequence,
                              int w, int height, const uint8_t binding[32],
                              const avatar_identity_t *identity, const uint16_t *pixels);
bool avatar_cache_header_valid(const avatar_cache_header_t *h, const uint8_t binding[32]);
bool avatar_cache_read(const char *path, const uint8_t binding[32],
                       avatar_cache_header_t *header, void *pixels, size_t capacity);
bool avatar_cache_write(const char *path, const avatar_cache_header_t *header, const void *pixels);
bool avatar_cache_write_while(const char *path, const avatar_cache_header_t *header,
                              const void *pixels, bool (*proceed)(void *), void *ctx);
/* Small metadata checkpoint for an unchanged image. A torn checkpoint never
 * invalidates the image slots, and cannot apply to another image generation. */
bool avatar_cache_identity_write(const char *path, const char *temporary, const avatar_cache_header_t *header);
bool avatar_cache_identity_read(const char *path, avatar_cache_header_t *header);
