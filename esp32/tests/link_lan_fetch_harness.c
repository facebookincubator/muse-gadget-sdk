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

// main/lan_fetch.c against a scripted home network (DNS and HTTP devices) and
// a fake VM that takes POST /fs/write: delivery modes, logins, redaction, the
// private-address rule and every error path. Run by test_link_lan_fetch.py.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lan_fetch.h"
#include "lwip/netdb.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "noise_control.h"

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "%s:%d: CHECK(%s) failed: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                       \
            fputc('\n', stderr);                                                \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

#define PASSWORD "s3cret!"
#define TOKEN "SECRET-TOKEN-123"

// ---- Clock, heap, tasks --------------------------------------------------------

static int64_t g_now_us = 1000000;
static size_t g_internal_free = 120 * 1024;

int64_t esp_timer_get_time(void) {
    return g_now_us;
}

void *heap_caps_malloc(size_t size, int caps) {
    (void)caps;
    return malloc(size);
}

size_t heap_caps_get_free_size(int caps) {
    return (caps & MALLOC_CAP_SPIRAM) ? 4000000 : g_internal_free;
}

size_t heap_caps_get_minimum_free_size(int caps) {
    return heap_caps_get_free_size(caps);
}

size_t heap_caps_get_largest_free_block(int caps) {
    return heap_caps_get_free_size(caps);
}

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, unsigned stack_depth,
                       void *params, unsigned priority, TaskHandle_t *out_handle) {
    (void)name;
    (void)stack_depth;
    (void)priority;
    (void)out_handle;
    task(params);   // runs to completion; vTaskDelete returns
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task) {
    (void)task;
}

void vTaskDelay(int ticks) {
    g_now_us += (int64_t)ticks * 1000;
}

unsigned uxTaskGetStackHighWaterMark(void *task) {
    (void)task;
    return 4096;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return (SemaphoreHandle_t)1;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait_ticks) {
    (void)semaphore;
    (void)wait_ticks;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    (void)semaphore;
    return pdTRUE;
}

void *esp_crt_bundle_attach;

// ---- Crypto ------------------------------------------------------------------

