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

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The optional M5Stack multi-app demo UI (CONFIG_MUSE_APPS_DEMO): a launcher
 * and three apps (Pet World, a diagnostics debugger, a hardware/SDK demo)
 * rendered to a 320x240 RGB565 framebuffer shown full-screen, in place of the
 * avatar face. muse_ui_start() calls this instead of building the avatar when
 * the demo is enabled. `indev` is the board's touch device; `frame_ms` is the
 * board's animation period.
 */
void muse_apps_demo_start(lv_indev_t *indev, int frame_ms);

#ifdef __cplusplus
}
#endif
