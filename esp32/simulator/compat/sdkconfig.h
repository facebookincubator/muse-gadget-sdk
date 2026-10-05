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

/* The desktop target models a PSRAM-equipped, dual-core board. */
#define CONFIG_SPIRAM 1
#define CONFIG_FREERTOS_UNICORE 0
#define CONFIG_MUSE_WATCHER_CAMERA 0
#define CONFIG_MUSE_HW_COMMANDS 0
#define CONFIG_MUSE_HATCH 1
#define CONFIG_MUSE_CONSOLE_UART 0
#define CONFIG_PM_PROFILING 0
#define CONFIG_MUSE_BOARD_SIMULATOR 1
