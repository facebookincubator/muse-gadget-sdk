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
#include "muse_local_press.h"
#include "sdkconfig.h"
#if CONFIG_MUSE_GADGET_BLE_AUDIO && !CONFIG_MUSE_ENABLED
#include "mg_ble.h"
#include "mg_press_src.h"
#endif
#include "muse_bootguard.h"
#include "stack_monitor.h"

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

static button_read_cb s_read = NULL;
static button_cb s_short_press_cb = NULL;
static button_cb s_double_press_cb = NULL;
static button_cb s_long_press_cb = NULL;
static button_claim_cb s_confirm_claim = NULL;
static int64_t s_pressed_after_us;   // the reported short press began after this
#if CONFIG_HOMEHUB_VOICE
static volatile button_press_cb s_press_cb = NULL;

void button_set_press_cb(button_press_cb cb) {
    s_press_cb = cb;
}
#endif

void button_set_confirm(button_claim_cb confirm) {
    s_confirm_claim = confirm;
}

int64_t button_pressed_after_us(void) {
    return s_pressed_after_us;
}

void button_set_reader(button_read_cb read) {
    s_read = read;
}

static bool read_button(bool *pressed) {
    if (s_read) return s_read(pressed);
    *pressed = gpio_get_level(BTN_GPIO) == 0;
    return true;
}

static void button_task(void *arg) {
    stack_monitor_t stack = STACK_MONITOR_INIT;
    bool was_pressed = false;
    int64_t press_start = 0;
    int64_t up_at = 0;          // the last poll that saw the button up
    int64_t press_after = 0;    // ... before the current press
    bool fired = false;
#if !CONFIG_MUSE_ENABLED
    bool update_press = false;   // this press confirmed an update: it does nothing else
#endif
    bool boot_press = false;
    int click_count = 0;
    int64_t last_release = 0;
#if CONFIG_HOMEHUB_VOICE
    bool claimed = false;
#endif

    while (1) {
        int64_t now = esp_timer_get_time();
        bool pressed = was_pressed;
        if (!read_button(&pressed)) {
            // No edge: drop any click or hold in progress.
            fired = true;
            click_count = 0;
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }

#if !CONFIG_MUSE_ENABLED
        // While an update over Bluetooth asks for its press (the light's confirm pattern, muse_local_press.h), a
        // press of the button itself (never an injected one) confirms it, and that press does nothing else (no tap,
        // hold, re-setup confirm or setup reset). A press at any other time never counts. Muse boards: muse_input.c.
        if (pressed && !was_pressed && muse_local_press_prompting()
            && muse_local_press_note("a press of the button on the update prompt")) {
            update_press = true;
        }
        if (update_press) {
            if (!pressed) {
                update_press = false;
                up_at = now;
            }
            was_pressed = pressed;
            stack_monitor_poll(&stack);
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }
#endif
        if (pressed && !was_pressed) {
            press_start = esp_timer_get_time();
            press_after = up_at;
            fired = false;
#if CONFIG_MUSE_GADGET_BLE_AUDIO && !CONFIG_MUSE_ENABLED
            // The gadget's own key, the button's real edge (never an injected one), opens a client's start_mic for
            // its press window on a screenless board, as muse_glue's press hook does on Muse boards. Without it a
            // proven start_mic answered press_required forever on a release image.
            mg_ble_press(MG_PRESS_SRC_KEY);
#endif
            // A press in the boot's first 3 s is the way into SAFE
            // (muse_bootguard_entry_watch, 10 s): never the 5 s setup reset.
            boot_press = muse_bootguard_entry_open();
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
            if (held_ms >= LONG_PRESS_MS && !boot_press) {
                ESP_LOGI(TAG, "long press detected");
                if (s_long_press_cb) s_long_press_cb();
                fired = true;
                click_count = 0;
            }
        } else if (!pressed && was_pressed && !fired) {
            int64_t held_ms = (esp_timer_get_time() - press_start) / 1000;
            if (held_ms >= 50 && held_ms <= SHORT_PRESS_MAX_MS) {
                if (click_count++ == 0) s_pressed_after_us = press_after;
                last_release = esp_timer_get_time();
            }
        }

        if (pressed && !was_pressed && !fired && s_confirm_claim && s_confirm_claim()) {
            // The setup prompt's press (one press confirms): no tap, double tap or 5 s hold for it.
            fired = true;
            click_count = 0;
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

        if (!pressed) up_at = now;
        was_pressed = pressed;
        stack_monitor_poll(&stack);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

static bool button_raw_down(void) {
    return gpio_get_level(BTN_GPIO) == 0;
}

void button_safe_entry_watch(void) {
    // The way into SAFE (muse_bootguard.h) from the boot's start: the button
    // is read raw before button_init() runs, once BLE is up.
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) == ESP_OK) {
        muse_bootguard_entry_watch(button_raw_down);
    }
}

bool button_init(button_cb on_short_press, button_cb on_double_press,
                 button_cb on_long_press) {
    s_short_press_cb = on_short_press;
    s_double_press_cb = on_double_press;
    s_long_press_cb = on_long_press;

    if (s_read) {
        xTaskCreate(button_task, "btn", 4096, NULL, 2, NULL);
        ESP_LOGI(TAG, "button ready (tap=retry wifi, 2x=rescan, hold %ds=reset setup)",
                 LONG_PRESS_MS / 1000);
        return true;
    }

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
