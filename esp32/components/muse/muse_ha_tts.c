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

#include "muse_ha_tts.h"
#include "muse_ha_tts_text.h"

#include "sdkconfig.h"

#if CONFIG_HA_TTS

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "muse_ha_tts";

#define HTTP_TIMEOUT_MS 15000              /* connecting, and each read */
#define RESP_MAX 1024                      /* tts_get_url's JSON: a URL and a path */
#define MP3_BYTES (32 * 1024)              /* between the HTTP task and the session */
#define CHUNK 2048
#define SEND_WAIT_MS 50                    /* how often a full buffer checks for a cancel */

static QueueHandle_t s_reqs;               /* char *: the text to fetch, in PSRAM, freed by the task */
static StreamBufferHandle_t s_mp3;
/* Not xStreamBufferCreateWithCaps: IDF 6.0.1's vStreamBufferDeleteWithCaps
 * deletes with vSemaphoreDelete and frees twice (espressif/esp-idf#18855). */
static StaticStreamBuffer_t *s_mp3_struct;
static uint8_t *s_mp3_store;
static SemaphoreHandle_t s_lock;           /* guards s_sock */
static int s_sock = -1;                    /* the open request's socket, for muse_ha_tts_cancel */
static char *s_auth;                       /* "Bearer <token>" */
static char s_base[160];                   /* CONFIG_HA_TTS_URL without a trailing slash */
static atomic_int s_state = MUSE_HA_TTS_IDLE;
static atomic_bool s_cancel;
static atomic_size_t s_bytes;

static char *psram_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

/*
 * A cancel has to wake a read that's blocked, which esp_http_client's own
 * timeout doesn't do: it applies to each transport read, so a server that
 * trickles, or keeps sending chunk extensions and never a body byte, can hold
 * a read for as long as it likes. esp_http_client_cancel_request is no help
 * from another task: it closes the transport under the reader and reconnects.
 * Instead the open request's socket is published here, and a cancel shuts it
 * down: the read fails at once and the request unwinds on its own task.
 */

/* Publishes c's socket; false if a cancel came first. */
static bool watch(esp_http_client_handle_t c)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sock = esp_http_client_get_socket(c);
    xSemaphoreGive(s_lock);
    return !atomic_load(&s_cancel);   /* checked after publishing, so no cancel is missed */
}

/* Closes a request opened by open_request(), unpublishing its socket first so a
 * cancel never shuts down a descriptor that's since been reused. */
static void finish(esp_http_client_handle_t c)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sock = -1;
    xSemaphoreGive(s_lock);
    esp_http_client_cleanup(c);
}

/* Opens a request to `url`, sends `json` if given, and reads the headers.
 * Returns the request, with its HTTP status in *status, or NULL. A cancel
 * before open returns is seen only when it returns. HTTP_TIMEOUT_MS bounds the
 * TCP connect and individual TLS receives, not the whole blocking handshake or
 * DNS lookup; after open returns, cancellation shuts down its socket. */
static esp_http_client_handle_t open_request(const char *url, esp_http_client_method_t method, bool auth,
                                             const char *json, int *status)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = CHUNK,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* only used for https */
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return NULL;
    }
    if (auth) {
        esp_http_client_set_header(c, "Authorization", s_auth);
    }
    int len = json ? (int)strlen(json) : 0;
    if (json) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
    }
    esp_err_t err = esp_http_client_open(c, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "can't reach %s: %s", s_base, esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return NULL;
    }
    if (!watch(c) || (json && esp_http_client_write(c, json, len) != len) || esp_http_client_fetch_headers(c) < 0) {
        if (!atomic_load(&s_cancel)) {
            ESP_LOGW(TAG, "request to %s failed", s_base);
        }
        finish(c);
        return NULL;
    }
    *status = esp_http_client_get_status_code(c);
    return c;
}

/* Hands MP3 to the session, waiting while its buffer is full. False on a cancel. */
static bool hand_over(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    while (len) {
        if (atomic_load(&s_cancel)) {
            return false;
        }
        size_t n = xStreamBufferSend(s_mp3, data, len, pdMS_TO_TICKS(SEND_WAIT_MS));
        data += n;
        len -= n;
        atomic_fetch_add(&s_bytes, n);
    }
    return true;
}

