/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* RPC 0x5054: 1 queues a line, 2 reads a replayable byte range, 3 reads the
 * gauge. The core alone parses commands and writes PTEST output. Iris owns USB.
 * All integers on the wire are little endian. A boot token fences stale hosts.
 * The ring holds both repeats' results without waiting for a host in capture. */
#define PTEST_SERVICE 0x5054
#define OUTPUT_BYTES (128 * 1024)
#define INPUT_BYTES 256
#define READ_HEADER_BYTES 16
static SemaphoreHandle_t s_mutex;
static uint8_t *s_output;
static uint8_t s_input[INPUT_BYTES];
static size_t s_input_pos, s_input_size;
static uint32_t s_boot, s_written;
static bool s_overflow, s_run_active;
static esp_pm_lock_handle_t s_awake;

esp_err_t muse_mosaico_ptest_battery(int *pct, int *mv, int *ma);

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) { p[i] = (uint8_t)(v >> (i * 8)); }
}

static esp_err_t rpc(const esp_iris_rpc_request_t *r, uint8_t *out,
                     size_t capacity, size_t *size, void *ctx)
{
    (void)ctx;
    *size = 0;
    if (!r || !r->payload || r->payload_size < 4) { return ESP_ERR_INVALID_ARG; }
    uint32_t boot = get32(r->payload);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t e = ESP_OK;
    if (r->method_id == 2) {
        if (r->payload_size != 8 || capacity < READ_HEADER_BYTES) {
            e = ESP_ERR_INVALID_ARG;
        } else {
            uint32_t oldest = s_written > OUTPUT_BYTES ? s_written - OUTPUT_BYTES : 0;
            /* A new host starts at the live tail, never a previous run's ACK. */
            uint32_t offset = boot ? get32(r->payload + 4) : s_written;
            if ((boot && boot != s_boot) || offset < oldest || offset > s_written || s_overflow) {
                e = ESP_ERR_INVALID_STATE;
            } else {
                size_t n = s_written - offset;
                if (n > capacity - READ_HEADER_BYTES) { n = capacity - READ_HEADER_BYTES; }
                put32(out, 1);
                put32(out + 4, s_boot);
                put32(out + 8, offset);
                put32(out + 12, s_written);
                for (size_t i = 0; i < n; ++i) { out[READ_HEADER_BYTES + i] = s_output[(offset + i) % OUTPUT_BYTES]; }
                *size = READ_HEADER_BYTES + n;
            }
        }
    } else if (boot != s_boot) {
        e = ESP_ERR_INVALID_STATE;
    } else if (r->method_id == 1) {
        size_t n = r->payload_size - 4;
        const uint8_t *line = r->payload + 4;
        if (n < 7 || n >= INPUT_BYTES || memcmp(line, ">ptest.", 7)) {
            e = ESP_ERR_INVALID_ARG;
        } else if (s_input_pos != s_input_size) {
            e = ESP_ERR_INVALID_STATE;
        } else {
            for (size_t i = 0; i < n; ++i) {
                if (line[i] < 32 || line[i] > 126) { e = ESP_ERR_INVALID_ARG; break; }
            }
            if (!e) {
                memcpy(s_input, line, n);
                s_input[n] = '\n';
                s_input_pos = 0;
                s_input_size = n + 1;
            }
        }
    } else if (r->method_id == 3 && r->payload_size == 4 && capacity >= 12) {
        int pct, mv, ma;
        e = s_run_active ? ESP_ERR_INVALID_STATE : muse_mosaico_ptest_battery(&pct, &mv, &ma);
        if (!e) {
            put32(out, pct); put32(out + 4, mv); put32(out + 8, ma);
            *size = 12;
        }
    } else {
        e = ESP_ERR_INVALID_ARG;
    }
    xSemaphoreGive(s_mutex);
    return e;
}

esp_err_t muse_ptest_board_console_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_output = heap_caps_malloc(OUTPUT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t e = s_mutex && s_output ? ESP_OK : ESP_ERR_NO_MEM;
    s_boot = esp_random();
    if (!s_boot) { s_boot = 1; }
    if (!e) { e = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ptest_iris", &s_awake); }
    if (!e) { e = esp_pm_lock_acquire(s_awake); }
    bool locked = !e;
    unsigned registered = 0;
    /* The USB device must stay responsive throughout every USB-input row. */
    for (unsigned method = 1; !e && method <= 3; ++method) {
        e = esp_iris_rpc_register(PTEST_SERVICE, method, rpc, NULL);
        if (!e) { ++registered; }
    }
    if (e) {
        for (unsigned method = 1; method <= registered; ++method) {
            (void)esp_iris_rpc_unregister(PTEST_SERVICE, method);
        }
        if (locked) { esp_pm_lock_release(s_awake); }
        if (s_awake) { esp_pm_lock_delete(s_awake); s_awake = NULL; }
        if (s_mutex) { vSemaphoreDelete(s_mutex); s_mutex = NULL; }
        heap_caps_free(s_output);
        s_output = NULL;
    }
    return e;
}

/* The same mutex covers the entire gauge read, so arm waits for any in-flight
 * read and further reads stay rejected through finish/abort restoration. */
void muse_ptest_board_run_active(bool active)
{
    if (!s_mutex) { return; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_run_active = active;
    xSemaphoreGive(s_mutex);
}

bool muse_ptest_iris_usb_connected(void)
{
    esp_iris_status_t status;
    return esp_iris_get_status(&status) == ESP_OK && status.link_connected
        && status.transport == ESP_IRIS_TRANSPORT_KIND_USB;
}

void muse_ptest_board_console_write(const char *buf, size_t n)
{
    if (!s_mutex || !s_output) { return; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (n > UINT32_MAX - s_written) { s_overflow = true; }
    else {
        for (size_t i = 0; i < n; ++i) { s_output[(s_written + i) % OUTPUT_BYTES] = buf[i]; }
        s_written += n;
    }
    xSemaphoreGive(s_mutex);
}

bool muse_ptest_board_console_getc(uint8_t *c)
{
    if (!s_mutex) { return false; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool available = s_input_pos < s_input_size;
    if (available) { *c = s_input[s_input_pos++]; }
    xSemaphoreGive(s_mutex);
    return available;
}

esp_err_t muse_ptest_board_console_flush(void)
{
    return s_output && !s_overflow ? ESP_OK : ESP_ERR_INVALID_STATE;
}
