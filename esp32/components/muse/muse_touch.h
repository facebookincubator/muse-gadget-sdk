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
#include <stdint.h>

#include "lvgl.h"

/*
 * The panel's touches for someone besides Muse's own screens: a phone's app
 * over musegadgets BLE (mg_command_ui_input touch events, and the gadget's
 * HID). Samples come from LVGL's own reads of the touch controller while
 * LVGL runs, and from the input task's polls while the video view has LVGL
 * paused. While a UI stream is on (muse_video_ui_active) Muse's screens see
 * no touches.
 */
typedef struct {
    /* Someone wants samples now (cheap; called on every read). */
    bool (*wanted)(void);
    /* One sample: pressed, and the point in panel pixels. */
    void (*sample)(bool pressed, uint16_t x, uint16_t y);
} muse_touch_sink_t;

/* Before the UI starts (or at any time); NULL clears it. */
void muse_touch_set_sink(const muse_touch_sink_t *sink);
/* The sink gets only the touches on what a phone may see: a UI stream's picture, or the face. While shareable()
 * says no (Settings, its keyboard, the passcode, the menu on screen) it gets none. NULL: every touch. */
void muse_touch_set_shareable(bool (*shareable)(void));
/* A passcode screen owns the panel now (the lock, its keypad or any other passcode entry): the sink gets no touch,
 * a touch under way ends there, and a finger still down when it closes stays out until it lifts. mg_ble asks it
 * too (mg_ble_platform_t.touch_private). Any task. */
bool muse_touch_private(void);
/* After the board's display_start: reads go through here. */
void muse_touch_attach(lv_indev_t *indev);
/* The touch LVGL read last came from the panel, not a bench press (muse_touch_press): for an event handler on the
 * LVGL task, whose touch it is. */
bool muse_touch_real(void);
/* The touch LVGL read last was a bench press a Nordic UART console line armed. */
bool muse_touch_press_wireless(void);
/* From the input task, every 10 ms: reads the panel itself while LVGL isn't. */
void muse_touch_poll(void);
/* Bench: a touch at (x1, y1) moved to (x2, y2) over ms, then lifted, fed to
 * the sink as if from the panel (10 ms samples, on a task of its own). */
void muse_touch_inject(int x1, int y1, int x2, int y2, int ms);
/* Bench: a press on Muse's own screen, as a finger's (muse_touch_script.h):
 * down at (x1, y1), moved to (x2, y2) over ms, lifted. LVGL and the video
 * view's poll read it in place of the panel; the sink gets none of it.
 * False without a touch screen. */
bool muse_touch_press(int x1, int y1, int x2, int y2, int ms);
/* The same along a path of n corners (2..6, one speed): a swipe through
 * keys (the passcode's tap=pass.swipe.). */
bool muse_touch_press_path(const int *xs, const int *ys, int n, int ms);

/* A bench gesture on a named control, worked out by the screen that draws
 * it (muse_home_bench_point, muse_settings_ui_bench_point,
 * the bench points): what muse_touch_press takes. The home's and
 * Sound page's run on the LVGL task (they read LVGL's objects). */
typedef struct {
    int x1, y1, x2, y2, ms;
} muse_touch_gesture_t;
#define MUSE_TOUCH_TAP_MS 80
#define MUSE_TOUCH_HOLD_MS 900    /* past LVGL's long press (400 ms) */
#define MUSE_TOUCH_DRAG_MS 400

/* A tap at the middle of obj, or false if obj isn't on the screen (hidden,
 * scrolled out, on another page). LVGL task. */
bool muse_touch_tap_obj(lv_obj_t *obj, int ms, muse_touch_gesture_t *g);
/* A drag along a 0-100 slider from its value now to value (0 and 100 run
 * past the ends, which clamp). LVGL task. */
bool muse_touch_slide_obj(lv_obj_t *slider, int value, muse_touch_gesture_t *g);
