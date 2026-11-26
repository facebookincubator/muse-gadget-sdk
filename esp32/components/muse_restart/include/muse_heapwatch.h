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

/* Bench diagnostics (CONFIG_MUSE_HEAP_WATCH): internal-heap lows as they
 * happen, and each task's heap peaks. No-ops when the option is off. */
#ifdef __cplusplus
extern "C" {
#endif

void muse_heapwatch_start(void);
/* Every task's current and peak heap use, per region, on stdout. */
void muse_heapwatch_print_tasks(void);
/* The internal allocations of 512 B or more (time, size, freed, return
 * addresses), on stdout. */
void muse_heapwatch_print_allocs(void);

#ifdef __cplusplus
}
#endif
