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
 * Hardware & M5Stack Core2 Showcase Demo App
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_demo_init(void);
void app_demo_update(uint32_t now_ms, float audio_level);
void app_demo_render(uint16_t *fb, uint32_t now_ms);
void app_demo_touch_down(int x, int y);
void app_demo_touch_move(int x, int y);
void app_demo_touch_up(void);
void app_demo_touch_cancel(void);

#ifdef __cplusplus
}
#endif