void mbedtls_platform_zeroize(void *buf, size_t len) {
    volatile unsigned char *p = buf;
    while (len--) *p++ = 0;
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t b[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16
             | (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t full = len / 64 * 64;
    for (size_t off = 0; off < full; off += 64) sha256_block(h, data + off);
    uint8_t tail[128] = {0};
    size_t rest = len - full;
    memcpy(tail, data + full, rest);
    tail[rest] = 0x80;
    size_t tail_len = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[tail_len - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < tail_len; off += 64) sha256_block(h, tail + off);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

struct mbedtls_md_info_t {
    int unused;
};
static const struct mbedtls_md_info_t g_sha256_info = {0};

const mbedtls_md_info_t *mbedtls_md_info_from_type(mbedtls_md_type_t type) {
    return type == MBEDTLS_MD_SHA256 ? &g_sha256_info : NULL;
}

int mbedtls_md(const mbedtls_md_info_t *info, const unsigned char *input, size_t len,
               unsigned char *output) {
    if (info != &g_sha256_info) return -1;
    sha256(input, len, output);
    return 0;
}

static void hex_of(const uint8_t *data, size_t len, char out[65]) {
    uint8_t hash[32];
    sha256(data, len, hash);
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", hash[i]);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen) {
    size_t need = (slen + 2) / 3 * 4;
    *olen = need + 1;
    if (!dst || dlen < need + 1) return -0x002A;   // MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL
    size_t o = 0;
    for (size_t i = 0; i < slen; i += 3) {
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < slen) v |= (uint32_t)src[i + 1] << 8;
        if (i + 2 < slen) v |= src[i + 2];
        dst[o++] = B64[v >> 18 & 63];
        dst[o++] = B64[v >> 12 & 63];
        dst[o++] = i + 1 < slen ? B64[v >> 6 & 63] : '=';
        dst[o++] = i + 2 < slen ? B64[v & 63] : '=';
    }
    dst[o] = '\0';
    *olen = o;
    return 0;
}

// Decodes into a new buffer; CHECKs that the text is well-formed base64.
static uint8_t *b64_decode(const char *text, size_t *out_len) {
    size_t n = strlen(text);
    CHECK(n % 4 == 0, "base64 length %zu", n);
    uint8_t *out = malloc(n / 4 * 3 + 1);
    CHECK(out != NULL, "out of memory");
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v = 0;
        int pad = 0;
        for (int j = 0; j < 4; j++) {
            char ch = text[i + j];
            const char *at = strchr(B64, ch);
            if (ch == '=') {
                pad++;
            } else {
                CHECK(at && ch && !pad, "bad base64 character '%c'", ch);
            }
            v = v << 6 | (ch == '=' ? 0 : (uint32_t)(at - B64));
        }
        out[o++] = (uint8_t)(v >> 16);
        if (pad < 2) out[o++] = (uint8_t)(v >> 8);
        if (pad < 1) out[o++] = (uint8_t)v;
    }
    *out_len = o;
    return out;
}

// ---- The home network: DNS ---------------------------------------------------

int lan_fake_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                         struct addrinfo **res) {
    (void)service;
    CHECK(hints && hints->ai_family == AF_INET, "lookups must ask for IPv4");
    static const struct {
        const char *name;
        const char *ips[3];
    } names[] = {
        {"camera.local", {"192.168.1.20"}},
        {"nas.lan", {"172.16.5.4"}},
        {"mixed.example", {"192.168.1.30", "93.184.216.34"}},
    };
    const char *ips[3] = {0};
    struct in_addr literal;
    if (inet_pton(AF_INET, node, &literal) == 1) {
        ips[0] = node;
    } else {
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (!strcmp(names[i].name, node)) memcpy(ips, names[i].ips, sizeof(ips));
        }
    }
    if (!ips[0]) return EAI_NONAME;
    struct addrinfo *head = NULL, **tail = &head;
    for (int i = 0; i < 3 && ips[i]; i++) {
        struct addrinfo *ai = calloc(1, sizeof(*ai));
        struct sockaddr_in *sin = calloc(1, sizeof(*sin));
        CHECK(ai && sin, "out of memory");
        sin->sin_family = AF_INET;
        inet_pton(AF_INET, ips[i], &sin->sin_addr);
        ai->ai_family = AF_INET;
        ai->ai_addr = (struct sockaddr *)sin;
        ai->ai_addrlen = sizeof(*sin);
        *tail = ai;
        tail = &ai->ai_next;
    }
    *res = head;
    return 0;
}

void lan_fake_freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res);
        res = next;
    }
}

// ---- The home network: HTTP devices --------------------------------------------

enum { LOGIN_NONE, LOGIN_BASIC, LOGIN_DIGEST, LOGIN_OTHER };

typedef struct {
    const char *where;          // host[:port]/path?query, as requested
    int status;
    const char *content_type;
    const uint8_t *body;
    size_t len;
    bool chunked;               // no Content-Length
    int login;
} route_t;

#define SMALL_LEN 60000
#define BIG_LEN 600000
static uint8_t g_small[SMALL_LEN];
static uint8_t g_big[BIG_LEN];
static const char HTML[] = "<html>login</html>";
static const char JSON_BODY[] = "{\"pv_watts\":1234}";

static route_t g_routes[16];
static int g_route_count;

static void add_route(const char *where, int status, const char *type, const void *body,
                      size_t len, bool chunked, int login) {
    g_routes[g_route_count++] = (route_t){where, status, type, body, len, chunked, login};
}

static void make_jpeg(uint8_t *buf, size_t len, uint32_t seed) {
    for (size_t i = 0; i < len; i++) {
        seed = seed * 1103515245u + 12345u;
        buf[i] = (uint8_t)(seed >> 16);
    }
    buf[0] = 0xFF;
    buf[1] = 0xD8;
    buf[len - 2] = 0xFF;
    buf[len - 1] = 0xD9;
}

