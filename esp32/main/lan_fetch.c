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

#include "lan_fetch.h"
#include "sdkconfig.h"

#if CONFIG_HOMEHUB_LAN_FETCH

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "noise_tunnel_internal.h"

static const char *TAG = "link.lan_fetch";

// The task's stack: the HTTP client, DNS and, for HTTPS, the TLS handshake.
#define STACK_HTTP          7168
#define STACK_TLS           9216
// Free internal RAM a fetch needs beyond its stack before it may start, and
// the floor that aborts one: the same budget as display.draw_url's download.
#define HTTP_BYTES          (6 * 1024)
#if CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC
#define TLS_BYTES           (8 * 1024)
#else
#define TLS_BYTES           (26 * 1024)
#endif
#define RESERVE_BYTES       (4 * 1024)
#define FLOOR_BYTES         (3 * 1024)
#define SOCKET_TIMEOUT_MS   10000
#define DEFAULT_TIMEOUT_MS  15000
#define MIN_TIMEOUT_MS      1000
#define MAX_TIMEOUT_MS      60000
#define DEFAULT_MAX_BYTES   (1024 * 1024)
#define MIN_MAX_BYTES       1024
#define HARD_MAX_BYTES      (2 * 1024 * 1024)
#define PATH_MAX_LEN        200
#define CONTENT_TYPE_MAX    64
// The body goes to the VM in POST /fs/write requests of at most this many
// bytes, the later ones appending. Each request's base64 is sent in pieces
// that stay under the session's 8 KB send limit.
#define UPLOAD_PART_BYTES   (48 * 1024)
#define UPLOAD_PIECE_BYTES  (4 * 1024 + 512)    // base64: 6 KB
#define UPLOAD_SEND_WAIT_MS 5000
#define UPLOAD_REPLY_MS     20000
#define UPLOAD_POLL_MS      20
#define UPLOAD_ERROR_MAX    160
// A VM that refuses POST /fs/write to this gadget's credentials gets the body
// in the command result as base64 instead (as camera.capture does), up to this.
#define INLINE_MAX_BYTES    (512 * 1024)

#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define BYTE_CAPS  (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

typedef struct {
    noise_ctrl_session_generation_t session_generation;
    char request_id[64];
    char *url;          // as given, credentials and all: wiped once used
    char *username;     // or NULL
    char *password;     // or NULL
    char *path;         // on the VM, or NULL for the default
    char *shown_url;    // for logs and the result: no login, no query
    size_t max_bytes;
    int timeout_ms;
    bool https;
    bool login;         // a username, or user:password@ in the URL
    char delivery;      // 'a'uto, 'f'ile or 'i'nline

    esp_http_client_handle_t http;
    int64_t deadline_us;
    bool timed_out;
    char content_type[CONTENT_TYPE_MAX];
} job_t;

static atomic_bool s_busy;
// When the VM last refused /fs/write (403, 404 or 405), and what it said:
// for FS_WRITE_RETRY_US after that, auto mode goes straight to inline and
// reports the refusal. Then it tries the file again, so a scope granted
// later takes effect without a restart.
#define FS_WRITE_RETRY_US   (10 * 60 * 1000000LL)
static int64_t s_fs_write_refused_us;
static char s_fs_write_refusal[UPLOAD_ERROR_MAX + 64];

static size_t internal_free(void) {
    return heap_caps_get_free_size(BYTE_CAPS);
}

static void free_secret(char *s) {
    if (!s) return;
    mbedtls_platform_zeroize(s, strlen(s));
    free(s);
}

static void job_free(job_t *job) {
    if (!job) return;
    free_secret(job->url);
    free_secret(job->username);
    free_secret(job->password);
    free(job->path);
    free(job->shown_url);
    free(job);
}

static cJSON *error_result(const char *code, const char *message) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return result;
}

// ---- URL ---------------------------------------------------------------------

