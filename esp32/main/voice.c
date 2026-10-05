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

#include "voice.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "button.h"
#include "config_store.h"
#include "led_status.h"
#if CONFIG_HOMEHUB_WIFI_IDLE_MAX_MODEM
#include "wifi_mgr.h"
#endif
#include "muse_chat.h"
#include "voice_board.h"
#include "voice_muse_chat.h"
#include "voice_player.h"
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
#include "voice_epaper_154g_led.h"
#include "link_pairing.h"
#endif

static const char *TAG = "link.voice";

#define KEY_VOLUME          "voice_volume"
#define DEFAULT_VOLUME      60
// Volume change per detent of the dial.
#define VOLUME_STEP         5
// Store a dialled volume once the dial has rested this long.
#define VOLUME_SAVE_MS      2000
#define DIAL_POLL_MS        20

#define CAPTURE_MAX_MS      15000
#define CAPTURE_MIN_MS      300
#define CAPTURE_CHUNK       (VOICE_MIC_RATE / 50)      // 20 ms
#define CAPTURE_MAX         (VOICE_MIC_RATE * CAPTURE_MAX_MS / 1000)
// Keep listening briefly after release so the last word is not clipped.
#define RELEASE_TAIL_MS     250
#define REPLY_CHUNK         (VOICE_PLAYER_RATE / 50)   // 20 ms

typedef enum {
    EVT_PRESS, EVT_RELEASE,
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    EVT_PAGE_CUE, EVT_ROTATE_CUE,
#endif
} voice_evt_t;

static QueueHandle_t s_events;
static atomic_bool s_ready;
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
// Replies are text: PWR cues are muted only while the mic records.
static atomic_bool s_capturing;
#endif
static atomic_int s_volume;

static int load_volume(void) {
    char buf[8];
    if (!config_get_str(KEY_VOLUME, buf, sizeof(buf)) || !buf[0]) return DEFAULT_VOLUME;
    int v = atoi(buf);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static bool store_volume(int volume) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", volume);
    return config_set_str(KEY_VOLUME, buf);
}

// Set the speaker volume and show it on the ring.
static void apply_volume(int volume) {
    atomic_store(&s_volume, volume);
    voice_board_set_volume(volume);
    led_status_show_volume(volume);
}

// Turns the volume with the dial. On an internal-RAM stack: storing the volume
// writes NVS.
#if !CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
static void dial_task(void *arg) {
    int64_t save_at = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DIAL_POLL_MS));
        int steps = voice_board_dial_steps();
        if (steps) {
            int volume = atomic_load(&s_volume) + steps * VOLUME_STEP;
            apply_volume(volume < 0 ? 0 : volume > 100 ? 100 : volume);
            save_at = esp_timer_get_time() + VOLUME_SAVE_MS * 1000LL;
        } else if (save_at && esp_timer_get_time() >= save_at) {
            save_at = 0;
            int volume = atomic_load(&s_volume);
            if (store_volume(volume)) ESP_LOGI(TAG, "volume %d", volume);
            else ESP_LOGW(TAG, "volume could not be stored");
        }
    }
}

#endif

// Did a press arrive? Releases are dropped.
static bool pressed_again(TickType_t wait) {
    voice_evt_t evt;
    while (xQueueReceive(s_events, &evt, wait) == pdTRUE) {
        if (evt == EVT_PRESS) return true;
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
        if (!atomic_load(&s_capturing) && (evt == EVT_PAGE_CUE || evt == EVT_ROTATE_CUE))
            voice_board_button_cue(evt == EVT_ROTATE_CUE);
#endif
        wait = 0;
    }
    return false;
}

#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
void voice_button_cue(bool long_hold) {
    if (!atomic_load(&s_ready) || atomic_load(&s_capturing)) return;
    voice_evt_t evt = long_hold ? EVT_ROTATE_CUE : EVT_PAGE_CUE;
    xQueueSend(s_events, &evt, 0);
}
#endif