static void setup_network(void) {
    make_jpeg(g_small, SMALL_LEN, 1);
    make_jpeg(g_big, BIG_LEN, 2);
    add_route("192.168.1.20/snapshot.jpg", 200, "image/jpeg", g_small, SMALL_LEN, false, LOGIN_NONE);
    add_route("camera.local/snapshot.jpg", 200, "image/jpeg", g_small, SMALL_LEN, false, LOGIN_NONE);
    add_route("192.168.1.20/big.jpg", 200, "image/jpeg", g_big, BIG_LEN, false, LOGIN_NONE);
    add_route("192.168.1.20/chunked.jpg", 200, "image/jpeg", g_big, BIG_LEN, true, LOGIN_NONE);
    add_route("192.168.1.20/basic.jpg", 200, "image/jpeg", g_small, SMALL_LEN, false, LOGIN_BASIC);
    add_route("192.168.1.20/digest.jpg", 200, "image/jpeg", g_small, SMALL_LEN, false, LOGIN_DIGEST);
    add_route("192.168.1.20/other.jpg", 200, "image/jpeg", g_small, SMALL_LEN, false, LOGIN_OTHER);
    add_route("192.168.1.20/x/ch0.jpg?token=" TOKEN, 200, "image/jpeg", g_small, SMALL_LEN, false,
              LOGIN_NONE);
    add_route("192.168.1.20/missing", 404, "text/html", HTML, strlen(HTML), false, LOGIN_NONE);
    add_route("192.168.1.20/empty", 200, "image/jpeg", "", 0, false, LOGIN_NONE);
    add_route("172.16.5.4/status.json", 200, "application/json", JSON_BODY, strlen(JSON_BODY), true,
              LOGIN_NONE);
}

struct fake_esp_http_client {
    char where[256];
    char user[64], pass[64];
    bool have_user;
    bool login_sent;
    int login_offered;
    http_event_handle_cb handler;
    void *user_data;
    const route_t *route;
    int status;
    size_t pos;
};

static int g_http_opens;
static int g_login_used;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    CHECK(config->disable_auto_redirect, "redirects must not be followed");
    struct fake_esp_http_client *c = calloc(1, sizeof(*c));
    CHECK(c != NULL, "out of memory");
    c->handler = config->event_handler;
    c->user_data = config->user_data;
    const char *rest = strstr(config->url, "://");
    CHECK(rest != NULL, "url %s", config->url);
    rest += 3;
    size_t auth_len = strcspn(rest, "/?#");
    const char *at = NULL;
    for (const char *p = rest; p < rest + auth_len; p++) {
        if (*p == '@') at = p;
    }
    if (at) {   // user:pass@ in the URL, as esp_http_client takes it
        const char *colon = memchr(rest, ':', (size_t)(at - rest));
        snprintf(c->user, sizeof(c->user), "%.*s", (int)((colon ? colon : at) - rest), rest);
        if (colon) snprintf(c->pass, sizeof(c->pass), "%.*s", (int)(at - colon - 1), colon + 1);
        c->have_user = true;
        rest = at + 1;
    }
    snprintf(c->where, sizeof(c->where), "%s", rest);
    return c;
}

esp_err_t esp_http_client_set_username(esp_http_client_handle_t c, const char *username) {
    snprintf(c->user, sizeof(c->user), "%s", username);
    c->have_user = true;
    return ESP_OK;
}

esp_err_t esp_http_client_set_password(esp_http_client_handle_t c, const char *password) {
    snprintf(c->pass, sizeof(c->pass), "%s", password);
    return ESP_OK;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len) {
    (void)write_len;
    if (!strncmp(c->where, "10.0.0.99", 9)) return ESP_ERR_HTTP_CONNECT;   // nothing there
    g_http_opens++;
    c->route = NULL;
    for (int i = 0; i < g_route_count; i++) {
        if (!strcmp(g_routes[i].where, c->where)) c->route = &g_routes[i];
    }
    c->pos = 0;
    return ESP_OK;
}

