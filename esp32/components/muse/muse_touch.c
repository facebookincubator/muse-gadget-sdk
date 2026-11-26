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
#include "muse_touch.h"
#include "muse_input.h"
#include "muse_passcode.h"
#include "muse_passcode_ui.h"
#include "muse_touch_script.h"

#include <stdlib.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "esp_timer.h"

#if CONFIG_MUSE_VIDEO
#include "muse_video.h"
#endif

/* LVGL reads its pointer every ~30 ms while it runs; quiet this long, it isn't. */
#define LVGL_QUIET_US 100000

static const muse_touch_sink_t *volatile s_sink;
static lv_indev_t *s_indev;
static lv_indev_read_cb_t s_board_read;
static volatile int64_t s_lvgl_read_t;
static volatile bool s_injecting;      /* a bench touch is on: the panel's own samples wait */
static muse_touch_script_t s_press;    /* a bench press on Muse's own screen (muse_touch_press) */
static portMUX_TYPE s_press_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_real;                    /* the last LVGL read was the panel's (no bench press on) */
static bool s_press_wireless;          /* the bench press was armed by a Nordic UART console line */
static bool s_user_down;               /* the last read's finger (the panel's or a bench press) */
static bool s_sink_down;               /* the sink's last sample was a press */
static bool s_held;                    /* a passcode screen took the panel: nothing reaches the sink until a lift */

/* A user's touch-down (the panel's, or a bench press: automation behaves like
 * a user): link modes sleep -> ready, through the input task. */
static void user_touch(bool down)
{
    if (down && !s_user_down) {
        muse_input_user_touch();
    }
    s_user_down = down;
}

void muse_touch_set_sink(const muse_touch_sink_t *sink)
{
    s_sink = sink;
}

static bool ui_stream(void)
{
#if CONFIG_MUSE_VIDEO
    return muse_video_ui_active();
#else
    return false;
#endif
}

/* The passcode's screens own LVGL's touches: locked (a UI stream stays held
 * behind the locked face) or a keypad up. They never reach the phone. */
static bool passcode_owns(void)
{
    return muse_passcode_locked() || muse_passcode_ui_covering();
}

bool muse_touch_private(void)
{
    return passcode_owns();
}

/* A passcode screen has the panel: a touch the sink has under way ends at its last point before (one lift), and
 * the finger that's down now stays out after it closes, until it lifts. */
static void hold_back(void)
{
    const muse_touch_sink_t *sink = s_sink;
    s_held = true;
    if (s_sink_down && sink) {
        sink->sample(false, 0, 0);
    }
    s_sink_down = false;
}

static bool (*volatile s_shareable)(void);

void muse_touch_set_shareable(bool (*shareable)(void))
{
    s_shareable = shareable;
}

static void feed(const lv_indev_data_t *d)
{
    const muse_touch_sink_t *sink = s_sink;
    bool (*shareable)(void) = s_shareable;
    bool pressed = d->state == LV_INDEV_STATE_PRESSED;
    if (s_held) {
        if (pressed) {
            return;   /* the finger that was on a passcode screen: none of it reaches the sink */
        }
        s_held = false;
    }
    if (!ui_stream() && shareable && !shareable()) {
        return;   /* Settings, a keyboard, the passcode, the menu: nobody else's */
    }
    if (sink && sink->wanted && !s_injecting && sink->wanted()) {
        s_sink_down = pressed;
        sink->sample(pressed, (uint16_t)(d->point.x < 0 ? 0 : d->point.x), (uint16_t)(d->point.y < 0 ? 0 : d->point.y));
    }
}

/* LVGL's read, through the board's. A bench press (muse_touch_press) takes
 * the panel's place while it lasts, for Muse's own screens only. */
static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    int64_t now = esp_timer_get_time();
    s_lvgl_read_t = now;
    s_board_read(indev, data);
    int x = 0, y = 0;
    portENTER_CRITICAL(&s_press_mux);
    int scripted = muse_touch_script_at(&s_press, now, &x, &y);
    portEXIT_CRITICAL(&s_press_mux);
    s_real = scripted < 0;   /* this read's touch: the panel's, or a bench press */
    if (scripted >= 0) {
        user_touch(scripted != 0);
        data->state = scripted ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->point.x = (int32_t)x;
        data->point.y = (int32_t)y;
        data->continue_reading = false;
        return;   /* not the phone's: no sample for the sink */
    }
    user_touch(data->state == LV_INDEV_STATE_PRESSED);   /* a UI stream's touch too: the phone's picture takes it */
    if (passcode_owns()) {
        hold_back();
        return;
    }
    feed(data);
    if (ui_stream()) {
        data->state = LV_INDEV_STATE_RELEASED;   /* the phone's picture has the touches */
    }
}

bool muse_touch_real(void)
{
    return s_real;
}

