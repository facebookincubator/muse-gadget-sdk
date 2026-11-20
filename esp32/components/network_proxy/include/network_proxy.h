/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NETWORK_PROXY_OFF,
    NETWORK_PROXY_HTTP,
    NETWORK_PROXY_SOCKS5,
    NETWORK_PROXY_INVALID,   // corrupt saved settings fail closed
} network_proxy_type_t;

#define NETWORK_PROXY_HOST_MAX 127
#define NETWORK_PROXY_AUTH_MAX 63
typedef struct network_proxy_config {
    network_proxy_type_t type;
    char host[NETWORK_PROXY_HOST_MAX + 1];
    uint16_t port;
    char username[NETWORK_PROXY_AUTH_MAX + 1];
    char password[NETWORK_PROXY_AUTH_MAX + 1];
} network_proxy_config_t;

bool network_proxy_valid(const network_proxy_config_t *proxy);
bool network_proxy_equal(const network_proxy_config_t *a, const network_proxy_config_t *b);
// Missing object means Off. Invalid input sets INVALID and returns false.
bool network_proxy_parse(const cJSON *object, network_proxy_config_t *out);
cJSON *network_proxy_json(const network_proxy_config_t *proxy);
void network_proxy_wipe(void *data, unsigned len);
void network_proxy_delete_json(cJSON *root);
// Local literals, localhost, single-label names and .local stay direct.
bool network_proxy_local_host(const char *host);
// Opens a TCP tunnel; owns and closes the socket on every failure. No fallback
// to direct egress when a configured proxy refuses or cannot be reached.
int network_proxy_open(const network_proxy_config_t *proxy, const char *host,
                       int port, int timeout_ms);

#ifdef __cplusplus
}
#endif
