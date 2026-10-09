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

#include "button.h"
#include "stack_monitor.h"
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
#include "epaper_154g_rotation.h"
#endif

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "link.button";

#define BTN_GPIO           CONFIG_HOMEHUB_BUTTON_GPIO
#define LONG_PRESS_MS      5000
#define SHORT_PRESS_MAX_MS 1000
#define DOUBLE_CLICK_MS    400
#define POLL_MS            50

static button_cb s_short_press_cb = NULL;
static button_cb s_double_press_cb = NULL;
static button_cb s_long_press_cb = NULL;
#if CONFIG_HOMEHUB_VOICE
static volatile button_press_cb s_press_cb = NULL;

void button_set_press_cb(button_press_cb cb) {
    s_press_cb = cb;
}
#endif

static void button_task(void *arg) {
    stack_monitor_t stack = STACK_MONITOR_INIT;
    bool was_pressed = false;
    int64_t press_start = 0;
    bool fired = false;
    int click_count = 0;
    int64_t last_release = 0;
#if CONFIG_HOMEHUB_VOICE
    bool claimed = false;
#endif

    while (1) {
        bool pressed = (gpio_get_level(BTN_GPIO) == 0);
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
        epaper_154g_power_button_poll(pressed);
#endif

        if (pressed && !was_pressed) {
            press_start = esp_timer_get_time();
            fired = false;
#if CONFIG_HOMEHUB_VOICE
            button_press_cb press_cb = s_press_cb;
            claimed = press_cb && press_cb(true);
            if (claimed) {
                // Not a tap or hold, and it ends any pending double click.
                fired = true;
                click_count = 0;
            }
        } else if (!pressed && was_pressed && claimed) {
            claimed = false;
            button_press_cb press_cb = s_press_cb;
            if (press_cb) press_cb(false);
#endif
        } else if (pressed && !fired) {
            int64_t held_ms = (esp_timer_get_time() - press_start) / 1000;
            if (held_ms >= LONG_PRESS_MS) {
                ESP_LOGI(TAG, "long press detected");
                if (s_long_press_cb) s_long_press_cb();
                fired = true;
                click_count = 0;
            }
        } else if (!pressed && was_pressed && !fired) {
            int64_t held_ms = (esp_timer_get_time() - press_start) / 1000;
            if (held_ms >= 50 && held_ms <= SHORT_PRESS_MAX_MS) {
                click_count++;
                last_release = esp_timer_get_time();
            }
        }

        if (!pressed && click_count > 0) {
            int64_t since_release = (esp_timer_get_time() - last_release) / 1000;
            if (since_release >= DOUBLE_CLICK_MS) {
                if (click_count >= 2) {
                    ESP_LOGI(TAG, "double press detected");
                    if (s_double_press_cb) s_double_press_cb();
                } else {
                    ESP_LOGI(TAG, "short press detected");
                    if (s_short_press_cb) s_short_press_cb();
                }
                click_count = 0;
            }
        }

        was_pressed = pressed;
        stack_monitor_poll(&stack);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

bool button_init(button_cb on_short_press, button_cb on_double_press,
                 button_cb on_long_press) {
    s_short_press_cb = on_short_press;
    s_double_press_cb = on_double_press;
    s_long_press_cb = on_long_press;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio config failed: %s", esp_err_to_name(err));
        return false;
    }

    xTaskCreate(button_task, "btn", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "button ready (GPIO %d: tap=retry wifi, 2x=rescan, hold %ds=reset setup)",
             BTN_GPIO, LONG_PRESS_MS / 1000);
    return true;
}