// Splits the URL's authority: where the host starts and ends, and whether
// there is a user:password@ before it. False if it isn't http(s)://host.
static bool split_url(const char *url, bool *https, const char **host, size_t *host_len,
                      bool *userinfo, const char **rest) {
    size_t scheme;
    if (strncasecmp(url, "http://", 7) == 0) {
        *https = false;
        scheme = 7;
    } else if (strncasecmp(url, "https://", 8) == 0) {
        *https = true;
        scheme = 8;
    } else {
        return false;
    }
    const char *auth = url + scheme;
    const char *end = auth + strcspn(auth, "/?#");
    const char *at = NULL;
    for (const char *p = auth; p < end; p++) {
        if (*p == '@') at = p;
    }
    *userinfo = at != NULL;
    const char *h = at ? at + 1 : auth;
    if (*h == '[') return false;    // IPv6 literals: not supported
    const char *colon = memchr(h, ':', (size_t)(end - h));
    *host = h;
    *host_len = (size_t)((colon ? colon : end) - h);
    *rest = h;
    return *host_len > 0;
}

// scheme://[***@]host[:port]/path[?***]: never the login or a query, which
// may carry a token.
static char *shown_url(const char *url, bool https, bool userinfo, const char *rest) {
    size_t path_len = strcspn(rest, "?#");
    bool query = rest[path_len] == '?';
    size_t cap = 8 + 4 + path_len + 5 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    snprintf(out, cap, "%s%s%.*s%s", https ? "https://" : "http://",
             userinfo ? "***@" : "", (int)path_len, rest, query ? "?***" : "");
    return out;
}

static bool private_ipv4(uint32_t ip) {
    return (ip >> 24) == 10                      // 10.0.0.0/8
        || (ip >> 20) == ((172u << 4) | 1)       // 172.16.0.0/12
        || (ip >> 16) == ((192u << 8) | 168)     // 192.168.0.0/16
        || (ip >> 16) == ((169u << 8) | 254);    // 169.254.0.0/16
}

// Every address the host resolves to must be a private IPv4 one. The HTTP
// client resolves the name again; a name is only as trustworthy as the home
// network's DNS, which already decides what the gadget talks to.
static const char *check_private_host(const char *host) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        return "dns_failed";
    }
    bool all_private = true;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)a->ai_addr;
        if (!private_ipv4(ntohl(sin->sin_addr.s_addr))) all_private = false;
    }
    freeaddrinfo(res);
    return all_private ? NULL : "not_private";
}

// ---- Download ----------------------------------------------------------------

static bool within_deadline(job_t *job) {
    int64_t left_ms = (job->deadline_us - esp_timer_get_time()) / 1000;
    if (left_ms <= 0) {
        job->timed_out = true;
        esp_http_client_set_timeout_ms(job->http, 1);
        return false;
    }
    esp_http_client_set_timeout_ms(job->http,
                                   left_ms < SOCKET_TIMEOUT_MS ? (int)left_ms : SOCKET_TIMEOUT_MS);
    return true;
}

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
    job_t *job = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER || evt->event_id == HTTP_EVENT_ON_DATA) {
        within_deadline(job);
    }
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value
        && strcasecmp(evt->header_key, "Content-Type") == 0) {
        strlcpy(job->content_type, evt->header_value, sizeof(job->content_type));
    }
    return ESP_OK;
}

typedef struct {
    uint8_t *body;
    size_t len;
    int status;
    const char *code;
    char message[96];
} download_t;

// Opens the URL, answering one 401 with the login if there is one. False with
// d->code and d->message set if it can't.
static bool http_open(job_t *job, download_t *d, int64_t *content_len) {
    for (int attempt = 0;; attempt++) {
        if (!within_deadline(job)) break;
        esp_err_t err = esp_http_client_open(job->http, 0);
        if (err != ESP_OK) {
            d->code = job->timed_out ? "timeout" : "connect_failed";
            snprintf(d->message, sizeof(d->message), "could not connect: %s", esp_err_to_name(err));
            return false;
        }
        *content_len = esp_http_client_fetch_headers(job->http);
        d->status = esp_http_client_get_status_code(job->http);
        if (d->status == 401 && job->login && attempt == 0) {
            err = esp_http_client_add_auth(job->http);
            esp_http_client_close(job->http);
            if (err != ESP_OK) {
                d->code = "auth_failed";
                snprintf(d->message, sizeof(d->message),
                         "the server asks for a login this gadget can't do (Basic or Digest only)");
                return false;
            }
            continue;
        }
        if (d->status == 200) return true;
        d->code = d->status == 401 || d->status == 403 ? "auth_failed" : "http_status";
        if (d->status >= 300 && d->status < 400) {
            snprintf(d->message, sizeof(d->message),
                     "server answered HTTP %d (redirect): pass the final URL", d->status);
        } else {
            snprintf(d->message, sizeof(d->message), "server answered HTTP %d", d->status);
        }
        return false;
    }
    d->code = "timeout";
    snprintf(d->message, sizeof(d->message), "download timed out");
    return false;
}

