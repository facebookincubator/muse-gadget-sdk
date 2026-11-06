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

// Drives image_fetch.c against a scripted HTTP server and clock: redirects
// must keep the scheme the fetch was sized for, and the whole download must
// finish within its deadline however slowly the server sends. A file from the
// VM comes from a fake daemon over Noise instead, in Range requests small
// enough that no reply frame overflows the session.

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "image_fetch.h"
#include "led_status.h"
#include "noise_control.h"
#include "noise_tunnel_internal.h"
#include "rom/tjpgd.h"

#define WIDTH 8
// Tall enough that a whole raw screen (64 KB) is four ranges of a VM file.
#define HEIGHT 4096
// The largest body the device may receive in one frame.
#define MAX_BODY (16 * 1024)
#define ROW_BYTES (WIDTH * 2)
#define MAX_RESPONSES 8

void *esp_crt_bundle_attach = NULL;

static void fail_at(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d: ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) fail_at(__FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

// Account for the production allocations, independently of the reply buffers
// constructed by this harness. A leak is caught even without LeakSanitizer.
static struct {
    void *ptr;
    size_t size;
    bool external;
} g_allocations[16];
static size_t g_internal_used, g_external_used;
static size_t g_internal_free = 200000, g_dma_free = 200000;
static size_t g_largest_block = 100000;
static size_t g_min_dma_free;
static unsigned g_alloc_calls, g_fail_alloc;
static bool g_fail_task;
static size_t g_body_bytes, g_pressure_after_body;
static int g_width = WIDTH;

static void record_dma(void) {
    size_t free = g_dma_free > g_internal_used ? g_dma_free - g_internal_used : 0;
    if (free < g_min_dma_free) g_min_dma_free = free;
}

static void reset_memory(void) {
    CHECK(!g_internal_used && !g_external_used, "download leaked %zu internal / %zu external bytes",
          g_internal_used, g_external_used);
    g_internal_free = g_dma_free = 200000;
    g_largest_block = 100000;
    g_min_dma_free = 200000;
    g_alloc_calls = g_fail_alloc = 0;
    g_fail_task = false;
    g_body_bytes = g_pressure_after_body = 0;
    g_width = WIDTH;
}

static void *image_alloc(size_t size, bool external) {
    if (++g_alloc_calls == g_fail_alloc) return NULL;
    // Model metadata/rounding too, so filling the byte budget with payload
    // alone cannot accidentally make every reduced range fail on hardware.
    size_t cost = size + 32;
    if (!external && cost > heap_caps_get_free_size(MALLOC_CAP_DMA)) return NULL;
    void *ptr = malloc(size);
    if (!ptr) return NULL;
    for (size_t i = 0; i < sizeof(g_allocations) / sizeof(g_allocations[0]); i++) {
        if (g_allocations[i].ptr) continue;
        g_allocations[i].ptr = ptr;
        g_allocations[i].size = cost;
        g_allocations[i].external = external;
        if (external) g_external_used += cost;
        else g_internal_used += cost;
        record_dma();
        return ptr;
    }
    CHECK(false, "too many live allocations");
    return NULL;
}

void *image_test_malloc(size_t size) {
    return image_alloc(size, false);
}

void *image_test_calloc(size_t count, size_t size) {
    void *ptr = image_test_malloc(count * size);
    if (ptr) memset(ptr, 0, count * size);
    return ptr;
}

char *image_test_strdup(const char *str) {
    size_t size = strlen(str) + 1;
    char *ptr = image_test_malloc(size);
    if (ptr) memcpy(ptr, str, size);
    return ptr;
}

void image_test_free(void *ptr) {
    if (!ptr) return;
    for (size_t i = 0; i < sizeof(g_allocations) / sizeof(g_allocations[0]); i++) {
        if (g_allocations[i].ptr != ptr) continue;
        if (g_allocations[i].external) g_external_used -= g_allocations[i].size;
        else g_internal_used -= g_allocations[i].size;
        g_allocations[i].ptr = NULL;
        free(ptr);
        return;
    }
    CHECK(false, "free of an untracked download allocation");
}

// ---- Clock -------------------------------------------------------------------

static int64_t g_now_us;

int64_t esp_timer_get_time(void) {
    return g_now_us;
}

// ---- Scripted server ---------------------------------------------------------

typedef struct {
    int status;
    const char *location;   // for redirects
    size_t body_len;        // raw RGB565 bytes, a pattern
    int chunk;              // bytes per read, 0 for all at once
    int64_t read_us;        // time each read takes
    int64_t open_us;        // time the connection takes
} response_t;

static response_t g_responses[MAX_RESPONSES];
static int g_response_count;
static int g_opens;
static int g_min_timeout_seen;
static int64_t g_max_timeout_overrun_us;

struct fake_esp_http_client {
    char url[256];
    int timeout_ms;
    http_event_handle_cb handler;
    void *user_data;
    const response_t *resp;
    size_t sent;
    bool open;
};

static void reset_server(void) {
    reset_memory();
    memset(g_responses, 0, sizeof(g_responses));
    g_response_count = 0;
    g_opens = 0;
    g_now_us = 1000000;
    g_min_timeout_seen = 1 << 30;
    g_max_timeout_overrun_us = 0;
}

static void add_response(response_t r) {
    CHECK(g_response_count < MAX_RESPONSES, "too many responses");
    g_responses[g_response_count++] = r;
}

// The next socket operation may block for up to its timeout: record whether
// the clock would pass the deadline the caller set.
static int64_t g_deadline_hint_us;

static void spend(esp_http_client_handle_t c, int64_t us) {
    if (us > (int64_t)c->timeout_ms * 1000) us = (int64_t)c->timeout_ms * 1000;
    g_now_us += us;
    if (g_deadline_hint_us && g_now_us - g_deadline_hint_us > g_max_timeout_overrun_us) {
        g_max_timeout_overrun_us = g_now_us - g_deadline_hint_us;
    }
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    esp_http_client_handle_t c = calloc(1, sizeof(*c));
    CHECK(c, "calloc");
    snprintf(c->url, sizeof(c->url), "%s", config->url);
    c->timeout_ms = config->timeout_ms;
    c->handler = config->event_handler;
    c->user_data = config->user_data;
    CHECK(config->disable_auto_redirect, "redirects must be followed by image_fetch");
    return c;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len) {
    (void)write_len;
    CHECK(!c->open, "open while open");
    CHECK(g_opens < g_response_count, "unscripted open #%d of %s", g_opens + 1, c->url);
    c->resp = &g_responses[g_opens++];
    c->sent = 0;
    c->open = true;
    spend(c, c->resp->open_us);
    return ESP_OK;
}

long long esp_http_client_fetch_headers(esp_http_client_handle_t c) {
    return (long long)c->resp->body_len;
}

int esp_http_client_get_status_code(esp_http_client_handle_t c) {
    return c->resp->status;
}

esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t c) {
    CHECK(c->open && c->resp->location, "no Location to follow");
    snprintf(c->url, sizeof(c->url), "%s", c->resp->location);
    return ESP_OK;
}

