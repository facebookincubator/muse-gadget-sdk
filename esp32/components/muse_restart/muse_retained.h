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

#include "esp_attr.h"
#include "sdkconfig.h"

/*
 * Where the restart mark, the crash record and the boot guard's record live:
 * RTC memory, kept across every reset. A chip without RTC memory (the
 * ESP32-C61), and a power-test image (CONFIG_MUSE_POWER_TEST, whose own state
 * fills the RTC slow segment), keep them in internal RAM that a reset doesn't
 * clear instead: they survive a software restart, a panic and a watchdog, not
 * deep sleep or a power cut.
 */
#if (CONFIG_SOC_RTC_FAST_MEM_SUPPORTED || CONFIG_SOC_RTC_SLOW_MEM_SUPPORTED) && !CONFIG_MUSE_POWER_TEST
#define MUSE_RETAINED_ATTR RTC_NOINIT_ATTR
#else
#define MUSE_RETAINED_ATTR __NOINIT_ATTR
#endif