static void header(esp_http_client_handle_t c, const char *key, const char *value) {
    esp_http_client_event_t evt = {
        .event_id = HTTP_EVENT_ON_HEADER,
        .client = c,
        .user_data = c->user_data,
        .header_key = (char *)key,
        .header_value = (char *)value,
    };
    c->handler(&evt);
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) {
    const route_t *r = c->route;
    if (!r) {
        c->status = 404;
        return 0;
    }
    bool authorized = r->login == LOGIN_NONE
        || (c->login_sent && c->have_user && !strcmp(c->user, "admin")
            && !strcmp(c->pass, PASSWORD));
    if (!authorized) {
        c->status = 401;
        c->login_offered = r->login;
        header(c, "WWW-Authenticate", r->login == LOGIN_BASIC ? "Basic realm=\"cam\""
                                      : r->login == LOGIN_DIGEST ? "Digest realm=\"cam\""
                                                                 : "Bearer");
        return 0;
    }
    c->status = r->status;
    if (r->content_type) header(c, "Content-Type", r->content_type);
    return r->chunked ? -1 : (int64_t)r->len;
}

int esp_http_client_get_status_code(esp_http_client_handle_t c) {
    return c->status;
}

esp_err_t esp_http_client_add_auth(esp_http_client_handle_t c) {
    if (c->status != 401) return ESP_OK;
    if (c->login_offered != LOGIN_BASIC && c->login_offered != LOGIN_DIGEST) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    c->login_sent = true;
    g_login_used = c->login_offered;
    return ESP_OK;
}

esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int timeout_ms) {
    (void)c;
    CHECK(timeout_ms >= 1 && timeout_ms <= 10000, "socket timeout %d", timeout_ms);
    return ESP_OK;
}

int esp_http_client_read(esp_http_client_handle_t c, char *buffer, int len) {
    const route_t *r = c->route;
    if (!r || c->status != r->status) return 0;
    size_t left = r->len - c->pos;
    size_t n = left < (size_t)len ? left : (size_t)len;
    if (n > 1460) n = 1460;   // a TCP segment at a time
    memcpy(buffer, r->body + c->pos, n);
    c->pos += n;
    return (int)n;
}

bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) {
    return !c->route || c->pos >= c->route->len;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t c) {
    (void)c;
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    mbedtls_platform_zeroize(c->pass, sizeof(c->pass));
    free(c);
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t err) {
    return err == ESP_ERR_HTTP_CONNECT ? "ESP_ERR_HTTP_CONNECT" : "ESP_FAIL";
}

// ---- The VM: POST /fs/write and command results --------------------------------

static int g_fs_status = 200;
static bool g_vm_down;
static int g_writes, g_cancels;
static int64_t g_next_id = 100;
static struct {
    int64_t id;
    noise_ctrl_req_cb cb;
    void *ctx;
    char *body;
    size_t len, cap;
} g_req;
static uint8_t *g_file;
static size_t g_file_len;
static char g_file_path[256];
static bool g_write_append[32];
static char *g_result;
static char g_result_id[64];

int64_t noise_ctrl_req_open(const char *verb, const char *path, const char *const *headers,
                            bool end_body, noise_ctrl_req_cb cb, void *ctx) {
    if (g_vm_down) return 0;
    CHECK(!strcmp(verb, "POST") && !strcmp(path, "/fs/write"), "unexpected %s %s", verb, path);
    CHECK(headers && !strcmp(headers[0], "Content-Type") && !strcmp(headers[1], "application/json")
          && !headers[2], "a JSON body must be declared");
    CHECK(!end_body, "the body follows the open");
    free(g_req.body);
    memset(&g_req, 0, sizeof(g_req));
    g_req.id = ++g_next_id;
    g_req.cb = cb;
    g_req.ctx = ctx;
    return g_req.id;
}

