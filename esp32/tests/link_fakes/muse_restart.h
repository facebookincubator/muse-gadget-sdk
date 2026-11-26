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

/* muse_restart.h for the host harnesses: a marked restart is esp_restart() here. */
#include <stdbool.h>
#include "esp_system.h"

typedef enum { MUSE_RESTART_OTHER = 1, MUSE_RESTART_CONSOLE, MUSE_RESTART_OTA, MUSE_RESTART_SETUP } muse_restart_why_t;
#define muse_restart(why) ((void)(why), esp_restart())