esp_err_t esp_http_client_get_url(esp_http_client_handle_t c, char *url, int len) {
    snprintf(url, (size_t)len, "%s", c->url);
    return ESP_OK;
}

esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int timeout_ms) {
    CHECK(timeout_ms > 0, "non-positive timeout %d", timeout_ms);
    c->timeout_ms = timeout_ms;
    if (timeout_ms < g_min_timeout_seen) g_min_timeout_seen = timeout_ms;
    return ESP_OK;
}

// Like the real client: keeps reading until it has `len` bytes or the body
// ends, raising HTTP_EVENT_ON_DATA after each socket read, and gives up only
// when a single socket read times out.
int esp_http_client_read(esp_http_client_handle_t c, char *buffer, int len) {
    CHECK(c->open, "read while closed");
    int got = 0;
    while (got < len && c->sent < c->resp->body_len) {
        if (c->resp->read_us > (int64_t)c->timeout_ms * 1000) {
            spend(c, c->resp->read_us);   // waits out the timeout
            return got ? got : -ESP_ERR_HTTP_EAGAIN;
        }
        size_t left = c->resp->body_len - c->sent;
        size_t n = (size_t)(len - got) < left ? (size_t)(len - got) : left;
        if (c->resp->chunk && n > (size_t)c->resp->chunk) n = (size_t)c->resp->chunk;
        spend(c, c->resp->read_us);
        for (size_t i = 0; i < n; i++) buffer[got + i] = (char)(c->sent + i);
        c->sent += n;
        got += (int)n;
        if (c->handler) {
            esp_http_client_event_t evt = {
                .event_id = HTTP_EVENT_ON_DATA, .client = c,
                .data = buffer + got - n, .data_len = (int)n, .user_data = c->user_data,
            };
            c->handler(&evt);
        }
    }
    return got;
}

bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) {
    return c->sent == c->resp->body_len;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t c) {
    c->open = false;
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    free(c);
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t err) {
    return err == ESP_ERR_TIMEOUT ? "ESP_ERR_TIMEOUT" : "ESP_FAIL";
}

// ---- Fake daemon -------------------------------------------------------------

