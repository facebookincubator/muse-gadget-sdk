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

/* Host fake for tests/video_slots/harness.c (tests/test_video_untrusted.py). */
#pragma once
#include <stdint.h>
typedef struct { int unused; } md5_context_t;
static inline void esp_rom_md5_init(md5_context_t *c) { (void)c; }
static inline void esp_rom_md5_update(md5_context_t *c, const void *b, uint32_t n) { (void)c, (void)b, (void)n; }
static inline void esp_rom_md5_final(uint8_t *d, md5_context_t *c) { (void)c; for (int i = 0; i < 16; i++) d[i] = 0; }