bool muse_touch_press_wireless(void)
{
    return !s_real && s_press_wireless;
}

void muse_touch_attach(lv_indev_t *indev)
{
    if (!indev || s_indev) {
        return;
    }
    lv_indev_read_cb_t cb = lv_indev_get_read_cb(indev);
    if (!cb) {
        return;
    }
    s_indev = indev;
    s_board_read = cb;
    lv_indev_set_read_cb(indev, read_cb);
}

void muse_touch_poll(void)
{
    const muse_touch_sink_t *sink = s_sink;
    if (!s_indev || !sink || !sink->wanted || esp_timer_get_time() - s_lvgl_read_t < LVGL_QUIET_US) {
        return;
    }
#if CONFIG_MUSE_VIDEO
    /* Only while the video view holds LVGL paused: with the screen off the
     * touch controller sleeps. */
    if (!muse_video_is_open()) {
        return;
    }
#else
    return;
#endif
    if (!sink->wanted()) {
        return;
    }
    lv_indev_data_t d = { .state = LV_INDEV_STATE_RELEASED };
    s_board_read(s_indev, &d);
    user_touch(d.state == LV_INDEV_STATE_PRESSED);
    if (passcode_owns()) {
        hold_back();
        return;   /* the lock or a keypad is taking the screen back from the video view */
    }
    feed(&d);
}

typedef struct {
    int x1, y1, x2, y2, ms;
} inject_t;

static void inject_task(void *arg)
{
    inject_t j = *(inject_t *)arg;
    free(arg);
    const muse_touch_sink_t *sink = s_sink;
    int steps = j.ms / 10 > 0 ? j.ms / 10 : 1;
    for (int i = 0; sink && i <= steps && !passcode_owns(); i++) {   /* a passcode screen ends it */
        int x = j.x1 + (j.x2 - j.x1) * i / steps, y = j.y1 + (j.y2 - j.y1) * i / steps;
        sink->sample(true, (uint16_t)x, (uint16_t)y);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (sink) {
        sink->sample(false, 0, 0);
    }
    s_injecting = false;
    vTaskDelete(NULL);
}

void muse_touch_inject(int x1, int y1, int x2, int y2, int ms)
{
    inject_t *j = malloc(sizeof(*j));
    if (!j) {
        return;
    }
    *j = (inject_t){ x1, y1, x2, y2, ms };
    muse_input_user_touch();   /* the bench's touch, a user's for link modes */
    s_injecting = true;
    if (xTaskCreate(inject_task, "touch_inject", 3072, j, 5, NULL) != pdPASS) {
        s_injecting = false;
        free(j);
    }
}

bool muse_touch_press(int x1, int y1, int x2, int y2, int ms)
{
    if (!s_indev) {
        return false;   /* no touch screen */
    }
    bool wireless = muse_input_line_wireless();
    portENTER_CRITICAL(&s_press_mux);
    muse_touch_script_arm(&s_press, x1, y1, x2, y2, ms, esp_timer_get_time());
    s_press_wireless = wireless;
    portEXIT_CRITICAL(&s_press_mux);
    return true;
}

bool muse_touch_press_path(const int *xs, const int *ys, int n, int ms)
{
    if (!s_indev) {
        return false;
    }
    bool wireless = muse_input_line_wireless();
    portENTER_CRITICAL(&s_press_mux);
    muse_touch_script_arm_path(&s_press, xs, ys, n, ms, esp_timer_get_time());
    s_press_wireless = wireless;
    portEXIT_CRITICAL(&s_press_mux);
    return true;
}

bool muse_touch_tap_obj(lv_obj_t *obj, int ms, muse_touch_gesture_t *g)
{
    if (!obj || !lv_obj_is_visible(obj)) {
        return false;
    }
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int x = (a.x1 + a.x2) / 2, y = (a.y1 + a.y2) / 2;
    *g = (muse_touch_gesture_t){ x, y, x, y, ms };
    return true;
}

bool muse_touch_slide_obj(lv_obj_t *slider, int value, muse_touch_gesture_t *g)
{
    if (!slider || !lv_obj_is_visible(slider)) {
        return false;
    }
    lv_area_t a;
    lv_obj_get_coords(slider, &a);
    int w = a.x2 - a.x1;
    int lo = lv_slider_get_min_value(slider), hi = lv_slider_get_max_value(slider);
    int now = lv_slider_get_value(slider);
    int from = a.x1 + (hi > lo ? w * (now - lo) / (hi - lo) : 0);
    int to = value <= 0 ? a.x1 - 24 : value >= 100 ? a.x2 + 24 : a.x1 + w * value / 100;
    int y = (a.y1 + a.y2) / 2;
    *g = (muse_touch_gesture_t){ from, y, to, y, MUSE_TOUCH_DRAG_MS };
    return true;
}
