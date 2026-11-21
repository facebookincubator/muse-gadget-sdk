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
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <openssl/ssl.h>
#define ESP_OK 0
#define ESP_TLS_ERR_SSL_WANT_READ (-6900)
#define ESP_TLS_ERR_SSL_WANT_WRITE (-6901)
typedef int esp_err_t;
typedef enum { ESP_TLS_INIT, ESP_TLS_CONNECTING, ESP_TLS_HANDSHAKE, ESP_TLS_DONE } esp_tls_conn_state_t;
typedef struct { int fd; esp_tls_conn_state_t state; SSL_CTX *ctx; SSL *ssl; } esp_tls_t;
typedef struct { int (*crt_bundle_attach)(void *); int timeout_ms; bool non_block; bool is_plain_tcp; } esp_tls_cfg_t;
esp_tls_t *esp_tls_init(void);
int esp_tls_conn_new_sync(const char *, int, int, const esp_tls_cfg_t *, esp_tls_t *);
int esp_tls_set_conn_sockfd(esp_tls_t *, int);
int esp_tls_set_conn_state(esp_tls_t *, esp_tls_conn_state_t);
int esp_tls_get_conn_sockfd(esp_tls_t *, int *);
void esp_tls_conn_destroy(esp_tls_t *);
ssize_t esp_tls_conn_read(esp_tls_t *, void *, size_t);
ssize_t esp_tls_conn_write(esp_tls_t *, const void *, size_t);
int esp_tls_get_bytes_avail(esp_tls_t *);
