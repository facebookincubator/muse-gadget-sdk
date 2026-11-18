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

/* components_mosaico/muse_mosaico_iris/muse_mirror_iris.c without ESP-Iris: Iris's pulls, and a new viewer,
 * happen inside the sender's waits, as the Iris task would while the mirror task waits for room. */
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_mirror_link.h"

#define MSG_MAX 1024                           /* a 2056-byte ring: wraps and fills quickly */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_IRIS_CHANNEL_SCREEN 3
#define ESP_IRIS_PIXEL_FORMAT_RGB565 1
#define MUSE_BIG_CAPS 0
#define pdTRUE 1
#define portMAX_DELAY (-1)
#define ESP_LOGE(tag, ...) ((void)(tag), fprintf(stderr, __VA_ARGS__))
typedef int *SemaphoreHandle_t;
typedef struct { uint16_t x, y, width, height; uint32_t stride; uint16_t format, quality; } esp_iris_media_desc_t;
typedef struct {
    esp_err_t (*begin)(const esp_iris_media_desc_t *, esp_iris_media_desc_t *, uint32_t *, void *);
    esp_err_t (*read)(uint32_t, uint8_t *, size_t, size_t *, void *);
    void (*end)(void *);
    void *user_ctx;
} esp_iris_screen_backend_t;
typedef struct {
    uint16_t service_id, method_id;
    uint32_t request_id, deadline_ms;
    const uint8_t *payload;
    size_t payload_size;
} esp_iris_rpc_request_t;
typedef esp_err_t (*esp_iris_rpc_handler_t)(const esp_iris_rpc_request_t *, uint8_t *, size_t, size_t *, void *);
static const char *esp_err_to_name(esp_err_t e) { (void)e; return "error"; }
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }

static int64_t clock_us;
static int64_t esp_timer_get_time(void) { return clock_us += 100; }

/* The ring's mutex; a pull finds it taken while the sender copies (lock_busy). */
static int mutex;
static bool lock_busy;
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex; }
static int xSemaphoreTake(SemaphoreHandle_t s, int ticks)
{
    if (ticks == 0 && lock_busy) return 0;
    assert(!*s);
    *s = 1;
    return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t s) { assert(*s); *s = 0; }

/* The sender's wait for room: Iris pulls, or a new viewer starts. */
static void iris_pulls(void);
static int delays;
static uint32_t start_in_wait;
static int start_epoch(uint32_t epoch);
static void vTaskDelay(int ticks)
{
    assert(ticks == 1);
    delays++;
    clock_us += 10000;
    if (start_in_wait) {
        uint32_t epoch = start_in_wait;
        start_in_wait = 0;
        assert(start_epoch(epoch) == 0);
    }
    iris_pulls();
}

/* Iris: the backend, the RPCs and whether the viewer's SCREEN stream runs. */
static esp_iris_screen_backend_t backend;
static esp_iris_rpc_handler_t handlers[4];
static int registered, refuse;
static esp_err_t esp_iris_screen_register(const esp_iris_screen_backend_t *b)
{
    if (refuse) return ESP_ERR_NO_MEM;
    backend = *b;
    registered++;
    return ESP_OK;
}
static esp_err_t esp_iris_rpc_register(uint16_t service, uint16_t method, esp_iris_rpc_handler_t h, void *ctx)
{
    (void)ctx;
    assert(service == 0x4d55 && method >= 1 && method <= 3);
    handlers[method] = h;
    registered++;
    return ESP_OK;
}
static bool streaming = true;
static bool esp_iris_media_is_streaming(int channel) { assert(channel == ESP_IRIS_CHANNEL_SCREEN); return streaming; }

/* The session's side of the link. */
static const muse_mirror_link_t *opened;
static int opens, closes, inputs;
static bool open_ok = true, input_ok = true;
static uint8_t last_input[8];
static size_t last_input_len;
static const char board[] = "B{\"w\":3,\"h\":2}";
static bool session_ready;
bool muse_mirror_link_open(const muse_mirror_link_t *link) { opened = link; opens++; return open_ok; }
bool muse_mirror_link_input(const muse_mirror_link_t *link, const uint8_t *msg, size_t len)
{
    assert(link == opened && len <= sizeof(last_input));
    memcpy(last_input, msg, len);
    last_input_len = len;
    inputs++;
    return input_ok;
}
void muse_mirror_link_close(const muse_mirror_link_t *link) { assert(link == opened); closes++; }
const char *muse_mirror_link_board(size_t *len)
{
    if (!session_ready) return NULL;
    *len = sizeof(board) - 1;
    return board;
}

#include "iris_impl.inc"

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* The viewer's end of the pipe: tiles of `capacity`, cut into the messages of the epoch it watches. */
static size_t capacity = 3840, pulls_per_wait = 1;
static uint32_t watched;
static uint8_t pipe_buf[8192], messages[32][2100];
static size_t pipe_len, message_len[32], message_count, stale_bytes;

