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

#ifdef __cplusplus
}
#endif
