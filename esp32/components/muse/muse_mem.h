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

#include "esp_heap_caps.h"
#include "sdkconfig.h"

/*
 * Where large, non-DMA buffers and task stacks go: PSRAM when the board has
 * it, internal RAM otherwise (the buffers shrink to match; see MUSE_LOW_MEM).
 */
#if CONFIG_SPIRAM
#define MUSE_BIG_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MUSE_LOW_MEM 0
#else
#define MUSE_BIG_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define MUSE_LOW_MEM 1
#endif

/*
 * Buffers and task stacks SIMD (PIE) code touches: main SRAM only
 * (MALLOC_CAP_DMA). The heap also hands out RTC/LP RAM as MALLOC_CAP_INTERNAL
 * once main SRAM runs low (CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP: the
 * S31's LP RAM at 0x2E000000, the S3's 8 KB of RTC fast memory), and PIE
 * loads from it fault (a decode task's stack in the S31's LP
 * RAM: a pie_load panic). On chips without SIMD (the C6, C5, C3) plain
 * internal: nothing there faults.
 * The free and largest-block checks before such an allocation use the same
 * caps.
 */
#if __has_include("soc/soc_caps.h")
#include "soc/soc_caps.h"
#endif
#if defined(SOC_SIMD_INSTRUCTION_SUPPORTED) && SOC_SIMD_INSTRUCTION_SUPPORTED
#define MUSE_SIMD_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
#else
#define MUSE_SIMD_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

/* Core for audio work: the second core when there is one. */
#if CONFIG_FREERTOS_UNICORE
#define MUSE_AUDIO_CORE 0
#else
#define MUSE_AUDIO_CORE 1
#endif

/*
 * LVGL's task on boards that start it themselves: on the audio core, just
 * below the audio task, which leaves the first core to Wi-Fi, the network and
 * the reply's decoding. Left unpinned, it stays on whichever core it first
 * touched the FPU on, usually the first.
 */
#define MUSE_UI_CORE MUSE_AUDIO_CORE
#define MUSE_UI_PRIORITY 5
