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
 * LVGL's profiler hooks (CONFIG_LV_USE_PROFILER, with LV_PROFILER_INCLUDE
 * naming this header), for the bench's mg.perf: each tagged LVGL function's
 * time is summed while muse_lv_prof_on is set, and LVGL's software draw
 * tasks are split by type ("draw:fill", "draw:image"...). Off, each hook
 * costs a call and a test. Included by LVGL itself, so plain C, no LVGL types.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void muse_lv_prof_begin(const char *tag);
void muse_lv_prof_end(const char *tag);

/* Starts a fresh tally (true) or stops counting (false). */
void muse_lv_prof_enable(bool on);
bool muse_lv_prof_enabled(void);
/* The `top` biggest items as " tag 12.3 ms/frame (n)", busiest first. */
void muse_lv_prof_report(char *buf, size_t cap, int top, int frames);

#ifdef __cplusplus
}
#endif

#define LV_PROFILER_BUILTIN_BEGIN_TAG(tag) muse_lv_prof_begin(tag)
#define LV_PROFILER_BUILTIN_END_TAG(tag) muse_lv_prof_end(tag)
#define LV_PROFILER_BUILTIN_BEGIN LV_PROFILER_BUILTIN_BEGIN_TAG(__func__)
#define LV_PROFILER_BUILTIN_END LV_PROFILER_BUILTIN_END_TAG(__func__)
