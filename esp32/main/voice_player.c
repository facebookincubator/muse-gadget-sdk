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


#include "voice_player.h"
#include "voice_board.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "link.voice_player";

// Ten seconds of reply, so synthesis can run well ahead of playback.
#define BUFFER_BYTES      (10 * VOICE_PLAYER_RATE * sizeof(int16_t))
// Start playing once this much is buffered, to ride out network hiccups.
#define PREBUFFER_BYTES   (600 * VOICE_PLAYER_RATE / 1000 * sizeof(int16_t))
// Samples moved to I2S per write: 20 ms.
#define CHUNK_SAMPLES     (VOICE_PLAYER_RATE / 50)
// Let the DMA buffers run out before turning the amplifier off.
#define AMP_TAIL_MS       120

#define BIT_IDLE          BIT0

static StreamBufferHandle_t s_buf;
static EventGroupHandle_t s_events;
static TaskHandle_t s_task;
static volatile bool s_active;
static volatile bool s_ended;
static volatile bool s_started;
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
static volatile bool s_failed;
#endif

// Voice PE: 16 kHz mono to 48 kHz stereo 32-bit, by interpolation. `last`
// carries the previous sample across chunks.
static size_t upsample(const int16_t *in, size_t n, int32_t *out, int16_t *last) {
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    // The ES8311's ADC and DAC share a 16 kHz bus. No rate conversion.
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = out[2 * i + 1] = (int32_t)in[i] * 65536;
    }
    (void)last;
    return n;
#else
    for (size_t i = 0; i < n; i++) {
        int32_t prev = *last, cur = in[i];
        int32_t s[3] = {(2 * prev + cur) / 3, (prev + 2 * cur) / 3, cur};
        for (int k = 0; k < 3; k++) {
            out[6 * i + 2 * k] = s[k] << 16;
            out[6 * i + 2 * k + 1] = s[k] << 16;
        }
        *last = in[i];
    }
    return 3 * n;
#endif
}

static void player_task(void *arg) {
    int16_t *in = heap_caps_malloc(CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int32_t *out = heap_caps_malloc(CHUNK_SAMPLES * 6 * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (!in || !out) {
        ESP_LOGE(TAG, "no memory for the player");
        vTaskSuspend(NULL);
        return;
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (s_active && !s_ended && xStreamBufferBytesAvailable(s_buf) < PREBUFFER_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
#if !CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
        if (s_active) voice_board_amp(true);
#endif
        int16_t last = 0;
        while (s_active) {
            size_t got = xStreamBufferReceive(s_buf, in, CHUNK_SAMPLES * sizeof(int16_t),
                                              pdMS_TO_TICKS(20));
            if (got < sizeof(int16_t)) {
                // Out of audio: done if the reply is complete, otherwise the
                // DMA plays silence until more arrives.
                if (s_ended && xStreamBufferIsEmpty(s_buf)) break;
                continue;
            }
            size_t frames = upsample(in, got / sizeof(int16_t), out, &last);
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
            if (voice_board_speaker_write(out, frames) != ESP_OK) {
                s_failed = true;
                s_active = false;
                break;
            }
#else
            voice_board_speaker_write(out, frames);
#endif
            s_started = true;
        }
        vTaskDelay(pdMS_TO_TICKS(AMP_TAIL_MS));
        voice_board_amp(false);
        s_active = false;
        xEventGroupSetBits(s_events, BIT_IDLE);
    }
}

esp_err_t voice_player_init(void) {
    s_buf = xStreamBufferCreateWithCaps(BUFFER_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_events = xEventGroupCreate();
    if (!s_buf || !s_events) return ESP_ERR_NO_MEM;
    xEventGroupSetBits(s_events, BIT_IDLE);
    // Above the network tasks, so playback keeps up while replies download.
    // Buffers and stack in PSRAM (it never touches flash): pairing needs an
    // 8 KB internal block for its TLS task.
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    const int stack_size = 4096;
#else
    const int stack_size = 3072;
#endif
    if (xTaskCreateWithCaps(player_task, "voice_play", stack_size, NULL, 5, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void voice_player_begin(void) {
    voice_player_stop();
    xEventGroupWaitBits(s_events, BIT_IDLE, pdFALSE, pdTRUE, portMAX_DELAY);
    // Nothing is blocked on the buffer now, so the reset succeeds.
    xStreamBufferReset(s_buf);
    s_ended = false;
    s_started = false;
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
    s_failed = false;
#endif
    s_active = true;
    xEventGroupClearBits(s_events, BIT_IDLE);
    xTaskNotifyGive(s_task);
}

esp_err_t voice_player_write(const int16_t *pcm, size_t samples) {
    const uint8_t *p = (const uint8_t *)pcm;
    size_t left = samples * sizeof(int16_t);
    while (left) {
        if (!s_active || s_ended) return ESP_ERR_INVALID_STATE;
        size_t sent = xStreamBufferSend(s_buf, p, left, pdMS_TO_TICKS(100));
        p += sent;
        left -= sent;
    }
    return ESP_OK;
}

void voice_player_end(void) {
    s_ended = true;
}

bool voice_player_wait(int timeout_ms) {
    return xEventGroupWaitBits(s_events, BIT_IDLE, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(timeout_ms)) & BIT_IDLE;
}

void voice_player_stop(void) {
    s_active = false;
}

bool voice_player_started(void) {
    return s_started;
}

#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
bool voice_player_failed(void) { return s_failed; }
#endif