// The file on the VM, whose byte at offset i is (uint8_t)i, and how the
// daemon serves it: one frame per turn of the session task, as much of the
// reply as it likes in each, like the real one (up to 64 KB).
static size_t g_file_len;
static size_t g_frame_bytes;
static int g_error_status;      // 0, or the status of every reply
static bool g_ignore_range;     // answer Range with the whole file
static size_t g_drop_at;        // the session ends once this many bytes are sent
static int g_stall_after;       // frames sent before the daemon goes quiet
static bool g_connected;        // false: the session refuses requests
static size_t g_alloc_limit;    // larger allocations fail; 0 for none

// Every request, and the one being answered.
#define MAX_REQUESTS 32
static char g_ranges[MAX_REQUESTS][48];   // "" for none
static int g_requests, g_cancels, g_frames_sent;
static size_t g_bytes_sent;
static noise_ctrl_req_cb g_vm_cb;
static void *g_vm_ctx;
static char g_vm_path[256];
static int64_t g_vm_id;
static bool g_answering, g_head_sent;
static int g_status;
static size_t g_pos, g_end;
static size_t g_max_body = MAX_BODY;

static void reset_vm(size_t file_len) {
    reset_memory();
    g_max_body = MAX_BODY;
    g_file_len = file_len;
    g_frame_bytes = 64 * 1024;
    g_alloc_limit = 0;
    g_error_status = 0;
    g_ignore_range = false;
    g_drop_at = 0;
    g_stall_after = 1 << 30;
    g_connected = true;
    memset(g_ranges, 0, sizeof(g_ranges));
    g_requests = g_cancels = g_frames_sent = 0;
    g_bytes_sent = 0;
    g_vm_cb = NULL;
    g_vm_path[0] = '\0';
    g_answering = false;
}

size_t noise_ctrl_req_max_body(void) {
    return g_max_body;
}

int64_t noise_ctrl_req_open(const char *verb, const char *path, const char *const *headers,
                            bool end_body, noise_ctrl_req_cb cb, void *ctx) {
    CHECK(strcmp(verb, "GET") == 0 && end_body, "expected a bodiless GET");
    if (!g_connected) return 0;
    CHECK(g_requests < MAX_REQUESTS, "too many requests");
    const char *range = NULL;
    for (const char *const *h = headers; h && *h; h += 2) {
        CHECK(strcmp(h[0], "Range") == 0, "unexpected header %s", h[0]);
        range = h[1];
    }
    // The real session holds this copy until TX dequeues it. Even while it is
    // queued, the download must leave enough DMA RAM for control results.
    size_t request_bytes = strlen(verb) + strlen(path) + 3;
    for (const char *const *h = headers; h && *h; h++) request_bytes += strlen(*h) + 1;
    void *request = image_test_malloc(request_bytes);
    if (!request) return 0;
    CHECK(heap_caps_get_free_size(MALLOC_CAP_DMA) >= NOISE_TX_DMA_RESERVE_BYTES,
          "download blocked Noise TX with only %zu DMA bytes free",
          heap_caps_get_free_size(MALLOC_CAP_DMA));
    image_test_free(request);
    snprintf(g_ranges[g_requests++], sizeof(g_ranges[0]), "%s", range ? range : "");
    snprintf(g_vm_path, sizeof(g_vm_path), "%s", path);
    g_vm_cb = cb;
    g_vm_ctx = ctx;
    g_answering = true;
    g_head_sent = false;
    unsigned from = 0, to = 0;
    if (g_error_status) {
        g_status = g_error_status;
        g_pos = g_end = 0;
    } else if (range && !g_ignore_range) {
        CHECK(sscanf(range, "bytes=%u-%u", &from, &to) == 2, "bad range %s", range);
        g_status = from < g_file_len ? 206 : 416;
        g_pos = from < g_file_len ? from : 0;
        g_end = from < g_file_len ? (to + 1 < g_file_len ? to + 1 : g_file_len) : 0;
    } else {
        g_status = 200;
        g_pos = 0;
        g_end = g_file_len;
    }
    return ++g_vm_id;
}

void noise_ctrl_req_cancel(int64_t id) {
    CHECK(id == g_vm_id, "cancelled stream %lld, not the open one", (long long)id);
    g_cancels++;
    g_answering = false;
}

static void vm_tick(void) {
    if (!g_answering || g_frames_sent >= g_stall_after) return;
    size_t n = g_end - g_pos;
    if (n > g_frame_bytes) n = g_frame_bytes;
    if (g_drop_at && g_bytes_sent + n > g_drop_at) {
        g_answering = false;
        g_vm_cb(g_vm_ctx, -1, NULL, 0, true);
        return;
    }
    CHECK(n <= MAX_BODY, "a %u-byte frame would end the session", (unsigned)n);
    uint8_t *data = n ? malloc(n) : NULL;
    for (size_t i = 0; i < n; i++) data[i] = (uint8_t)(g_pos + i);
    int status = g_head_sent ? 0 : g_status;
    g_head_sent = true;
    g_pos += n;
    g_bytes_sent += n;
    g_frames_sent++;
    bool end = g_pos == g_end;
    if (end) g_answering = false;
    g_vm_cb(g_vm_ctx, status, data, n, end);
    free(data);
}

