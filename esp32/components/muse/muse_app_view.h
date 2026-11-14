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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The screen an app runs on (CONFIG_MUSE_APP_VIEW; WebAssembly apps run in
 * components/muse_apps). While it's open, a full-screen root object covers
 * the Muse UI (the pairing card stays above it), the app builds its LVGL
 * objects under the root, and it gets touch, gestures and the buttons by
 * role. The power key (or the exit button held 2 s), a voice call or the video
 * view closes it.
 */

/* What the app gets (the apps runtime checks these match components/muse_apps/sdk/api's EV_ and
 * BUTTON_ values). */
enum {
    MUSE_APP_EV_BUTTON = 2,     /* a = MUSE_APP_BUTTON_*, b = MUSE_APP_BTN_* */
    MUSE_APP_EV_TOUCH_DOWN = 4, /* a = x, b = y: a touch on the root, not on a clickable object */
    MUSE_APP_EV_TOUCH_MOVE = 5,
    MUSE_APP_EV_TOUCH_UP = 6,
    MUSE_APP_EV_GESTURE = 7,    /* a = MUSE_APP_GESTURE_* */
};
enum {
    MUSE_APP_BUTTON_PRIMARY = 1, /* the talk button */
    MUSE_APP_BUTTON_SECONDARY,   /* the speaker or aux button */
    MUSE_APP_BUTTON_UP,
    MUSE_APP_BUTTON_DOWN,
    MUSE_APP_BUTTON_LEFT,
    MUSE_APP_BUTTON_RIGHT,
    MUSE_APP_BUTTON_ENTER,
    MUSE_APP_BUTTON_BACK,
};
enum { MUSE_APP_BTN_DOWN = 1, MUSE_APP_BTN_UP, MUSE_APP_BTN_LONG };
enum { MUSE_APP_GESTURE_LEFT = 1, MUSE_APP_GESTURE_RIGHT, MUSE_APP_GESTURE_UP, MUSE_APP_GESTURE_DOWN };
/* Why the view asks the app to close. */
enum { MUSE_APP_STOP_USER = 1, MUSE_APP_STOP_CALL = 3, MUSE_APP_STOP_VIDEO = 4 };

typedef struct {
    /* Touch and gestures (LVGL's task), buttons (the input task or a timer):
     * copy and return. */
    void (*input)(int type, int a, int b);
    /* Close the app: the power key or the exit hold, or a call or video starting (any task). */
    void (*exit)(int reason);
} muse_app_view_cbs_t;

/* From the app's task, without LVGL's lock: open creates the root (false
 * before the UI is up), close deletes it and everything under it. */
bool muse_app_view_open(const muse_app_view_cbs_t *cbs);
void muse_app_view_close(void);
bool muse_app_view_is_open(void);

/* LVGL's lock, for building under the root from the app's task. */
void muse_app_view_lock(void);
void muse_app_view_unlock(void);
/* The root, with the lock held; NULL while closed. */
lv_obj_t *muse_app_view_root(void);

/* While on, the talk key records for Muse rather than going to the app. */
void muse_app_view_talk_to_muse(bool on);
void muse_app_view_size(int *w, int *h, bool *round);
/* A 2 s hold of PRIMARY (the talk button, physical or touch) closes the app
 * or launcher, unless the app has given it back to Muse. Its release after
 * the hold reaches nobody. */
int muse_app_view_exit_button(void);

/* Which buttons this board reports, as 1 << MUSE_APP_BUTTON_*. */
unsigned muse_app_view_buttons_present(void);

/* Keypad navigation for the app (its ui_navigation): while set, fn gets each
 * press of UP, DOWN, LEFT, RIGHT, ENTER and BACK (MUSE_APP_BUTTON_*) before
 * the app, which doesn't see the ones it takes (returns true for). Called on
 * the input task. NULL turns it off; closing the view does too. */
typedef bool (*muse_app_view_nav_fn)(int button);
void muse_app_view_set_nav(muse_app_view_nav_fn fn);

/* What opens the apps' launcher when the user picks Apps in Settings (from
 * LVGL's task: copy and return), and Settings asking for it. False if nothing
 * is set. */
void muse_app_view_set_home(void (*open)(void));
bool muse_app_view_home(void);

/* A call or the video view is starting: closes the app first. */
void muse_app_view_preempt(int reason);

/* Muse UI hooks. tick: LVGL's task, each frame; true while the view covers
 * the face. buttons: the input task; returns the edges it didn't take.
 * power: true if it took the power key to close the app. */
bool muse_app_view_tick(void);
unsigned muse_app_view_buttons(unsigned ev);
bool muse_app_view_power(void);

#ifdef __cplusplus
}
#endif