// Reads the whole body into PSRAM, no more than job->max_bytes.
static bool http_read_body(job_t *job, download_t *d, int64_t content_len) {
    if (content_len > (int64_t)job->max_bytes) {
        d->code = "too_large";
        snprintf(d->message, sizeof(d->message), "body is %lld bytes, over max_bytes %u",
                 (long long)content_len, (unsigned)job->max_bytes);
        return false;
    }
    // One byte past the cap tells a body that runs over from one that fits.
    size_t cap = content_len > 0 ? (size_t)content_len : job->max_bytes + 1;
    d->body = heap_caps_malloc(cap, PSRAM_CAPS);
    if (!d->body) {
        d->code = "out_of_memory";
        snprintf(d->message, sizeof(d->message), "no room for %u bytes", (unsigned)cap);
        return false;
    }
    while (d->len < cap) {
        if (internal_free() < FLOOR_BYTES) {
            d->code = "out_of_memory";
            snprintf(d->message, sizeof(d->message), "memory ran low");
            return false;
        }
        if (!within_deadline(job)) break;
        int n = esp_http_client_read(job->http, (char *)d->body + d->len, (int)(cap - d->len));
        if (n < 0) break;
        if (n == 0) {
            if (!esp_http_client_is_complete_data_received(job->http)) break;
            if (d->len == 0) {
                d->code = "empty_body";
                snprintf(d->message, sizeof(d->message), "the body was empty");
                return false;
            }
            return true;
        }
        d->len += (size_t)n;
    }
    if (d->len > job->max_bytes) {
        d->code = "too_large";
        snprintf(d->message, sizeof(d->message), "body runs over max_bytes %u",
                 (unsigned)job->max_bytes);
        return false;
    }
    if (content_len > 0 && d->len == (size_t)content_len) return true;
    d->code = job->timed_out ? "timeout" : "download_failed";
    snprintf(d->message, sizeof(d->message), job->timed_out ? "download timed out"
                                                             : "the download broke off");
    return false;
}

static bool download(job_t *job, download_t *d) {
    esp_http_client_config_t cfg = {
        .url = job->url,
        .crt_bundle_attach = job->https ? esp_crt_bundle_attach : NULL,
        .timeout_ms = SOCKET_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
        .max_authorization_retries = 1,
        .user_agent = "MuseGadget lan.fetch",
        .event_handler = on_http_event,
        .user_data = job,
    };
    job->http = esp_http_client_init(&cfg);
    if (!job->http) {
        d->code = "out_of_memory";
        snprintf(d->message, sizeof(d->message), "could not start the HTTP client");
        return false;
    }
    if (job->username) {
        esp_http_client_set_username(job->http, job->username);
        esp_http_client_set_password(job->http, job->password ? job->password : "");
    }
    // The client keeps its own copy of the login; ours is no longer needed.
    free_secret(job->username);
    free_secret(job->password);
    job->username = job->password = NULL;

    int64_t content_len = 0;
    bool ok = http_open(job, d, &content_len) && http_read_body(job, d, content_len);
    esp_http_client_cleanup(job->http);
    job->http = NULL;
    return ok;
}

// ---- Upload to the VM ----------------------------------------------------------

// The reply to the one POST /fs/write in flight, on the Noise session task.
// Frames for an earlier, abandoned request carry an older generation.
static struct {
    SemaphoreHandle_t lock;
    uint32_t gen;
    int status;
    bool done;
    char body[UPLOAD_ERROR_MAX];
    size_t body_len;
} s_up;

static void on_upload_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end) {
    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    if ((uint32_t)(uintptr_t)ctx == s_up.gen && !s_up.done) {
        if (status) s_up.status = status;
        size_t room = sizeof(s_up.body) - 1 - s_up.body_len;
        if (len && room) {
            size_t n = len < room ? len : room;
            memcpy(s_up.body + s_up.body_len, data, n);
            s_up.body_len += n;
            s_up.body[s_up.body_len] = '\0';
        }
        s_up.done = end || status < 0;
    }
    xSemaphoreGive(s_up.lock);
}