static void vm_write(void) {
    cJSON *body = cJSON_ParseWithLength(g_req.body, g_req.len);
    CHECK(body != NULL, "/fs/write body is not JSON");
    cJSON *path = cJSON_GetObjectItem(body, "path");
    cJSON *overwrite = cJSON_GetObjectItem(body, "overwrite");
    cJSON *append = cJSON_GetObjectItem(body, "append");
    cJSON *parent = cJSON_GetObjectItem(body, "create_parent");
    cJSON *data = cJSON_GetObjectItem(body, "data_base64");
    CHECK(cJSON_IsString(path) && cJSON_IsBool(overwrite) && cJSON_IsBool(append)
          && cJSON_IsBool(parent) && cJSON_IsString(data),
          "every /fs/write needs path, the three booleans and data_base64");
    CHECK(cJSON_IsTrue(parent), "create_parent");
    CHECK(cJSON_IsTrue(append) != cJSON_IsTrue(overwrite), "the first part overwrites, the rest append");
    if (g_writes < 32) g_write_append[g_writes] = cJSON_IsTrue(append);
    g_writes++;
    const char *reply = "{\"error\":\"route not allowed for this credential's scopes\"}";
    if (g_fs_status == 200) {
        size_t n;
        uint8_t *bytes = b64_decode(data->valuestring, &n);
        if (cJSON_IsTrue(append)) {
            CHECK(!strcmp(g_file_path, path->valuestring), "appending to another file");
        } else {
            free(g_file);
            g_file = NULL;
            g_file_len = 0;
            snprintf(g_file_path, sizeof(g_file_path), "%s", path->valuestring);
        }
        g_file = realloc(g_file, g_file_len + n + 1);
        CHECK(g_file != NULL, "out of memory");
        memcpy(g_file + g_file_len, bytes, n);
        g_file_len += n;
        free(bytes);
        reply = "{\"ok\":true}";
    }
    cJSON_Delete(body);
    g_req.cb(g_req.ctx, g_fs_status, (const uint8_t *)reply, strlen(reply), true);
}

bool noise_ctrl_req_send(int64_t id, const void *data, size_t len, bool end_body, int wait_ms) {
    (void)wait_ms;
    CHECK(id == g_req.id, "send on request %lld", (long long)id);
    CHECK(len <= 8192, "a %zu-byte piece is over the session's 8 KB limit", len);
    if (g_req.len + len + 1 > g_req.cap) {
        g_req.cap = (g_req.len + len + 1) * 2;
        g_req.body = realloc(g_req.body, g_req.cap);
        CHECK(g_req.body != NULL, "out of memory");
    }
    memcpy(g_req.body + g_req.len, data, len);
    g_req.len += len;
    g_req.body[g_req.len] = '\0';
    if (end_body) vm_write();
    return true;
}

void noise_ctrl_req_cancel(int64_t id) {
    (void)id;
    g_cancels++;
}

size_t noise_ctrl_req_max_body(void) {
    return 12 * 1024;
}

void noise_ctrl_send_command_result(noise_ctrl_session_generation_t session_generation,
                                    const char *request_id, cJSON *result) {
    CHECK(session_generation == 7, "result for session %llu", (unsigned long long)session_generation);
    free(g_result);
    g_result = cJSON_PrintUnformatted(result);
    snprintf(g_result_id, sizeof(g_result_id), "%s", request_id);
    cJSON_Delete(result);
}

// ---- Running the command -------------------------------------------------------

static cJSON *run(const char *params_json) {
    free(g_result);
    g_result = NULL;
    cJSON *params = cJSON_Parse(params_json);
    CHECK(params != NULL, "test params %s", params_json);
    cJSON *ret = lan_fetch_command(params, "req-1", 7);
    cJSON_Delete(params);
    if (cJSON_IsTrue(cJSON_GetObjectItem(ret, "_async"))) {
        cJSON_Delete(ret);
        CHECK(g_result != NULL && !strcmp(g_result_id, "req-1"), "no result for %s", params_json);
        ret = cJSON_Parse(g_result);
    }
    char *text = cJSON_PrintUnformatted(ret);
    CHECK(!strstr(text, PASSWORD) && !strstr(text, TOKEN), "a secret in the result: %s", text);
    free(text);
    return ret;
}

static cJSON *payload_of(cJSON *ret) {
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(ret, "ok")), "expected ok: %s",
          cJSON_PrintUnformatted(ret));
    return cJSON_GetObjectItem(ret, "payload");
}