/* Asks HA where the MP3 for `text` is (/api/tts_get_url): its URL in url. */
static bool audio_url(const char *text, char *url, size_t cap)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "engine_id", CONFIG_HA_TTS_ENGINE);
    cJSON_AddStringToObject(body, "message", text);
    if (CONFIG_HA_TTS_LANGUAGE[0]) {
        cJSON_AddStringToObject(body, "language", CONFIG_HA_TTS_LANGUAGE);
    }
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    char *resp = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json || !resp) {
        cJSON_free(json);
        free(resp);
        return false;
    }
    char post[320];
    snprintf(post, sizeof(post), "%s/api/tts_get_url", s_base);
    int status = 0, got = 0, n;
    esp_http_client_handle_t c = open_request(post, HTTP_METHOD_POST, true, json, &status);
    cJSON_free(json);
    if (!c) {
        free(resp);
        return false;
    }
    while (got < RESP_MAX - 1 && (n = esp_http_client_read(c, resp + got, RESP_MAX - 1 - got)) > 0) {
        got += n;
    }
    resp[got] = '\0';
    finish(c);
    if (status != 200) {
        ESP_LOGW(TAG, "tts_get_url: HTTP %d %.120s", status, resp);
        free(resp);
        return false;
    }
    cJSON *parsed = cJSON_Parse(resp);
    bool have = muse_ha_tts_audio_url(url, cap, s_base,
                                      cJSON_GetStringValue(cJSON_GetObjectItem(parsed, "path")),
                                      cJSON_GetStringValue(cJSON_GetObjectItem(parsed, "url")));
    cJSON_Delete(parsed);
    free(resp);
    if (!have) {
        ESP_LOGW(TAG, "tts_get_url: no url in the response");
    }
    return have;
}

static bool fetch(const char *text)
{
    /* 1. Have HA render it. */
    char url[320];
    if (!audio_url(text, url, sizeof(url)) || atomic_load(&s_cancel)) {
        return false;
    }

    /* 2. Stream the MP3 to the session. No token: the proxy URL is its own
     * credential, so the token doesn't go out twice. */
    int status = 0;
    esp_http_client_handle_t c = open_request(url, HTTP_METHOD_GET, false, NULL, &status);
    if (!c) {
        return false;
    }
    bool ok = false;
    if (status != 200) {
        ESP_LOGW(TAG, "GET %s: HTTP %d", url, status);
        goto out;
    }
    static uint8_t buf[CHUNK];
    muse_ha_tts_mp3_t mp3 = { 0 };
    muse_ha_tts_mp3_result_t r = MUSE_HA_TTS_MP3_OK;
    int n = 0;
    while (r == MUSE_HA_TTS_MP3_OK && (n = esp_http_client_read(c, (char *)buf, sizeof(buf))) > 0) {
        r = muse_ha_tts_mp3_feed(&mp3, buf, n, hand_over, NULL);
    }
    if (r == MUSE_HA_TTS_MP3_OK && n == 0 && esp_http_client_is_complete_data_received(c)) {
        r = muse_ha_tts_mp3_end(&mp3, hand_over, NULL);
        ok = r == MUSE_HA_TTS_MP3_OK;
    }
    if (r == MUSE_HA_TTS_MP3_WAV) {
        ESP_LOGW(TAG, "HA sent WAV, not MP3; set the engine's output to MP3");
    } else if (r == MUSE_HA_TTS_MP3_NOT_MP3) {
        ESP_LOGW(TAG, "HA's answer from %s isn't MP3", url);
    } else if (!ok && !atomic_load(&s_cancel)) {
        ESP_LOGW(TAG, "MP3 download cut off");
    }
out:
    finish(c);
    return ok;
}

static void tts_task(void *arg)
{
    (void)arg;
    for (;;) {
        char *text;
        xQueueReceive(s_reqs, &text, portMAX_DELAY);
        muse_ha_tts_clean(text);
        int64_t t0 = esp_timer_get_time();
        bool ok = fetch(text);
        if (ok) {
            ESP_LOGI(TAG, "fetched %u bytes of MP3 in %lld ms", (unsigned)atomic_load(&s_bytes),
                     (esp_timer_get_time() - t0) / 1000);
        }
        atomic_store(&s_state, ok ? MUSE_HA_TTS_OK : MUSE_HA_TTS_FAILED);
        free(text);
    }
}

