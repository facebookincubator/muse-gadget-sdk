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

#include "bug_report.h"
#include "stack_monitor.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "diagnostic_log.h"
#include "identity.h"
#include "wifi_mgr.h"

#define BUG_REPORT_SCHEMA_VERSION 1
#define BUG_REPORT_REPORT_TYPE "hatch_link_bug_report"
#define BUG_REPORT_REDACTION_PROFILE "hatch-link-support-v1"
#define BUG_REPORT_LOG_FORMAT "hatch-link-application-log-v1"
#define BUG_REPORT_LOG_SENSITIVITY "restricted"
#define BUG_REPORT_LOG_ENCODING "base64"
#define BUG_REPORT_TASK_STACK_BYTES 12288
#define BUG_REPORT_TASK_PRIORITY 3

#define TAG "link.bug_report"

typedef struct {
    noise_ctrl_session_generation_t session_generation;
    char source_request_id[BUG_REPORT_REQUEST_ID_MAX_BYTES + 1];
} bug_report_task_args_t;

typedef struct {
    const esp_app_desc_t *app;
    uint64_t uptime_seconds;
    const char *reset_reason;
    size_t heap_internal_free;
    size_t heap_internal_min_free;
    size_t heap_internal_largest_free;
    size_t heap_psram_free;
    bool wifi_connected;
    bool has_rssi;
    int8_t rssi_dbm;
    char running_partition[17];
    char boot_partition[17];
    const char *ota_state;
} bug_report_snapshot_t;

static atomic_bool s_report_active = ATOMIC_VAR_INIT(false);

static void secure_zero(void *value, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)value;
    while (len--) *p++ = 0;
}

static void *log_buffer_alloc(size_t len) {
#if CONFIG_SPIRAM
    return heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(len);
#endif
}

static const char *reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        case ESP_RST_USB: return "usb";
        case ESP_RST_JTAG: return "jtag";
        case ESP_RST_EFUSE: return "efuse";
        case ESP_RST_PWR_GLITCH: return "power_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        case ESP_RST_UNKNOWN:
        default: return "unknown";
    }
}

static const char *ota_state_name(esp_ota_img_states_t state) {
    switch (state) {
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID: return "valid";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted";
        case ESP_OTA_IMG_UNDEFINED: return "undefined";
        default: return "unknown";
    }
}

