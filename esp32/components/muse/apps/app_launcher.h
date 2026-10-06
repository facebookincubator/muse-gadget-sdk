/*
 * App Selection Menu / Launcher
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_launcher_init(void);
void app_launcher_update(uint32_t now_ms);
void app_launcher_render(uint16_t *fb, uint32_t now_ms);
void app_launcher_touch_down(int x, int y);
void app_launcher_touch_move(int x, int y);
void app_launcher_touch_up(void);

#ifdef __cplusplus
}
#endif
