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

#include "muse_hw.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_state.h"

static const char *TAG = "muse_hw";

#define RING 32
#define SWIPE_PX 40         /* moved this far between down and lift: a swipe */
#define LONG_MS 600         /* held this long without moving: a long press */
#define WAIT_POLL_MS 20
#define LED_TICK_MS 20

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static muse_hw_event_t s_ring[RING];   /* in PSRAM where there is some */
static uint32_t s_seq;
static int64_t s_capture_until;

/* Touch in progress, from the board's touch task only. */
static bool s_down;
static int s_x0, s_y0, s_x, s_y;
static int64_t s_down_us;
static int64_t s_wheel_down_us;

static muse_hw_listener_fn s_listener;

void muse_hw_set_listener(muse_hw_listener_fn fn)
{
    s_listener = fn;
}

static void post_full(muse_hw_event_type_t type, int x, int y, int x2, int y2, int32_t value, const char *id,
                      const char *app, const char *ui_event, const char *text)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    muse_hw_event_t *ev = &s_ring[s_seq % RING];
    *ev = (muse_hw_event_t){
        .seq = s_seq + 1, .us = now, .type = type, .x = x, .y = y, .x2 = x2, .y2 = y2, .value = value,
    };
    if (id) {
        strncpy(ev->id, id, sizeof(ev->id) - 1);
    }
    if (app) {
        strncpy(ev->app, app, sizeof(ev->app) - 1);
    }
    if (ui_event) {
        strncpy(ev->ui_event, ui_event, sizeof(ev->ui_event) - 1);
    }
    if (text) {
        strncpy(ev->text, text, sizeof(ev->text) - 1);
    }
    s_seq++;
    muse_hw_event_t copy = *ev;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "%s (%d,%d) %ld %s %s %s", muse_hw_event_name(type), x, y, (long)value, id ? id : "",
             app ? app : "", ui_event ? ui_event : "");
    if (s_listener) {
        s_listener(&copy);
    }
}

static void post_id(muse_hw_event_type_t type, int x, int y, int x2, int y2, int32_t value, const char *id)
{
    post_full(type, x, y, x2, y2, value, id, NULL, NULL, NULL);
}

static void post(muse_hw_event_type_t type, int x, int y, int x2, int y2, int32_t value)
{
    post_id(type, x, y, x2, y2, value, NULL);
}

const char *muse_hw_event_name(muse_hw_event_type_t type)
{
    switch (type) {
    case MUSE_HW_TAP: return "tap";
    case MUSE_HW_LONG_PRESS: return "long_press";
    case MUSE_HW_SWIPE: return "swipe";
    case MUSE_HW_WHEEL_CLICK: return "wheel_click";
    case MUSE_HW_WHEEL_TURN: return "wheel_turn";
    case MUSE_HW_BUTTON: return "button";
    case MUSE_HW_DETECTION: return "detection";
    case MUSE_HW_UI: return "ui";
    case MUSE_HW_PET: return "pet";
    }
    return "?";
}

const char *muse_hw_swipe_dir(const muse_hw_event_t *ev)
{
    int dx = ev->x2 - ev->x, dy = ev->y2 - ev->y;
    if (abs(dx) >= abs(dy)) {
        return dx < 0 ? "left" : "right";
    }
    return dy < 0 ? "up" : "down";
}

void muse_hw_touch(bool down, int x, int y)
{
    int64_t now = esp_timer_get_time();
    if (down) {
        if (!s_down) {
            s_down = true;
            s_x0 = x;
            s_y0 = y;
            s_down_us = now;
        }
        s_x = x;
        s_y = y;
        return;
    }
    if (!s_down) {
        return;
    }
    s_down = false;
    int dx = s_x - s_x0, dy = s_y - s_y0;
    int32_t held_ms = (int32_t)((now - s_down_us) / 1000);
    if (dx * dx + dy * dy >= SWIPE_PX * SWIPE_PX) {
        post(MUSE_HW_SWIPE, s_x0, s_y0, s_x, s_y, held_ms);
    } else if (held_ms >= LONG_MS) {
        post(MUSE_HW_LONG_PRESS, s_x0, s_y0, s_x0, s_y0, held_ms);
    } else {
        post(MUSE_HW_TAP, s_x0, s_y0, s_x0, s_y0, held_ms);
    }
}

void muse_hw_wheel_push(bool down)
{
    int64_t now = esp_timer_get_time();
    if (down) {
        s_wheel_down_us = now;
    } else if (s_wheel_down_us) {
        post(MUSE_HW_WHEEL_CLICK, 0, 0, 0, 0, (int32_t)((now - s_wheel_down_us) / 1000));
        s_wheel_down_us = 0;
    }
}

