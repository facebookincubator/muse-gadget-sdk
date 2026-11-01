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

#pragma once

// The subset of esp_http_client that lan_fetch.c uses: streaming reads, the
// response's headers as events, and answering a 401 with a login.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_HTTP_CONNECT 0x7003

struct fake_esp_http_client;
typedef struct fake_esp_http_client *esp_http_client_handle_t;

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_HEADER = 3,
    HTTP_EVENT_ON_DATA = 4,
} esp_http_client_event_id_t;

typedef struct {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void *data;
    int data_len;
    void *user_data;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);

typedef struct {
    const char *url;
    void *crt_bundle_attach;
    int timeout_ms;
    int buffer_size;
    int buffer_size_tx;
    bool disable_auto_redirect;
    int max_authorization_retries;
    const char *user_agent;
    http_event_handle_cb event_handler;
    void *user_data;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_username(esp_http_client_handle_t client, const char *username);
esp_err_t esp_http_client_set_password(esp_http_client_handle_t client, const char *password);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_add_auth(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

const char *esp_err_to_name(esp_err_t err);
