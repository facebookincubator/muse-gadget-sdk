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
 *
 * Host stand-in for an ESP-IDF header, so ExecuTorch's ESP-NN delegate (in
 * the pytorch/executorch ESP-IDF component) builds off the chip, for host
 * builds of the runner such as the host tests. The runner drives one
 * delegate from one thread, with no PSRAM. These headers go away once
 * ExecuTorch builds the delegate for other hosts itself.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_DMA (1u << 3)
#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_DEFAULT (1u << 12)

/* One heap, and it counts as internal: PSRAM requests fail, so the
 * delegate takes its internal-RAM path. */
static inline void *heap_caps_aligned_alloc(size_t align, size_t size, uint32_t caps)
{
    if (caps & MALLOC_CAP_SPIRAM) {
        return NULL;
    }
    return aligned_alloc(align, (size + align - 1) & ~(align - 1));
}

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    return (caps & MALLOC_CAP_SPIRAM) ? NULL : malloc(size);
}

static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    return (caps & MALLOC_CAP_SPIRAM) ? NULL : calloc(n, size);
}

static inline void heap_caps_free(void *p)
{
    free(p);
}

static inline size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    return (caps & MALLOC_CAP_SPIRAM) ? 0 : ((size_t)1 << 30);
}

static inline size_t heap_caps_get_total_size(uint32_t caps)
{
    return (caps & MALLOC_CAP_SPIRAM) ? 0 : ((size_t)1 << 30);
}

static inline size_t heap_caps_get_free_size(uint32_t caps)
{
    return heap_caps_get_total_size(caps);
}
