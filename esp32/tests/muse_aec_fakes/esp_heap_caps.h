/* SPDX-License-Identifier: Apache-2.0 */

#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps);