// ---- Board -------------------------------------------------------------------

static unsigned g_stack;

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, unsigned stack_depth,
                       void *params, unsigned priority, TaskHandle_t *out_handle) {
    (void)name;
    (void)priority;
    (void)out_handle;
    if (g_fail_task) return pdFALSE;
    g_stack = stack_depth;
    g_internal_used += stack_depth;
    record_dma();
    task(params);   // runs to completion; vTaskDelete returns
    g_internal_used -= stack_depth;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task) {
    (void)task;
}

static void vm_tick(void);

// The download task polls for a file from the VM; each wait is a turn of the
// session task, which may deliver the next scripted frame.
void vTaskDelay(int ticks) {
    g_now_us += (int64_t)ticks * 1000;
    vm_tick();
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return (SemaphoreHandle_t)1;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait_ticks) {
    (void)semaphore;
    (void)wait_ticks;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    (void)semaphore;
    return pdTRUE;
}


size_t heap_caps_get_free_size(int caps) {
    size_t capacity = caps == MALLOC_CAP_DMA ? g_dma_free : g_internal_free;
    return capacity > g_internal_used ? capacity - g_internal_used : 0;
}

size_t heap_caps_get_minimum_free_size(int caps) {
    (void)caps;
    return 200000;
}

size_t heap_caps_get_largest_free_block(int caps) {
    size_t block = g_largest_block;
    if (caps == MALLOC_CAP_8BIT && g_alloc_limit && block > g_alloc_limit) block = g_alloc_limit;
    size_t free = heap_caps_get_free_size(caps);
    return block < free ? block : free;
}

void *heap_caps_malloc(size_t size, int caps) {
    if (g_alloc_limit && size > g_alloc_limit) return NULL;
    void *ptr = image_alloc(size, (caps & MALLOC_CAP_SPIRAM) != 0);
    if (ptr) {
        g_body_bytes = size;
        g_dma_free -= g_pressure_after_body;
        record_dma();
    }
    return ptr;
}

bool led_status_display_info(int *width, int *height) {
    *width = g_width;
    *height = HEIGHT;
    return true;
}

static int g_rows_drawn;
static size_t g_next_byte;

// Every body is the pattern (uint8_t)offset, so the screen must get it in
// order, nothing lost or repeated.
bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels) {
    (void)x;
    (void)y;
    const uint8_t *b = (const uint8_t *)pixels;
    for (int i = 0; i < w * h * 2; i++, g_next_byte++) {
        CHECK(b[i] == (uint8_t)g_next_byte, "byte %u drawn as %u", (unsigned)g_next_byte, b[i]);
    }
    g_rows_drawn += h;
    return true;
}

void led_status_draw_done(void) {
}

JRESULT jd_prepare(JDEC *jd, UINT (*infunc)(JDEC *, BYTE *, UINT), void *pool,
                   UINT sz_pool, void *dev) {
    (void)jd;
    (void)infunc;
    (void)pool;
    (void)sz_pool;
    (void)dev;
    return JDR_FMT1;
}

JRESULT jd_decomp(JDEC *jd, UINT (*outfunc)(JDEC *, void *, JRECT *), BYTE scale) {
    (void)jd;
    (void)outfunc;
    (void)scale;
    return JDR_FMT1;
}

// ---- Cases -------------------------------------------------------------------

static image_fetch_result_t g_result;
static char g_message[128];
static bool g_done;

static void on_done(const image_fetch_result_t *result, void *user) {
    (void)user;
    g_result = *result;
    snprintf(g_message, sizeof(g_message), "%s", result->message ? result->message : "");
    g_result.message = g_message;
    g_done = true;
}

static void fetch(const char *url) {
    const char *code = NULL, *message = NULL;
    g_done = false;
    g_rows_drawn = 0;
    g_next_byte = 0;
    g_deadline_hint_us = g_now_us + 60 * 1000000LL;
    CHECK(image_fetch_start(url, 0, on_done, NULL, &code, &message),
          "start %s refused: %s", url, message ? message : "");
    CHECK(g_done, "download did not finish");
}

static void fetch_vm(const char *path) {
    const char *code = NULL, *message = NULL;
    g_done = false;
    g_rows_drawn = 0;
    g_next_byte = 0;
    CHECK(image_fetch_start_vm(path, 0, on_done, NULL, &code, &message),
          "start %s refused: %s", path, message ? message : "");
    CHECK(g_done, "fetch did not finish");
}

static void expect_failure(const char *message) {
    CHECK(!g_result.ok, "expected failure '%s', got success", message);
    CHECK(strcmp(g_result.message, message) == 0, "expected '%s', got '%s'", message,
          g_result.message);
}

