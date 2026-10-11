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
 * M5Stack Core2 Multi-App Manager Implementation
 */

#include "app_manager.h"
#include "apps/pet_world.h"
#include "apps/app_launcher.h"
#include "apps/app_debugger.h"
#include "apps/app_demo.h"
#include "esp_log.h"
#include "muse_ui.h"

static const char *TAG = "app_manager";
static app_id_t s_current_app = APP_LAUNCHER;

/*
 * Pet World (room swipe) and the hardware demo (particle fling) use horizontal
 * drags, which would otherwise slide the framework's tileview to the settings
 * page mid-gesture. Disable that swipe for them; the launcher and the debugger
 * are tap-only, so swipe-left to settings stays available there.
 */
static void apply_swipe(app_id_t app)
{
    muse_ui_set_swipe_enabled(app == APP_LAUNCHER || app == APP_DEBUGGER);
}

void app_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing Multi-App System...");
    app_launcher_init();
    pet_world_init();
    app_debugger_init();
    app_demo_init();
    s_current_app = APP_LAUNCHER;
    apply_swipe(s_current_app);
}

void app_manager_set_app(app_id_t app)
{
    if (app < APP_COUNT) {
        ESP_LOGI(TAG, "Switching to App %d", (int)app);
        s_current_app = app;
        apply_swipe(app);
    }
}

app_id_t app_manager_get_app(void)
{
    return s_current_app;
}

void app_manager_open_launcher(void)
{
    ESP_LOGI(TAG, "Returning to App Launcher");
    s_current_app = APP_LAUNCHER;
    apply_swipe(s_current_app);
}

void app_manager_update(uint32_t now_ms, int mode, float audio_level, const char *caption)
{
    // Always keep Pet World simulated in background so hunger/energy & speech work
    pet_world_update(now_ms, mode, audio_level, caption);

    switch (s_current_app) {
    case APP_LAUNCHER:
        app_launcher_update(now_ms);
        break;
    case APP_DEBUGGER:
        app_debugger_update(now_ms, mode, audio_level, caption);
        break;
    case APP_DEMO:
        app_demo_update(now_ms, audio_level);
        break;
    case APP_PET:
    default:
        break;
    }
}

void app_manager_render(uint16_t *fb, uint32_t now_ms)
{
    switch (s_current_app) {
    case APP_LAUNCHER:
        app_launcher_render(fb, now_ms);
        break;
    case APP_DEBUGGER:
        app_debugger_render(fb, now_ms);
        break;
    case APP_DEMO:
        app_demo_render(fb, now_ms);
        break;
    case APP_PET:
    default:
        pet_world_render(fb, now_ms);
        break;
    }
}

void app_manager_touch_down(int x, int y)
{
    switch (s_current_app) {
    case APP_LAUNCHER:
        app_launcher_touch_down(x, y);
        break;
    case APP_DEBUGGER:
        app_debugger_touch_down(x, y);
        break;
    case APP_DEMO:
        app_demo_touch_down(x, y);
        break;
    case APP_PET:
    default:
        pet_world_touch_down(x, y);
        break;
    }
}

void app_manager_touch_move(int x, int y)
{
    switch (s_current_app) {
    case APP_LAUNCHER:
        app_launcher_touch_move(x, y);
        break;
    case APP_DEBUGGER:
        app_debugger_touch_move(x, y);
        break;
    case APP_DEMO:
        app_demo_touch_move(x, y);
        break;
    case APP_PET:
    default:
        pet_world_touch_move(x, y);
        break;
    }
}

void app_manager_touch_up(void)
{
    switch (s_current_app) {
    case APP_LAUNCHER:
        app_launcher_touch_up();
        break;
    case APP_DEBUGGER:
        app_debugger_touch_up();
        break;
    case APP_DEMO:
        app_demo_touch_up();
        break;
    case APP_PET:
    default:
        pet_world_touch_up();
        break;
    }
}

void app_manager_touch_cancel(void)
{
    switch (s_current_app) {
    case APP_LAUNCHER:
        app_launcher_touch_cancel();
        break;
    case APP_DEBUGGER:
        app_debugger_touch_cancel();
        break;
    case APP_DEMO:
        app_demo_touch_cancel();
        break;
    case APP_PET:
    default:
        pet_world_touch_up();   /* just ends any room-swipe drag; no action */
        break;
    }
}
