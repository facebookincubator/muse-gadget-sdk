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
 * The optional M5Stack multi-app demo UI (CONFIG_MUSE_APPS_DEMO): a launcher and
 * three apps (Pet World, a diagnostics debugger, a hardware/SDK demo) drawn to a
 * 320x240 RGB565 framebuffer shown as the face content, in place of the avatar.
 *
 * It plugs into the normal muse_ui flow rather than replacing it: muse_ui's
 * frame_tick still runs sleep, brightness, the pairing prompt and the settings
 * menu first, then calls muse_apps_demo_tick() to render the apps. The pairing
 * and menu overlays live on the top LVGL layer, so they draw over this canvas.
 *
 * muse_apps_demo_build() creates the canvas as a child of `parent` (the face),
 * wires touch to the app manager, and returns it. It returns NULL if the app
 * framebuffer couldn't be allocated, in which case muse_ui falls back to the
 * avatar. muse_apps_demo_tick() renders one frame; it is a no-op if build failed.
 */
lv_obj_t *muse_apps_demo_build(lv_obj_t *parent, lv_indev_t *indev);
void muse_apps_demo_tick(void);

#ifdef __cplusplus
}
#endif
