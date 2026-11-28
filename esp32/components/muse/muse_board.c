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

#include "muse_board.h"

#include "esp_check.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "muse_board";

#define DEBOUNCE_SAMPLES 3
#define DEBOUNCE_POLL_MS 10     /* the input task's awake poll */

static TaskHandle_t s_waiter;

/* A button reached the level muse_gpio_buttons_wait() armed. Level-triggered,
 * so off until armed again. */
static void on_button_line(void *arg)
{
    gpio_intr_disable((gpio_num_t)(intptr_t)arg);
    BaseType_t woken = pdFALSE;
    if (s_waiter) {
        vTaskNotifyGiveFromISR(s_waiter, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

static esp_err_t button_init(muse_gpio_button_t *b, gpio_num_t gpio, bool active_high)
{
    *b = (muse_gpio_button_t){ .gpio = gpio, .active_high = active_high };
    /* Input-only pads (the classic ESP32's 34-39) have no pull-up; their
     * boards wire one. */
    bool pull_up = !active_high && GPIO_IS_VALID_OUTPUT_GPIO(gpio);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio %d", gpio);
    /* For muse_gpio_buttons_wait(); the line doesn't interrupt until then.
     * Installing twice works but logs an error. */
    static bool isr_service;
    if (!isr_service) {
        esp_err_t err = gpio_install_isr_service(0);
        ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "gpio isr");
        isr_service = true;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(gpio, on_button_line, (void *)(intptr_t)gpio), TAG, "gpio %d isr", gpio);
    return gpio_intr_disable(gpio);
}

void muse_board_pin_hold(gpio_num_t pin, int level)
{
    if (pin == GPIO_NUM_NC) {
        return;
    }
    gpio_reset_pin(pin);
    gpio_set_level(pin, level);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
}

esp_err_t muse_gpio_button_init(muse_gpio_button_t *b, gpio_num_t gpio)
{
    return button_init(b, gpio, false);
}

bool muse_gpio_safe_key(gpio_num_t gpio, bool pull)
{
    static uint64_t set;   /* the GPIOs already made inputs */
    if (!(set & BIT64(gpio))) {
        set |= BIT64(gpio);
        gpio_set_direction(gpio, GPIO_MODE_INPUT);
        if (pull) {
            gpio_pullup_en(gpio);
        }
    }
    return gpio_get_level(gpio) == 0;
}

esp_err_t muse_gpio_button_init_high(muse_gpio_button_t *b, gpio_num_t gpio)
{
    return button_init(b, gpio, true);
}

static bool raw_pressed(const muse_gpio_button_t *b)
{
    return gpio_get_level(b->gpio) == b->active_high;
}

unsigned muse_gpio_button_poll(muse_gpio_button_t *b)
{
    bool raw = raw_pressed(b);
    if (raw == b->pressed) {
        b->stable = 0;
        return 0;
    }
    /* Asleep, a press is what woke the input task: it counts on its first sample. */
    if (++b->stable < DEBOUNCE_SAMPLES && !(raw && muse_state_asleep())) {
        return 0;
    }
    b->stable = 0;
    b->pressed = raw;
    return raw ? MUSE_BTN_TALK_PRESS : MUSE_BTN_TALK_RELEASE;
}

void muse_gpio_buttons_wait(muse_gpio_button_t *const *b, int n, int timeout_ms)
{
    for (int i = 0; i < n; i++) {
        if (raw_pressed(b[i]) != b[i]->pressed) {
            /* Mid-change: debounce at the awake poll rate. */
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_POLL_MS));
            return;
        }
    }
    /* Not cleared first: a stale notification (a line that changed back
     * before the last wait ended) only makes this return early. */
    s_waiter = xTaskGetCurrentTaskHandle();
    esp_sleep_enable_gpio_wakeup();
    for (int i = 0; i < n; i++) {
        /* Wake on the level the button would change to. */
        bool high = b[i]->pressed != b[i]->active_high;
        gpio_wakeup_enable(b[i]->gpio, high ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
        gpio_intr_enable(b[i]->gpio);
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));
    for (int i = 0; i < n; i++) {
        gpio_intr_disable(b[i]->gpio);
        gpio_wakeup_disable(b[i]->gpio);
    }
}
