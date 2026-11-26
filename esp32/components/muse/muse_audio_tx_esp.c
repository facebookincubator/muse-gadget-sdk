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

/*
 * The board side of muse_audio_tx.h: the speaker channel's on_sent callback,
 * the lock between it and the tasks, and the cache sync on chips whose DMA
 * reads internal memory through a cache. A board attaches its TX channel in
 * audio_init, before enabling it (ESP-IDF takes callbacks only then).
 */

#include <stdlib.h>

#include "sdkconfig.h"
#include "driver/i2s_common.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "soc/soc_caps.h"
#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
#include "esp_cache.h"
#endif

#include "muse_audio.h"
#include "muse_audio_dma.h"
#include "muse_audio_tx.h"
#include "muse_board.h"

#if CONFIG_MUSE_GADGET_BLE_AUDIO   /* the clear's only caller is the musegadgets player (mg_glue.c) */
static const char *TAG = "muse_audio";

static muse_audio_tx_t s_tx;   /* internal RAM: the ISR's */
static portMUX_TYPE s_tx_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_tx_on;

static bool IRAM_ATTR on_sent(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx)
{
    (void)handle;
    (void)ctx;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_tx_mux);
    muse_audio_tx_sent(&s_tx, event->dma_buf, event->size, now);
    portEXIT_CRITICAL_ISR(&s_tx_mux);
    return false;
}

esp_err_t muse_audio_attach_tx(i2s_chan_handle_t tx)
{
    unsigned desc = muse_board->i2s_dma_desc ? muse_board->i2s_dma_desc : MUSE_AUDIO_DMA_DEFAULT_DESC;
    unsigned frames = muse_board->i2s_dma_frames ? muse_board->i2s_dma_frames : MUSE_AUDIO_DMA_DEFAULT_FRAMES;
    uint32_t ring_len = (uint32_t)(desc * frames);
    int16_t *ring = malloc(ring_len * sizeof(int16_t));
    if (!ring) {
        ESP_LOGW(TAG, "speaker clear off: no memory");
        return ESP_ERR_NO_MEM;
    }
    muse_audio_tx_init(&s_tx, desc, frames, MUSE_AUDIO_RATE, ring, ring_len);
    const i2s_event_callbacks_t cbs = { .on_sent = on_sent };
    esp_err_t err = i2s_channel_register_event_callback(tx, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "speaker clear off: %s", esp_err_to_name(err));
        free(ring);
        muse_audio_tx_init(&s_tx, desc, frames, MUSE_AUDIO_RATE, NULL, 0);
        return err;
    }
    s_tx_on = true;
    return ESP_OK;
}

size_t muse_audio_tx_note_begin(size_t frames)
{
    if (!s_tx_on) {
        return frames;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_tx_mux);
    size_t n = muse_audio_tx_begin(&s_tx, frames, now);
    portEXIT_CRITICAL(&s_tx_mux);
    return n;
}

void muse_audio_tx_note_write(const int16_t *mono, size_t frames, bool mute)
{
    if (!s_tx_on) {
        return;
    }
    portENTER_CRITICAL(&s_tx_mux);
    muse_audio_tx_wrote(&s_tx, mono, frames, mute);
    portEXIT_CRITICAL(&s_tx_mux);
}

size_t muse_audio_speaker_clear(int16_t *keep, size_t max)
{
    if (!s_tx_on) {
        return 0;
    }
    portENTER_CRITICAL(&s_tx_mux);
    size_t n = muse_audio_tx_clear(&s_tx, esp_timer_get_time(), keep, max);
    void *bufs[MUSE_AUDIO_TX_MAX_DESC];
    unsigned nbufs = s_tx.known;
    size_t bytes = (size_t)s_tx.frames * 4;
    for (unsigned i = 0; i < nbufs; i++) {
        bufs[i] = s_tx.bufs[i];
    }
    portEXIT_CRITICAL(&s_tx_mux);
#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
    for (unsigned i = 0; i < nbufs; i++) {
        esp_cache_msync(bufs[i], bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);   /* as the driver does after it writes them */
    }
#else
    (void)bufs;
    (void)bytes;
#endif
    return n;
}
#else
/* Without BLE gadget audio nothing clears the speaker: no callback, no ring,
 * and muse_audio writes as it always did. */
esp_err_t muse_audio_attach_tx(i2s_chan_handle_t tx)
{
    (void)tx;
    return ESP_OK;
}

size_t muse_audio_tx_note_begin(size_t frames)
{
    return frames;
}

void muse_audio_tx_note_write(const int16_t *mono, size_t frames, bool mute)
{
    (void)mono;
    (void)frames;
    (void)mute;
}

size_t muse_audio_speaker_clear(int16_t *keep, size_t max)
{
    (void)keep;
    (void)max;
    return 0;
}
#endif
