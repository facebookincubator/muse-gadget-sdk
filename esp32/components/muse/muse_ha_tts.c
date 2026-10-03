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
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "muse_ha_tts";

#define HTTP_TIMEOUT_MS 15000
#define RESP_MAX 1024                      /* tts_get_url's JSON: a URL and a path */
#define MP3_BYTES (32 * 1024)              /* between the HTTP task and the session */
#define CHUNK 2048
#define SEND_WAIT_MS 50                    /* how often a full buffer checks for a cancel */

static QueueHandle_t s_reqs;               /* char *: the text to fetch, in PSRAM, freed by the task */
static StreamBufferHandle_t s_mp3;
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

static esp_http_client_handle_t client_for(const char *url, esp_http_client_method_t method)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = CHUNK,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* only used for https */
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) {
        esp_http_client_set_header(c, "Authorization", s_auth);
    }
    return c;
}

/* POSTs JSON to HA. Returns the HTTP status (0 if it couldn't connect); the
 * response body lands in resp, if given. */
static int post_json(const char *path, cJSON *body, char *resp, size_t resp_cap)
{
    if (resp) {
        resp[0] = '\0';   /* the callers log it, even when nothing came back */
    }
    char url[320];
    snprintf(url, sizeof(url), "%s%s", s_base, path);
    char *json = cJSON_PrintUnformatted(body);
    if (!json) {
        return 0;
    }
    int status = 0;
    esp_http_client_handle_t c = client_for(url, HTTP_METHOD_POST);
    if (!c) {
        cJSON_free(json);
        return 0;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    int len = (int)strlen(json);
    esp_err_t err = esp_http_client_open(c, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "can't reach %s: %s", s_base, esp_err_to_name(err));
        goto out;
    }
    if (esp_http_client_write(c, json, len) != len || esp_http_client_fetch_headers(c) < 0) {
        ESP_LOGW(TAG, "POST %s failed", path);
        goto out;
    }
    status = esp_http_client_get_status_code(c);
    if (resp) {
        int got = 0, n;
        while (got < (int)resp_cap - 1 && (n = esp_http_client_read(c, resp + got, resp_cap - 1 - got)) > 0) {
            got += n;
        }
        resp[got] = '\0';
    }
out:
    esp_http_client_cleanup(c);
    cJSON_free(json);
    return status;
}

static cJSON *tts_body(const char *text)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "message", text);
    if (CONFIG_HA_TTS_LANGUAGE[0]) {
        cJSON_AddStringToObject(body, "language", CONFIG_HA_TTS_LANGUAGE);
    }
    return body;
}

/* Hands MP3 to the session, waiting while its buffer is full. False on a cancel. */
static bool hand_over(const uint8_t *data, size_t len)
{
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

static bool fetch(const char *text)
{
    /* 1. Have HA render it: tts_get_url answers with where the MP3 is. */
    cJSON *body = tts_body(text);
    cJSON_AddStringToObject(body, "engine_id", CONFIG_HA_TTS_ENGINE);
    char *resp = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        cJSON_Delete(body);
        return false;
    }
    int status = post_json("/api/tts_get_url", body, resp, RESP_MAX);
    cJSON_Delete(body);
    if (status != 200) {
        ESP_LOGW(TAG, "tts_get_url: HTTP %d %.120s", status, resp);
        free(resp);
        return false;
    }

    char url[320];
    cJSON *json = cJSON_Parse(resp);
    bool have = muse_ha_tts_audio_url(url, sizeof(url), s_base,
                                      cJSON_GetStringValue(cJSON_GetObjectItem(json, "path")),
                                      cJSON_GetStringValue(cJSON_GetObjectItem(json, "url")));
    cJSON_Delete(json);
    free(resp);
    if (!have) {
        ESP_LOGW(TAG, "tts_get_url: no url in the response");
        return false;
    }
    if (atomic_load(&s_cancel)) {
        return false;
    }

    /* 2. Stream the MP3 to the session. */
    esp_http_client_handle_t c = client_for(url, HTTP_METHOD_GET);
    if (!c) {
        return false;
    }
    bool ok = false;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "can't fetch %s: %s", url, esp_err_to_name(err));
        goto out;
    }
    esp_http_client_fetch_headers(c);
    status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(TAG, "GET %s: HTTP %d", url, status);
        goto out;
    }
    static uint8_t buf[CHUNK];
    bool first = true;
    size_t skip = 0;                       /* what's left of an ID3v2 tag */
    int n;
    while ((n = esp_http_client_read(c, (char *)buf, sizeof(buf))) > 0) {
        if (first) {
            first = false;
            if (n >= 4 && !memcmp(buf, "RIFF", 4)) {
                ESP_LOGW(TAG, "HA sent WAV, not MP3; set the engine's output to MP3");
                goto out;
            }
            /* HA tags its MP3 (ID3v2): skip the tag so the decoder never
             * mistakes its bytes for a frame. */
            skip = muse_ha_tts_id3_size(buf, n);
        }
        size_t drop = skip < (size_t)n ? skip : (size_t)n;
        skip -= drop;
        if (!hand_over(buf + drop, n - drop)) {
            goto out;
        }
    }
    ok = n == 0 && esp_http_client_is_complete_data_received(c);
    if (!ok) {
        ESP_LOGW(TAG, "MP3 download cut off");
    }
out:
    esp_http_client_cleanup(c);
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
    s_mp3 = xStreamBufferCreateWithCaps(MP3_BYTES, 1, MALLOC_CAP_SPIRAM);
    /* Stack in PSRAM, as the session's: TLS (for an https URL) runs here. */
    if (!s_auth || !s_reqs || !s_mp3 ||
        xTaskCreatePinnedToCoreWithCaps(tts_task, "ha_tts", 16 * 1024, NULL, 4, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
        s_reqs = NULL;
        return;
    }
    snprintf(s_auth, auth_len, "Bearer %s", CONFIG_HA_TTS_TOKEN);
    ESP_LOGI(TAG, "replies speak through %s (%s)", s_base, CONFIG_HA_TTS_ENGINE);
}

bool muse_ha_tts_fetch(const char *text)
{
    if (!s_reqs || atomic_load(&s_state) == MUSE_HA_TTS_RUNNING) {
        return false;
    }
    /* The task is idle, so nothing is blocked on the buffer: safe to reset. */
    xStreamBufferReset(s_mp3);
    atomic_store(&s_cancel, false);
    atomic_store(&s_bytes, 0);
    char *copy = text && text[0] ? psram_strdup(text) : NULL;
    if (!copy) {
        return false;
    }
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
}

#else  /* !CONFIG_HA_TTS */

void muse_ha_tts_start(void) {}
bool muse_ha_tts_fetch(const char *text) { (void)text; return false; }
size_t muse_ha_tts_read(void *buf, size_t cap) { (void)buf; (void)cap; return 0; }
muse_ha_tts_state_t muse_ha_tts_state(void) { return MUSE_HA_TTS_IDLE; }
size_t muse_ha_tts_bytes(void) { return 0; }
void muse_ha_tts_cancel(void) {}

#endif
