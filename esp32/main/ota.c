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

#include "ota.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"

#include <stddef.h>

bool ota_is_enabled(void) {
#if CONFIG_HOMEHUB_OTA_ENABLED
    return true;
#else
    return false;
#endif
}

#if CONFIG_MUSE_BOARD_ESP_MOSAICO && CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY
#include "esp_ota_ops.h"
#include "esp_partition.h"
#endif

bool ota_installed_by_vibe_mode(void) {
#if CONFIG_MUSE_BOARD_ESP_MOSAICO && CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY
    // Only an ESP-Mosaico build installed through its Vibe Mode firmware
    // (the ESP-Iris overlay, which links Espressif's app-side Recovery
    // support). On the board, Vibe Mode sits in `factory` beside its
    // `sysmeta` records and writes apps into an OTA slot: check all of that
    // is there, so nothing else that boots PENDING_VERIFY passes for it.
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    const esp_partition_t *sysmeta = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "sysmeta");
    esp_app_desc_t vibe_mode;
    esp_ota_img_states_t state;
    return running && factory && sysmeta
        && running->type == ESP_PARTITION_TYPE_APP
        && running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN
        && running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX
        && running->address != factory->address
        && esp_ota_get_partition_description(factory, &vibe_mode) == ESP_OK
        && esp_ota_get_state_partition(running, &state) == ESP_OK
        && state == ESP_OTA_IMG_PENDING_VERIFY
        // device.ota's own update, if one ever lands here, keeps its check.
        && !ota_verify_recorded_for_running();
#else
    return false;
#endif
}

#if CONFIG_HOMEHUB_OTA_ENABLED
#include "stack_monitor.h"
#include "config_store.h"
#include "esp_ota_ops.h"
#include <inttypes.h>
#include <stdint.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "link.ota";

#define OTA_HTTP_TIMEOUT_MS        30000
#define OTA_MAX_REDIRECTS          5
#define OTA_HTTP_TX_BUFFER_BYTES   4096
#define OTA_USER_AGENT_BYTES       64


typedef struct {
    char *url;
    bool force;
    ota_status_cb cb;
    void *user;
} ota_ctx_t;

// Parse a leading "[v]MAJOR.MINOR.PATCH" into three components. Non-numeric
// suffixes (e.g. git-describe "-15-gabc-dirty") are ignored.
static void parse_ver(const char *s, long out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!s) return;
    if (*s == 'v' || *s == 'V') s++;
    for (int i = 0; i < 3 && *s; i++) {
        char *end;
        out[i] = strtol(s, &end, 10);
        if (end == s) break;
        s = end;
        if (*s == '.') s++; else break;
    }
}

// True if `cand` is a newer version than `cur`. Numeric semver components win;
// if they're equal, any string difference (e.g. differing git-describe builds)
// counts as newer, and an identical string counts as not-newer.
static bool version_is_newer(const char *cand, const char *cur) {
    long a[3], b[3];
    parse_ver(cand, a);
    parse_ver(cur, b);
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    if (cand && cur && strcmp(cand, cur) != 0) return true;
    return false;
}

static void emit(ota_ctx_t *ctx, ota_result_t result, const char *detail,
                 const char *new_version, const char *running_version) {
    if (!ctx->cb) return;
    ota_event_t ev = {
        .result = result,
        .detail = detail,
        .new_version = new_version,
        .running_version = running_version,
    };
    ctx->cb(&ev, ctx->user);
}

static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt) {
    const char *phase = evt->user_data ? (const char *)evt->user_data : "ota";
    switch (evt->event_id) {
        case HTTP_EVENT_HEADERS_SENT:
            ESP_LOGI(TAG, "%s request headers sent", phase);
            break;
        case HTTP_EVENT_ON_HEADER:
#if CONFIG_HOMEHUB_DEV_BUILD
            ESP_LOGI(TAG, "%s response header: %s", phase,
                     evt->header_key ? evt->header_key : "<unknown>");
#endif
            break;
        case HTTP_EVENT_REDIRECT: {
            esp_http_client_redirect_event_data_t *redirect =
                (esp_http_client_redirect_event_data_t *)evt->data;
            ESP_LOGI(TAG, "%s redirect status=%d", phase,
                     redirect ? redirect->status_code : 0);
            break;
        }
        case HTTP_EVENT_ERROR:
            ESP_LOGW(TAG, "%s http event error", phase);
            break;
        default:
            break;
    }
    return ESP_OK;
}