static const char *str(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

static double num(cJSON *obj, const char *key) {
    return cJSON_GetNumberValue(cJSON_GetObjectItem(obj, key));
}

static void expect_error(const char *params, const char *code, const char *message_part) {
    cJSON *ret = run(params);
    cJSON *error = cJSON_GetObjectItem(ret, "error");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(ret, "ok")) && error, "expected %s for %s", code, params);
    CHECK(!strcmp(str(error, "code"), code), "%s: code %s, expected %s (%s)", params,
          str(error, "code"), code, str(error, "message"));
    CHECK(strstr(str(error, "message"), message_part), "%s: message '%s' lacks '%s'", params,
          str(error, "message"), message_part);
    cJSON_Delete(ret);
}

// The payload's metadata, and the bytes it describes, match the source.
static void expect_body(cJSON *payload, const uint8_t *body, size_t len) {
    char want[65];
    hex_of(body, len, want);
    CHECK(num(payload, "bytes") == (double)len, "bytes %.0f, expected %zu", num(payload, "bytes"), len);
    CHECK(!strcmp(str(payload, "sha256"), want), "sha256 %s, expected %s", str(payload, "sha256"), want);
}

static void expect_inline(cJSON *payload, const uint8_t *body, size_t len) {
    expect_body(payload, body, len);
    CHECK(!strcmp(str(payload, "delivery"), "inline"), "delivery %s", str(payload, "delivery"));
    size_t n;
    uint8_t *got = b64_decode(str(payload, "data_base64"), &n);
    CHECK(n == len && !memcmp(got, body, len), "inline body differs (%zu bytes)", n);
    free(got);
}

static void expect_file(cJSON *payload, const char *path, const uint8_t *body, size_t len) {
    expect_body(payload, body, len);
    CHECK(!strcmp(str(payload, "delivery"), "file"), "delivery %s", str(payload, "delivery"));
    CHECK(!strcmp(str(payload, "path"), path), "path %s, expected %s", str(payload, "path"), path);
    CHECK(!strcmp(g_file_path, path) && g_file_len == len && !memcmp(g_file, body, len),
          "the VM's %s differs from the source (%zu bytes)", g_file_path, g_file_len);
    CHECK(!cJSON_GetObjectItem(payload, "data_base64"), "a file delivery carries no inline body");
}

// ---- Cases ---------------------------------------------------------------------

static void sha256_known_answer(void) {
    char hex[65];
    hex_of((const uint8_t *)"abc", 3, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "the harness's SHA-256 is wrong: %s", hex);
}

static void inline_snapshot(void) {
    cJSON *ret = run("{\"url\":\"http://192.168.1.20/snapshot.jpg\",\"delivery\":\"inline\"}");
    cJSON *p = payload_of(ret);
    expect_inline(p, g_small, SMALL_LEN);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(p, "jpeg")), "a JPEG");
    CHECK(!strcmp(str(p, "content_type"), "image/jpeg"), "content_type %s", str(p, "content_type"));
    CHECK(num(p, "http_status") == 200, "http_status");
    CHECK(!strcmp(str(p, "url"), "http://192.168.1.20/snapshot.jpg"), "url %s", str(p, "url"));
    CHECK(!cJSON_GetObjectItem(p, "file_error") && g_writes == 0, "inline never writes a file");
    cJSON_Delete(ret);
}

static void file_delivery_in_parts(void) {
    int before = g_writes;
    cJSON *ret = run("{\"url\":\"http://camera.local/snapshot.jpg\",\"delivery\":\"file\"}");
    expect_file(payload_of(ret), "lan-fetch/latest.jpg", g_small, SMALL_LEN);
    // 60000 bytes in 48 KB parts: an overwrite, then an append.
    CHECK(g_writes - before == 2 && !g_write_append[before] && g_write_append[before + 1],
          "%d writes", g_writes - before);
    cJSON_Delete(ret);

    before = g_writes;
    ret = run("{\"url\":\"http://192.168.1.20/big.jpg\",\"delivery\":\"file\","
              "\"path\":\"cams/roof.jpg\",\"max_bytes\":2097152}");
    expect_file(payload_of(ret), "cams/roof.jpg", g_big, BIG_LEN);
    CHECK(g_writes - before == 13, "600000 bytes took %d writes", g_writes - before);
    cJSON_Delete(ret);
}

