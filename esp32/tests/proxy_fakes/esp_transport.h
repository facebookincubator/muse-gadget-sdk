// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
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
