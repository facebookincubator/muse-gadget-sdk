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

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bug_report.h"
#include "cJSON.h"
#include "diagnostic_log.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "tunnel_netif.h"

static TaskFunction_t g_pending_task;
static void *g_pending_arg;
static bool g_task_create_fails;
static bool g_log_snapshot_fails;
static bool g_base64_fails;
static bool g_fail_psram_allocations;
static uint32_t g_overwritten_lines;
static uint32_t g_dropped_lines;
static uint32_t g_truncated_lines;
static noise_ctrl_session_generation_t g_submitted_generation;
static char g_expected_request_id[BUG_REPORT_REQUEST_ID_MAX_BYTES + 1];
static char *g_payload_json;
static const char *g_log_data;
static size_t g_log_len;
static bool g_log_truncated;

static void fail_at(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d: ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

#define CHECK(condition, ...) \
    do { if (!(condition)) fail_at(__FILE__, __LINE__, __VA_ARGS__); } while (0)

static void reset_fakes(void) {
    free(g_payload_json);
    g_payload_json = NULL;
    g_pending_task = NULL;
    g_pending_arg = NULL;
    g_task_create_fails = false;
    g_log_snapshot_fails = false;
    g_base64_fails = false;
    g_fail_psram_allocations = false;
    g_overwritten_lines = 0;
    g_dropped_lines = 0;
    g_truncated_lines = 0;
    g_submitted_generation = 0;
    g_log_data = NULL;
    g_log_len = 0;
    g_log_truncated = false;
    snprintf(g_expected_request_id, sizeof(g_expected_request_id),
             "request-1");
}

BaseType_t xTaskCreate(TaskFunction_t task, const char *name,
                       unsigned stack_depth, void *params,
                       unsigned priority, TaskHandle_t *out_handle) {
    (void)name;
    (void)stack_depth;
    (void)priority;
    if (g_task_create_fails) return pdFALSE;
    CHECK(g_pending_task == NULL, "unexpected second pending task");
    g_pending_task = task;
    g_pending_arg = params;
    if (out_handle) *out_handle = (TaskHandle_t)1;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task) { (void)task; }
void vTaskDelay(int ticks) { (void)ticks; }

static void run_pending_task(void) {
    CHECK(g_pending_task != NULL, "no task pending");
    TaskFunction_t task = g_pending_task;
    void *arg = g_pending_arg;
    g_pending_task = NULL;
    g_pending_arg = NULL;
    task(arg);
}

void *heap_caps_malloc(size_t size, int caps) {
    if ((caps & MALLOC_CAP_SPIRAM) && g_fail_psram_allocations) {
        return NULL;
    }
    return malloc(size);
}

size_t heap_caps_get_free_size(int caps) {
    return caps == MALLOC_CAP_SPIRAM ? 4000000u : 64000u;
}

size_t heap_caps_get_minimum_free_size(int caps) {
    (void)caps;
    return 48000u;
}

size_t heap_caps_get_largest_free_block(int caps) {
    (void)caps;
    return 32000u;
}

bool diagnostic_log_snapshot(diagnostic_log_snapshot_t *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    if (g_log_snapshot_fails) return false;
    if (!g_log_data || g_log_len == 0) return true;
    snapshot->data = malloc(g_log_len + 1);
    CHECK(snapshot->data != NULL, "log snapshot allocation failed");
    memcpy(snapshot->data, g_log_data, g_log_len);
    snapshot->data[g_log_len] = '\0';
    snapshot->len = g_log_len;
    snapshot->truncated = g_log_truncated;
    snapshot->overwritten_lines = g_overwritten_lines;
    snapshot->dropped_lines = g_dropped_lines;
    snapshot->truncated_lines = g_truncated_lines;
    return true;
}

void diagnostic_log_snapshot_free(diagnostic_log_snapshot_t *snapshot) {
    free(snapshot->data);
    memset(snapshot, 0, sizeof(*snapshot));
}

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t required = 4 * ((slen + 2) / 3);
    if (olen) *olen = required;
    if (g_base64_fails || !dst || dlen < required + 1) return -1;
    size_t in = 0;
    size_t out = 0;
    while (in < slen) {
        size_t remaining = slen - in;
        uint32_t a = src[in++];
        uint32_t b = remaining > 1 ? src[in++] : 0;
        uint32_t c = remaining > 2 ? src[in++] : 0;
        uint32_t value = (a << 16) | (b << 8) | c;
        dst[out++] = (unsigned char)alphabet[(value >> 18) & 0x3f];
        dst[out++] = (unsigned char)alphabet[(value >> 12) & 0x3f];
        dst[out++] = remaining > 1
                         ? (unsigned char)alphabet[(value >> 6) & 0x3f]
                         : (unsigned char)'=';
        dst[out++] = remaining > 2
                         ? (unsigned char)alphabet[value & 0x3f]
                         : (unsigned char)'=';
    }
    dst[out] = '\0';
    if (olen) *olen = out;
    return 0;
}

