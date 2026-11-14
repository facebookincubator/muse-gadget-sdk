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

#include "muse_wake.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_timer.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "model_path.h"
#include "sdkconfig.h"

#include "muse_audio.h"
#include "muse_input.h"
#include "muse_mem.h"
#include "muse_settings.h"
#include "muse_voice.h"

static const char *TAG = "muse_wake";

/* The engine is a build-time choice (CONFIG_MUSE_WAKE_WORD_ENGINE). */
#if defined(CONFIG_MUSE_WAKE_WORD_WAKENET)
#define WAKE_WAKENET 1
#else
#define WAKE_WAKENET 0
#endif

#define FEED_DEPTH 24              /* 20 ms chunks: about half a second of slack */
#define MN_ACC_SAMPLES 8192        /* AFE output waiting for a full MultiNet frame */
#define MN_CMD_ID 1
#define MN_DETECT_MS 5760          /* MultiNet listens this long, then restarts */
#define WAKE_REFRACTORY_MS 3000    /* one trigger per call start */

typedef struct {
    int16_t pcm[MUSE_AUDIO_CHUNK];
    uint16_t frames;
} feed_chunk_t;

static QueueHandle_t s_feed_q;
static StaticQueue_t s_feed_q_ctrl;
static atomic_bool s_ready;        /* engines up: muse_wake_feed() may queue */
static atomic_uint s_dropped;

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static int16_t *s_feed_acc;        /* idle chunks gathered into one AFE feed frame */
static size_t s_feed_acc_n;
static int s_feed_chunksize;

#if !WAKE_WAKENET
static esp_mn_iface_t *s_mn;
static model_iface_data_t *s_mn_data;
static int16_t *s_mn_acc;          /* AFE output gathered into one MultiNet frame */
static size_t s_mn_acc_n;
static int s_mn_chunksize;
#endif

void muse_wake_feed(const int16_t *mono, size_t frames)
{
    if (!s_feed_q || !atomic_load(&s_ready) || !muse_settings_wake_word()) {
        return;
    }
    if (frames > MUSE_AUDIO_CHUNK) {
        frames = MUSE_AUDIO_CHUNK;
    }
    feed_chunk_t chunk;
    memcpy(chunk.pcm, mono, frames * sizeof(int16_t));
    chunk.frames = (uint16_t)frames;
    if (xQueueSend(s_feed_q, &chunk, 0) != pdTRUE) {
        unsigned dropped = atomic_fetch_add(&s_dropped, 1) + 1;
        if (dropped == 1 || dropped % 100 == 0) {
            ESP_LOGW(TAG, "detector behind: %u chunks dropped", dropped);
        }
    }
}

static bool engines_init(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models || !models->num) {
        ESP_LOGE(TAG, "no speech models in the \"model\" partition: wake word off "
                      "(flash the whole image, not just the app)");
        if (models) {
            esp_srmodel_deinit(models);
        }
        return false;
    }
    afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) {
        ESP_LOGE(TAG, "AFE config failed: wake word off");
        return false;
    }
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
#if WAKE_WAKENET
    char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "jarvis");
    if (!wn_name) {
        ESP_LOGE(TAG, "the WakeNet \"Jarvis\" engine is selected but no wn9_jarvis_tts "
                      "model is packed: set CONFIG_SR_WN_WN9_JARVIS_TTS=y");
        return false;
    }
    cfg->wakenet_init = true;
    cfg->wakenet_model_name = wn_name;
#else
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (!mn_name) {
        ESP_LOGE(TAG, "the MultiNet \"Hi Muse\" engine is selected but no English "
                      "MultiNet model is packed: set CONFIG_SR_MN_EN_MULTINET7_QUANT=y");
        return false;
    }
    cfg->wakenet_init = false;   /* only the command below may trigger */
#endif
    s_afe = esp_afe_handle_from_config(cfg);
    if (!s_afe) {
        ESP_LOGE(TAG, "no AFE handle: wake word off");
        return false;
    }
    s_afe_data = s_afe->create_from_config(cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create failed: wake word off");
        return false;
    }
    if (s_afe->get_samp_rate(s_afe_data) != MUSE_AUDIO_RATE) {
        ESP_LOGW(TAG, "AFE wants %d Hz, the mic gives %d", s_afe->get_samp_rate(s_afe_data),
                 MUSE_AUDIO_RATE);
    }
    s_feed_chunksize = s_afe->get_feed_chunksize(s_afe_data);
    s_feed_acc = heap_caps_malloc((size_t)s_feed_chunksize * sizeof(int16_t), MUSE_BIG_CAPS);
    if (!s_feed_acc) {
        ESP_LOGE(TAG, "no memory for the feed buffer: wake word off");
        return false;
    }
#if WAKE_WAKENET
    ESP_LOGI(TAG, "listening for \"Jarvis\" (%s)", wn_name);
#else
    s_mn = esp_mn_handle_from_name(mn_name);
    if (!s_mn) {
        ESP_LOGE(TAG, "no MultiNet handle for %s: wake word off", mn_name);
        return false;
    }
    s_mn_data = s_mn->create(mn_name, MN_DETECT_MS);
    if (!s_mn_data) {
        ESP_LOGE(TAG, "MultiNet create failed: wake word off");
        return false;
    }
    if (esp_mn_commands_alloc(s_mn, s_mn_data) != ESP_OK
        || esp_mn_commands_add(MN_CMD_ID, "HI MUSE") != ESP_OK) {
        ESP_LOGE(TAG, "MultiNet commands failed: wake word off");
        return false;
    }
    /* update() returns the phrases MultiNet couldn't parse (NULL: none).
     * Ours is the only command, so its presence in that list is the failure
     * that matters. */
    esp_mn_error_t *bad = esp_mn_commands_update();
    for (int i = 0; bad && i < bad->num; i++) {
        if (bad->phrases[i] && bad->phrases[i]->string
            && !strcmp(bad->phrases[i]->string, "HI MUSE")) {
            ESP_LOGE(TAG, "\"HI MUSE\" is not a usable MultiNet command: wake word off");
            return false;
        }
    }
    s_mn_chunksize = s_mn->get_samp_chunksize(s_mn_data);
    s_mn_acc = heap_caps_malloc(MN_ACC_SAMPLES * sizeof(int16_t), MUSE_BIG_CAPS);
    if (!s_mn_acc) {
        ESP_LOGE(TAG, "no memory for the MultiNet buffer: wake word off");
        return false;
    }
    ESP_LOGI(TAG, "listening for \"Hi Muse\" (%s)", mn_name);