#if CONFIG_HOMEHUB_DEV_BUILD
static void log_request_headers(const char *phase, const char *method,
                                const char *url, const char *user_agent) {
    (void)url;
    ESP_LOGI(TAG, "%s request: %s", phase, method);
    ESP_LOGI(TAG, "%s request header: User-Agent: %s",
             phase, user_agent ? user_agent : "");
    ESP_LOGI(TAG, "%s request header: Accept: <unset>", phase);
    ESP_LOGI(TAG, "%s request header: Range: <unset>", phase);
}
#else
static void log_request_headers(const char *phase, const char *method,
                                const char *url, const char *user_agent) {
    (void)phase;
    (void)method;
    (void)url;
    (void)user_agent;
}
#endif

// The record of an installed update (ota.h): "<partition address>:<ELF
// SHA-256>" in hex.
#define OTA_VERIFY_KEY "ota_verify"
#define OTA_VERIFY_LEN (8 + 1 + 64)

static void ota_verify_format(uint32_t address, const uint8_t sha[32], char out[OTA_VERIFY_LEN + 1]) {
    int n = snprintf(out, OTA_VERIFY_LEN + 1, "%08" PRIx32 ":", address);
    for (int i = 0; i < 32; i++) {
        n += snprintf(out + n, (size_t)(OTA_VERIFY_LEN + 1 - n), "%02x", sha[i]);
    }
}

bool ota_verify_recorded_for_running(void) {
    const esp_partition_t *part = esp_ota_get_running_partition();
    char want[OTA_VERIFY_LEN + 1], have[OTA_VERIFY_LEN + 2];
    if (!part || !config_get_str(OTA_VERIFY_KEY, have, sizeof(have))) {
        return false;
    }
    ota_verify_format(part->address, esp_app_get_description()->app_elf_sha256, want);
    return strcmp(have, want) == 0;
}

void ota_verify_forget(void) {
    config_erase_key(OTA_VERIFY_KEY);
}