// One POST /fs/write of `len` bytes. False with message set if it failed, and
// *http_status the VM's answer if it gave one.
static bool upload_part(const char *path, const uint8_t *data, size_t len, bool append,
                        uint8_t *b64, size_t b64_cap, int *http_status, char *message,
                        size_t message_len) {
    // {"path":...,"overwrite":...,"append":...,"create_parent":true,"data_base64":"
    // then the base64 in pieces, then "}. cJSON escapes the path.
    cJSON *head = cJSON_CreateObject();
    if (!head) {
        snprintf(message, message_len, "out of memory");
        return false;
    }
    cJSON_AddStringToObject(head, "path", path);
    cJSON_AddBoolToObject(head, "overwrite", !append);
    cJSON_AddBoolToObject(head, "append", append);
    cJSON_AddBoolToObject(head, "create_parent", true);
    cJSON_AddStringToObject(head, "data_base64", "");
    char *prefix = cJSON_PrintUnformatted(head);
    cJSON_Delete(head);
    if (!prefix) {
        snprintf(message, message_len, "out of memory");
        return false;
    }
    size_t prefix_len = strlen(prefix) - 2;     // drop the closing "}

    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    uint32_t token = ++s_up.gen;
    s_up.status = 0;
    s_up.done = false;
    s_up.body_len = 0;
    s_up.body[0] = '\0';
    xSemaphoreGive(s_up.lock);

    const char *headers[] = {"Content-Type", "application/json", NULL};
    int64_t id = noise_ctrl_req_open("POST", "/fs/write", headers, false, on_upload_frame,
                                     (void *)(uintptr_t)token);
    bool sent = id != 0 && noise_ctrl_req_send(id, prefix, prefix_len, false, UPLOAD_SEND_WAIT_MS);
    free(prefix);
    for (size_t off = 0; sent && off < len; off += UPLOAD_PIECE_BYTES) {
        size_t n = len - off < UPLOAD_PIECE_BYTES ? len - off : UPLOAD_PIECE_BYTES;
        size_t olen = 0;
        sent = mbedtls_base64_encode(b64, b64_cap, &olen, data + off, n) == 0
            && noise_ctrl_req_send(id, b64, olen, false, UPLOAD_SEND_WAIT_MS);
    }
    sent = sent && noise_ctrl_req_send(id, "\"}", 2, true, UPLOAD_SEND_WAIT_MS);

    int64_t deadline = esp_timer_get_time() + UPLOAD_REPLY_MS * 1000LL;
    bool done = false;
    while (sent && !done && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(UPLOAD_POLL_MS));
        xSemaphoreTake(s_up.lock, portMAX_DELAY);
        done = s_up.done;
        xSemaphoreGive(s_up.lock);
    }

    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    s_up.gen++;     // drop anything still to come for this request
    int status = s_up.status;
    char body[UPLOAD_ERROR_MAX];
    strlcpy(body, s_up.body, sizeof(body));
    xSemaphoreGive(s_up.lock);
    if (id && !done) noise_ctrl_req_cancel(id);
    *http_status = done ? status : 0;

    if (!id) {
        snprintf(message, message_len, "the VM session is down");
    } else if (!sent) {
        snprintf(message, message_len, "could not send the file to the VM");
    } else if (!done) {
        snprintf(message, message_len, "the VM did not answer /fs/write");
    } else if (status < 200 || status >= 300) {
        snprintf(message, message_len, "/fs/write answered %d: %s", status, body);
    } else {
        return true;
    }
    return false;
}

static bool upload(const char *path, const uint8_t *data, size_t len, int *http_status,
                   char *message, size_t message_len) {
    size_t b64_cap = ((UPLOAD_PIECE_BYTES + 2) / 3) * 4 + 1;
    uint8_t *b64 = heap_caps_malloc(b64_cap, PSRAM_CAPS);
    if (!b64) {
        snprintf(message, message_len, "out of memory");
        return false;
    }
    bool ok = true;
    for (size_t off = 0; ok && off < len; off += UPLOAD_PART_BYTES) {
        size_t n = len - off < UPLOAD_PART_BYTES ? len - off : UPLOAD_PART_BYTES;
        ok = upload_part(path, data + off, n, off > 0, b64, b64_cap, http_status, message,
                         message_len);
    }
    free(b64);
    return ok;
}

// ---- Task --------------------------------------------------------------------

static bool is_jpeg(const uint8_t *b, size_t n) {
    return n >= 4 && b[0] == 0xFF && b[1] == 0xD8 && b[n - 2] == 0xFF && b[n - 1] == 0xD9;
}

