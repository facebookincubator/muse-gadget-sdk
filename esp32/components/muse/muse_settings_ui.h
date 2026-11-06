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

#include <stdbool.h>

#include "lvgl.h"

/*
 * The settings tile (swipe left from Muse): Wi-Fi, Hatch, Bluetooth, Sound,
 * Sleep and Power pages. Runs entirely in the LVGL task; hardware state is
 * polled from the owning modules.
 */

void muse_settings_ui_build(lv_obj_t *tile);

/* Call periodically from the LVGL task; `visible` = settings tile is showing. */
void muse_settings_ui_tick(bool visible);

/* True when a sub-page is open (the tileview must not steal horizontal swipes). */
bool muse_settings_ui_in_subpage(void);

#if LV_USE_SNAPSHOT
/* Bench screenshots: opens a settings page by name ("home", "wifi", "muse",
 * "ble", "sound", "sleep", "battery", "power") on the next tick. */
bool muse_settings_ui_bench_open(const char *name);
#endif
