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
#if defined(__GNUC__)
#pragma GCC system_header
#endif

/* ESP-IDF's libc and macOS supply strlcpy/strlcat; glibc versions commonly used by
 * devservers do not. Map production calls to the simulator implementation on
 * hosts that need it. macOS keeps its fortified libc macro and native symbol. */
#include_next <string.h>

#include <stddef.h>

#if !defined(__APPLE__)
#ifdef __cplusplus
extern "C" {
#endif

size_t muse_sim_strlcpy(char *destination, const char *source, size_t size);
size_t muse_sim_strlcat(char *destination, const char *source, size_t size);

#ifdef __cplusplus
}
#endif

#define strlcpy muse_sim_strlcpy
#define strlcat muse_sim_strlcat
#endif
