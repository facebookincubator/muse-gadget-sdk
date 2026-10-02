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

/**
 * @file lv_conf.h
 * LVGL 9.5 configuration for the desktop UI simulator.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* Match the display format and refresh period used by Muse firmware. */
#define LV_COLOR_DEPTH 16
#define LV_DEF_REFR_PERIOD 15

/* The host C library gives sanitizers visibility into LVGL allocations. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* Two draw units match the SenseCAP Watcher profile. Use POSIX primitives
 * to keep the draw workers independent of SDL video shutdown. Both supported
 * desktop hosts provide pthreads. */
#define LV_USE_OS LV_OS_PTHREAD
#define LV_DRAW_SW_DRAW_UNIT_CNT 2
#define LV_DRAW_SW_COMPLEX 1

#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_OBJ 0

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_OBJ_STYLE_CACHE 1

/* Keep these in sync with devices/sdkconfig.muse and the Watcher overlay. */
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_UNSCII_8 1
#define LV_FONT_UNSCII_16 1
#define LV_FONT_DEFAULT &lv_font_montserrat_20

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1
#define LV_THEME_DEFAULT_TRANSITION_TIME 80

/* Keep the production UI's console snapshot hook available for debugging. */
#define LV_USE_SNAPSHOT 1

/* Use LVGL's software renderer and SDL only for the host window/input layer.
 * Software rendering also works with SDL_VIDEODRIVER=dummy in CI. */
#define LV_USE_DRAW_SDL 0
#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH <SDL.h>
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT 1
#define LV_SDL_ACCELERATED 0
#define LV_SDL_FULLSCREEN 0
#define LV_SDL_DIRECT_EXIT 0
#define LV_SDL_MOUSEWHEEL_MODE LV_SDL_MOUSEWHEEL_MODE_ENCODER

#endif /* LV_CONF_H */