static void test_http_redirect_to_https_is_refused(void) {
    reset_server();
    add_response((response_t){.status = 302, .location = "https://evil.example/x"});
    fetch("http://host/img");
    CHECK(g_stack == 6144, "http fetch should get the HTTP stack, got %u", g_stack);
    expect_failure("redirect changed scheme");
    CHECK(g_opens == 1, "TLS must never be attempted: %d opens", g_opens);
}

static void test_https_redirect_to_http_is_refused(void) {
    reset_server();
    add_response((response_t){.status = 301, .location = "http://192.168.1.1/admin"});
    fetch("https://host/img");
    CHECK(g_stack == 8192, "https fetch should get the TLS stack, got %u", g_stack);
    expect_failure("redirect changed scheme");
    CHECK(g_opens == 1, "downgrade must not be followed: %d opens", g_opens);
}

static void test_scheme_check_ignores_case(void) {
    reset_server();
    add_response((response_t){.status = 307, .location = "HTTPS://cdn.example/img"});
    add_response((response_t){.status = 200, .body_len = 4 * ROW_BYTES});
    fetch("https://host/img");
    CHECK(g_result.ok, "same scheme in capitals should work: %s", g_result.message);
}

static void test_same_scheme_redirects_are_followed(void) {
    reset_server();
    add_response((response_t){.status = 302, .location = "http://cdn.example/a"});
    add_response((response_t){.status = 308, .location = "http://cdn.example/b"});
    add_response((response_t){.status = 200, .body_len = 6 * ROW_BYTES});
    fetch("http://host/img");
    CHECK(g_result.ok, "same-scheme redirects should work: %s", g_result.message);
    CHECK(g_opens == 3, "expected 3 opens, got %d", g_opens);
    CHECK(g_rows_drawn == 6, "expected 6 rows, got %d", g_rows_drawn);
}

static void test_trickled_body_hits_the_deadline(void) {
    reset_server();
    // One byte every 9 s never trips the 10 s per-read timeout.
    add_response((response_t){.status = 200, .body_len = HEIGHT * ROW_BYTES, .chunk = 1,
                              .read_us = 9 * 1000000LL});
    fetch("http://slow/img");
    expect_failure("download timed out");
    CHECK(strcmp(g_result.code, "download_failed") == 0, "code %s", g_result.code);
    CHECK(g_max_timeout_overrun_us == 0,
          "a read could block %lld us past the deadline", (long long)g_max_timeout_overrun_us);
    CHECK(g_min_timeout_seen < 10000, "the last read's timeout was not clamped");
    CHECK(g_result.ms <= 60000, "took %d ms", g_result.ms);

    // The busy flag is released, so the next download may start.
    reset_server();
    add_response((response_t){.status = 200, .body_len = ROW_BYTES});
    fetch("http://host/img");
    CHECK(g_result.ok, "next download after a timeout: %s", g_result.message);
}

// Each open is bounded by the socket timeout and the redirects by their
// limit, so a slow chain ends on its own well inside the deadline.
static void test_slow_redirect_chain_stops_at_the_limit(void) {
    reset_server();
    for (int i = 0; i < 4; i++) {
        add_response((response_t){.status = 302, .location = "http://next/",
                                  .open_us = 30000000});
    }
    fetch("http://host/img");
    CHECK(g_opens == 4, "expected the first open and 3 redirects, got %d", g_opens);
    CHECK(strcmp(g_result.message, "server answered HTTP 302") == 0, "%s", g_result.message);
    CHECK(g_max_timeout_overrun_us == 0, "an open could block past the deadline");
}

static void test_vm_file_is_drawn(void) {
    reset_server();
    reset_vm(5 * ROW_BYTES);
    fetch_vm("/home/me/pics/a b%.raw");
    CHECK(strcmp(g_vm_path, "/fs/raw/home/me/pics/a%20b%25.raw") == 0, "path %s", g_vm_path);
    CHECK(g_result.ok, "VM file should draw: %s", g_result.message);
    CHECK(g_rows_drawn == 5, "expected 5 rows, got %d", g_rows_drawn);
    CHECK(g_result.bytes == 5 * ROW_BYTES, "bytes %u", (unsigned)g_result.bytes);
    CHECK(g_requests == 1, "a small file takes one request, got %d", g_requests);
    CHECK(strcmp(g_ranges[0], "bytes=0-16383") == 0, "even the first is bounded: %s",
          g_ranges[0]);
    CHECK(!g_cancels, "a finished request needs no cancel");
    CHECK(g_opens == 0, "no HTTP for a VM file");
}

