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

#include "muse_account_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "network_proxy_tls.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "muse_account_api";

#define FETCH_URL "https://api.muse.ai/fetch_vms"
#define MAX_RESPONSE (32 * 1024)
#define ATTEMPTS 3   /* the first DNS lookup after Wi-Fi comes up often fails */

typedef struct {
    char *buf;
    size_t len;
} resp_t;

static esp_err_t on_http(esp_http_client_event_t *evt)
{
    resp_t *r = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || r->len + evt->data_len > MAX_RESPONSE) {
        return ESP_OK;
    }
    memcpy(r->buf + r->len, evt->data, evt->data_len);
    r->len += evt->data_len;
    return ESP_OK;
}

/* Returns the HTTP status, or 0 if the request never completed. */
static int fetch(const char *auth, resp_t *r)
{
    esp_http_client_config_t cfg = {
        .url = FETCH_URL,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .event_handler = on_http,
        .user_data = r,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    network_proxy_config_t proxy;
    network_proxy_get_active(&proxy);
    bool use_proxy = proxy.type != NETWORK_PROXY_OFF;
    network_proxy_wipe(&proxy, sizeof(proxy));
    esp_transport_handle_t transport = use_proxy ? network_proxy_http_transport(true) : NULL;
    if (use_proxy && !transport) return 0;
#if CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
    cfg.transport = transport;
#else
    if (transport) {
        esp_transport_destroy(transport);
        ESP_LOGE(TAG, "proxy requires CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT=y");
        return 0;
    }
#endif
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        if (transport) esp_transport_destroy(transport);
        return 0;
    }
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "X-API-Version", "1.0.0");
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (transport) esp_transport_destroy(transport);
    /* perform() reports 401 as an error; the status still tells us what happened. */
    if (status == 401 || status == 403) {
        return status;
    }
    return err == ESP_OK ? status : 0;
}

static void copy_str(char *out, size_t cap, const cJSON *item)
{
    out[0] = '\0';
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(out, item->valuestring, cap);
    }
}

/* The id is also the first label of the VM's wss://<id>.<domain>/ URL. */
static void id_from_url(const char *url, char *out, size_t cap)
{
    const char *p = url ? strstr(url, "://") : NULL;
    const char *dot = p ? strchr(p + 3, '.') : NULL;
    out[0] = '\0';
    if (dot && (size_t)(dot - p - 3) < cap) {
        memcpy(out, p + 3, dot - p - 3);
        out[dot - p - 3] = '\0';
    }
}

int muse_hatch_api_find_vm(const char *device_token, const char *want_vm, muse_hatch_vm_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t auth_len = strlen(device_token) + 8;
    char *auth = malloc(auth_len);
    resp_t r = { .buf = heap_caps_malloc(MAX_RESPONSE, MALLOC_CAP_SPIRAM) };
    if (!auth || !r.buf) {
        free(auth);
        free(r.buf);
        return MUSE_HATCH_API_FAILED;
    }
    snprintf(auth, auth_len, "Bearer %s", device_token);

    int status = 0;
    for (int i = 0; i < ATTEMPTS; i++) {
        r.len = 0;
        status = fetch(auth, &r);
        if (status == 401 || status == 403 || (status >= 200 && status < 500)) {
            break;
        }
        ESP_LOGW(TAG, "fetch_vms attempt %d failed (HTTP %d)", i + 1, status);
        vTaskDelay(pdMS_TO_TICKS(500 << i));
    }
    free(auth);
    if (status == 401 || status == 403) {
        ESP_LOGW(TAG, "fetch_vms: token rejected (HTTP %d)", status);
        free(r.buf);
        return MUSE_HATCH_API_AUTH;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "fetch_vms failed (HTTP %d)", status);
        free(r.buf);
        return MUSE_HATCH_API_FAILED;
    }

    int rc = MUSE_HATCH_API_FAILED;
    cJSON *root = cJSON_ParseWithLength(r.buf, r.len);
    free(r.buf);
    cJSON *list = cJSON_GetObjectItem(root, "vm_list");
    const cJSON *pick = NULL, *entry;
    int count = 0;
    cJSON_ArrayForEach(entry, list) {
        const cJSON *url = cJSON_GetObjectItem(entry, "vm_ws_url");
        if (!cJSON_IsString(url)) {
            url = cJSON_GetObjectItem(entry, "vm_url");
        }
        if (!cJSON_IsString(cJSON_GetObjectItem(entry, "vm_auth_token"))) {
            continue;
        }
        char id[128];
        copy_str(id, sizeof(id), cJSON_GetObjectItem(entry, "vm_id"));
        if (!id[0]) {
            id_from_url(cJSON_IsString(url) ? url->valuestring : NULL, id, sizeof(id));
        }
        count++;
        ESP_LOGI(TAG, "VM %s (%s)%s", id, cJSON_GetStringValue(cJSON_GetObjectItem(entry, "vm_name")) ?: "",
                 cJSON_IsTrue(cJSON_GetObjectItem(entry, "default")) ? " default" : "");
        bool match = want_vm[0] ? strcmp(id, want_vm) == 0
                                : (!pick || cJSON_IsTrue(cJSON_GetObjectItem(entry, "default")));
        if (match && id[0]) {
            pick = entry;
            strlcpy(out->vm_id, id, sizeof(out->vm_id));
        }
    }
    if (pick) {
        copy_str(out->vm_name, sizeof(out->vm_name), cJSON_GetObjectItem(pick, "vm_name"));
        out->vm_token = strdup(cJSON_GetObjectItem(pick, "vm_auth_token")->valuestring);
        rc = out->vm_token ? 0 : MUSE_HATCH_API_FAILED;
    } else {
        ESP_LOGW(TAG, "fetch_vms: %d VMs, none %s%s", count, want_vm[0] ? "with id " : "usable", want_vm);
    }
    cJSON_Delete(root);
    return rc;
}
