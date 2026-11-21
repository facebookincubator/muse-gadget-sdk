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
#include "esp_tls.h"
typedef struct transport *esp_transport_handle_t;
typedef int (*connect_func)(esp_transport_handle_t, const char *, int, int);
typedef int (*io_read_func)(esp_transport_handle_t, char *, int, int);
typedef int (*io_func)(esp_transport_handle_t, const char *, int, int);
typedef int (*trans_func)(esp_transport_handle_t);
typedef int (*poll_func)(esp_transport_handle_t, int);
struct transport { void *data; connect_func connect; io_read_func read; io_func write; trans_func close, destroy; poll_func poll_read, poll_write; };
esp_transport_handle_t esp_transport_init(void);
int esp_transport_destroy(esp_transport_handle_t);
void *esp_transport_get_context_data(esp_transport_handle_t);
int esp_transport_set_context_data(esp_transport_handle_t, void *);
int esp_transport_set_func(esp_transport_handle_t, connect_func, io_read_func, io_func, trans_func, poll_func, poll_func, trans_func);
int esp_transport_set_default_port(esp_transport_handle_t, int);
