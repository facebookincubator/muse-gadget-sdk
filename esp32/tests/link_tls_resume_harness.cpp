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

// The VM WebSocket's TLS session resumption (the tls_resume_* helpers in
// main/noise_control.cpp, sliced in by test_link_tls_resume.py): a session is
// kept after a handshake, offered only to its host, kept across a connect that
// failed before TLS began, and forgotten once TLS itself failed.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define ESP_OK 0
#define MALLOC_CAP_INTERNAL 0
typedef int esp_err_t;
typedef enum { ESP_TLS_INIT = 0, ESP_TLS_CONNECTING, ESP_TLS_HANDSHAKE, ESP_TLS_FAIL, ESP_TLS_DONE } esp_tls_conn_state_t;
struct esp_tls_client_session_t { int id; };
struct esp_tls_t { esp_tls_conn_state_t conn_state; int session_id; };
struct esp_tls_cfg_t { esp_tls_client_session_t *client_session; };

static const char *TAG = "link.noise";
static int s_live;   // sessions made and not yet freed

static void log_line(const char *, const char *, ...) {}
#define ESP_LOGI(tag, ...) log_line(tag, __VA_ARGS__)
static size_t heap_caps_get_free_size(int) { return 0; }
static esp_tls_client_session_t *esp_tls_get_client_session(esp_tls_t *tls) {
    auto *s = static_cast<esp_tls_client_session_t *>(malloc(sizeof(esp_tls_client_session_t)));
    assert(s);
    s->id = tls->session_id;
    s_live++;
    return s;
}
static void esp_tls_free_client_session(esp_tls_client_session_t *s) {
    assert(s && s_live > 0);
    s_live--;
    free(s);
}
static esp_err_t esp_tls_get_conn_state(esp_tls_t *tls, esp_tls_conn_state_t *state) {
    *state = tls->conn_state;
    return ESP_OK;
}

#include "tls_resume.inc"

static int offered(const char *host) {
    esp_tls_cfg_t cfg = {};
    tls_resume_offer(&s_tls_resume, host, &cfg);
    return cfg.client_session ? cfg.client_session->id : 0;
}

int main() {
    esp_tls_t made = {ESP_TLS_DONE, 1};
    assert(!offered("vm.example"));
    tls_resume_keep(&s_tls_resume, "vm.example", &made);
    assert(offered("vm.example") == 1 && s_live == 1);

    // DNS or the TCP connect failed (Wi-Fi down): esp-tls never left
    // ESP_TLS_INIT, so the session stays for the next try.
    esp_tls_t unreachable = {ESP_TLS_INIT, 0};
    tls_resume_failed(&s_tls_resume, &unreachable);
    tls_resume_failed(&s_tls_resume, &unreachable);
    assert(offered("vm.example") == 1 && s_live == 1);

    // Any failure once TLS began forgets it, a refused session included.
    const esp_tls_conn_state_t in_tls[] = {ESP_TLS_CONNECTING, ESP_TLS_HANDSHAKE, ESP_TLS_FAIL};
    int id = 2;
    for (esp_tls_conn_state_t state : in_tls) {
        made.session_id = id++;
        tls_resume_keep(&s_tls_resume, "vm.example", &made);
        assert(offered("vm.example") == made.session_id && s_live == 1);
        esp_tls_t failed = {state, 0};
        tls_resume_failed(&s_tls_resume, &failed);
        assert(!offered("vm.example") && s_live == 0);
    }

    // A session is never offered to another host.
    tls_resume_keep(&s_tls_resume, "vm.example", &made);
    assert(!offered("other.example") && s_live == 0);
    puts("ok");
}
