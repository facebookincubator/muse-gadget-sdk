// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
// Host ESP-TLS substitute: real TLS with OpenSSL, on the socket adopted by
// production network_proxy_tls_connect(). Everything stays on loopback.
#include "network_proxy_tls.h"
#include "noise_upgrade.h"
#include <arpa/inet.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int64_t esp_timer_get_time(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
int esp_crt_bundle_attach(void *ctx) {
    return SSL_CTX_load_verify_locations(ctx, getenv("PROXY_TEST_CA"), NULL) == 1 ? 0 : -1;
}
esp_tls_t *esp_tls_init(void) {
    esp_tls_t *t = calloc(1, sizeof(*t));
    if (t) t->fd = -1;
    return t;
}
int esp_tls_set_conn_sockfd(esp_tls_t *t, int fd) { t->fd = fd; return 0; }
int esp_tls_set_conn_state(esp_tls_t *t, esp_tls_conn_state_t s) { t->state = s; return 0; }
int esp_tls_get_conn_sockfd(esp_tls_t *t, int *fd) { *fd = t->fd; return 0; }
void esp_tls_conn_destroy(esp_tls_t *t) {
    if (!t) return;
    SSL_free(t->ssl);
    SSL_CTX_free(t->ctx);
    if (t->fd >= 0) close(t->fd);
    free(t);
}
int esp_tls_conn_new_sync(const char *host, int hostlen, int port,
                          const esp_tls_cfg_t *cfg, esp_tls_t *t) {
    assert(hostlen >= 0 && (size_t)hostlen == strlen(host));
    assert(!cfg->is_plain_tcp && !cfg->non_block && cfg->crt_bundle_attach);
    if (t->state == ESP_TLS_INIT) {
        // A direct path in this harness may reach only the test-owned server.
        // cloud.invalid must be resolved by the proxy, never by this client.
        if (strcmp(host, "127.0.0.1") && strcmp(host, "localhost")) return -1;
        network_proxy_config_t off = {0};
        t->fd = network_proxy_open(&off, host, port, cfg->timeout_ms);
        if (t->fd < 0) return -1;
    } else assert(t->state == ESP_TLS_CONNECTING && t->fd >= 0);
    t->ctx = SSL_CTX_new(TLS_client_method());
    if (!t->ctx || cfg->crt_bundle_attach(t->ctx)) return -1;
    SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER, NULL);
    t->ssl = SSL_new(t->ctx);
    if (!t->ssl || !SSL_set_fd(t->ssl, t->fd)) return -1;
    struct in_addr v4;
    if (inet_pton(AF_INET, host, &v4) == 1) {
        if (!X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(t->ssl), host)) return -1;
    } else {
        if (!X509_VERIFY_PARAM_set1_host(SSL_get0_param(t->ssl), host, 0) || !SSL_set_tlsext_host_name(t->ssl, host)) return -1;
    }
    if (SSL_connect(t->ssl) != 1) return -1;
    t->state = ESP_TLS_DONE;
    return 1;
}
ssize_t esp_tls_conn_read(esp_tls_t *t, void *b, size_t n) { return SSL_read(t->ssl, b, n); }
ssize_t esp_tls_conn_write(esp_tls_t *t, const void *b, size_t n) { return SSL_write(t->ssl, b, n); }
int esp_tls_get_bytes_avail(esp_tls_t *t) { return SSL_pending(t->ssl); }

esp_transport_handle_t esp_transport_init(void) { return calloc(1, sizeof(struct transport)); }
int esp_transport_destroy(esp_transport_handle_t t) {
    if (t->destroy) t->destroy(t);
    free(t);
    return 0;
}
void *esp_transport_get_context_data(esp_transport_handle_t t) { return t->data; }
int esp_transport_set_context_data(esp_transport_handle_t t, void *d) { t->data = d; return 0; }
int esp_transport_set_default_port(esp_transport_handle_t t, int p) { (void)t; (void)p; return 0; }
int esp_transport_set_func(esp_transport_handle_t t, connect_func c, io_read_func r, io_func w,
                           trans_func close, poll_func pr, poll_func pw, trans_func d) {
    t->connect = c; t->read = r; t->write = w; t->close = close;
    t->poll_read = pr; t->poll_write = pw; t->destroy = d;
    return 0;
}

int main(int argc, char **argv) {
    assert(argc == 6);
    signal(SIGPIPE, SIG_IGN);
    cJSON *json = cJSON_Parse(argv[1]);
    network_proxy_config_t config;
    bool valid = network_proxy_parse(json, &config);
    network_proxy_delete_json(json);
    if (!valid) return 2;
    network_proxy_set_active(&config);
    network_proxy_wipe(&config, sizeof(config));
    const char *host = argv[2], *mode = argv[5];
    int port = atoi(argv[3]), ms = atoi(argv[4]);
    if (!strcmp(mode, "local")) return network_proxy_local_host(host) ? 0 : 3;
    char request[512], response[64] = {0};
    int n = noise_upgrade_build_request("/v1/noise", "test-vm", host, "fake-device-token", request, sizeof(request));
    assert(n > 0);
    esp_tls_t *tls = NULL;
    esp_transport_handle_t http = NULL;
    int fd = -1, result = 1;
    if (!strcmp(mode, "tls")) {
        tls = esp_tls_init();
        esp_tls_cfg_t cfg = {.crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = ms};
        if (network_proxy_tls_connect(host, strlen(host), port, &cfg, tls) != 1) goto done;
        if (esp_tls_conn_write(tls, request, n) != n) goto done;
        n = esp_tls_conn_read(tls, response, sizeof(response));
    } else if ((!strcmp(mode, "http") || !strcmp(mode, "http_plain"))) {
        http = network_proxy_http_transport(!strcmp(mode, "http"));
        if (!http || http->connect(http, host, port, ms)) goto done;
        if (http->write(http, request, n, ms) != n) goto done;
        n = http->read(http, response, sizeof(response), ms);
    } else {
        network_proxy_get_active(&config);
        fd = network_proxy_open(&config, host, port, ms);
        network_proxy_wipe(&config, sizeof(config));
        if (fd < 0) goto done;
        if (send(fd, request, n, 0) != n) goto done;
        n = recv(fd, response, sizeof(response), 0);
    }
    result = noise_upgrade_classify(response, n > 0 ? n : 0, false) == NOISE_UPGRADE_OK ? 0 : 1;
done:
    if (fd >= 0) close(fd);
    if (tls) esp_tls_conn_destroy(tls);
    if (http) esp_transport_destroy(http);
    return result;
}
