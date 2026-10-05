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

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_battery.h"
#include "muse_board.h"
#include "muse_ble.h"
#include "muse_chat.h"
#include "muse_input.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"

static const char *TAG = "muse";

/* Applies saved settings to hardware; runs in whichever task changed them. */
static void on_setting(muse_setting_t what)
{
    switch (what) {
    case MUSE_SETTING_VOLUME:
        muse_audio_set_volume(muse_settings_volume());
        break;
    case MUSE_SETTING_MIC_GAIN:
        muse_audio_set_mic_gain(muse_settings_mic_gain());
        break;
    case MUSE_SETTING_WIFI:
        muse_wifi_apply();
        break;
    case MUSE_SETTING_BLE:
        muse_ble_apply();
        break;
    case MUSE_SETTING_HATCH:
        muse_hatch_config_changed();
        break;
    default:
        break;   /* brightness, sleep and the speaker are polled where they're used */
    }
}

const muse_board_t *muse_board;

void muse_app_run(const muse_board_t *board)
{
    muse_board = board;
    ESP_LOGI(TAG, "board: %s", board->name);
    ESP_ERROR_CHECK(board->init());
    ESP_ERROR_CHECK(muse_settings_init());
    muse_settings_set_listener(on_setting);
    muse_state_init();
    muse_battery_init();
    muse_state_set_caption("WAKING UP...");
    ESP_ERROR_CHECK(muse_ui_start());
    ESP_LOGI(TAG, "UI built: free internal %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    QueueHandle_t q = xQueueCreate(16, sizeof(muse_input_event_t));
    ESP_ERROR_CHECK(muse_input_start(q));

    /* Let the boot animation (flame ignites, eyes open) play out. */
    vTaskDelay(pdMS_TO_TICKS(1400));
    if (!board->audio_init) {
        ESP_LOGI(TAG, "display-only board: no microphone or speaker");
        muse_state_set_mode(MUSE_MODE_IDLE);
        muse_state_set_caption("%s", "");
    } else if (muse_voice_start(q) != ESP_OK) {
        ESP_LOGE(TAG, "voice pipeline unavailable");
    } else {
        muse_state_set_mode(MUSE_MODE_IDLE);
        muse_state_set_caption("%s", "");   /* the button icons say how to talk */
    }

    muse_hatch_start();
    /* Home Link owns the radios; these just hand it the saved settings. */
    muse_wifi_apply();
    muse_ble_apply();
    ESP_LOGI(TAG, "ready: free heap %u internal, %u psram",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