// Streaming OTA on a dedicated task (callers are often on the WS event-handler
// or BLE-host thread, which must not block). Reads the incoming descriptor,
// applies the version gate, then downloads/verifies/installs and reboots.
static void ota_task(void *arg) {
    ota_ctx_t *ctx = (ota_ctx_t *)arg;
    char running[sizeof(esp_app_get_description()->version) + 1];
    memcpy(running, esp_app_get_description()->version, sizeof(running) - 1);
    running[sizeof(running) - 1] = '\0';
    char user_agent[OTA_USER_AGENT_BYTES];
    snprintf(user_agent, sizeof(user_agent), "HatchLink/%s", running);
    ESP_LOGI(TAG, "begin OTA (force=%d, running=%s)", ctx->force, running);

    const char *ota_url = ctx->url;
    log_request_headers("ota download", "GET", ota_url, user_agent);

    esp_http_client_config_t http_cfg = {
        .url = ota_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = user_agent,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .max_redirection_count = OTA_MAX_REDIRECTS,
        .buffer_size_tx = OTA_HTTP_TX_BUFFER_BYTES,
        .event_handler = ota_http_event_handler,
        .user_data = (void *)"ota download",
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK || !handle) {
        char msg[96];
        snprintf(msg, sizeof(msg), "begin failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", msg);
        emit(ctx, OTA_RESULT_FAILED, msg, NULL, running);
        goto done;
    }

    esp_app_desc_t new_desc;
    err = esp_https_ota_get_img_desc(handle, &new_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not read image descriptor: %s",
                 esp_err_to_name(err));
        esp_https_ota_abort(handle);
        emit(ctx, OTA_RESULT_FAILED, "could not read image descriptor", NULL, running);
        goto done;
    }
    char new_version[sizeof(new_desc.version) + 1];
    memcpy(new_version, new_desc.version, sizeof(new_desc.version));
    new_version[sizeof(new_desc.version)] = '\0';
    ESP_LOGI(TAG, "incoming version=%s", new_version);

    if (!ctx->force && !version_is_newer(new_version, running)) {
        ESP_LOGW(TAG, "skipping: %s not newer than %s", new_version, running);
        esp_https_ota_abort(handle);
        emit(ctx, OTA_RESULT_SKIPPED, "image not newer than running version",
             new_version, running);
        goto done;
    }

    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        // streaming; yield briefly between reads
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "download failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", msg);
        esp_https_ota_abort(handle);
        emit(ctx, OTA_RESULT_FAILED, msg, new_version, running);
        goto done;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "incomplete image received");
        esp_https_ota_abort(handle);
        emit(ctx, OTA_RESULT_FAILED, "incomplete image received", new_version, running);
        goto done;
    }

    // The next boot holds this image to the health check (app.c), so record it
    // before finish() can switch the boot partition.
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    char record[OTA_VERIFY_LEN + 1];
    if (target) {
        ota_verify_format(target->address, new_desc.app_elf_sha256, record);
    }
    if (!target || !config_set_str(OTA_VERIFY_KEY, record)) {
        ESP_LOGE(TAG, "could not record the update for its health check");
        esp_https_ota_abort(handle);
        emit(ctx, OTA_RESULT_FAILED, "could not record the update", new_version, running);
        goto done;
    }

    // finish() validates SHA-256 + signature (when enabled) and sets the boot
    // partition. Do NOT abort after calling finish.
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ota_verify_forget();
        char msg[96];
        snprintf(msg, sizeof(msg), "verify/finish failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", msg);
        emit(ctx, OTA_RESULT_FAILED, msg, new_version, running);
        goto done;
    }

    ESP_LOGI(TAG, "applied %s -> %s, rebooting", running, new_version);
    emit(ctx, OTA_RESULT_APPLIED, "applied", new_version, running);
    // Let any status notification flush before the reboot tears down the link.
    vTaskDelay(pdMS_TO_TICKS(750));
    stack_monitor_record(NULL);
    esp_restart();

done:
    free(ctx->url);
    free(ctx);
    stack_monitor_record(NULL);
    vTaskDelete(NULL);
}

void ota_start(const char *url, bool force, ota_status_cb cb, void *user) {
    ota_ctx_t *ctx = url && *url ? calloc(1, sizeof(*ctx)) : NULL;
    if (ctx) {
        ctx->url = strdup(url);
        ctx->force = force;
        ctx->cb = cb;
        ctx->user = user;
    }
    if (!ctx || !ctx->url) {
        ESP_LOGE(TAG, "could not start OTA: %s",
                 url && *url ? "out of memory" : "empty URL");
        if (ctx) free(ctx);
        if (cb) {
            ota_event_t ev = { .result = OTA_RESULT_FAILED,
                               .detail = url && *url ? "out of memory" : "empty url" };
            cb(&ev, user);
        }
        return;
    }
    // 8 KB stack: TLS handshake + flash writes during the OTA download.
    if (xTaskCreate(ota_task, "ota", 16384, ctx, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start OTA task");
        free(ctx->url);
        free(ctx);
        if (cb) {
            ota_event_t ev = { .result = OTA_RESULT_FAILED,
                               .detail = "could not start ota task" };
            cb(&ev, user);
        }
    }
}

#else

bool ota_verify_recorded_for_running(void) {
    return false;
}

void ota_verify_forget(void) {
}

void ota_start(const char *url, bool force, ota_status_cb cb, void *user) {
    (void)url;
    (void)force;
    if (cb) {
        ota_event_t ev = {
            .result = OTA_RESULT_SKIPPED,
            .detail = "OTA disabled in this build",
            .new_version = NULL,
            .running_version = esp_app_get_description()->version,
        };
        cb(&ev, user);
    }
}

#endif
