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
#include <stddef.h>
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t c, size_t n, unsigned caps) { (void)caps; return calloc(c, n); }
static inline void heap_caps_free(void *p) { free(p); }
static inline void *heap_caps_realloc(void *p, size_t n, unsigned caps) { (void)caps; return realloc(p, n); }
static inline void *heap_caps_aligned_alloc(size_t a, size_t n, unsigned caps)
{
    (void)caps;
    void *p = NULL;
    return posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, n) ? NULL : p;
}
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 1 << 20; }
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 1 << 20; }
static inline size_t heap_caps_get_minimum_free_size(unsigned caps) { (void)caps; return 1 << 20; }