static bool pull(void)
{
    uint8_t tile[3840];
    size_t size = 999;
    esp_err_t err = backend.read(0, tile, capacity, &size, NULL);
    if (err != ESP_OK) {
        assert(!size);
        return false;
    }
    assert(size % STRIDE == 0 && size <= capacity && size >= TILE + 1);
    uint32_t epoch = u32(tile), valid = u32(tile + 4);
    assert(valid && TILE + valid <= size && size - (TILE + valid) < STRIDE);
    for (size_t i = TILE + valid; i < size; i++) assert(!tile[i]);
    if (epoch != watched) {
        stale_bytes += valid;
        return true;
    }
    memcpy(pipe_buf + pipe_len, tile + TILE, valid);
    pipe_len += valid;
    while (pipe_len >= 4 && pipe_len >= 4 + u32(pipe_buf)) {
        size_t n = u32(pipe_buf);
        assert(n && message_count < 32);
        memcpy(messages[message_count], pipe_buf + 4, n);
        message_len[message_count++] = n;
        memmove(pipe_buf, pipe_buf + 4 + n, pipe_len - 4 - n);
        pipe_len -= 4 + n;
    }
    return true;
}

static void iris_pulls(void)
{
    for (size_t i = 0; i < pulls_per_wait; i++) pull();
}

static void drain(void)
{
    while (pull()) {
    }
}

static esp_err_t call(int method, const uint8_t *payload, size_t n, uint8_t *out, size_t cap, size_t *size)
{
    esp_iris_rpc_request_t r = { .service_id = 0x4d55, .method_id = method, .payload = payload, .payload_size = n };
    return handlers[method](&r, out, cap, size, NULL);
}

static esp_err_t stream(uint8_t op, uint32_t epoch, uint32_t message, uint32_t offset)
{
    uint8_t p[13] = { op }, out[13];
    put32(p + 1, epoch); put32(p + 5, message); put32(p + 9, offset);
    size_t size = 99;
    esp_err_t err = call(2, p, sizeof(p), out, sizeof(out), &size);
    assert(err == ESP_OK ? size == 13 && !memcmp(out, p, 13) : size == 0);
    return err;
}

static int start_epoch(uint32_t epoch) { return stream(0, epoch, 0, 0); }

static void fill(uint8_t *m, size_t n, int seed)
{
    for (size_t i = 0; i < n; i++) m[i] = (uint8_t)(seed + i * 7);
}