// Stream the microphone into the turn until release (plus a tail) or the
// limit. Returns the number of samples, 0 if the capture failed.
static size_t record(void) {
    static int16_t chunk[CAPTURE_CHUNK];
    if (voice_board_mic_start() != ESP_OK) return 0;
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    atomic_store(&s_capturing, true);
    // Steady green means the warm-up/discard finished: speaking can start.
    led_status_set_voice(LED_VOICE_LISTENING);
#endif
    size_t samples = 0;
    int64_t stop_at = 0;
    while (samples < CAPTURE_MAX) {
        voice_evt_t evt;
        if (!stop_at && xQueueReceive(s_events, &evt, 0) == pdTRUE && evt == EVT_RELEASE) {
            stop_at = esp_timer_get_time() + RELEASE_TAIL_MS * 1000LL;
        }
        if (stop_at && esp_timer_get_time() >= stop_at) break;
        int peak = 0;
        size_t got = voice_board_mic_read(chunk, CAPTURE_CHUNK, &peak);
        if (!got) break;
        muse_hatch_turn_audio(chunk, got);
        samples += got;
        led_status_set_level(peak / 12000.0f);
    }
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    atomic_store(&s_capturing, false);
#else
    voice_board_mic_stop();
#endif
    if (samples >= CAPTURE_MAX) ESP_LOGI(TAG, "capture limit reached");
    return samples;
}

static bool fail(const char *why) {
    ESP_LOGW(TAG, "turn failed: %s", why);
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    voice_epaper_154g_show_text(NULL, NULL);
#endif
    led_status_set_voice(LED_VOICE_ERROR);
    return false;
}

// Play the reply as it arrives. Returns true if a new press interrupted it.
static bool reply(void) {
    static int16_t pcm[REPLY_CHUNK];
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    static char text[1024], heard[1024], shown[1024];
    shown[0] = '\0';
    heard[0] = '\0';
#else
    char text[96];
#endif
    bool done = false;
    size_t played = 0;
    int64_t t0 = esp_timer_get_time();
    voice_player_begin();
    for (;;) {
        if (pressed_again(0)) {
            muse_hatch_turn_cancel();
            voice_player_stop();
            return true;
        }
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            switch (ev) {
            case MUSE_HATCH_EV_HEARD:
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
                snprintf(heard, sizeof(heard), "%s", text);
#endif
                ESP_LOGI(TAG, "heard: %s", text);
                led_status_set_voice(LED_VOICE_THINKING);
                break;
            case MUSE_HATCH_EV_REPLY:
                if (!voice_player_started()) led_status_set_voice(LED_VOICE_BUFFERING);
                break;
            case MUSE_HATCH_EV_DONE:
                done = true;
                break;
            case MUSE_HATCH_EV_ERROR:
                voice_player_stop();
                return fail(text);
            default:
                break;
            }
        }
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
        if (muse_hatch_turn_reply(text, sizeof(text)) && strcmp(text, shown) != 0) {
            snprintf(shown, sizeof(shown), "%s", text);
            voice_epaper_154g_show_text(text, heard);
            ESP_LOGI(TAG, "caption: reply text after %.1fs (%u bytes)",
                     (esp_timer_get_time() - t0) / 1000000.0f, (unsigned)strlen(text));
        }
#endif
        // After DONE the reply's audio is all decoded; drain what's left.
        size_t n = muse_hatch_turn_read(pcm, REPLY_CHUNK, done ? 0 : 20);
        if (n) {
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
            if (voice_player_write(pcm, n) != ESP_OK) {
                muse_hatch_turn_cancel();
                voice_player_stop();
                return fail("speaker unavailable");
            }
#else
            voice_player_write(pcm, n);
#endif
            played += n;
        } else if (done) {
            break;
        }
        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);
    }
    voice_player_end();
    while (!voice_player_wait(0)) {
        if (pressed_again(pdMS_TO_TICKS(40))) {
            voice_player_stop();
            return true;
        }
        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);
    }
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    if (voice_player_failed()) return fail("speaker unavailable");
#endif
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    ESP_LOGI(TAG, "reply: %.1fs of PCM, %.1fs total", (double)played / VOICE_PLAYER_RATE,
             (esp_timer_get_time() - t0) / 1e6);
#else
    ESP_LOGI(TAG, "reply: %.1fs of speech, %.1fs total", (double)played / VOICE_PLAYER_RATE,
             (esp_timer_get_time() - t0) / 1e6);
#endif
    led_status_set_voice(LED_VOICE_IDLE);
    return false;
}

