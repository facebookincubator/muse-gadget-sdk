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

/*
 * WAMR's linear memory, from muse_apps.c to muse_apps_wamr_mem.c (built into
 * WAMR's library in place of its ESP-IDF memory-map layer). Plain C types only:
 * both sides include it.
 */

#pragma once

#include <stddef.h>

/* size bytes, zeroed, in PSRAM and the running app's budget; NULL if not. */
void *muse_apps_linear_map(size_t size);
/* old (old_size bytes, NULL for none) grown to new_size: the new bytes zeroed,
 * the old block freed; NULL (old kept) if not. */
void *muse_apps_linear_remap(void *old, size_t old_size, size_t new_size);
void muse_apps_linear_unmap(void *p);