/* Undoes a start that failed partway. */
static void stop(void)
{
    if (s_reqs) {
        vQueueDelete(s_reqs);
        s_reqs = NULL;
    }
    if (s_mp3) {
        vStreamBufferDelete(s_mp3);
        s_mp3 = NULL;
    }
    heap_caps_free(s_mp3_struct);
    heap_caps_free(s_mp3_store);
    s_mp3_struct = NULL;
    s_mp3_store = NULL;
    if (s_lock) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    heap_caps_free(s_auth);
    s_auth = NULL;
}

void muse_ha_tts_start(void)
{
    if (s_reqs) {
        return;
    }
    if (!CONFIG_HA_TTS_TOKEN[0]) {
        ESP_LOGW(TAG, "no Home Assistant token set; replies stay silent");
        return;
    }
    muse_ha_tts_base(s_base, sizeof(s_base), CONFIG_HA_TTS_URL);
    size_t auth_len = sizeof("Bearer ") + strlen(CONFIG_HA_TTS_TOKEN);
    s_auth = heap_caps_malloc(auth_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_reqs = xQueueCreate(1, sizeof(char *));   /* one fetch at a time */
    s_mp3_struct = heap_caps_calloc(1, sizeof(StaticStreamBuffer_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mp3_store = heap_caps_malloc(MP3_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_mp3_struct && s_mp3_store) {
        s_mp3 = xStreamBufferCreateStatic(MP3_BYTES, 1, s_mp3_store, s_mp3_struct);
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_auth || !s_reqs || !s_mp3 || !s_lock) {
        ESP_LOGE(TAG, "start failed");
        stop();
        return;
    }
    snprintf(s_auth, auth_len, "Bearer %s", CONFIG_HA_TTS_TOKEN);
    /* Stack in PSRAM, as the session's: TLS (for an https URL) runs here. */
    if (xTaskCreatePinnedToCoreWithCaps(tts_task, "ha_tts", 16 * 1024, NULL, 4, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
        stop();
        return;
    }
    if (!strncmp(s_base, "http://", 7)) {
        ESP_LOGW(TAG, "%s is plain HTTP: the token crosses your network unencrypted", s_base);
    }
    ESP_LOGI(TAG, "replies speak through %s (%s)", s_base, CONFIG_HA_TTS_ENGINE);
}

bool muse_ha_tts_fetch(const char *text)
{
    if (!s_reqs || atomic_load(&s_state) == MUSE_HA_TTS_RUNNING) {
        return false;
    }
    char *copy = text && text[0] ? psram_strdup(text) : NULL;
    if (!copy) {
        return false;
    }
    /* The task is idle, so nothing is blocked on the buffer: safe to reset. */
    xStreamBufferReset(s_mp3);
    atomic_store(&s_cancel, false);
    atomic_store(&s_bytes, 0);
    atomic_store(&s_state, MUSE_HA_TTS_RUNNING);
    if (xQueueSend(s_reqs, &copy, 0) != pdTRUE) {
        atomic_store(&s_state, MUSE_HA_TTS_IDLE);
        free(copy);
        return false;
    }
    return true;
}

size_t muse_ha_tts_read(void *buf, size_t cap)
{
    return s_mp3 ? xStreamBufferReceive(s_mp3, buf, cap, 0) : 0;
}

muse_ha_tts_state_t muse_ha_tts_state(void)
{
    return (muse_ha_tts_state_t)atomic_load(&s_state);
}

size_t muse_ha_tts_bytes(void)
{
    return atomic_load(&s_bytes);
}

void muse_ha_tts_cancel(void)
{
    atomic_store(&s_cancel, true);
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_sock >= 0) {
        shutdown(s_sock, SHUT_RDWR);   /* wakes a blocked read; finish() closes it */
    }
    xSemaphoreGive(s_lock);
}

#else  /* !CONFIG_HA_TTS */

void muse_ha_tts_start(void) {}
bool muse_ha_tts_fetch(const char *text) { (void)text; return false; }
size_t muse_ha_tts_read(void *buf, size_t cap) { (void)buf; (void)cap; return 0; }
muse_ha_tts_state_t muse_ha_tts_state(void) { return MUSE_HA_TTS_IDLE; }
size_t muse_ha_tts_bytes(void) { return 0; }
void muse_ha_tts_cancel(void) {}

#endif