const char *identity_node_id(void) { return "musegadget-a1b2c3"; }

bool wifi_mgr_is_connected(void) { return true; }

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info) {
    ap_info->rssi = -47;
    return ESP_OK;
}

bool noise_tunnel_is_connected(void) { return true; }

void tunnel_netif_get_stats(tunnel_stats_t *out) {
    *out = (tunnel_stats_t){
        .rx_pkts = 11,
        .rx_bytes = 1200,
        .tx_pkts = 12,
        .tx_bytes = 1300,
        .tx_dropped = 2,
    };
}

void noise_ctrl_send_command_result(
    noise_ctrl_session_generation_t session_generation,
    const char *request_id, cJSON *result) {
    CHECK(strcmp(request_id, g_expected_request_id) == 0,
          "request ID ownership mismatch: %s", request_id);
    g_submitted_generation = session_generation;

    cJSON *payload = cJSON_GetObjectItem(result, "payload_json");
    free(g_payload_json);
    g_payload_json = payload && cJSON_IsString(payload)
                         ? strdup(payload->valuestring)
                         : NULL;
    cJSON_Delete(result);
}

static const esp_app_desc_t g_app = {
    .version = "1.2.3",
    .idf_ver = "v6.0.3",
};

const esp_app_desc_t *esp_app_get_description(void) { return &g_app; }
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_PANIC; }

static const esp_partition_t g_running = {.label = "ota_0"};
static const esp_partition_t g_boot = {.label = "ota_1"};

const esp_partition_t *esp_ota_get_running_partition(void) {
    return &g_running;
}

const esp_partition_t *esp_ota_get_boot_partition(void) { return &g_boot; }

esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition,
                                      esp_ota_img_states_t *state) {
    CHECK(partition == &g_running, "unexpected OTA partition");
    *state = ESP_OTA_IMG_VALID;
    return ESP_OK;
}

static const char *report_json(void) {
    CHECK(g_payload_json != NULL, "missing report payload");
    return g_payload_json;
}

static void expect_start_and_run(const char *request_id) {
    snprintf(g_expected_request_id, sizeof(g_expected_request_id), "%s",
             request_id);
    CHECK(bug_report_start(request_id, 41) == BUG_REPORT_START_ACCEPTED,
          "bug report did not start");
    run_pending_task();
}

static void test_success_owns_input_and_returns_restricted_log_report(void) {
    static const char events[] =
        "I (100) link.app: connected ssid=Office\n"
        "W (200) link.noise_ctrl: reconnecting VM vm-123\n";
    g_log_data = events;
    g_log_len = strlen(events);

    char request_id[32] = "request-1";
    CHECK(bug_report_start(request_id, 41) == BUG_REPORT_START_ACCEPTED,
          "bug report did not start");
    strcpy(request_id, "mutated");
    CHECK(bug_report_start("request-2", 41) == BUG_REPORT_START_BUSY,
          "second report should be busy");
    run_pending_task();

    const char *report = report_json();
    CHECK(strlen(report) <= BUG_REPORT_MAX_BYTES,
          "report exceeded cap: %zu", strlen(report));
    CHECK(strlen(g_payload_json) <= BUG_REPORT_MAX_BYTES,
          "missing or oversized report");
    CHECK(strstr(report, "\"report_type\":\"hatch_link_bug_report\"")
              && strstr(report, "\"source_request_id\":\"request-1\"")
              && strstr(report, "\"firmware_version\":\"1.2.3\"")
              && strstr(report, "\"rssi_dbm\":-47")
              && strstr(report, "\"tail_base64\":")
              && strstr(report, "\"sensitivity\":\"restricted\"")
              && strstr(report, "\"contains_pii\":true"),
          "diagnostic schema incomplete: %s", report);
    CHECK(strstr(report, "\"report_id\"") == NULL,
          "report duplicated the correlation ID");
    const char *forbidden[] = {
        "\"title\"", "\"context\"", "\"access_token\"",
        "\"refresh_token\"", "\"password\"", "\"ssid\"",
        "\"bssid\"", "\"username\"", "\"user_id\"",
        "\"vm_url\"", "\"public_key\"", "\"private_key\"",
        "\"coredump\"", "\"local_ip\"", "\"setup_stage\"", NULL,
    };
    for (const char **field = forbidden; *field; field++) {
        CHECK(strstr(report, *field) == NULL,
              "report contains forbidden field %s", *field);
    }
    CHECK(g_submitted_generation == 41,
          "result lost its Noise session generation");

    printf("PRIMARY_PAYLOAD_JSON=%s\n", g_payload_json);
}

