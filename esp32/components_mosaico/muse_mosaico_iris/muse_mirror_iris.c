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
#include "sdkconfig.h"
#if CONFIG_MUSE_MIRROR_IRIS
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "muse_mem.h"
#include "muse_mirror_session.h"
#include "muse_mirror_link.h"

/* The mirror's messages over ESP-Iris (protocols/MIRROR.md). Out: a byte pipe
 * that Iris pulls through its SCREEN channel as the viewer's credit allows,
 * so nothing waits on a request from the computer. In: RPCs. */
#define SERVICE 0x4d55          /* "MU" */
#define METHOD_BOARD 1
#define METHOD_STREAM 2
#define METHOD_INPUT 3
#define MARKER 0x4d55           /* in the stream's quality: the viewer's pipe, not a screenshot */
#define STRIDE 256              /* a pipe row; a pull is whole rows, the last one padded */
#define ROWS 65535
#define TILE 8                  /* each pull starts with epoch:u32, valid:u32 */
#ifndef MSG_MAX
#define MSG_MAX MUSE_MIRROR_MSG_MAX
#endif
#define RING (2 * (MSG_MAX + 4))
enum { OP_START, OP_END = 2 };

static const char *TAG = "muse_mirror";
static atomic_uint s_epoch;     /* the viewer's, 0 with none */
static SemaphoreHandle_t s_lock;
static uint8_t *s_ring;
static size_t s_head, s_used;   /* under s_lock */

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t n)
{
    for (int i = 0; i < 4; i++) p[i] = n >> (8 * i);
}

static void ring_put(const void *data, size_t n)
{
    size_t at = (s_head + s_used) % RING, first = n < RING - at ? n : RING - at;
    memcpy(s_ring + at, data, first);
    memcpy(s_ring, (const uint8_t *)data + first, n - first);
    s_used += n;
}

static void ring_take(uint8_t *out, size_t n)
{
    size_t first = n < RING - s_head ? n : RING - s_head;
    memcpy(out, s_ring + s_head, first);
    memcpy(out + first, s_ring, n - first);
    s_head = (s_head + n) % RING;
    s_used -= n;
}

/* Under s_lock: a new viewer, or none, starts with nothing queued. */
static void viewer_set(uint32_t epoch)
{
    atomic_store(&s_epoch, epoch);
    s_head = s_used = 0;
}

static bool iris_send(const void *msg, size_t len, int ms)
{
    uint32_t epoch = atomic_load(&s_epoch);
    if (!epoch || len + 4 > RING) return false;
    uint8_t hdr[4];
    put32(hdr, len);
    int64_t until = esp_timer_get_time() + ms * 1000LL;
    for (;;) {
        if (!esp_iris_media_is_streaming(ESP_IRIS_CHANNEL_SCREEN)) return false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t now = atomic_load(&s_epoch);
        bool room = RING - s_used >= len + 4;
        if (now == epoch && room) {
            ring_put(hdr, 4);
            ring_put(msg, len);
        }
        xSemaphoreGive(s_lock);
        if (now != epoch) return now != 0;   /* a new viewer: the message was the last one's */
        if (room) return true;
        if (esp_timer_get_time() >= until) return false;
        vTaskDelay(1);
    }
}

static const muse_mirror_link_t s_iris = { .send = iris_send, .audio = true };

static esp_err_t pipe_begin(const esp_iris_media_desc_t *requested, esp_iris_media_desc_t *actual,
                            uint32_t *total, void *ctx)
{
    (void)ctx;
    if (!requested || requested->quality != MARKER) return ESP_ERR_NOT_SUPPORTED;
    *actual = (esp_iris_media_desc_t){ .width = STRIDE / 2, .height = ROWS, .stride = STRIDE,
                                      .format = ESP_IRIS_PIXEL_FORMAT_RGB565, .quality = MARKER };
    *total = STRIDE * ROWS;
    return ESP_OK;
}

