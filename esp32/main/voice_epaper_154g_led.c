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


#include "voice_epaper_154g_led.h"
#include "led_status.h"

#include <stdatomic.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define VOICE_LED_GPIO 3 // active low
static atomic_int s_voice = LED_VOICE_IDLE;
static atomic_uint s_error_generation;

// ---- LED pattern (host-tested) ----
static bool voice_led_on(led_voice_t state, uint64_t ms) {
    switch (state) {
    case LED_VOICE_LISTENING: return true;
    case LED_VOICE_TRANSCRIBING:
    case LED_VOICE_THINKING: return ms % 1000 < 500;
    case LED_VOICE_BUFFERING: return ms % 500 < 250;
    case LED_VOICE_SPEAKING: return ms % 250 < 125;
    case LED_VOICE_ERROR: return ms < 1200 && ms % 240 < 120;
    default: return false;
    }
}
// ---- End LED pattern ----

static void led_task(void *arg) {
    led_voice_t previous = LED_VOICE_IDLE;
    unsigned seen_error = atomic_load(&s_error_generation);
    uint64_t since = esp_timer_get_time() / 1000;
    for (;;) {
        uint64_t now = esp_timer_get_time() / 1000;
        led_voice_t state = atomic_load(&s_voice);
        unsigned generation = atomic_load(&s_error_generation);
        if (state != previous || generation != seen_error) {
            since = now; previous = state; seen_error = generation;
        }
        gpio_set_level(VOICE_LED_GPIO, voice_led_on(state, now - since) ? 0 : 1);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool voice_epaper_154g_led_init(void) {
    gpio_set_level(VOICE_LED_GPIO, 1);
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << VOICE_LED_GPIO, .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&cfg) != ESP_OK) return false;
    return xTaskCreate(led_task, "voice_led", 2048, NULL, 3, NULL) == pdPASS;
}

// No e-paper mutex or SPI refresh here: feedback must not wait 20 seconds.
void led_status_set_voice(led_voice_t state) {
    atomic_store(&s_voice, state);
    if (state == LED_VOICE_ERROR) atomic_fetch_add(&s_error_generation, 1);
}
void led_status_set_level(float level) { (void)level; }
void led_status_show_volume(int percent) { (void)percent; }
