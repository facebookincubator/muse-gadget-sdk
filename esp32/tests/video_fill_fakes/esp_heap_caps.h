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
/* Host fake for tests/video_fill_harness.c. */
#pragma once

#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)

static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t c, size_t n, unsigned caps) { (void)caps; return calloc(c, n); }
static inline void *heap_caps_aligned_alloc(size_t align, size_t n, unsigned caps)
{
    (void)caps;
    return aligned_alloc(align, (n + align - 1) / align * align);
}
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 1 << 20; }
