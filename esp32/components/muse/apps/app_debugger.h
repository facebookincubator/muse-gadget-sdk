/*
 * Interactive Diagnostics & Step-by-Step Debugger App
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_debugger_init(void);
void app_debugger_update(uint32_t now_ms, int mode, float audio_level, const char *caption);
void app_debugger_render(uint16_t *fb, uint32_t now_ms);
void app_debugger_touch_down(int x, int y);
void app_debugger_touch_move(int x, int y);
void app_debugger_touch_up(void);

#ifdef __cplusplus
}
#endif
