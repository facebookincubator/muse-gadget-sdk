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
 * M5Stack Core2 Multi-App Manager
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_LAUNCHER = 0,
    APP_PET      = 1,
    APP_DEBUGGER = 2,
    APP_DEMO     = 3,
    APP_COUNT
} app_id_t;

void app_manager_init(void);
void app_manager_set_app(app_id_t app);
app_id_t app_manager_get_app(void);
void app_manager_open_launcher(void);

void app_manager_update(uint32_t now_ms, int mode, float audio_level, const char *caption);
void app_manager_render(uint16_t *fb, uint32_t now_ms);

void app_manager_touch_down(int x, int y);
void app_manager_touch_move(int x, int y);
void app_manager_touch_up(void);
void app_manager_touch_cancel(void);

#ifdef __cplusplus
}
#endif