void muse_hw_wheel_turn(int steps)
{
    if (steps) {
        post(MUSE_HW_WHEEL_TURN, 0, 0, 0, 0, steps);
    }
}

void muse_hw_button(const char *id)
{
    post_id(MUSE_HW_BUTTON, 0, 0, 0, 0, 0, id);
}

void muse_hw_detection(const char *label, int score, int x, int y, int w, int h)
{
    post_id(MUSE_HW_DETECTION, x, y, w, h, score, label);
}

void muse_hw_ui(const char *app, const char *widget, const char *event, int value, const char *text)
{
    post_full(MUSE_HW_UI, 0, 0, 0, 0, value, widget, app, event, text);
}

void muse_hw_pet(const char *what, int value)
{
    post_full(MUSE_HW_PET, 0, 0, 0, 0, value, what, NULL, NULL, NULL);
}

uint32_t muse_hw_last_seq(void)
{
    taskENTER_CRITICAL(&s_lock);
    uint32_t seq = s_seq;
    taskEXIT_CRITICAL(&s_lock);
    return seq;
}

int muse_hw_events(uint32_t after, muse_hw_event_t *out, int max, int wait_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    for (;;) {
        int n = 0;
        taskENTER_CRITICAL(&s_lock);
        uint32_t first = s_seq > RING ? s_seq - RING + 1 : 1;
        for (uint32_t seq = after + 1 > first ? after + 1 : first; seq <= s_seq && n < max; seq++) {
            out[n++] = s_ring[(seq - 1) % RING];
        }
        taskEXIT_CRITICAL(&s_lock);
        if (n || esp_timer_get_time() >= deadline) {
            return n;
        }
        vTaskDelay(pdMS_TO_TICKS(WAIT_POLL_MS));
    }
}

void muse_hw_capture(int64_t until_us)
{
    taskENTER_CRITICAL(&s_lock);
    s_capture_until = until_us;
    taskEXIT_CRITICAL(&s_lock);
    if (until_us) {
        muse_state_set_asleep(false);
    }
}

bool muse_hw_captured(void)
{
    taskENTER_CRITICAL(&s_lock);
    int64_t until = s_capture_until;
    taskEXIT_CRITICAL(&s_lock);
    return until && esp_timer_get_time() < until;
}

static muse_hw_console_fn s_console;

void muse_hw_set_console(muse_hw_console_fn fn)
{
    s_console = fn;
}

bool muse_hw_console(const char *json)
{
    if (!s_console) {
        return false;
    }
    s_console(json);
    return true;
}

/* ---- The RGB light ---- */

static SemaphoreHandle_t s_led_mutex;
static esp_timer_handle_t s_led_timer;
static uint8_t s_r, s_g, s_b;
static muse_hw_led_mode_t s_mode;
static int s_period_ms;
static int64_t s_effect_t0;

bool muse_hw_has_led(void)
{
    return muse_board && muse_board->set_led;
}

static void led_write(float level)
{
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    muse_board->set_led((uint8_t)(s_r * level), (uint8_t)(s_g * level), (uint8_t)(s_b * level));
    xSemaphoreGive(s_led_mutex);
}

static void led_tick(void *arg)
{
    (void)arg;
    float phase = (float)((esp_timer_get_time() - s_effect_t0) / 1000 % s_period_ms) / s_period_ms;
    if (s_mode == MUSE_HW_LED_BLINK) {
        led_write(phase < 0.5f ? 1.0f : 0.0f);
    } else {
        /* Squared, so the dim end of the breath lasts as long as it looks. */
        float l = (1.0f - cosf(phase * 2.0f * (float)M_PI)) / 2.0f;
        led_write(l * l);
    }
}

bool muse_hw_led(uint8_t r, uint8_t g, uint8_t b, muse_hw_led_mode_t mode, int period_ms)
{
    if (!muse_hw_has_led()) {
        return false;
    }
    if (!s_led_mutex) {
        s_led_mutex = xSemaphoreCreateMutex();
        const esp_timer_create_args_t args = { .callback = led_tick, .name = "muse_led" };
        if (!s_led_mutex || esp_timer_create(&args, &s_led_timer) != ESP_OK) {
            return false;
        }
    }
    esp_timer_stop(s_led_timer);
    s_r = r;
    s_g = g;
    s_b = b;
    s_mode = mode;
    s_period_ms = period_ms < 2 * LED_TICK_MS ? 2 * LED_TICK_MS : period_ms;
    s_effect_t0 = esp_timer_get_time();
    if (mode == MUSE_HW_LED_BLINK || mode == MUSE_HW_LED_BREATHE) {
        return esp_timer_start_periodic(s_led_timer, LED_TICK_MS * 1000) == ESP_OK;
    }
    led_write(mode == MUSE_HW_LED_SOLID ? 1.0f : 0.0f);
    return true;
}