#endif
    return true;
}

#if !WAKE_WAKENET
/* Feeds one AFE output frame to MultiNet; true when "Hi Muse" was heard. */
static bool mn_detect(const int16_t *pcm, size_t frames)
{
    if (frames > MN_ACC_SAMPLES || s_mn_acc_n + frames > MN_ACC_SAMPLES) {
        /* The detector is drained every fetch, so this is a stall: keep the
         * newest audio and drop whatever was waiting. */
        s_mn_acc_n = 0;
        if (frames > MN_ACC_SAMPLES) {
            pcm += frames - MN_ACC_SAMPLES;
            frames = MN_ACC_SAMPLES;
        }
    }
    memcpy(s_mn_acc + s_mn_acc_n, pcm, frames * sizeof(int16_t));
    s_mn_acc_n += frames;
    bool heard = false;
    while (s_mn_acc_n >= (size_t)s_mn_chunksize) {
        esp_mn_state_t state = s_mn->detect(s_mn_data, s_mn_acc);
        if (state == ESP_MN_STATE_DETECTING) {
            memmove(s_mn_acc, s_mn_acc + s_mn_chunksize,
                    (s_mn_acc_n - (size_t)s_mn_chunksize) * sizeof(int16_t));
            s_mn_acc_n -= (size_t)s_mn_chunksize;
            continue;
        }
        /* Detected or timed out: the recogniser starts over, and so does
         * the buffer (its frames are inside the window just judged). */
        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = s_mn->get_results(s_mn_data);
            if (r && r->num > 0) {
                ESP_LOGI(TAG, "heard \"%s\" (%.2f)", r->string, (double)r->prob[0]);
                heard = r->command_id[0] == MN_CMD_ID;
            }
        }
        s_mn->clean(s_mn_data);
        s_mn_acc_n = 0;
    }
    return heard;
}
#endif

static void wake_task(void *arg)
{
    (void)arg;
    if (!engines_init()) {
        vTaskDelete(NULL);
    }
    atomic_store(&s_ready, true);
    bool parked = false;
    int64_t refractory_until = 0;
    for (;;) {
        /* Parked while the setting is off or a call is up: no feed reaches
         * the AFE, and what was queued is dropped, so a word said during a
         * call can't start another one when it ends. */
        if (!muse_settings_wake_word() || muse_voice_in_call()) {
            if (!parked) {
                parked = true;
                xQueueReset(s_feed_q);
                s_feed_acc_n = 0;
#if !WAKE_WAKENET
                s_mn_acc_n = 0;
                s_mn->clean(s_mn_data);
#endif
                s_afe->reset_buffer(s_afe_data);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        parked = false;
        /* Pump everything the voice task captured into the AFE. */
        feed_chunk_t chunk;
        while (xQueueReceive(s_feed_q, &chunk, 0) == pdTRUE) {
            size_t off = 0;
            while (off < chunk.frames) {
                size_t take = (size_t)s_feed_chunksize - s_feed_acc_n;
                if (take > chunk.frames - off) {
                    take = chunk.frames - off;
                }
                memcpy(s_feed_acc + s_feed_acc_n, chunk.pcm + off, take * sizeof(int16_t));
                s_feed_acc_n += take;
                off += take;
                if (s_feed_acc_n == (size_t)s_feed_chunksize) {
                    s_afe->feed(s_afe_data, s_feed_acc);
                    s_feed_acc_n = 0;
                }
            }
        }
        afe_fetch_result_t *res = s_afe->fetch_with_delay(s_afe_data, pdMS_TO_TICKS(100));
        if (!res) {
            continue;
        }
        bool heard;
#if WAKE_WAKENET
        heard = res->wakeup_state == WAKENET_DETECTED;
#else
        heard = res->data && res->data_size > 0
                && mn_detect(res->data, (size_t)res->data_size / sizeof(int16_t));
#endif
        int64_t now = esp_timer_get_time();
        if (heard && now >= refractory_until) {
            refractory_until = now + WAKE_REFRACTORY_MS * 1000;
            ESP_LOGI(TAG, "wake word: starting a call");
            muse_input_wake_word();
        }
    }
}

esp_err_t muse_wake_start(void)
{
    uint8_t *storage = heap_caps_malloc(FEED_DEPTH * sizeof(feed_chunk_t), MUSE_BIG_CAPS);
    if (!storage) {
        return ESP_ERR_NO_MEM;
    }
    s_feed_q = xQueueCreateStatic(FEED_DEPTH, sizeof(feed_chunk_t), storage, &s_feed_q_ctrl);
    if (!s_feed_q) {
        heap_caps_free(storage);
        return ESP_FAIL;
    }
    if (xTaskCreatePinnedToCoreWithCaps(wake_task, "muse_wake", 8192, NULL, 4, NULL, 0,
                                        MUSE_BIG_CAPS) != pdPASS) {
        xTaskCreate(wake_task, "muse_wake", 8192, NULL, 4, NULL);
    }
    return ESP_OK;
}