/* The Iris task: never waits; with nothing to send it is asked again within 10 ms. */
static esp_err_t pipe_read(uint32_t offset, uint8_t *out, size_t capacity, size_t *size, void *ctx)
{
    (void)offset; (void)ctx;
    *size = 0;
    if (!out || capacity < STRIDE) return ESP_ERR_INVALID_ARG;
    capacity -= capacity % STRIDE;     /* whole rows: the padding below stays inside */
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    uint32_t epoch = atomic_load(&s_epoch);
    size_t n = s_used < capacity - TILE ? s_used : capacity - TILE;
    if (epoch && n) {
        put32(out, epoch);
        put32(out + 4, n);
        ring_take(out + TILE, n);
    }
    xSemaphoreGive(s_lock);
    if (!epoch || !n) return ESP_ERR_NOT_FOUND;
    size_t used = TILE + n, padded = (used + STRIDE - 1) / STRIDE * STRIDE;
    memset(out + used, 0, padded - used);
    *size = padded;
    return ESP_OK;
}

static esp_err_t board_rpc(const esp_iris_rpc_request_t *request, uint8_t *response,
                           size_t capacity, size_t *size, void *ctx)
{
    (void)ctx;
    *size = 0;
    size_t len;
    const char *board = muse_mirror_link_board(&len);
    if (!board) return ESP_ERR_INVALID_STATE;
    if (!request || request->payload_size || !response) return ESP_ERR_INVALID_ARG;
    int n = snprintf((char *)response, capacity, "%.*s,\"iris_pipe\":1}", (int)len - 2, board + 1);
    if (n < 0 || (size_t)n >= capacity) return ESP_ERR_INVALID_SIZE;
    *size = n;
    return ESP_OK;
}

static esp_err_t stream_rpc(const esp_iris_rpc_request_t *request, uint8_t *response,
                            size_t capacity, size_t *size, void *ctx)
{
    (void)ctx;
    *size = 0;
    if (!request || !request->payload || request->payload_size != 13 || !response || capacity < 13) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *p = request->payload;
    uint32_t epoch = get32(p + 1);
    if ((p[0] != OP_START && p[0] != OP_END) || !epoch || get32(p + 5) || get32(p + 9)) return ESP_ERR_INVALID_ARG;
    if (p[0] == OP_START) {
        if (!esp_iris_media_is_streaming(ESP_IRIS_CHANNEL_SCREEN)) return ESP_ERR_INVALID_STATE;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t was = atomic_load(&s_epoch);
        viewer_set(epoch);
        xSemaphoreGive(s_lock);
        if (!muse_mirror_link_open(&s_iris)) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            viewer_set(was);
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_STATE;      /* the cable has the viewer */
        }
    } else if (epoch == atomic_load(&s_epoch)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        viewer_set(0);
        xSemaphoreGive(s_lock);
        muse_mirror_link_close(&s_iris);
    }
    memcpy(response, p, 13);
    *size = 13;
    return ESP_OK;
}

static esp_err_t input_rpc(const esp_iris_rpc_request_t *request, uint8_t *response,
                           size_t capacity, size_t *size, void *ctx)
{
    (void)ctx;
    *size = 0;
    if (!request || !request->payload || request->payload_size < 5 || !response || capacity < 4) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t epoch = get32(request->payload);
    if (epoch != atomic_load(&s_epoch)) return ESP_ERR_INVALID_STATE;
    if (!muse_mirror_link_input(&s_iris, request->payload + 4, request->payload_size - 4)) {
        return ESP_ERR_INVALID_STATE;   /* the queue is full, or the session dropped this viewer */
    }
    memcpy(response, request->payload, 4);
    *size = 4;
    return ESP_OK;
}

void muse_mirror_iris_register(void)
{
    static const esp_iris_screen_backend_t pipe = { .begin = pipe_begin, .read = pipe_read };
    s_lock = xSemaphoreCreateMutex();
    s_ring = heap_caps_malloc(RING, MUSE_BIG_CAPS);
    esp_err_t err = s_lock && s_ring ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = esp_iris_screen_register(&pipe);
    if (err == ESP_OK) err = esp_iris_rpc_register(SERVICE, METHOD_BOARD, board_rpc, NULL);
    if (err == ESP_OK) err = esp_iris_rpc_register(SERVICE, METHOD_STREAM, stream_rpc, NULL);
    if (err == ESP_OK) err = esp_iris_rpc_register(SERVICE, METHOD_INPUT, input_rpc, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "no mirror over ESP-Iris: %s", esp_err_to_name(err));
}
#endif
