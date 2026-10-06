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

#include "voice_epaper_154g.h"
#include "led_status.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "muse_chat.h"

#define VOICE_LED_GPIO 3 // active low
#define ERROR_BLINK_MS 1200
#define REPLY_BYTES    1024

static const char *TAG = "link.voice_led";
static atomic_int s_voice = LED_VOICE_IDLE;
static atomic_uint s_error_generation;
static atomic_bool s_mic_ready;
static TaskHandle_t s_task;
static char *s_reply, *s_shown;

static bool voice_led_on(led_voice_t state, uint64_t ms) {
    switch (state) {
    case LED_VOICE_LISTENING: return true;
    case LED_VOICE_TRANSCRIBING:
    case LED_VOICE_THINKING: return ms % 1000 < 500;
    case LED_VOICE_BUFFERING: return ms % 500 < 250;
    case LED_VOICE_SPEAKING: return ms % 250 < 125;
    case LED_VOICE_ERROR: return ms < ERROR_BLINK_MS && ms % 240 < 120;
    default: return false;
    }
}

// The text goes on screen as it arrives, ahead of the reply's silent pacing.
static void show_reply(void) {
    if (!s_reply || !muse_hatch_turn_reply(s_reply, REPLY_BYTES) || !strcmp(s_reply, s_shown)) return;
    memcpy(s_shown, s_reply, REPLY_BYTES);
    voice_epaper_154g_show_text(s_reply);
    ESP_LOGI(TAG, "reply text: %u bytes", (unsigned)strlen(s_reply));
}

static void led_task(void *arg) {
    led_voice_t previous = LED_VOICE_IDLE;
    unsigned seen_error = atomic_load(&s_error_generation);
    uint64_t since = esp_timer_get_time() / 1000;
    for (;;) {
        uint64_t now = esp_timer_get_time() / 1000;
        led_voice_t state = atomic_load(&s_voice);
        // voice.c shows LISTENING before the mic is up; blink until it is.
        if (state == LED_VOICE_LISTENING && !atomic_load(&s_mic_ready)) state = LED_VOICE_BUFFERING;
        unsigned generation = atomic_load(&s_error_generation);
        if (state != previous || generation != seen_error) {
            since = now; previous = state; seen_error = generation;
        }
        gpio_set_level(VOICE_LED_GPIO, voice_led_on(state, now - since) ? 0 : 1);
        // Sleep until the next state once the LED has nothing left to show.
        bool still = state == LED_VOICE_IDLE ||
                     (state == LED_VOICE_ERROR && now - since >= ERROR_BLINK_MS);
        ulTaskNotifyTake(pdTRUE, still ? portMAX_DELAY : pdMS_TO_TICKS(20));
    }
}

bool voice_epaper_154g_led_init(void) {
    gpio_set_level(VOICE_LED_GPIO, 1);
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << VOICE_LED_GPIO, .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&cfg) != ESP_OK) return false;
    s_reply = heap_caps_calloc(1, REPLY_BYTES, MALLOC_CAP_SPIRAM);
    s_shown = heap_caps_calloc(1, REPLY_BYTES, MALLOC_CAP_SPIRAM);
    // The stack is in PSRAM: this task must not touch flash.
    if (s_reply && s_shown && xTaskCreateWithCaps(led_task, "voice_led", 3072, NULL, 3, &s_task,
                                                  MALLOC_CAP_SPIRAM) == pdPASS) return true;
    free(s_reply);
    free(s_shown);
    s_reply = s_shown = NULL;
    return false;
}

void voice_epaper_154g_mic_ready(bool ready) {
    atomic_store(&s_mic_ready, ready);
    if (s_task) xTaskNotifyGive(s_task);
}

// voice.c reports every step of a turn here, on its own task, the only task
// that may read the turn: the reply text is picked up here as well. The
// e-paper only queues it; nothing here waits for a 20-second refresh.
void led_status_set_voice(led_voice_t state) {
    if (state == LED_VOICE_LISTENING && s_shown) s_shown[0] = '\0';
    else if (state == LED_VOICE_ERROR) voice_epaper_154g_show_text(NULL);
    else show_reply();
    atomic_store(&s_voice, state);
    if (state == LED_VOICE_ERROR) atomic_fetch_add(&s_error_generation, 1);
    if (s_task) xTaskNotifyGive(s_task);
}
void led_status_set_level(float level) { (void)level; }
void led_status_show_volume(int percent) { (void)percent; }