static void test_validation_and_task_failure_release_gate(void) {
    CHECK(bug_report_start(NULL, 41)
              == BUG_REPORT_START_INVALID_REQUEST_ID,
          "NULL request ID accepted");
    CHECK(bug_report_start("", 41)
              == BUG_REPORT_START_INVALID_REQUEST_ID,
          "empty request ID accepted");
    CHECK(bug_report_start("request with spaces", 41)
              == BUG_REPORT_START_INVALID_REQUEST_ID,
          "unsafe request ID accepted");
    char too_long[BUG_REPORT_REQUEST_ID_MAX_BYTES + 2];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    CHECK(bug_report_start(too_long, 41)
              == BUG_REPORT_START_INVALID_REQUEST_ID,
          "oversized request ID accepted");
    CHECK(bug_report_start("request-1", 0)
              == BUG_REPORT_START_INVALID_SESSION,
          "zero session accepted");

    g_task_create_fails = true;
    CHECK(bug_report_start("request-1", 41)
              == BUG_REPORT_START_TASK_UNAVAILABLE,
          "task failure not surfaced");
    g_task_create_fails = false;
    expect_start_and_run("request-1");
}

static void test_logs_degrade_safely_and_maximum_stays_bounded(void) {
    static const char events[] = "I (1) link.app: boot\n";
    g_log_data = events;
    g_log_len = strlen(events);
    g_fail_psram_allocations = true;
    expect_start_and_run("request-1");
    CHECK(strstr(report_json(), "\"available\":false"),
          "persistent PSRAM failure should keep a report without logs");
    CHECK(g_payload_json != NULL, "small internal fallback did not complete");

    reset_fakes();
    g_log_data = events;
    g_log_len = strlen(events);
    g_base64_fails = true;
    expect_start_and_run("request-1");
    CHECK(strstr(report_json(), "\"available\":false"),
          "base64 failure should keep a report without logs");

    reset_fakes();
    g_log_snapshot_fails = true;
    expect_start_and_run("request-1");
    CHECK(strstr(report_json(), "\"available\":false"),
          "missing log snapshot should keep a report without logs");

    reset_fakes();
    char *large_events = malloc(DIAGNOSTIC_LOG_CAPACITY_BYTES + 1);
    CHECK(large_events != NULL, "large event allocation failed");
    for (size_t i = 0; i < DIAGNOSTIC_LOG_CAPACITY_BYTES; i++) {
        large_events[i] = (i % 64 == 63) ? '\n' : 'x';
    }
    large_events[DIAGNOSTIC_LOG_CAPACITY_BYTES] = '\0';
    g_log_data = large_events;
    g_log_len = DIAGNOSTIC_LOG_CAPACITY_BYTES;
    g_log_truncated = true;
    g_overwritten_lines = 7;
    g_dropped_lines = 2;
    g_truncated_lines = 1;
    expect_start_and_run("request-1");
    CHECK(strlen(report_json()) <= BUG_REPORT_MAX_BYTES,
          "maximum log report exceeded cap");
    CHECK(g_payload_json && strlen(g_payload_json) <= BUG_REPORT_MAX_BYTES,
          "maximum log report exceeded cap");
    CHECK(strstr(report_json(), "\"truncated\":true"),
          "snapshot truncation flag was lost");
    CHECK(strstr(report_json(), "\"overwritten_lines\":7")
              && strstr(report_json(), "\"dropped_lines\":2")
              && strstr(report_json(), "\"truncated_lines\":1"),
          "log-loss counters were not serialized");
    free(large_events);
}

#define RUN_TEST(test) \
    do { \
        reset_fakes(); \
        test(); \
        CHECK(g_pending_task == NULL, "test left a pending task"); \
    } while (0)

int main(void) {
    RUN_TEST(test_success_owns_input_and_returns_restricted_log_report);
    RUN_TEST(test_validation_and_task_failure_release_gate);
    RUN_TEST(test_logs_degrade_safely_and_maximum_stays_bounded);
    reset_fakes();
    return 0;
}