static void auto_uses_the_file_while_the_vm_takes_it(void) {
    cJSON *ret = run("{\"url\":\"http://172.16.5.4/status.json\"}");
    cJSON *p = payload_of(ret);
    expect_file(p, "lan-fetch/latest.json", (const uint8_t *)JSON_BODY, strlen(JSON_BODY));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(p, "jpeg")), "JSON isn't a JPEG");
    cJSON_Delete(ret);
}

static void refused_file_falls_back_and_retries(void) {
    g_fs_status = 403;
    expect_error("{\"url\":\"http://192.168.1.20/snapshot.jpg\",\"delivery\":\"file\"}",
                 "upload_failed", "/fs/write answered 403");

    // Within 10 minutes of a refusal, auto goes straight to inline and says why.
    int before = g_writes;
    cJSON *ret = run("{\"url\":\"http://192.168.1.20/snapshot.jpg\"}");
    cJSON *p = payload_of(ret);
    expect_inline(p, g_small, SMALL_LEN);
    CHECK(strstr(str(p, "file_error"), "403"), "file_error '%s'", str(p, "file_error"));
    CHECK(g_writes == before, "no new write");
    cJSON_Delete(ret);

    // After that, auto tries the file again, and falls back when it's refused.
    before = g_writes;
    g_now_us += 11LL * 60 * 1000000;
    ret = run("{\"url\":\"http://192.168.1.20/snapshot.jpg\"}");
    p = payload_of(ret);
    expect_inline(p, g_small, SMALL_LEN);
    CHECK(strstr(str(p, "file_error"), "403") && g_writes - before == 1, "auto tried the file once");
    cJSON_Delete(ret);

    // A body over the inline limit can't fall back.
    expect_error("{\"url\":\"http://192.168.1.20/big.jpg\",\"max_bytes\":2097152}", "too_large",
                 "inline limit");

    // After that, a granted scope takes effect without a restart.
    g_fs_status = 200;
    g_now_us += 11LL * 60 * 1000000;
    ret = run("{\"url\":\"http://192.168.1.20/snapshot.jpg\"}");
    p = payload_of(ret);
    expect_file(p, "lan-fetch/latest.jpg", g_small, SMALL_LEN);
    CHECK(!cJSON_GetObjectItem(p, "file_error"), "a delivered file reports no error");
    cJSON_Delete(ret);
}

static void logins(void) {
    cJSON *ret = run("{\"url\":\"http://admin:" PASSWORD "@192.168.1.20/basic.jpg\",\"delivery\":\"inline\"}");
    cJSON *p = payload_of(ret);
    expect_inline(p, g_small, SMALL_LEN);
    CHECK(g_login_used == LOGIN_BASIC, "answered with Basic");
    CHECK(!strcmp(str(p, "url"), "http://***@192.168.1.20/basic.jpg"), "url %s", str(p, "url"));
    cJSON_Delete(ret);

    g_login_used = LOGIN_NONE;
    ret = run("{\"url\":\"http://192.168.1.20/digest.jpg\",\"username\":\"admin\","
              "\"password\":\"" PASSWORD "\",\"delivery\":\"inline\"}");
    expect_inline(payload_of(ret), g_small, SMALL_LEN);
    CHECK(g_login_used == LOGIN_DIGEST, "answered with Digest");
    cJSON_Delete(ret);

    ret = run("{\"url\":\"http://192.168.1.20/x/ch0.jpg?token=" TOKEN "\",\"delivery\":\"inline\"}");
    p = payload_of(ret);
    expect_inline(p, g_small, SMALL_LEN);
    CHECK(!strcmp(str(p, "url"), "http://192.168.1.20/x/ch0.jpg?***"), "url %s", str(p, "url"));
    cJSON_Delete(ret);

    expect_error("{\"url\":\"http://192.168.1.20/basic.jpg\"}", "auth_failed", "HTTP 401");
    expect_error("{\"url\":\"http://admin:wrong@192.168.1.20/digest.jpg\"}", "auth_failed", "HTTP 401");
    expect_error("{\"url\":\"http://192.168.1.20/other.jpg\",\"username\":\"admin\","
                 "\"password\":\"" PASSWORD "\"}", "auth_failed", "Basic or Digest only");
}