static const char *extension(const char *content_type, bool jpeg) {
    if (jpeg || strncasecmp(content_type, "image/jpeg", 10) == 0) return "jpg";
    if (strncasecmp(content_type, "image/png", 9) == 0) return "png";
    if (strncasecmp(content_type, "image/gif", 9) == 0) return "gif";
    if (strncasecmp(content_type, "image/webp", 10) == 0) return "webp";
    if (strncasecmp(content_type, "application/json", 16) == 0) return "json";
    if (strncasecmp(content_type, "text/", 5) == 0) return "txt";
    return "bin";
}

// The body as base64 in the payload, for a VM that won't take the file.
static bool add_inline(cJSON *payload, const download_t *d, char *message, size_t message_len) {
    if (d->len > INLINE_MAX_BYTES) {
        snprintf(message, message_len,
                 "%u bytes is over the %u KB inline limit: lower max_bytes or the "
                 "camera's resolution", (unsigned)d->len, (unsigned)(INLINE_MAX_BYTES / 1024));
        return false;
    }
    size_t cap = ((d->len + 2) / 3) * 4 + 1, olen = 0;
    unsigned char *b64 = heap_caps_malloc(cap, PSRAM_CAPS);
    bool ok = b64 && mbedtls_base64_encode(b64, cap, &olen, d->body, d->len) == 0
           && cJSON_AddStringToObject(payload, "data_base64", (const char *)b64);
    free(b64);
    if (!ok) snprintf(message, message_len, "out of memory for the inline result");
    return ok;
}

// Writes the body to the VM, or puts it in the result: per job->delivery, and
// in auto mode inline only once the VM has refused /fs/write.
static cJSON *deliver(job_t *job, const download_t *d, const char *path, bool jpeg,
                      const char *sha256, int fetch_ms) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON *payload = cJSON_AddObjectToObject(result, "payload");
    cJSON_AddNumberToObject(payload, "bytes", (double)d->len);
    if (sha256) cJSON_AddStringToObject(payload, "sha256", sha256);
    cJSON_AddStringToObject(payload, "content_type", job->content_type);
    cJSON_AddBoolToObject(payload, "jpeg", jpeg);
    cJSON_AddNumberToObject(payload, "http_status", d->status);
    cJSON_AddNumberToObject(payload, "fetch_ms", fetch_ms);
    cJSON_AddStringToObject(payload, "url", job->shown_url);

    char message[UPLOAD_ERROR_MAX + 64];
    bool recently_refused = s_fs_write_refused_us
        && esp_timer_get_time() - s_fs_write_refused_us < FS_WRITE_RETRY_US;
    bool to_file = job->delivery == 'f' || (job->delivery == 'a' && !recently_refused);
    if (to_file) {
        int64_t up_start = esp_timer_get_time();
        int status = 0;
        bool written = upload(path, d->body, d->len, &status, message, sizeof(message));
        ESP_LOGI(TAG, "wrote %u bytes to %s: %s", (unsigned)d->len, path,
                 written ? "ok" : message);
        if (written) {
            s_fs_write_refused_us = 0;
            s_fs_write_refusal[0] = '\0';
            cJSON_AddStringToObject(payload, "delivery", "file");
            cJSON_AddStringToObject(payload, "path", path);
            cJSON_AddNumberToObject(payload, "upload_ms",
                                    (int)((esp_timer_get_time() - up_start) / 1000));
            return result;
        }
        bool refused = status == 403 || status == 404 || status == 405;
        if (refused) {
            s_fs_write_refused_us = esp_timer_get_time();
            strlcpy(s_fs_write_refusal, message, sizeof(s_fs_write_refusal));
        }
        if (job->delivery == 'f' || !refused) {
            cJSON_Delete(result);
            return error_result("upload_failed", message);
        }
    }
    // Auto mode after a refusal: say why the body came back inline.
    if (job->delivery == 'a' && s_fs_write_refusal[0]) {
        cJSON_AddStringToObject(payload, "file_error", s_fs_write_refusal);
    }
    if (!add_inline(payload, d, message, sizeof(message))) {
        cJSON_Delete(result);
        return error_result("too_large", message);
    }
    cJSON_AddStringToObject(payload, "delivery", "inline");
    ESP_LOGI(TAG, "returning %u bytes inline", (unsigned)d->len);
    return result;
}