static void test_vm_missing_file(void) {
    reset_server();
    reset_vm(100);
    g_error_status = 404;
    fetch_vm("nope.jpg");
    CHECK(strcmp(g_vm_path, "/fs/raw/nope.jpg") == 0, "path %s", g_vm_path);
    expect_failure("VM answered HTTP 404");
    CHECK(g_rows_drawn == 0, "nothing should be drawn");
}

static void test_vm_large_file_is_read_in_ranges(void) {
    reset_server();
    reset_vm(2500 * ROW_BYTES);   // 40000 bytes: two full ranges and 7232
    fetch_vm("big.raw");
    CHECK(g_result.ok, "large file should draw: %s", g_result.message);
    CHECK(g_rows_drawn == 2500, "expected 2500 rows, got %d", g_rows_drawn);
    CHECK(g_result.bytes == 40000, "bytes %u", (unsigned)g_result.bytes);
    CHECK(g_requests == 3, "expected three ranges, got %d requests", g_requests);
    CHECK(strcmp(g_ranges[1], "bytes=16384-32767") == 0, "range %s", g_ranges[1]);
    CHECK(strcmp(g_ranges[2], "bytes=32768-49151") == 0, "range %s", g_ranges[2]);
    CHECK(!g_cancels, "nothing to cancel, got %d", g_cancels);
}

// A file of whole ranges: the range after the last one is past the end, as
// is the first of an empty file.
static void test_vm_file_of_whole_ranges_ends_on_416(void) {
    reset_server();
    reset_vm(4 * MAX_BODY);
    fetch_vm("screen.raw");
    CHECK(g_result.ok, "whole screen should draw: %s", g_result.message);
    CHECK(g_rows_drawn == HEIGHT, "expected %d rows, got %d", HEIGHT, g_rows_drawn);
    CHECK(g_requests == 5, "expected four ranges and a 416, got %d", g_requests);
    CHECK(strcmp(g_ranges[4], "bytes=65536-81919") == 0, "range %s", g_ranges[4]);

    reset_server();
    reset_vm(0);
    fetch_vm("empty.raw");
    CHECK(g_result.ok && g_rows_drawn == 0, "empty file: %s", g_result.message);
}

static void test_vm_range_failures(void) {
    // A daemon that ignores Range is fine for a file that fits...
    reset_server();
    reset_vm(5 * ROW_BYTES);
    g_ignore_range = true;
    fetch_vm("small.raw");
    CHECK(g_result.ok && g_rows_drawn == 5, "small file without Range: %s", g_result.message);

    // ...but not for a larger one, which it sends in smaller frames.
    reset_server();
    reset_vm(2500 * ROW_BYTES);
    g_ignore_range = true;
    g_frame_bytes = 4096;
    fetch_vm("big.raw");
    expect_failure("the VM ignored Range and sent the whole file");
    CHECK(g_rows_drawn == 0, "rows %d", g_rows_drawn);
    CHECK(g_cancels == 1, "the rest is cancelled");

    // The session ending during the second range.
    reset_server();
    reset_vm(2500 * ROW_BYTES);
    g_drop_at = 20000;
    fetch_vm("big.raw");
    expect_failure("download failed");
    CHECK(strcmp(g_result.code, "download_failed") == 0, "code %s", g_result.code);
    CHECK(g_rows_drawn == MAX_BODY / ROW_BYTES, "rows %d", g_rows_drawn);
}

// The buffer is allocated once, up front: an allocation failure refuses the
// command rather than failing partway.
static void test_vm_buffer_is_allocated_up_front(void) {
    const char *code = NULL, *message = NULL;
    reset_server();
    reset_vm(2500 * ROW_BYTES);
    g_fail_alloc = 3;  // context and path succeed, body fails
    CHECK(!image_fetch_start_vm("big.raw", 0, on_done, NULL, &code, &message),
          "should refuse without the buffer");
    CHECK(strcmp(code, "out_of_memory") == 0, "code %s", code);
    CHECK(g_requests == 0, "nothing should be requested");

    // Exactly enough is enough for any size of file.
    reset_vm(2500 * ROW_BYTES);
    g_alloc_limit = MAX_BODY;
    fetch_vm("big.raw");
    CHECK(g_result.ok && g_rows_drawn == 2500, "with just the buffer: %s", g_result.message);
}

