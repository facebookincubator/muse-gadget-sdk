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

#ifdef __cplusplus
}
#endif
