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
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM };
enum { MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, ESP_PM_NO_LIGHT_SLEEP = 3,
       portMAX_DELAY = -1, ESP_IRIS_TRANSPORT_KIND_USB = 1 };
typedef int *SemaphoreHandle_t;
typedef int *esp_pm_lock_handle_t;
static int semaphore, held, locks, handlers, gauge_reads;
static bool connected = true;
static int transport = ESP_IRIS_TRANSPORT_KIND_USB;
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &semaphore; }
static void xSemaphoreTake(SemaphoreHandle_t s, int wait) { assert(!held); held = 1; }
static void xSemaphoreGive(SemaphoreHandle_t s) { assert(held); held = 0; }
static void vSemaphoreDelete(SemaphoreHandle_t s) {}
static void *heap_caps_malloc(size_t n, unsigned flags) { assert(flags == 3); return malloc(n); }
static void heap_caps_free(void *p) { free(p); }
static unsigned esp_random(void) { return 1234; }
static esp_err_t esp_pm_lock_create(int type, int arg, const char *name, esp_pm_lock_handle_t *out) { assert(type == ESP_PM_NO_LIGHT_SLEEP); *out = &locks; return ESP_OK; }
static esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t lock) { locks++; return ESP_OK; }
static esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t lock) { locks--; return ESP_OK; }
static esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t lock) { return ESP_OK; }
typedef struct { bool link_connected; int transport; } esp_iris_status_t;
static esp_err_t esp_iris_get_status(esp_iris_status_t *s) { s->link_connected = connected; s->transport = transport; return ESP_OK; }
typedef struct { uint16_t method_id; const uint8_t *payload; size_t payload_size; } esp_iris_rpc_request_t;
typedef esp_err_t (*handler_t)(const esp_iris_rpc_request_t *, uint8_t *, size_t, size_t *, void *);
static esp_err_t esp_iris_rpc_register(unsigned service, unsigned method, handler_t fn, void *ctx) { assert(service == 0x5054 && method == (unsigned)++handlers); return ESP_OK; }
static esp_err_t esp_iris_rpc_unregister(unsigned service, unsigned method) { handlers--; return ESP_OK; }
esp_err_t muse_mosaico_ptest_battery(int *pct, int *mv, int *ma) { assert(held); gauge_reads++; *pct = 100; *mv = 4200; *ma = -1; return ESP_OK; }
#include "../components_mosaico/muse_mosaico_iris/muse_ptest_iris.c"

static esp_err_t request(unsigned method, const uint8_t *payload, size_t n, uint8_t *out, size_t cap, size_t *size)
{
    esp_iris_rpc_request_t r = { .method_id = method, .payload = payload, .payload_size = n };
    return rpc(&r, out, cap, size, NULL);
}
int main(void)
{
    assert(muse_ptest_board_console_init() == ESP_OK && locks == 1 && handlers == 3);
    assert(muse_ptest_iris_usb_connected());
    transport = 2;
    assert(!muse_ptest_iris_usb_connected());
    transport = ESP_IRIS_TRANSPORT_KIND_USB;
    connected = false;
    assert(!muse_ptest_iris_usb_connected());
    uint8_t payload[300] = {0}, out[64], byte;
    size_t size;
    assert(request(2, payload, 8, out, sizeof(out), &size) == ESP_OK && size == 16);
    assert(get32(out) == 1 && get32(out + 4) == 1234);
    put32(payload, 999);
    memcpy(payload + 4, ">ptest.status", 13);
    assert(request(1, payload, 17, out, sizeof(out), &size) == ESP_ERR_INVALID_STATE);
    put32(payload, 1234);
    assert(request(1, payload, 17, out, sizeof(out), &size) == ESP_OK);
    assert(request(1, payload, 17, out, sizeof(out), &size) == ESP_ERR_INVALID_STATE);
    for (unsigned i = 0; i < 14; i++) {
        assert(muse_ptest_board_console_getc(&byte));
        assert(byte == (i == 13 ? '\n' : payload[4 + i]));
    }
    assert(!muse_ptest_board_console_getc(&byte));
    payload[12] = '\n';
    assert(request(1, payload, 17, out, sizeof(out), &size) == ESP_ERR_INVALID_ARG);
    assert(!muse_ptest_board_console_getc(&byte));
    assert(request(1, payload, 260, out, sizeof(out), &size) == ESP_ERR_INVALID_ARG);
    muse_ptest_board_console_write("PTEST abc\n", 10);
    put32(payload, 0);
    assert(request(2, payload, 8, out, sizeof(out), &size) == ESP_OK && size == 16 && get32(out + 8) == 10);
    put32(payload, 1234);
    put32(payload + 4, 11);
    assert(request(2, payload, 8, out, sizeof(out), &size) == ESP_ERR_INVALID_STATE);
    put32(payload + 4, 0);
    assert(request(2, payload, 8, out, 20, &size) == ESP_OK && size == 20);
    assert(get32(out + 12) == 10 && memcmp(out + 16, "PTES", 4) == 0);
    /* Reads are replayable, never destructive on a lost RPC response. */
    assert(request(2, payload, 8, out, 20, &size) == ESP_OK && memcmp(out + 16, "PTES", 4) == 0);
    put32(payload + 4, 4);
    assert(request(2, payload, 8, out, 64, &size) == ESP_OK && size == 22 && memcmp(out + 16, "T abc\n", 6) == 0);
    put32(payload, 999);
    assert(request(2, payload, 8, out, 64, &size) == ESP_ERR_INVALID_STATE);
    put32(payload, 1234);
    assert(request(3, payload, 4, out, 64, &size) == ESP_OK && size == 12);
    assert(get32(out) == 100 && (int32_t)get32(out + 8) == -1);
    assert(gauge_reads == 1);
    muse_ptest_board_run_active(true);
    assert(request(3, payload, 4, out, 64, &size) == ESP_ERR_INVALID_STATE && size == 0);
    assert(gauge_reads == 1); /* Rejected before any I2C transaction. */
    muse_ptest_board_run_active(false);
    assert(request(3, payload, 4, out, 64, &size) == ESP_OK && gauge_reads == 2);
    char *large = malloc(OUTPUT_BYTES);
    memset(large, 'z', OUTPUT_BYTES);
    muse_ptest_board_console_write(large, OUTPUT_BYTES);
    /* The old cursor was overwritten; a truncated JSON line must not pass. */
    assert(request(2, payload, 8, out, 64, &size) == ESP_ERR_INVALID_STATE);
    put32(payload + 4, 10);
    assert(request(2, payload, 8, out, 64, &size) == ESP_OK && out[16] == 'z');
    assert(muse_ptest_board_console_flush() == ESP_OK);
    free(large); free(s_output);
    puts("ok");
}