static void test_vm_range_size_only_shrinks_when_needed(void) {
    reset_server();
    reset_vm(1250 * ROW_BYTES);
    g_max_body = 10 * 1024;  // the normal display session's receive limit
    g_dma_free = 37 * 1024;
    fetch_vm("big.raw");
    CHECK(g_result.ok && g_rows_drawn == 1250, "file under DMA pressure: %s", g_result.message);
#if CONFIG_SPIRAM
    CHECK(g_body_bytes == g_max_body, "PSRAM must keep the maximum range, got %zu", g_body_bytes);
#else
    CHECK(g_body_bytes > 6 * 1024 && g_body_bytes < 8 * 1024,
          "moderate pressure should use the available intermediate range, got %zu", g_body_bytes);
#endif
    CHECK(g_min_dma_free >= NOISE_TX_DMA_RESERVE_BYTES,
          "DMA reserve was consumed: %zu", g_min_dma_free);
    // The same buffer is reused, including across raw-row boundaries.
    for (int i = 0; i < g_requests; i++) {
        unsigned from, to;
        CHECK(sscanf(g_ranges[i], "bytes=%u-%u", &from, &to) == 2, "bad range");
        CHECK(to - from + 1 == g_body_bytes, "range size changed during a download");
    }

    reset_server();
    reset_vm(1250 * ROW_BYTES);
    g_max_body = 10 * 1024;
    fetch_vm("big.raw");
    CHECK(g_result.ok && g_body_bytes == g_max_body, "ample memory must use the maximum");
}

static void test_vm_allocation_limit(void) {
    reset_server();
    reset_vm(1250 * ROW_BYTES);
    g_max_body = 10 * 1024;
    g_alloc_limit = 9 * 1024;
#if CONFIG_SPIRAM
    const char *code = NULL, *message = NULL;
    CHECK(!image_fetch_start_vm("big.raw", 0, on_done, NULL, &code, &message),
          "PSRAM does not shrink the range on allocation failure");
    CHECK(strcmp(code, "out_of_memory") == 0 && !g_requests, "expected immediate refusal");
#else
    fetch_vm("big.raw");
    CHECK(g_result.ok && g_body_bytes == g_alloc_limit, "use the largest allocatable range");
#endif
    CHECK(!g_internal_used && !g_external_used, "allocation limit leaked memory");
}