static void fetch_task(void *arg) {
    job_t *job = arg;
    int64_t start = esp_timer_get_time();
    job->deadline_us = start + (int64_t)job->timeout_ms * 1000;
    download_t d = {0};
    cJSON *result = NULL;

    const char *host_error = NULL;
    {
        bool https, userinfo;
        const char *host, *rest;
        size_t host_len;
        char host_buf[128];
        split_url(job->url, &https, &host, &host_len, &userinfo, &rest);
        snprintf(host_buf, sizeof(host_buf), "%.*s", (int)host_len, host);
        host_error = check_private_host(host_buf);
    }
    if (host_error) {
        result = error_result(host_error, !strcmp(host_error, "dns_failed")
            ? "could not resolve the host"
            : "the host is not on the home network (private IPv4 addresses only)");
    } else if (!download(job, &d)) {
        result = error_result(d.code, d.message);
    }
    int fetch_ms = (int)((esp_timer_get_time() - start) / 1000);
    free_secret(job->url);
    job->url = NULL;
    ESP_LOGI(TAG, "GET %s: %s, %u bytes in %d ms", job->shown_url,
             result ? "failed" : "ok", (unsigned)d.len, fetch_ms);

    if (!result) {
        bool jpeg = is_jpeg(d.body, d.len);
        char path[PATH_MAX_LEN + 24];
        if (job->path) {
            strlcpy(path, job->path, sizeof(path));
        } else {
            snprintf(path, sizeof(path), "lan-fetch/latest.%s", extension(job->content_type, jpeg));
        }
        uint8_t hash[32];
        char hex[65];
        const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        bool hashed = md && mbedtls_md(md, d.body, d.len, hash) == 0;
        for (int i = 0; hashed && i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);

        result = deliver(job, &d, path, jpeg, hashed ? hex : NULL, fetch_ms);
    }
    free(d.body);

    ESP_LOGI(TAG, "done: int=%uK free, stack %u free", (unsigned)(internal_free() / 1024),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (job->request_id[0]) {
        noise_ctrl_send_command_result(job->session_generation, job->request_id, result);
    } else {
        char *text = cJSON_PrintUnformatted(result);   // from the console
        printf("@lan.fetch %s\n", text ? text : "{}");
        free(text);
        cJSON_Delete(result);
    }
    job_free(job);
    atomic_store(&s_busy, false);
    vTaskDelete(NULL);
}

// ---- Command -----------------------------------------------------------------

static char *string_param(cJSON *params, const char *key, const char **error) {
    cJSON *item = cJSON_GetObjectItem(params, key);
    if (!item || cJSON_IsNull(item)) return NULL;
    if (!cJSON_IsString(item) || !item->valuestring) {
        *error = key;
        return NULL;
    }
    return strdup(item->valuestring);
}

static bool int_param(cJSON *params, const char *key, int lo, int hi, int *out) {
    cJSON *item = cJSON_GetObjectItem(params, key);
    if (!item || cJSON_IsNull(item)) return true;
    if (!cJSON_IsNumber(item) || item->valuedouble < lo || item->valuedouble > hi) return false;
    *out = (int)item->valuedouble;
    return true;
}

static bool path_ok(const char *path) {
    size_t n = strlen(path);
    if (!n || n > PATH_MAX_LEN || path[0] == '/' || path[n - 1] == '/') return false;
    for (const char *p = path; *p; p++) {
        if ((unsigned char)*p < 0x20) return false;
    }
    // No ".." segment: the file stays under the user's home.
    for (const char *seg = path; seg; seg = strchr(seg, '/') ? strchr(seg, '/') + 1 : NULL) {
        if (strncmp(seg, "..", 2) == 0 && (seg[2] == '/' || seg[2] == '\0')) return false;
    }
    return true;
}

cJSON *lan_fetch_command(cJSON *params, const char *request_id,
                         noise_ctrl_session_generation_t session_generation) {
    cJSON *url_item = params ? cJSON_GetObjectItem(params, "url") : NULL;
    if (!cJSON_IsString(url_item) || !url_item->valuestring || !url_item->valuestring[0]) {
        return error_result("missing_param", "url is required");
    }
    bool https, userinfo;
    const char *host, *rest;
    size_t host_len;
    if (!split_url(url_item->valuestring, &https, &host, &host_len, &userinfo, &rest)
        || host_len >= 128) {
        return error_result("invalid_params", "url must be http://host/... or https://host/...");
    }
    int max_bytes = DEFAULT_MAX_BYTES, timeout_ms = DEFAULT_TIMEOUT_MS;
    if (!int_param(params, "max_bytes", MIN_MAX_BYTES, HARD_MAX_BYTES, &max_bytes)) {
        return error_result("invalid_params", "max_bytes must be 1024..2097152");
    }
    if (!int_param(params, "timeout_ms", MIN_TIMEOUT_MS, MAX_TIMEOUT_MS, &timeout_ms)) {
        return error_result("invalid_params", "timeout_ms must be 1000..60000");
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        return error_result("busy", "another lan.fetch is still running");
    }
    if (!s_up.lock && !(s_up.lock = xSemaphoreCreateMutex())) {
        atomic_store(&s_busy, false);
        return error_result("out_of_memory", "failed to allocate");
    }
    size_t stack = https ? STACK_TLS : STACK_HTTP;
    size_t need = stack + RESERVE_BYTES + NOISE_TX_DMA_RESERVE_BYTES
                + (https ? TLS_BYTES : HTTP_BYTES);
    if (internal_free() < need || heap_caps_get_largest_free_block(BYTE_CAPS) < stack) {
        atomic_store(&s_busy, false);
        return error_result("out_of_memory", https
            ? "not enough free memory for an HTTPS fetch; try http://"
            : "not enough free memory for a fetch");
    }

    const char *bad = NULL;
    job_t *job = calloc(1, sizeof(*job));
    if (job) {
        job->url = strdup(url_item->valuestring);
        job->username = string_param(params, "username", &bad);
        job->password = string_param(params, "password", &bad);
        job->path = string_param(params, "path", &bad);
        char *delivery = string_param(params, "delivery", &bad);
        job->delivery = !delivery || !strcmp(delivery, "auto") ? 'a'
                      : !strcmp(delivery, "file") ? 'f'
                      : !strcmp(delivery, "inline") ? 'i' : '?';
        free(delivery);
        job->shown_url = shown_url(url_item->valuestring, https, userinfo, rest);
    }
    const char *code = NULL, *message = NULL;
    if (!job || !job->url || !job->shown_url) {
        code = "out_of_memory";
        message = "failed to allocate";
    } else if (bad) {
        code = "invalid_params";
        message = !strcmp(bad, "path") ? "path must be a string"
                : !strcmp(bad, "username") ? "username must be a string"
                : !strcmp(bad, "delivery") ? "delivery must be a string"
                : "password must be a string";
    } else if (job->password && !job->username) {
        code = "invalid_params";
        message = "password needs a username";
    } else if (job->delivery == '?') {
        code = "invalid_params";
        message = "delivery must be auto, file or inline";
    } else if (job->path && !path_ok(job->path)) {
        code = "invalid_params";
        message = "path must be relative, at most 200 characters, with no .. segments";
    }
    if (code) {
        job_free(job);
        atomic_store(&s_busy, false);
        return error_result(code, message);
    }
    job->session_generation = session_generation;
    strlcpy(job->request_id, request_id, sizeof(job->request_id));
    job->max_bytes = (size_t)max_bytes;
    job->timeout_ms = timeout_ms;
    job->https = https;
    job->login = userinfo || job->username;

    if (xTaskCreate(fetch_task, "lan_fetch", stack, job, 4, NULL) != pdPASS) {
        job_free(job);
        atomic_store(&s_busy, false);
        return error_result("out_of_memory", "failed to start the fetch task");
    }
    cJSON *async = cJSON_CreateObject();
    cJSON_AddBoolToObject(async, "_async", true);
    return async;
}

bool lan_fetch_console(const char *line) {
    if (strncmp(line, "lan.fetch ", 10) != 0) return false;
    cJSON *params = cJSON_Parse(line + 10);
    cJSON *result = params ? lan_fetch_command(params, "", 0)
                           : error_result("invalid_params", "expected lan.fetch {json}");
    cJSON_Delete(params);
    if (!cJSON_IsTrue(cJSON_GetObjectItem(result, "_async"))) {
        char *text = cJSON_PrintUnformatted(result);
        printf("@lan.fetch %s\n", text ? text : "{}");
        free(text);
    }
    cJSON_Delete(result);
    return true;
}

#else

bool lan_fetch_console(const char *line) {
    (void)line;
    return false;
}

#endif  // CONFIG_HOMEHUB_LAN_FETCH
