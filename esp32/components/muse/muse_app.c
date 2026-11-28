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

#include "muse_f2.h"
#include "muse_restart.h"
#include "sdkconfig.h"
#if CONFIG_MUSE_HEAP_WATCH
#include "muse_heapwatch.h"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "muse_audio.h"
#if CONFIG_MUSE_AVATAR_SYNC
#include "muse_avatar.h"
#endif
#include "muse_battery.h"
#include "muse_board.h"
#include "muse_ble.h"
#include "muse_chat.h"
#include "muse_input.h"
#include "muse_passcode.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_text.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_VIDEO_WIFI
#include "muse_video_wifi.h"
#endif
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER || CONFIG_MUSE_LATEST_REPLY
#include "muse_review.h"
#endif

static const char *TAG = "muse";

/* A speaker change the gadget didn't make
 * shows at once (muse_ui.c's volume_watch), so a dark panel lights for it. */
static void wake_for_outside_volume(void)
{
    muse_vol_by_t by;
    muse_settings_volume_changes(&by);
    if (by == MUSE_VOL_BY_PHONE) {
        muse_input_wake("volume set by phone");
    } else if (by == MUSE_VOL_BY_BENCH) {
        muse_input_wake("volume set by bench");
    }
}

/* Applies saved settings to hardware; runs in whichever task changed them. */
static void on_setting(muse_setting_t what)
{
    switch (what) {
    case MUSE_SETTING_VOLUME:
        muse_audio_set_volume(muse_settings_volume());
        wake_for_outside_volume();
        break;
    case MUSE_SETTING_SPEAKER:
        wake_for_outside_volume();
        break;
    case MUSE_SETTING_MIC_GAIN:
        /* A client's set_mic_gain keeps its connection's gain; the new
         * Settings value applies once that connection ends. */
        muse_audio_set_mic_gain(muse_audio_mic_gain());
        break;
    case MUSE_SETTING_WIFI:
        muse_wifi_apply();
        break;
    case MUSE_SETTING_BLE:
        muse_ble_apply();
        break;
    case MUSE_SETTING_HATCH:
#if CONFIG_MUSE_AVATAR_SYNC
        muse_avatar_pairing_changed();
#endif
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
    muse_boot_mark("board_init");
    ESP_ERROR_CHECK(board->init());
    muse_boot_mark("board_ready");
    ESP_ERROR_CHECK(muse_settings_init());
    muse_passcode_init();   /* the passcode lock: on, it starts locked (a reboot locks) */
    muse_settings_set_listener(on_setting);
    muse_state_init();
#if CONFIG_MUSE_AVATAR_SYNC
    muse_avatar_start();
#endif
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER || CONFIG_MUSE_LATEST_REPLY
#if CONFIG_MUSE_LATEST_REPLY
    if (muse_review_init() != ESP_OK) ESP_LOGW(TAG, "latest reply storage unavailable");
#else
    ESP_ERROR_CHECK(muse_review_init());
#endif
#endif
    muse_battery_init();
    muse_battery_set_shutdown_mv(board->battery_shutdown_mv);   /* low-battery shutdown */
#if CONFIG_MUSE_LATIN_FONT
    muse_text_keep_latin();   /* before the first caption, so it keeps its accents */
#endif
    muse_state_set_caption("Waking up...");
    /* Before the UI: the touch boards' settings show Phone camera only when
     * a transport is there as they're built. Its server waits for Wi-Fi. */
#if CONFIG_MUSE_VIDEO_WIFI
    muse_video_wifi_start();
#endif
    ESP_ERROR_CHECK(muse_ui_start());
    ESP_LOGI(TAG, "UI built: free internal %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    muse_boot_mark("ui_built");
#if CONFIG_MUSE_HEAP_WATCH
    muse_heapwatch_start();   /* bench only */
#endif

    QueueHandle_t q = xQueueCreate(16, sizeof(muse_input_event_t));
    ESP_ERROR_CHECK(muse_input_start(q));

    /* Start audio first: its I2S DMA buffers need internal RAM, which
     * Link's Wi-Fi and TLS take as they come up. Started later, the codec
     * open can fail to get them and crash in i2s_rx_channel_start (Waveshare
     * 1.75C with live voice calls). The home shows as soon as it's drawable
     * (fast start): no fixed hold for the boot animation. */
    bool has_audio = board->audio_init || board->mic_init;
    esp_err_t voice_rc = has_audio ? muse_voice_start(q) : ESP_OK;
    if (!has_audio) {
        ESP_LOGI(TAG, "display-only board: no microphone or speaker");
        muse_state_set_mode(MUSE_MODE_IDLE);
        muse_state_set_caption("%s", "");
    } else if (voice_rc != ESP_OK) {
        ESP_LOGE(TAG, "voice pipeline unavailable");
    } else {
        muse_state_set_mode(MUSE_MODE_IDLE);
        muse_state_set_caption("%s", "");   /* the button icons say how to talk */
    }

    muse_f2_boot();   /* the F2 flash check, when armed (muse_f2.h) */
    muse_hatch_start();
    /* Home Link owns the radios; these just hand it the saved settings. */
    muse_wifi_apply();
    muse_ble_apply();
    ESP_LOGI(TAG, "ready: free heap %u internal, %u psram",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