static void test_vm_wide_rows_and_long_path_are_budgeted(void) {
    reset_server();
    reset_vm(5 * 1024 * 2);
    g_width = 1024;  // four raw rows need more RAM than the JPEG pool
    g_max_body = 10 * 1024;
    g_dma_free = 45 * 1024;
    char path[1025];
    memset(path, 'a', sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    fetch_vm(path);
    CHECK(g_result.ok && g_rows_drawn == 5, "wide image from long path: %s", g_result.message);
    CHECK(g_min_dma_free >= NOISE_TX_DMA_RESERVE_BYTES,
          "rows or request metadata consumed the reserve: %zu", g_min_dma_free);
#if CONFIG_SPIRAM
    CHECK(g_body_bytes == g_max_body, "PSRAM should use the maximum");
#else
    CHECK(g_body_bytes >= 2048 && g_body_bytes < g_max_body, "rows and path need a smaller buffer");
#endif
}

static void test_vm_minimum_range_and_immediate_refusal(void) {
    const char *code = NULL, *message = NULL;
#if !CONFIG_SPIRAM
    reset_server();
    reset_vm(5 * ROW_BYTES);
    g_dma_free = 32 * 1024;
    fetch_vm("big.raw");
    CHECK(g_result.ok && g_body_bytes >= 2048 && g_body_bytes < 3072,
          "severe pressure should use a small range, got %zu", g_body_bytes);
    // Use the measured allocation cost to place this board exactly on the
    // 2 KiB boundary, then one byte below it.
    size_t capacity = g_dma_free - (g_body_bytes - 2048);
    reset_vm(5 * ROW_BYTES);
    g_dma_free = capacity;
    fetch_vm("big.raw");
    CHECK(g_result.ok && g_body_bytes == 2048, "minimum range should still draw");
    reset_vm(5 * ROW_BYTES);
    g_dma_free = capacity - 1;
    CHECK(!image_fetch_start_vm("big.raw", 0, on_done, NULL, &code, &message),
          "must refuse below the minimum range");
    CHECK(!g_body_bytes && !g_requests, "refusal must not allocate a body or open a request");
#endif
    // The original stall: internal RAM is plentiful, but only 26 KiB is DMA
    // capable. Refuse before starting a task, on PSRAM boards as well.
    reset_server();
    reset_vm(5 * ROW_BYTES);
    g_dma_free = 26 * 1024;
    int64_t start = esp_timer_get_time();
    CHECK(!image_fetch_start_vm("big.raw", 0, on_done, NULL, &code, &message),
          "must preserve the DMA reserve");
    CHECK(strcmp(code, "out_of_memory") == 0 && !g_requests, "expected immediate refusal");
    CHECK(esp_timer_get_time() == start, "refusal waited for the download timeout");
    CHECK(!g_internal_used && !g_external_used, "refusal leaked memory");
    reset_vm(5 * ROW_BYTES);
    fetch_vm("big.raw");
    CHECK(g_result.ok, "refusal must release the busy flag");
}

static void test_vm_post_allocation_budget(void) {
    const char *code = NULL, *message = NULL;
    reset_server();
    reset_vm(100);
    // Another task consumes DMA RAM between the sample and body allocation.
    g_pressure_after_body = 180000;
    CHECK(!image_fetch_start_vm("big.raw", 0, on_done, NULL, &code, &message),
          "must recheck the actual remaining memory");
    CHECK(g_body_bytes && !g_requests, "expected refusal after allocating the body");
    CHECK(!g_internal_used && !g_external_used, "post-allocation refusal leaked the body");
}

static void test_vm_startup_failures_release_memory(void) {
    const char *code = NULL, *message = NULL;
    for (unsigned fail = 1; fail <= 3; fail++) {
        reset_server();
        reset_vm(5 * ROW_BYTES);
        g_fail_alloc = fail;  // context, path, or body
        CHECK(!image_fetch_start_vm("big.raw", 0, on_done, NULL, &code, &message),
              "allocation %u should fail", fail);
        CHECK(strcmp(code, "out_of_memory") == 0 && !g_requests, "expected startup refusal");
        CHECK(!g_internal_used && !g_external_used, "allocation %u leaked memory", fail);
        reset_vm(5 * ROW_BYTES);
        fetch_vm("big.raw");
        CHECK(g_result.ok, "allocation failure must release the busy flag");
    }
    reset_vm(5 * ROW_BYTES);
    g_fail_task = true;
    CHECK(!image_fetch_start_vm("big.raw", 0, on_done, NULL, &code, &message),
          "task start should fail");
    CHECK(!g_requests && !g_internal_used && !g_external_used, "task failure leaked memory");
    reset_vm(5 * ROW_BYTES);
    fetch_vm("big.raw");
    CHECK(g_result.ok, "task failure must release the busy flag");
}

static void test_vm_session_drop(void) {
    reset_server();
    reset_vm(5 * ROW_BYTES);
    g_frame_bytes = ROW_BYTES;
    g_drop_at = 2 * ROW_BYTES;
    fetch_vm("a.raw");
    expect_failure("the VM connection dropped");
    CHECK(g_rows_drawn == 0, "a partial file is not drawn");
}

static void test_vm_timeout_drops_late_frames(void) {
    reset_server();
    reset_vm(5 * ROW_BYTES);
    g_frame_bytes = ROW_BYTES;
    g_stall_after = 1;
    fetch_vm("slow.raw");
    expect_failure("download timed out");
    CHECK(g_cancels == 1, "a timed-out request is cancelled");
    CHECK(g_result.ms <= 60100, "took %d ms", g_result.ms);

    // The session task delivers the rest before it gets to the cancel: the
    // next fetch must not see it.
    noise_ctrl_req_cb late = g_vm_cb;
    void *late_ctx = g_vm_ctx;
    reset_server();
    reset_vm(2 * ROW_BYTES);
    late(late_ctx, 0, (const uint8_t *)"xx", 2, true);
    fetch_vm("next.raw");
    CHECK(g_result.ok, "next fetch after a timeout: %s", g_result.message);
    CHECK(g_rows_drawn == 2, "expected 2 rows, got %d", g_rows_drawn);
}

static void test_vm_refusals(void) {
    // A session that can't take the request (down, or its queue full).
    reset_server();
    reset_vm(100);
    g_connected = false;
    fetch_vm("a.jpg");
    expect_failure("could not send the request to the VM");
    CHECK(!g_cancels, "nothing to cancel");

    const char *code = NULL, *message = NULL;
    reset_vm(0);
    CHECK(!image_fetch_start_vm("///", 0, on_done, NULL, &code, &message),
          "should refuse a path naming no file");
    CHECK(strcmp(code, "invalid_params") == 0, "code %s", code);
}

int main(void) {
    test_http_redirect_to_https_is_refused();
    test_https_redirect_to_http_is_refused();
    test_scheme_check_ignores_case();
    test_same_scheme_redirects_are_followed();
    test_trickled_body_hits_the_deadline();
    test_slow_redirect_chain_stops_at_the_limit();
    test_vm_file_is_drawn();
    test_vm_missing_file();
    test_vm_large_file_is_read_in_ranges();
    test_vm_file_of_whole_ranges_ends_on_416();
    test_vm_range_failures();
    test_vm_buffer_is_allocated_up_front();
    test_vm_range_size_only_shrinks_when_needed();
    test_vm_allocation_limit();
    test_vm_wide_rows_and_long_path_are_budgeted();
    test_vm_minimum_range_and_immediate_refusal();
    test_vm_post_allocation_budget();
    test_vm_startup_failures_release_memory();
    test_vm_session_drop();
    test_vm_timeout_drops_late_frames();
    test_vm_refusals();
    puts("image_fetch harness: ok");
    return 0;
}
