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

#pragma once

#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_RETENTION 8

/* The harness's "h" command sets what these report. */
size_t heap_caps_get_free_size(int caps);
size_t heap_caps_get_minimum_free_size(int caps);
size_t heap_caps_get_largest_free_block(int caps);

static inline void *heap_caps_calloc(size_t n, size_t size, int caps) {
    (void)caps;
    return calloc(n, size);
}
