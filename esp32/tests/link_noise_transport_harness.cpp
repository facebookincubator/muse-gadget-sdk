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

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <sys/types.h>
#include "esp_log.h"
#include "noise_upgrade.h"

static const char *TAG = "noise-test";
struct esp_tls_t {};
static bool s_running = true;
static const char *s_auth_token = "synthetic";
static const char *s_vm_id = "synthetic-vm";
static const char *s_noise_host = "synthetic.example";
#define NOISE_PATH "/v1/noise"
#define ESP_TLS_ERR_SSL_WANT_READ (-6900)
#define ESP_TLS_ERR_SSL_WANT_WRITE (-6901)
static constexpr int64_t WS_FRAME_TIMEOUT_US = 15000000;
static int64_t now;
static std::string reply;
static size_t reply_pos;
static int64_t esp_timer_get_time() { return now; }
static void vTaskDelay(int ticks) { now += ticks * 1000; }
static ssize_t write_all(esp_tls_t *, const char *, size_t n) { return n; }
static ssize_t esp_tls_conn_read(esp_tls_t *, char *out, size_t n) {
    size_t take = std::min(n, reply.size() - reply_pos);
    memcpy(out, reply.data() + reply_pos, take);
    reply_pos += take;
    return take;
}
static void esp_tls_conn_destroy(esp_tls_t *) {}

// These are extracted from the checked-out production source by the runner.
#include "noise_reconnect.inc"
#include "noise_upgrade.inc"
static void note_attempt_failed(const char *) {}
static session_result_t upgrade_result(esp_tls_t *tls) {
#include "noise_upgrade_result.inc"
    return SESSION_OK;
}

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

static unsigned classification_failures;

static void expect_status(const char *response, int expected) {
    reply = response;
    reply_pos = 0;
    now = 0;
    int got = upgrade_result(nullptr);
    if (got != expected) {
        fprintf(stderr, "expected session result %d, got %d for %s", expected, got, response);
        classification_failures++;
    }
}

static void test_upgrade_status_classification(void) {
    expect_status("HTTP/1.1 101 Switching Protocols\r\n\r\n", SESSION_OK);
    expect_status("HTTP/1.1 401 Unauthorized\r\n\r\n", SESSION_AUTH_FAILED);
    expect_status("HTTP/1.1 403 Forbidden\r\n\r\n", SESSION_AUTH_FAILED);
    expect_status("HTTP/1.0 401 Unauthorized\r\n\r\n", SESSION_AUTH_FAILED);
    expect_status("HTTP/1.1 500 Internal Server Error\r\n\r\n", SESSION_FAILED);
    expect_status("not http\r\n\r\n", SESSION_FAILED);
    expect_status("HTTP/1", SESSION_FAILED);
}

static void test_reconnect_policy_requests_vm_refresh_after_short_failures(void) {
    uint32_t backoff = 1000;
    uint8_t failures = 0;

    CHECK(update_reconnect_policy(SESSION_FAILED, 100, &backoff, &failures) == 0,
          "first short failure should not request refresh");
    CHECK(backoff == 2000, "first backoff=%u", (unsigned)backoff);
    CHECK(failures == 1, "first failures=%u", failures);

    CHECK(update_reconnect_policy(SESSION_FAILED, 200, &backoff, &failures) == 0,
          "second short failure should not request refresh");
    CHECK(backoff == 4000, "second backoff=%u", (unsigned)backoff);
    CHECK(failures == 2, "second failures=%u", failures);

    CHECK(update_reconnect_policy(SESSION_FAILED, 300, &backoff, &failures) == 1,
          "third short failure should request VM refresh");
    CHECK(backoff == 8000, "third backoff=%u", (unsigned)backoff);
    CHECK(failures == 0, "refresh should reset short failure counter");
}

static void test_reconnect_policy_resets_after_stable_session(void) {
    uint32_t backoff = 8000;
    uint8_t failures = 2;

    CHECK(update_reconnect_policy(SESSION_FAILED, 31000, &backoff, &failures) == 0,
          "stable session should not request refresh");
    CHECK(backoff == 1000, "stable session should reset backoff");
    CHECK(failures == 0, "stable session should reset failures");
}

static void test_reconnect_policy_auth_failure_uses_long_backoff(void) {
    uint32_t backoff = 4000;
    uint8_t failures = 2;

    CHECK(update_reconnect_policy(SESSION_AUTH_FAILED, 10, &backoff, &failures) == 0,
          "auth failure already has explicit auth status");
    CHECK(backoff == 60000, "auth failure backoff=%u", (unsigned)backoff);
    CHECK(failures == 0, "auth failure should reset generic failures");
}

int main(void) {
    test_reconnect_policy_requests_vm_refresh_after_short_failures();
    test_reconnect_policy_resets_after_stable_session();
    test_reconnect_policy_auth_failure_uses_long_backoff();
    test_upgrade_status_classification();
    CHECK(classification_failures == 0, "%u HTTP auth classification cases failed",
          classification_failures);
    return 0;
}
