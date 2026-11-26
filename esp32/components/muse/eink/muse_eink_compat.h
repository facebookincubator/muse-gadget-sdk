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

/* The e-paper (FreeInk) builds have no LVGL, so none of the LVGL UI's touch,
 * home or passcode screens (muse_touch.h, muse_home.h, muse_passcode_ui.h,
 * muse_settings_ui.h). What muse_input.c's bench words reach of them
 * answers here that there's nothing on the screen to press. */

#include <stdbool.h>

typedef struct {
    int x1, y1, x2, y2, ms;
} muse_touch_gesture_t;
#define MUSE_TOUCH_TAP_MS 80

static inline void muse_touch_poll(void) {}
static inline void muse_touch_inject(int x1, int y1, int x2, int y2, int ms)
{
    (void)x1; (void)y1; (void)x2; (void)y2; (void)ms;
}
static inline bool muse_touch_press(int x1, int y1, int x2, int y2, int ms)
{
    (void)x1; (void)y1; (void)x2; (void)y2; (void)ms;
    return false;
}
static inline bool muse_touch_press_path(const int *xs, const int *ys, int n, int ms)
{
    (void)xs; (void)ys; (void)n; (void)ms;
    return false;
}
static inline bool muse_home_bench_point(const char *what, muse_touch_gesture_t *g)
{
    (void)what; (void)g;
    return false;
}
static inline bool muse_settings_ui_bench_point(const char *what, muse_touch_gesture_t *g)
{
    (void)what; (void)g;
    return false;
}
static inline bool muse_settings_ui_bench_open(const char *name)
{
    (void)name;
    return false;
}
static inline int muse_passcode_ui_bench_path(const char *keys, int *xs, int *ys, int max)
{
    (void)keys; (void)xs; (void)ys; (void)max;
    return 0;
}
static inline bool muse_passcode_ui_bench_point(const char *what, muse_touch_gesture_t *g)
{
    (void)what; (void)g;
    return false;
}