static void home_network_only(void) {
    expect_error("{\"url\":\"http://8.8.8.8/x.jpg\"}", "not_private", "home network");
    expect_error("{\"url\":\"http://mixed.example/x.jpg\"}", "not_private", "home network");
    expect_error("{\"url\":\"http://nowhere.invalid/x.jpg\"}", "dns_failed", "resolve");
    expect_error("{\"url\":\"http://[fd00::1]/x.jpg\"}", "invalid_params", "http://");
    expect_error("{\"url\":\"ftp://192.168.1.20/x.jpg\"}", "invalid_params", "http://");
}

static void server_and_body_errors(void) {
    expect_error("{\"url\":\"http://192.168.1.20/missing\"}", "http_status", "HTTP 404");
    expect_error("{\"url\":\"http://10.0.0.99/x.jpg\"}", "connect_failed", "ESP_ERR_HTTP_CONNECT");
    expect_error("{\"url\":\"http://192.168.1.20/snapshot.jpg\",\"max_bytes\":4096}", "too_large",
                 "over max_bytes");
    expect_error("{\"url\":\"http://192.168.1.20/chunked.jpg\",\"max_bytes\":4096}", "too_large",
                 "runs over max_bytes");
    expect_error("{\"url\":\"http://192.168.1.20/empty\"}", "empty_body", "empty");
}

static void bad_params(void) {
    expect_error("{}", "missing_param", "url");
    expect_error("{\"url\":\"http://192.168.1.20/x\",\"max_bytes\":10}", "invalid_params", "max_bytes");
    expect_error("{\"url\":\"http://192.168.1.20/x\",\"timeout_ms\":100}", "invalid_params", "timeout_ms");
    expect_error("{\"url\":\"http://192.168.1.20/x\",\"path\":\"../etc/passwd\"}", "invalid_params", "..");
    expect_error("{\"url\":\"http://192.168.1.20/x\",\"path\":\"a/../../b\"}", "invalid_params", "..");
    expect_error("{\"url\":\"http://192.168.1.20/x\",\"path\":\"/etc/passwd\"}", "invalid_params", "relative");
    expect_error("{\"url\":\"http://192.168.1.20/x\",\"password\":\"" PASSWORD "\"}", "invalid_params",
                 "username");
    expect_error("{\"url\":\"http://192.168.1.20/x\",\"delivery\":\"email\"}", "invalid_params", "delivery");
}

static void memory_and_session(void) {
    g_internal_free = 20 * 1024;
    expect_error("{\"url\":\"http://192.168.1.20/snapshot.jpg\"}", "out_of_memory", "memory");
    g_internal_free = 120 * 1024;

    g_vm_down = true;
    expect_error("{\"url\":\"http://192.168.1.20/snapshot.jpg\",\"delivery\":\"file\"}", "upload_failed",
                 "session is down");
    g_vm_down = false;
}

static void console(void) {
    CHECK(!lan_fetch_console("status"), "other console lines aren't lan.fetch's");
    CHECK(lan_fetch_console("lan.fetch {\"url\":\"http://192.168.1.20/snapshot.jpg\",\"delivery\":\"inline\"}"),
          "the console runs lan.fetch");
    CHECK(lan_fetch_console("lan.fetch not json"), "a bad line is answered, not passed on");
}

int main(void) {
    setup_network();
    sha256_known_answer();
    inline_snapshot();
    file_delivery_in_parts();
    auto_uses_the_file_while_the_vm_takes_it();
    refused_file_falls_back_and_retries();
    logins();
    home_network_only();
    server_and_body_errors();
    bad_params();
    memory_and_session();
    console();
    free(g_req.body);
    free(g_file);
    free(g_result);
    printf("lan.fetch harness: all cases passed\n");
    return 0;
}
