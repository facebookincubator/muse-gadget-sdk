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
#include "network_proxy_tls.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static network_proxy_config_t s_active;

void network_proxy_set_active(const network_proxy_config_t *p) {
    portENTER_CRITICAL(&s_lock);
    s_active = *p;
    portEXIT_CRITICAL(&s_lock);
}

void network_proxy_get_active(network_proxy_config_t *out) {
    portENTER_CRITICAL(&s_lock);
    *out = s_active;
    portEXIT_CRITICAL(&s_lock);
}

int network_proxy_tls_connect(const char *host, int hostlen, int port,
                               const esp_tls_cfg_t *cfg, esp_tls_t *tls) {
    if (!cfg || !tls || !host || hostlen <= 0 || hostlen > 255) return -1;
    network_proxy_config_t p;
    network_proxy_get_active(&p);
    char target[256];
    memcpy(target, host, hostlen);
    target[hostlen] = 0;
    if (p.type == NETWORK_PROXY_OFF || network_proxy_local_host(target)) {
        network_proxy_wipe(&p, sizeof(p));
        return esp_tls_conn_new_sync(host, hostlen, port, cfg, tls);
    }
    if (cfg->non_block || cfg->is_plain_tcp || cfg->timeout_ms <= 0) {
        network_proxy_wipe(&p, sizeof(p));
        return -1;
    }
    int64_t started = esp_timer_get_time();
    int fd = network_proxy_open(&p, target, port, cfg->timeout_ms);
    network_proxy_wipe(&p, sizeof(p));
    if (fd < 0) return -1;
    if (esp_tls_set_conn_sockfd(tls, fd) != ESP_OK) { close(fd); return -1; }
    // ESP-IDF v6: CONNECTING creates the SSL handle on the adopted socket.
    // The original host/cfg still provide SNI and certificate verification.
    if (esp_tls_set_conn_state(tls, ESP_TLS_CONNECTING) != ESP_OK) return -1;
    esp_tls_cfg_t remaining = *cfg;
    int elapsed = (esp_timer_get_time() - started) / 1000;
    if (elapsed >= cfg->timeout_ms) return -1;
    remaining.timeout_ms -= elapsed;
    return esp_tls_conn_new_sync(host, hostlen, port, &remaining, tls);
}

typedef struct { esp_tls_t *tls; int fd; bool use_tls; } proxy_transport_t;

static int transport_close(esp_transport_handle_t t) {
    proxy_transport_t *ctx = esp_transport_get_context_data(t);
    if (ctx->tls) esp_tls_conn_destroy(ctx->tls);
    ctx->tls = NULL;
    if (ctx->fd >= 0) close(ctx->fd);
    ctx->fd = -1;
    return 0;
}

static int transport_connect(esp_transport_handle_t t, const char *host, int port, int ms) {
    proxy_transport_t *ctx = esp_transport_get_context_data(t);
    transport_close(t);
    if (!ctx->use_tls) {
        network_proxy_config_t proxy;
        network_proxy_get_active(&proxy);
        ctx->fd = network_proxy_open(&proxy, host, port, ms);
        network_proxy_wipe(&proxy, sizeof(proxy));
        return ctx->fd >= 0 ? 0 : -1;
    }
    ctx->tls = esp_tls_init();
    if (!ctx->tls) return -1;
    esp_tls_cfg_t cfg = {.crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = ms};
    if (network_proxy_tls_connect(host, strlen(host), port, &cfg, ctx->tls) == 1) return 0;
    transport_close(t);
    return -1;
}

static int transport_socket(esp_transport_handle_t t) {
    proxy_transport_t *ctx = esp_transport_get_context_data(t);
    int fd = ctx->fd;
    if (ctx->tls) esp_tls_get_conn_sockfd(ctx->tls, &fd);
    return fd;
}

static int transport_poll(esp_transport_handle_t t, int ms, bool write) {
    proxy_transport_t *ctx = esp_transport_get_context_data(t);
    if (!ctx->tls && ctx->fd < 0) return -1;
    if (!write && ctx->tls && esp_tls_get_bytes_avail(ctx->tls) > 0) return 1;
    int fd = transport_socket(t);
    if (fd < 0) return -1;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    return select(fd + 1, write ? NULL : &set, write ? &set : NULL, NULL, ms < 0 ? NULL : &tv);
}

static int transport_poll_read(esp_transport_handle_t t, int ms) { return transport_poll(t, ms, false); }
static int transport_poll_write(esp_transport_handle_t t, int ms) { return transport_poll(t, ms, true); }

static int transport_read(esp_transport_handle_t t, char *buf, int len, int ms) {
    int r = transport_poll_read(t, ms);
    if (r <= 0) return r;
    proxy_transport_t *ctx = esp_transport_get_context_data(t);
    r = ctx->tls ? esp_tls_conn_read(ctx->tls, buf, len) : recv(ctx->fd, buf, len, 0);
    if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) return 0;
    return r == 0 ? -1 : r;
}

static int transport_write(esp_transport_handle_t t, const char *buf, int len, int ms) {
    int r = transport_poll_write(t, ms);
    if (r <= 0) return r;
    proxy_transport_t *ctx = esp_transport_get_context_data(t);
    r = ctx->tls ? esp_tls_conn_write(ctx->tls, buf, len) : send(ctx->fd, buf, len, 0);
    if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) return 0;
    return r;
}

static int transport_destroy(esp_transport_handle_t t) {
    transport_close(t);
    free(esp_transport_get_context_data(t));
    return 0;
}

esp_transport_handle_t network_proxy_http_transport(bool use_tls) {
    esp_transport_handle_t t = esp_transport_init();
    proxy_transport_t *ctx = calloc(1, sizeof(*ctx));
    if (!t || !ctx) { if (t) esp_transport_destroy(t); free(ctx); return NULL; }
    ctx->fd = -1;
    ctx->use_tls = use_tls;
    esp_transport_set_context_data(t, ctx);
    esp_transport_set_func(t, transport_connect, transport_read, transport_write,
                            transport_close, transport_poll_read, transport_poll_write,
                            transport_destroy);
    esp_transport_set_default_port(t, use_tls ? 443 : 80);
    return t;
}
