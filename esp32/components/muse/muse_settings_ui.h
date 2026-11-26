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

#include <stdbool.h>

#include "lvgl.h"
#include "muse_touch.h"

/*
 * The settings tile (swipe left from Muse): Wi-Fi, Muse, Bluetooth, Sound,
 * Sleep and Power pages. Runs entirely in the LVGL task; hardware state is
 * polled from the owning modules.
 */

void muse_settings_ui_build(lv_obj_t *tile);

/* Call periodically from the LVGL task; `visible` = settings tile is showing. */
void muse_settings_ui_tick(bool visible);

/* True when a sub-page is open (the tileview must not steal horizontal swipes). */
bool muse_settings_ui_in_subpage(void);
/* Back to the menu's first page (the menu's 30 s idle close). */
void muse_settings_ui_go_home(void);
/* Opens a page by name ("sound", "display", "talk", "about"...): bench and renderer. */
void muse_settings_ui_open(const char *name);
/* About's "Last reset" and Developer's Restart; weak defaults (the
 * chip's reset reason, esp_restart) that the firmware's restart record may
 * replace. */
const char *muse_settings_ui_last_reset(void);
void muse_settings_ui_restart(void);
/* The page on show, for the bench: "home", "wifi", "sleep" and so on. */
const char *muse_settings_ui_page_name(void);
/* Bench: where a named control is (muse_touch.h; LVGL task): the Sound
 * page's speaker and volume<N>; back (a sub-page's title); about.fw, developer (the Settings row),
 * dev.switch, dev.tests, tests.vibration, vib.pulse<MS>, vib.<preset>,
 * vib.stop, vib.minus, vib.plus. */
bool muse_settings_ui_bench_point(const char *what, muse_touch_gesture_t *g);
/* The named control on the page on show, scrolled into view; NULL if it
 * isn't there (the simulator's press=). */
lv_obj_t *muse_settings_ui_find(const char *what);

#if LV_USE_SNAPSHOT
/* Bench screenshots (the console's ui.settings=NAME): opens a settings page
 * by name on the next tick: "home", "wifi", "muse", "ble" (or "bluetooth"),
 * "sound", "sleep" (or "display"), "battery", and muse_settings_ui_open()'s
 * other names. False for a name it doesn't know. */
bool muse_settings_ui_bench_open(const char *name);
#if CONFIG_MUSE_AVATAR_SYNC && CONFIG_MUSE_UI_THEME
/* Bench regression only: queue a click on the current Avatar/Appearance
 * primary row or back button. Executed by the LVGL owner, never the console. */
bool muse_settings_ui_bench_action(const char *action);
#endif
#endif
