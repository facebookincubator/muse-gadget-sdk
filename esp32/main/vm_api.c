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

#include "vm_api.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "link.vm_api";
static char s_api_base[256] = VM_API_DEFAULT_BASE_URL;
static char s_sdk_token[64];
#define FETCH_PATH "/fetch_vms"
#define MINT_TOKEN_PATH "/device_token/mint"
#define REFRESH_TOKEN_PATH "/device_token/refresh"

#define MAX_RESPONSE_BYTES (32 * 1024)
// Retry transient network/DNS failures. The first DNS query after WiFi gets
// an IP often loses to a race (DHCP hands DNS via DNS option, but the lwIP
// resolver may not be ready yet — symptom: getaddrinfo returns EAI_FAIL).
// Backoff doubles between the four attempts: 500ms, 1s, 2s.
#define MAX_FETCH_ATTEMPTS 4
#define INITIAL_BACKOFF_MS 500

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    const char *auth_header;
} resp_buf_t;

static bool http_origin(esp_http_client_handle_t client, char *out, size_t cap) {
    if (esp_http_client_get_url(client, out, (int)cap) != ESP_OK) return false;
    // get_url returns the resolved scheme, host and numeric port.
    char *scheme_end = strstr(out, "://");
    char *path = scheme_end ? strchr(scheme_end + 3, '/') : NULL;
    if (!path) return false;
    *path = '\0';
    return true;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    resp_buf_t *r = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_REDIRECT && r) {
        char before[384], after[384];
        bool have_origin = http_origin(evt->client, before, sizeof(before));
        // Remove first: set_redirection schedules the next request even if
        // this event callback subsequently returns an error.
        if (r->auth_header && *r->auth_header
            && esp_http_client_delete_header(evt->client, "Authorization") != ESP_OK) {
            return ESP_FAIL;
        }
        if (esp_http_client_set_redirection(evt->client) != ESP_OK) return ESP_FAIL;
        if (!have_origin || !http_origin(evt->client, after, sizeof(after))
            || strcasecmp(before, after) != 0) {
            r->auth_header = NULL;
        }
        if (r->auth_header && *r->auth_header) {
            return esp_http_client_set_header(evt->client, "Authorization", r->auth_header);
        }
        return ESP_OK;
    }
    if (evt->event_id != HTTP_EVENT_ON_DATA || !r) return ESP_OK;
    if (r->len + evt->data_len > MAX_RESPONSE_BYTES) {
        ESP_LOGW(TAG, "response too large, dropping tail");
        return ESP_OK;
    }
    if (r->len + evt->data_len > r->cap) {
        size_t new_cap = r->cap ? r->cap * 2 : 1024;
        while (new_cap < r->len + evt->data_len) new_cap *= 2;
        if (new_cap > MAX_RESPONSE_BYTES) new_cap = MAX_RESPONSE_BYTES;
        char *nb = realloc(r->buf, new_cap);
        if (!nb) return ESP_FAIL;
        r->buf = nb;
        r->cap = new_cap;
    }
    memcpy(r->buf + r->len, evt->data, evt->data_len);
    r->len += evt->data_len;
    return ESP_OK;
}

void vm_api_set_base_url(const char *url) {
    snprintf(s_api_base, sizeof(s_api_base), "%s",
             url && *url ? url : VM_API_DEFAULT_BASE_URL);
#if CONFIG_HOMEHUB_RELAY_PORT
    // The relay host hijacks/handles inbound 443 elsewhere; talk to it on the
    // relay port. The SNI/cert hostname stays the bare domain, so append the
    // port only when the URL has none of its own.
    const char *p = strstr(s_api_base, "://");
    const char *slash = p ? strchr(p + 3, '/') : NULL;
    const char *colon = p ? strchr(p + 3, ':') : NULL;
    if (p && (!colon || (slash && colon > slash))) {
        size_t n = strlen(s_api_base);
        snprintf(s_api_base + n, sizeof(s_api_base) - n, ":%d",
                 CONFIG_HOMEHUB_RELAY_PORT);
    }
#endif
}

void vm_api_set_sdk_token(const char *sdk_token) {
    if (sdk_token && strlen(sdk_token) >= sizeof(s_sdk_token)) {
        ESP_LOGE(TAG, "SDK token too long; sending none");
        sdk_token = NULL;
    }
    snprintf(s_sdk_token, sizeof(s_sdk_token), "%s", sdk_token ? sdk_token : "");
}

static void make_api_url(char *out, size_t out_cap, const char *path) {
    snprintf(out, out_cap, "%s%s", s_api_base, path);
}

static char *str_dup(const cJSON *item) {
    if (!cJSON_IsString(item) || !item->valuestring) return strdup("");
    return strdup(item->valuestring);
}

