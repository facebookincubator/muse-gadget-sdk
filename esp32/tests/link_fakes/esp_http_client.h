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

#include <stddef.h>
#include <stdbool.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1

typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST = 1,
} esp_http_client_method_t;

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_HEADERS_SENT = 2,
    HTTP_EVENT_ON_HEADER = 3,
    HTTP_EVENT_ON_DATA = 4,
    HTTP_EVENT_REDIRECT = 7,
} esp_http_client_event_id_t;

struct fake_esp_http_client;
typedef struct fake_esp_http_client *esp_http_client_handle_t;

typedef struct {
    esp_http_client_event_id_t event_id;
    void *user_data;
    void *data;
    int data_len;
    const char *header_key;
    const char *header_value;
    esp_http_client_handle_t client;
} esp_http_client_event_t;

typedef struct {
    int status_code;
} esp_http_client_redirect_event_data_t;

typedef esp_err_t (*esp_http_client_event_cb_t)(esp_http_client_event_t *evt);

typedef struct {
    const char *url;
    const char *user_agent;
    esp_http_client_method_t method;
    int timeout_ms;
    int buffer_size;
    int buffer_size_tx;
    esp_http_client_event_cb_t event_handler;
    void *user_data;
    void *crt_bundle_attach;
    bool disable_auto_redirect;
    bool keep_alive_enable;
    int max_redirection_count;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                     const char *key, const char *value);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client,
                                         const char *data, int len);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
esp_err_t esp_http_client_get_url(esp_http_client_handle_t client, char *url, int len);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char *key);
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t client);

const char *esp_err_to_name(esp_err_t err);
