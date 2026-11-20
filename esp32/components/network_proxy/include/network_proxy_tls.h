/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include "network_proxy.h"
#include "esp_tls.h"
#include "esp_transport.h"

#ifdef __cplusplus
extern "C" {
#endif
// Memory-only snapshot: transport tasks never read NVS.
void network_proxy_set_active(const network_proxy_config_t *proxy);
void network_proxy_get_active(network_proxy_config_t *out);
// Same return/ownership contract as esp_tls_conn_new_sync. Blocking TLS only.
int network_proxy_tls_connect(const char *host, int hostlen, int port,
                               const esp_tls_cfg_t *cfg, esp_tls_t *tls);
// Caller destroys the transport after esp_http_client_cleanup (also on init
// failure). use_tls follows the HTTP client URL scheme.
esp_transport_handle_t network_proxy_http_transport(bool use_tls);
#ifdef __cplusplus
}
#endif
