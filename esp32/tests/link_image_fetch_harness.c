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
// finish within its deadline however slowly the server sends.

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "image_fetch.h"
#include "led_status.h"
#include "rom/tjpgd.h"

#define WIDTH 8
#define HEIGHT 16
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

#if CONFIG_HOMEHUB_WIFI_IDLE_MAX_MODEM
static int transfers;
void wifi_mgr_transfer(bool active) {
    if (active) { assert(transfers == 0); transfers++; }
    else { assert(transfers == 1); transfers--; }
}
#endif

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
#if CONFIG_HOMEHUB_WIFI_IDLE_MAX_MODEM
    assert(transfers == 1);
#endif
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

// ---- Board -------------------------------------------------------------------

static unsigned g_stack;

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, unsigned stack_depth,
                       void *params, unsigned priority, TaskHandle_t *out_handle) {
    (void)name;
    (void)priority;
    (void)out_handle;
    g_stack = stack_depth;
    task(params);   // runs to completion; vTaskDelete returns
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task) {
#if CONFIG_HOMEHUB_WIFI_IDLE_MAX_MODEM
    assert(transfers == 0);
#endif
    (void)task;
}

void vTaskDelay(int ticks) {
    (void)ticks;
}

size_t heap_caps_get_free_size(int caps) {
    (void)caps;
    return 200000;
}

size_t heap_caps_get_minimum_free_size(int caps) {
    (void)caps;
    return 200000;
}

size_t heap_caps_get_largest_free_block(int caps) {
    (void)caps;
    return 100000;
}

void *heap_caps_malloc(size_t size, int caps) {
    (void)caps;
    return malloc(size);
}

bool led_status_display_info(int *width, int *height) {
    *width = WIDTH;
    *height = HEIGHT;
    return true;
}

static int g_rows_drawn;

bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels) {
    (void)x;
    (void)y;
    (void)w;
    (void)pixels;
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
    g_deadline_hint_us = g_now_us + 60 * 1000000LL;
    CHECK(image_fetch_start(url, 0, on_done, NULL, &code, &message),
          "start %s refused: %s", url, message ? message : "");
    CHECK(g_done, "download did not finish");
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

int main(void) {
    test_http_redirect_to_https_is_refused();
    test_https_redirect_to_http_is_refused();
    test_scheme_check_ignores_case();
    test_same_scheme_redirects_are_followed();
    test_trickled_body_hits_the_deadline();
    test_slow_redirect_chain_stops_at_the_limit();
    puts("image_fetch harness: ok");
    return 0;
}
