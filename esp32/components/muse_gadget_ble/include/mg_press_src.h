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
 * Where a press came from, for start_mic's press rule (mgcommands.h, start_mic): only one the device itself
 * detected (a finger on the panel, a physical key, its own wake event) lets a client open the mic on a release
 * build. A console or Nordic UART word, an SMP or protocol command, a charger plug-in: INJECTED, never.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MG_PRESS_SRC_NONE 0
#define MG_PRESS_SRC_PANEL 1      /* a finger on the panel */
#define MG_PRESS_SRC_KEY 2        /* a physical key */
#define MG_PRESS_SRC_WAKE 3       /* the device's own wake event (none on ESP32 today) */
#define MG_PRESS_SRC_INJECTED 4   /* a console, Nordic UART, SMP or protocol source */

#define MG_PRESS_WINDOW_MS 5000   /* a press opens the mic to start_mic for this long (mg-session-v1.json mic_gate) */

static inline bool mg_press_src_physical(uint8_t src)
{
    return src == MG_PRESS_SRC_PANEL || src == MG_PRESS_SRC_KEY || src == MG_PRESS_SRC_WAKE;
}
