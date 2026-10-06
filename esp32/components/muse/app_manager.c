/*
 * M5Stack Core2 Multi-App Manager Implementation
 */

#include "app_manager.h"
#include "avatar/pet_world.h"
#include "apps/app_launcher.h"
#include "apps/app_debugger.h"
#include "apps/app_demo.h"
#include "esp_log.h"

static const char *TAG = "app_manager";
static app_id_t s_current_app = APP_LAUNCHER;

void app_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing Multi-App System...");
    app_launcher_init();
    pet_world_init();
    app_debugger_init();
    app_demo_init();
    s_current_app = APP_LAUNCHER;
}

void app_manager_set_app(app_id_t app)
{
    if (app < APP_COUNT) {
        ESP_LOGI(TAG, "Switching to App %d", (int)app);
        s_current_app = app;
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