static void collect_snapshot(bug_report_snapshot_t *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->app = esp_app_get_description();
    snapshot->uptime_seconds = (uint64_t)(esp_timer_get_time() / 1000000LL);
    snapshot->reset_reason = reset_reason_name(esp_reset_reason());
    snapshot->heap_internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snapshot->heap_internal_min_free =
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    snapshot->heap_internal_largest_free =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    snapshot->heap_psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    snapshot->wifi_connected = wifi_mgr_is_connected();
    wifi_ap_record_t ap = {0};
    if (snapshot->wifi_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snapshot->has_rssi = true;
        snapshot->rssi_dbm = ap.rssi;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    snprintf(snapshot->running_partition, sizeof(snapshot->running_partition),
             "%s", running ? running->label : "unknown");
    snprintf(snapshot->boot_partition, sizeof(snapshot->boot_partition),
             "%s", boot ? boot->label : "unknown");
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    snapshot->ota_state = running
                              && esp_ota_get_state_partition(running, &state)
                                     == ESP_OK
                          ? ota_state_name(state)
                          : "unknown";
}

static cJSON *add_object(cJSON *parent, const char *name) {
    cJSON *child = cJSON_CreateObject();
    if (!child) return NULL;
    if (!cJSON_AddItemToObject(parent, name, child)) {
        cJSON_Delete(child);
        return NULL;
    }
    return child;
}

static bool add_string(cJSON *object, const char *name, const char *value) {
    return cJSON_AddStringToObject(object, name, value ? value : "") != NULL;
}

static bool add_number(cJSON *object, const char *name, double value) {
    return cJSON_AddNumberToObject(object, name, value) != NULL;
}

static bool add_bool(cJSON *object, const char *name, bool value) {
    return cJSON_AddBoolToObject(object, name, value) != NULL;
}

static char *encode_log_tail(const diagnostic_log_snapshot_t *logs) {
    if (!logs || logs->len == 0) {
        char *empty = log_buffer_alloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    if (!logs->data || logs->len > DIAGNOSTIC_LOG_CAPACITY_BYTES) return NULL;

    size_t encoded_len = 4 * ((logs->len + 2) / 3);
    char *encoded = log_buffer_alloc(encoded_len + 1);
    if (!encoded) return NULL;
    size_t written = 0;
    int rc = mbedtls_base64_encode(
        (unsigned char *)encoded, encoded_len + 1, &written,
        (const unsigned char *)logs->data, logs->len);
    if (rc != 0 || written != encoded_len) {
        secure_zero(encoded, encoded_len + 1);
        free(encoded);
        return NULL;
    }
    encoded[written] = '\0';
    return encoded;
}

// Drops the oldest device_health connectivity events until about `excess`
// bytes of serialized report are gone. False if there was nothing to drop.
static bool drop_oldest_events(cJSON *root, size_t excess) {
    cJSON *health = cJSON_GetObjectItem(root, "device_health");
    cJSON *events = cJSON_GetObjectItem(health, "connectivity_events");
    size_t dropped = 0;
    while (dropped < excess && cJSON_IsArray(events)) {
        cJSON *oldest = events->child;
        if (!oldest) break;
        // The string, its quotes and a comma.
        dropped += (cJSON_IsString(oldest) ? strlen(oldest->valuestring) : 0) + 3;
        cJSON_DeleteItemFromArray(events, 0);
    }
    return dropped > 0;
}

static char *build_report_json(const bug_report_snapshot_t *snapshot,
                               const char *source_request_id,
                               const diagnostic_log_snapshot_t *logs,
                               bool logs_available) {
    char *encoded_logs = logs_available ? encode_log_tail(logs) : NULL;
    if (logs_available && !encoded_logs) logs_available = false;

    cJSON *root = cJSON_CreateObject();
    bool ok = root
              && add_number(root, "schema_version", BUG_REPORT_SCHEMA_VERSION)
              && add_string(root, "report_type", BUG_REPORT_REPORT_TYPE)
              && add_string(root, "source_request_id", source_request_id)
              && add_string(root, "node_id", identity_node_id())
              && add_string(root, "redaction_profile",
                            BUG_REPORT_REDACTION_PROFILE);

    cJSON *device = ok ? add_object(root, "device") : NULL;
    ok = device
         && add_string(device, "model_id", "hatch-link")
         && add_string(device, "firmware_version",
                       snapshot->app ? snapshot->app->version : "unknown")
         && add_string(device, "idf_version",
                       snapshot->app ? snapshot->app->idf_ver : "unknown")
         && add_string(device, "reset_reason", snapshot->reset_reason);

    cJSON *runtime = ok ? add_object(root, "runtime") : NULL;
    ok = runtime
         && add_number(runtime, "uptime_seconds",
                       (double)snapshot->uptime_seconds)
         && add_number(runtime, "heap_internal_free",
                       (double)snapshot->heap_internal_free)
         && add_number(runtime, "heap_internal_min_free",
                       (double)snapshot->heap_internal_min_free)
         && add_number(runtime, "heap_internal_largest_free",
                       (double)snapshot->heap_internal_largest_free)
         && add_number(runtime, "heap_psram_free",
                       (double)snapshot->heap_psram_free);

    cJSON *network = ok ? add_object(root, "network") : NULL;
    ok = network
         && add_bool(network, "wifi_connected", snapshot->wifi_connected);
    if (ok && snapshot->has_rssi) {
        ok = add_number(network, "rssi_dbm", snapshot->rssi_dbm);
    }

    cJSON *ota = ok ? add_object(root, "ota") : NULL;
    ok = ota
         && add_string(ota, "running_partition", snapshot->running_partition)
         && add_string(ota, "boot_partition", snapshot->boot_partition)
         && add_string(ota, "state", snapshot->ota_state);

    // The device.health payload: connectivity history and tunnel state.
    // Optional: a report without it still beats no report.
    if (ok) {
        cJSON *health = noise_ctrl_device_health_json(true);
        if (health) {
            if (!cJSON_AddItemToObject(root, "device_health", health)) {
                cJSON_Delete(health);
            }
        }
    }

    cJSON *log_object = ok ? add_object(root, "logs") : NULL;
    ok = log_object
         && add_bool(log_object, "available", logs_available)
         && add_string(log_object, "format", BUG_REPORT_LOG_FORMAT)
         && add_string(log_object, "encoding", BUG_REPORT_LOG_ENCODING)
         && add_string(log_object, "sensitivity",
                       BUG_REPORT_LOG_SENSITIVITY)
         && add_bool(log_object, "contains_pii", true)
         && add_bool(log_object, "truncated",
                     logs_available && logs && logs->truncated)
         && add_number(log_object, "overwritten_lines",
                       logs_available && logs ? logs->overwritten_lines : 0)
         && add_number(log_object, "dropped_lines",
                       logs_available && logs ? logs->dropped_lines : 0)
         && add_number(log_object, "truncated_lines",
                       logs_available && logs ? logs->truncated_lines : 0)
         && add_string(log_object, "tail_base64",
                       logs_available ? encoded_logs : "");

    if (encoded_logs) {
        secure_zero(encoded_logs, strlen(encoded_logs));
        free(encoded_logs);
    }

    // Over the cap, keep the newest connectivity events that fit rather than
    // lose the whole report.
    char *report = NULL;
    size_t report_len = 0;
    while (ok) {
        report = cJSON_PrintUnformatted(root);
        if (!report) break;
        report_len = strlen(report);
        if (report_len <= BUG_REPORT_MAX_BYTES) break;
        secure_zero(report, report_len);
        cJSON_free(report);
        report = NULL;
        if (!drop_oldest_events(root, report_len - BUG_REPORT_MAX_BYTES)) break;
    }
    cJSON_Delete(root);
    if (!report) return NULL;
    if (report_len == 0) {
        cJSON_free(report);
        return NULL;
    }
    return report;
}

static cJSON *make_success_result(const char *report) {
    cJSON *result = cJSON_CreateObject();
    if (!result || !add_bool(result, "ok", true)
        || !add_string(result, "payload_json", report)) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

static cJSON *make_failure_result(const char *code, const char *message) {
    cJSON *result = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    if (!result || !error || !add_bool(result, "ok", false)
        || !add_string(error, "code", code)
        || !add_string(error, "message", message)
        || !cJSON_AddItemToObject(result, "error", error)) {
        cJSON_Delete(error);
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

static void bug_report_task(void *arg) {
    bug_report_task_args_t *args = (bug_report_task_args_t *)arg;
    diagnostic_log_snapshot_t logs = {0};
    bug_report_snapshot_t snapshot;
    char *report = NULL;
    cJSON *result = NULL;

    ESP_LOGI(TAG, "bug report requested");
    collect_snapshot(&snapshot);
    bool logs_available = diagnostic_log_snapshot(&logs);
    report = build_report_json(&snapshot, args->source_request_id,
                               &logs, logs_available);
    diagnostic_log_snapshot_free(&logs);

    if (!report) {
        ESP_LOGE(TAG, "bug report generation failed");
        result = make_failure_result(
            "generation_failed",
            CONFIG_GADGET_PRODUCT_NAME " could not build the bounded diagnostic report");
    } else {
        result = make_success_result(report);
        if (!result) {
            ESP_LOGE(TAG, "bug report result allocation failed");
            result = make_failure_result(
                "out_of_memory",
                CONFIG_GADGET_PRODUCT_NAME " could not allocate the command result");
        }
    }
    if (report) {
        secure_zero(report, strlen(report));
        cJSON_free(report);
    }

    if (result) {
        noise_ctrl_send_command_result(
            args->session_generation, args->source_request_id, result);
    }
    atomic_store_explicit(&s_report_active, false, memory_order_release);
    secure_zero(args, sizeof(*args));
    free(args);
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

static bool request_id_is_valid(const char *request_id, size_t *length) {
    if (!request_id) return false;
    const char *end = memchr(request_id, '\0',
                             BUG_REPORT_REQUEST_ID_MAX_BYTES + 1);
    if (!end || end == request_id) return false;
    size_t len = (size_t)(end - request_id);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)request_id[i];
        bool valid = (c >= 'a' && c <= 'z')
                     || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9')
                     || c == '-' || c == '_' || c == '.' || c == ':';
        if (!valid) return false;
    }
    if (length) *length = len;
    return true;
}

bug_report_start_status_t bug_report_start(
    const char *source_request_id,
    noise_ctrl_session_generation_t session_generation) {
    size_t request_id_len = 0;
    if (!request_id_is_valid(source_request_id, &request_id_len)) {
        return BUG_REPORT_START_INVALID_REQUEST_ID;
    }
    if (!session_generation) return BUG_REPORT_START_INVALID_SESSION;

    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s_report_active, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return BUG_REPORT_START_BUSY;
    }

    bug_report_task_args_t *args = calloc(1, sizeof(*args));
    if (!args) {
        atomic_store_explicit(&s_report_active, false, memory_order_release);
        return BUG_REPORT_START_OUT_OF_MEMORY;
    }
    args->session_generation = session_generation;
    memcpy(args->source_request_id, source_request_id, request_id_len);

    if (xTaskCreate(bug_report_task, "bug_report", BUG_REPORT_TASK_STACK_BYTES,
                    args, BUG_REPORT_TASK_PRIORITY, NULL) != pdPASS) {
        secure_zero(args, sizeof(*args));
        free(args);
        atomic_store_explicit(&s_report_active, false, memory_order_release);
        return BUG_REPORT_START_TASK_UNAVAILABLE;
    }
    return BUG_REPORT_START_ACCEPTED;
}
