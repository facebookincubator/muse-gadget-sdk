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
#include <assert.h>
#include <stdio.h>
// Portable host CSPRNG; firmware continues using esp_fill_random on the board.
#if TEST_COUNT_RNG_DRAWS
extern int test_rng_draws;   // every draw, so a refused operation can show it drew nothing
#endif
static inline void esp_fill_random(void *p, size_t n) {
#if TEST_COUNT_RNG_DRAWS
    test_rng_draws++;
#endif
    FILE *random = fopen("/dev/urandom", "rb");
    assert(random && fread(p, 1, n, random) == n);
    fclose(random);
}