// Returns true if a new press interrupted the turn.
static bool run_turn(void) {
    voice_player_stop();
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    // Finish any old speaker task before the cue shares its I2S channel.
    while (!voice_player_wait(40)) {}
    // Switch to the reply layout now, so the reply only redraws its text band.
    voice_epaper_154g_show_text("...", NULL);
    if (voice_board_mic_start() != ESP_OK) return fail("microphone unavailable");
    if (voice_board_cue(false) != ESP_OK) {
        voice_board_mic_stop();
        return fail("speaker unavailable");
    }
#endif
    led_status_set_level(0);
#if !CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    led_status_set_voice(LED_VOICE_LISTENING);
#endif
    muse_hatch_turn_begin();
    size_t samples = record();
    if (samples < VOICE_MIC_RATE * CAPTURE_MIN_MS / 1000) {
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
        voice_board_mic_stop();
#endif
        muse_hatch_turn_cancel();
        if (!samples) return fail("microphone unavailable");
        ESP_LOGI(TAG, "press too short");
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
        voice_epaper_154g_show_text(NULL, NULL);
#endif
        led_status_set_voice(LED_VOICE_IDLE);
        return false;
    }
    ESP_LOGI(TAG, "recorded %.1fs", (double)samples / VOICE_MIC_RATE);
    led_status_set_voice(LED_VOICE_TRANSCRIBING);
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    if (voice_board_cue(true) != ESP_OK) {
        voice_board_mic_stop();
        muse_hatch_turn_cancel();
        return fail("speaker unavailable");
    }
    voice_board_mic_stop();
#endif
    muse_hatch_turn_end();
    return reply();
}

// Runs on the button task. Claims the press only when a turn can run, so the
// button keeps its setup role otherwise.
static bool on_press(bool pressed) {
    if (pressed) {
        if (!atomic_load(&s_ready) || voice_board_muted()) return false;
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
        if (link_pairing_confirmation_required()) return false;
#endif
        voice_hatch_refresh();
        if (!muse_hatch_ready()) return false;
    }
    voice_evt_t evt = pressed ? EVT_PRESS : EVT_RELEASE;
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    if (xQueueSend(s_events, &evt, 0) != pdTRUE) return false;
    if (pressed) led_status_set_voice(LED_VOICE_BUFFERING);
#else
    xQueueSend(s_events, &evt, 0);
#endif
    return true;
}

static void voice_task(void *arg) {
    if (voice_board_init() != ESP_OK || voice_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio hardware unavailable; voice chat disabled");
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
        led_status_set_voice(LED_VOICE_ERROR);
#endif
        vTaskSuspend(NULL);
        return;
    }
    voice_board_set_volume(atomic_load(&s_volume));
    atomic_store(&s_ready, true);
#if !CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    if (xTaskCreate(dial_task, "dial", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the dial");
    }
#endif
    ESP_LOGI(TAG, "ready");

    for (;;) {
        if (!pressed_again(portMAX_DELAY)) continue;
#if CONFIG_HOMEHUB_WIFI_IDLE_MAX_MODEM
        wifi_mgr_transfer(true);
#endif
        while (run_turn()) {
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
            while (!voice_player_wait(40)) {}
            voice_board_log_idle();
#endif
        }
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
        while (!voice_player_wait(40)) {}
        voice_board_log_idle();
#endif
#if CONFIG_HOMEHUB_WIFI_IDLE_MAX_MODEM
        wifi_mgr_transfer(false);
#endif
    }
}

void voice_init(void) {
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    if (!voice_epaper_154g_led_init()) {
        ESP_LOGE(TAG, "failed to start voice LED; voice chat disabled");
        return;
    }
#endif
    s_events = xQueueCreate(8, sizeof(voice_evt_t));
    if (!s_events) {
        ESP_LOGE(TAG, "no memory for voice chat");
        return;
    }
    atomic_store(&s_volume, load_volume());
    voice_hatch_refresh();
    muse_hatch_start();
    // The stack is in PSRAM, so the task must not touch flash (NVS): pairing
    // needs an 8 KB internal block for its TLS task.
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    // Cue and the counted stereo write each have a 1280-byte stack buffer.
    const int stack_size = 6144;
#else
    const int stack_size = 4096;
#endif
    if (xTaskCreateWithCaps(voice_task, "voice", stack_size, NULL, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "failed to start voice chat");
        return;
    }
    button_set_press_cb(on_press);
}

cJSON *voice_configure_command(cJSON *params) {
    cJSON *volume = cJSON_GetObjectItem(params, "volume");
    const char *why = NULL;
    if (volume) {
        if (!cJSON_IsNumber(volume) || volume->valueint < 0 || volume->valueint > 100) {
            why = "volume must be 0-100";
        } else {
            if (!store_volume(volume->valueint)) why = "volume could not be stored";
            else if (atomic_load(&s_ready)) apply_volume(volume->valueint);
            else atomic_store(&s_volume, volume->valueint);
        }
    }

    cJSON *result = cJSON_CreateObject();
    if (why) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "code", "invalid_params");
        cJSON_AddStringToObject(error, "message", why);
        cJSON_AddItemToObject(result, "error", error);
        return result;
    }
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "volume", atomic_load(&s_volume));
    return result;
}