static int http_json(const char *url, esp_http_client_method_t method,
                     const char *auth_header, const char *body,
                     resp_buf_t *resp, int *status_out) {
    if (!url || !resp || !status_out) return VM_API_ERR_FAILED;
    *resp = (resp_buf_t){ .auth_header = auth_header };
    *status_out = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .event_handler = http_event_handler,
        .user_data = resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return VM_API_ERR_FAILED;

    if (auth_header && *auth_header) {
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    esp_http_client_set_header(client, "X-API-Version", "1.0.0");
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    *status_out = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    // Check HTTP status first — esp_http_client_perform returns
    // ESP_ERR_NOT_SUPPORTED on 401 responses (authentication challenge),
    // but we handle auth ourselves via bearer tokens.
    if (*status_out == 401 || *status_out == 403) {
        ESP_LOGW(TAG, "HTTP %d auth failure", *status_out);
        return VM_API_ERR_AUTH;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        *status_out = 0;
        return VM_API_ERR_FAILED;
    }
    if (*status_out < 200 || *status_out >= 300) {
        ESP_LOGW(TAG, "HTTP %d", *status_out);
        return VM_API_ERR_FAILED;
    }
    return 0;
}

static bool http_status_retryable(int status) {
    return status <= 0 || status == 408 || status == 429 ||
           (status >= 500 && status < 600);
}

static int http_json_with_retries(const char *url, esp_http_client_method_t method,
                                  const char *auth_header, const char *body,
                                  resp_buf_t *resp, int *status_out) {
    int backoff_ms = INITIAL_BACKOFF_MS;
    int status = 0;
    if (status_out) *status_out = 0;

    for (int attempt = 1; attempt <= MAX_FETCH_ATTEMPTS; attempt++) {
        free(resp->buf);
        resp->buf = NULL;
        resp->len = 0;
        resp->cap = 0;

        int rc = http_json(url, method, auth_header, body, resp, &status);
        if (status_out) *status_out = status;
        if (rc == 0) {
            ESP_LOGI(TAG, "HTTP %d, %d bytes (attempt %d)",
                     status, (int)resp->len, attempt);
            return 0;
        }
        if (rc == VM_API_ERR_AUTH) {
            free(resp->buf);
            resp->buf = NULL;
            return VM_API_ERR_AUTH;
        }
        if (!http_status_retryable(status)) {
            ESP_LOGW(TAG, "HTTP %d is not retryable", status);
            free(resp->buf);
            resp->buf = NULL;
            return VM_API_ERR_FAILED;
        }
        if (attempt < MAX_FETCH_ATTEMPTS) {
            ESP_LOGW(TAG, "attempt %d failed (status=%d), retry in %d ms",
                     attempt, status, backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms *= 2;
        } else {
            ESP_LOGE(TAG, "all %d attempts failed (last status=%d)",
                     MAX_FETCH_ATTEMPTS, status);
            free(resp->buf);
            resp->buf = NULL;
            return VM_API_ERR_FAILED;
        }
    }
    return VM_API_ERR_FAILED;
}

static char *make_device_body(const char *device_id) {
    if (!device_id || !*device_id) return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    if (!cJSON_AddStringToObject(root, "device_id", device_id)
        || (s_sdk_token[0] && !cJSON_AddStringToObject(root, "sdk_token", s_sdk_token))) {
        cJSON_Delete(root);
        return NULL;
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static int make_bearer(const char *token, char *out, size_t out_cap) {
    if (!token || !*token || !out || out_cap == 0) return VM_API_ERR_FAILED;
    int n = snprintf(out, out_cap, "Bearer %s", token);
    if (n < 0 || n >= (int)out_cap) {
        ESP_LOGE(TAG, "auth token too long");
        return VM_API_ERR_FAILED;
    }
    return 0;
}

static int make_refresh_bearer(const char *refresh_token, char *out, size_t out_cap) {
    if (!refresh_token || !*refresh_token || !out || out_cap == 0) {
        return VM_API_ERR_FAILED;
    }
    const char *raw = strrchr(refresh_token, ':');
    raw = raw ? raw + 1 : refresh_token;
    if (!*raw) return VM_API_ERR_FAILED;
    int n = snprintf(out, out_cap, "Bearer hatch_refresh:%s", raw);
    if (n < 0 || n >= (int)out_cap) {
        ESP_LOGE(TAG, "refresh token too long");
        return VM_API_ERR_FAILED;
    }
    return 0;
}

int vm_api_fetch_vms(const char *access_token, vm_info_t *out, int max) {
    if (!access_token || !out || max <= 0) return VM_API_ERR_FAILED;

    char auth_header[2048];
    if (make_bearer(access_token, auth_header, sizeof(auth_header)) != 0) {
        return VM_API_ERR_FAILED;
    }

    char url[320];
    make_api_url(url, sizeof(url), FETCH_PATH);

    resp_buf_t resp = {0};
    int rc = http_json_with_retries(url, HTTP_METHOD_GET, auth_header, NULL,
                                    &resp, NULL);
    if (rc != 0) return rc;

    cJSON *root = cJSON_ParseWithLength(resp.buf, resp.len);
    free(resp.buf);
    if (!cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "response not an object");
        cJSON_Delete(root);
        return VM_API_ERR_FAILED;
    }

    cJSON *err_title = cJSON_GetObjectItem(root, "error_title");
    if (cJSON_IsString(err_title)) {
        cJSON *err_summary = cJSON_GetObjectItem(root, "error_summary");
        ESP_LOGE(TAG, "fetch_vms error: %s — %s",
                 cJSON_IsString(err_title) ? err_title->valuestring : "?",
                 cJSON_IsString(err_summary) ? err_summary->valuestring : "?");
        cJSON_Delete(root);
        return VM_API_ERR_FAILED;
    }
    cJSON *backend_err = cJSON_GetObjectItem(root, "backend_error_code");
    if (cJSON_IsString(backend_err)) {
        ESP_LOGE(TAG, "fetch_vms backend error: %s",
                 cJSON_IsString(backend_err) ? backend_err->valuestring : "?");
        cJSON_Delete(root);
        return VM_API_ERR_FAILED;
    }

    cJSON *vm_list = cJSON_GetObjectItem(root, "vm_list");
    if (!cJSON_IsArray(vm_list)) {
        ESP_LOGE(TAG, "fetch_vms: missing or invalid vm_list");
        cJSON_Delete(root);
        return VM_API_ERR_FAILED;
    }

    int count = 0;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, vm_list) {
        if (count >= max) break;
        cJSON *url_item = cJSON_GetObjectItem(entry, "vm_ws_url");
        if (!cJSON_IsString(url_item)) {
            url_item = cJSON_GetObjectItem(entry, "vm_url");
        }
        cJSON *tok_item = cJSON_GetObjectItem(entry, "vm_auth_token");
        if (!cJSON_IsString(url_item) || !cJSON_IsString(tok_item)
            || !url_item->valuestring || !tok_item->valuestring) {
            continue;
        }
        char *vm_url = strdup(url_item->valuestring);
        char *vm_auth_token = strdup(tok_item->valuestring);
        char *vm_name = str_dup(cJSON_GetObjectItem(entry, "vm_name"));
        cJSON *id_item = cJSON_GetObjectItem(entry, "vm_id");
        char *vm_id = (cJSON_IsString(id_item) && id_item->valuestring
                       && *id_item->valuestring)
                      ? strdup(id_item->valuestring) : NULL;
        if (!vm_url || !vm_auth_token || !vm_name) {
            free(vm_url);
            free(vm_auth_token);
            free(vm_name);
            free(vm_id);
            vm_list_free(out, count);
            cJSON_Delete(root);
            return VM_API_ERR_FAILED;
        }
        out[count].vm_url = vm_url;
        out[count].vm_auth_token = vm_auth_token;
        out[count].vm_name = vm_name;
        out[count].vm_id = vm_id;
        cJSON *def = cJSON_GetObjectItem(entry, "default");
        out[count].is_default = cJSON_IsTrue(def);
        count++;
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "%d VMs found", count);
    return count;
}

static int parse_token_pair(resp_buf_t *resp, vm_device_tokens_t *out) {
    if (!resp || !out) return VM_API_ERR_FAILED;
    cJSON *root = cJSON_ParseWithLength(resp->buf, resp->len);
    free(resp->buf);
    resp->buf = NULL;
    if (!cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "token response not an object");
        cJSON_Delete(root);
        return VM_API_ERR_FAILED;
    }
    cJSON *access = cJSON_GetObjectItem(root, "access_token");
    cJSON *refresh = cJSON_GetObjectItem(root, "refresh_token");
    if (!cJSON_IsString(access) || !access->valuestring || !*access->valuestring
        || !cJSON_IsString(refresh) || !refresh->valuestring || !*refresh->valuestring) {
        ESP_LOGE(TAG, "token response missing token pair");
        cJSON_Delete(root);
        return VM_API_ERR_FAILED;
    }
    out->access_token = strdup(access->valuestring);
    out->refresh_token = strdup(refresh->valuestring);
    cJSON_Delete(root);
    if (!out->access_token || !out->refresh_token) {
        vm_device_tokens_free(out);
        return VM_API_ERR_FAILED;
    }
    return 0;
}

int vm_api_mint_device_token(const char *access_token, const char *device_id,
                             vm_device_tokens_t *out) {
    if (!access_token || !*access_token || !device_id || !*device_id || !out) {
        return VM_API_ERR_FAILED;
    }
    *out = (vm_device_tokens_t){0};

    char auth_header[2048];
    if (make_bearer(access_token, auth_header, sizeof(auth_header)) != 0) {
        return VM_API_ERR_FAILED;
    }
    char *body = make_device_body(device_id);
    if (!body) {
        ESP_LOGE(TAG, "device_id body allocation failed");
        return VM_API_ERR_FAILED;
    }
    char url[320];
    make_api_url(url, sizeof(url), MINT_TOKEN_PATH);

    resp_buf_t resp = {0};
    int rc = http_json_with_retries(url, HTTP_METHOD_POST, auth_header, body,
                                    &resp, NULL);
    cJSON_free(body);
    if (rc != 0) return rc;
    rc = parse_token_pair(&resp, out);
    if (rc == 0) ESP_LOGI(TAG, "device token minted");
    return rc;
}

static int do_refresh(const char *auth_header, const char *device_id,
                      vm_device_tokens_t *out, int *status_out) {
    char *body = make_device_body(device_id);
    if (!body) {
        ESP_LOGE(TAG, "device_id body allocation failed");
        return VM_API_ERR_FAILED;
    }
    char url[320];
    make_api_url(url, sizeof(url), REFRESH_TOKEN_PATH);
    if (s_sdk_token[0]) ESP_LOGI(TAG, "refresh carries SDK token %.12s", s_sdk_token);

    resp_buf_t resp = {0};
    int rc = http_json_with_retries(url, HTTP_METHOD_POST, auth_header, body,
                                    &resp, status_out);
    cJSON_free(body);
    if (rc != 0) return rc;
    return parse_token_pair(&resp, out);
}

int vm_api_refresh_device_token(const char *access_token,
                                const char *refresh_token,
                                const char *device_id,
                                vm_device_tokens_t *out,
                                vm_token_refresh_method_t *method) {
    if (!device_id || !*device_id || !out) return VM_API_ERR_FAILED;
    bool have_access = access_token && *access_token;
    bool have_refresh = refresh_token && *refresh_token;
    if (!have_access && !have_refresh) return VM_API_ERR_FAILED;
    *out = (vm_device_tokens_t){0};
    if (method) *method = VM_TOKEN_REFRESH_NONE;

    char auth_header[2048];
    int status = 0;
    // Treated as "the access leg already failed" when the caller skips it, so
    // the fall-through to the refresh leg below is the same code path either
    // way.
    int rc = VM_API_ERR_AUTH;
    bool access_leg_401 = true;

    if (have_access) {
        if (make_bearer(access_token, auth_header, sizeof(auth_header)) != 0) {
            return VM_API_ERR_FAILED;
        }
        rc = do_refresh(auth_header, device_id, out, &status);
        if (rc == 0) {
            if (method) *method = VM_TOKEN_REFRESH_ACCESS_TOKEN;
            ESP_LOGI(TAG, "device token refreshed via access token");
            return 0;
        }
        access_leg_401 = (rc == VM_API_ERR_AUTH && status == 401);
    } else {
        ESP_LOGI(TAG, "skipping access-token refresh leg (caller says it was "
                      "already rejected)");
    }

    if (!access_leg_401 || !have_refresh) {
        return rc;
    }

    ESP_LOGI(TAG, "access token rejected, trying refresh token");
    if (make_refresh_bearer(refresh_token, auth_header, sizeof(auth_header)) != 0) {
        return VM_API_ERR_FAILED;
    }
    rc = do_refresh(auth_header, device_id, out, &status);
    if (rc == 0) {
        if (method) *method = VM_TOKEN_REFRESH_REFRESH_TOKEN;
        ESP_LOGI(TAG, "device token refreshed via refresh token");
    }
    return rc;
}

void vm_device_tokens_free(vm_device_tokens_t *tokens) {
    if (!tokens) return;
    free(tokens->access_token);
    free(tokens->refresh_token);
    tokens->access_token = NULL;
    tokens->refresh_token = NULL;
}

void vm_list_free(vm_info_t *vms, int count) {
    if (!vms) return;
    for (int i = 0; i < count; i++) {
        free(vms[i].vm_url);
        free(vms[i].vm_auth_token);
        free(vms[i].vm_name);
        free(vms[i].vm_id);
        vms[i].vm_url = vms[i].vm_auth_token = vms[i].vm_name = NULL;
        vms[i].vm_id = NULL;
    }
}

const vm_info_t *vm_find_default(const vm_info_t *vms, int count) {
    for (int i = 0; i < count; i++) if (vms[i].is_default) return &vms[i];
    return count > 0 ? &vms[0] : NULL;
}

const vm_info_t *vm_find_by_url(const vm_info_t *vms, int count, const char *url) {
    if (!url) return NULL;
    for (int i = 0; i < count; i++) {
        if (vms[i].vm_url && strcmp(vms[i].vm_url, url) == 0) return &vms[i];
    }
    return NULL;
}
