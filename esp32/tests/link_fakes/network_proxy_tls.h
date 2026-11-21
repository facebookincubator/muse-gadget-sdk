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
#include "network_proxy.h"
#include "esp_tls.h"
#include <string.h>
typedef void *esp_transport_handle_t;
static inline void network_proxy_get_active(network_proxy_config_t *p) { memset(p, 0, sizeof(*p)); }
static inline esp_transport_handle_t network_proxy_http_transport(bool tls) { (void)tls; return NULL; }
static inline int esp_transport_destroy(esp_transport_handle_t t) { (void)t; return 0; }
static inline int network_proxy_tls_connect(const char *h, int n, int port, const esp_tls_cfg_t *c, esp_tls_t *t) {
    return esp_tls_conn_new_sync(h, n, port, c, t);
}
static inline void network_proxy_wipe_fake(void *p, unsigned n) { memset(p, 0, n); }
#define network_proxy_wipe network_proxy_wipe_fake