int main(void)
{
    refuse = 1;
    muse_mirror_iris_register();
    assert(!registered);
    free(s_ring);
    refuse = 0;
    muse_mirror_iris_register();
    assert(registered == 4 && !backend.end);

    uint8_t out[64], msg[2100];
    size_t size;

    /* The pipe only for the viewer that asks for it by its marker; a screenshot is refused. */
    esp_iris_media_desc_t want = { .quality = 0 }, got;
    uint32_t total = 0;
    assert(backend.begin(&want, &got, &total, NULL) == ESP_ERR_NOT_SUPPORTED);
    want.quality = 0x4d55;
    assert(backend.begin(&want, &got, &total, NULL) == ESP_OK);
    assert(got.width == 128 && got.height == 65535 && got.stride == 256 && got.format == 1 && got.quality == 0x4d55);
    assert(total == 256u * 65535u);

    /* The board metadata, once the session runs. */
    assert(call(1, NULL, 0, out, sizeof(out), &size) == ESP_ERR_INVALID_STATE);
    session_ready = true;
    assert(call(1, NULL, 0, out, sizeof(out), &size) == ESP_OK);
    assert(size == strlen("{\"w\":3,\"h\":2,\"iris_pipe\":1}") && !memcmp(out, "{\"w\":3,\"h\":2,\"iris_pipe\":1}", size));
    assert(call(1, NULL, 0, out, 20, &size) == ESP_ERR_INVALID_SIZE && !size);
    assert(call(1, msg, 1, out, sizeof(out), &size) == ESP_ERR_INVALID_ARG);

    /* Malformed commands; a start without the stream, or one the session refuses. */
    uint8_t bad[13] = { 0 };
    assert(call(2, bad, 12, out, sizeof(out), &size) == ESP_ERR_INVALID_ARG);
    assert(stream(1, 7, 0, 0) == ESP_ERR_INVALID_ARG);
    assert(stream(3, 7, 0, 0) == ESP_ERR_INVALID_ARG);
    assert(stream(0, 0, 0, 0) == ESP_ERR_INVALID_ARG);
    assert(stream(0, 7, 1, 0) == ESP_ERR_INVALID_ARG);
    assert(stream(2, 7, 0, 5) == ESP_ERR_INVALID_ARG);
    assert(!s_iris.send(msg, 1, 3000) && !pull());
    streaming = false;
    assert(stream(0, 7, 0, 0) == ESP_ERR_INVALID_STATE && !opens);
    streaming = true;
    open_ok = false;
    assert(stream(0, 7, 0, 0) == ESP_ERR_INVALID_STATE && opens == 1 && !s_iris.send(msg, 1, 3000));
    open_ok = true;

    /* A viewer: messages queue whole, and each pull is whole rows with the epoch and its valid bytes. */
    assert(stream(0, 7, 0, 0) == ESP_OK && opened == &s_iris);
    watched = 7;
    assert(!pull());                                /* nothing yet: Iris asks again */
    fill(msg, 300, 1);
    assert(s_iris.send("B{}", 3, 3000) && s_iris.send(msg, 300, 3000) && s_iris.send("O\1", 2, 3000));
    lock_busy = true;
    assert(!pull());                                /* the sender holds the ring: Iris asks again */
    lock_busy = false;
    drain();
    assert(message_count == 3 && message_len[0] == 3 && !memcmp(messages[0], "B{}", 3));
    assert(message_len[1] == 300 && !memcmp(messages[1], msg, 300) && message_len[2] == 2);

    /* Small pulls split a message, and the ring wraps around its end. */
    capacity = 512;
    for (int k = 0; k < 4; k++) {
        fill(msg, 1500, 10 + k);
        message_count = 0;
        assert(s_iris.send(msg, 1500, 3000));
        drain();
        assert(message_count == 1 && message_len[0] == 1500 && !memcmp(messages[0], msg, 1500));
    }
    /* A pull of no whole number of rows still gets whole rows, inside it. */
    capacity = 300;
    fill(msg, 1500, 20);
    message_count = 0;
    assert(s_iris.send(msg, 1500, 3000));
    drain();
    assert(message_count == 1 && message_len[0] == 1500 && !memcmp(messages[0], msg, 1500));
    capacity = 512;

    /* A full ring: the sender waits for Iris to pull, and gives up when nothing is pulled. */
    fill(msg, 2000, 3);
    assert(s_iris.send(msg, 2000, 3000));
    message_count = 0;
    delays = 0;
    assert(s_iris.send(msg, 1000, 3000) && delays > 0);
    drain();
    assert(message_count == 2 && message_len[0] == 2000 && message_len[1] == 1000 && !memcmp(messages[1], msg, 1000));
    pulls_per_wait = 0;
    assert(s_iris.send(msg, 2000, 3000) && !s_iris.send(msg, 2000, 3000));
    assert(!s_iris.send(msg, RING, 3000));          /* never fits */
    pulls_per_wait = 1;
    drain();

    /* The stream gone: sends fail at once. */
    streaming = false;
    assert(!s_iris.send(msg, 1, 3000));
    streaming = true;

    /* A new viewer while the sender waits for room: that message was the last one's, the old bytes go
     * with it, and the new viewer's pipe holds only its own. */
    pulls_per_wait = 0;
    assert(s_iris.send(msg, 2000, 3000));
    opens = 0;
    start_in_wait = 9;
    assert(s_iris.send(msg, 1000, 3000) && opens == 1);
    assert(!pull());
    watched = 9;
    message_count = 0;
    stale_bytes = 0;
    assert(s_iris.send("B{}", 3, 3000));
    pulls_per_wait = 1;
    drain();
    assert(message_count == 1 && message_len[0] == 3 && !stale_bytes);

    /* Input carries the viewer's epoch; another epoch, or a full queue, is refused. */
    uint8_t in[6] = { 0, 0, 0, 0, 3, 2 };
    put32(in, 9);
    assert(call(3, in, sizeof(in), out, sizeof(out), &size) == ESP_OK && size == 4 && !memcmp(out, in, 4));
    assert(inputs == 1 && last_input_len == 2 && last_input[0] == 3 && last_input[1] == 2);
    input_ok = false;
    assert(call(3, in, sizeof(in), out, sizeof(out), &size) == ESP_ERR_INVALID_STATE && !size);
    input_ok = true;
    put32(in, 10);
    assert(call(3, in, sizeof(in), out, sizeof(out), &size) == ESP_ERR_INVALID_STATE && inputs == 2);
    assert(call(3, in, 4, out, sizeof(out), &size) == ESP_ERR_INVALID_ARG);

    /* Leaving: another epoch's end changes nothing; its own closes the link and empties the pipe. */
    assert(s_iris.send("x", 1, 3000));
    assert(stream(2, 10, 0, 0) == ESP_OK && !closes);
    assert(stream(2, 9, 0, 0) == ESP_OK && closes == 1);
    assert(!pull() && !s_iris.send(msg, 1, 3000));
    put32(in, 9);
    assert(call(3, in, sizeof(in), out, sizeof(out), &size) == ESP_ERR_INVALID_STATE);
    puts("iris link ok");
    return 0;
}
