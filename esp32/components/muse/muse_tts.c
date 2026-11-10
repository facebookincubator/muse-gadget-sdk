/*
 * Copyright (c) 2026 ledienbien-ai
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

/*
 * Fetches a reply's speech (muse_tts.h). A task of its own does the HTTPS
 * requests, so the chat session's task never waits on them: it asks with
 * muse_tts_speak() and picks the MP3 up with muse_tts_read() as it lands in
 * a stream buffer. Each request has a ticket; asking again or cancelling
 * makes a new one, and the task drops whatever belongs to an old one.
 */
#include "muse_tts.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "muse_tts";

#define TEXT_MAX 1024              /* a reply message, as muse_chat_session.cpp keeps it */
#define MP3_BYTES (48 * 1024)      /* fetched and not yet read: 6 s at the 64 kbps it comes in */
#define READ_BYTES 1024
#define HTTP_TIMEOUT_MS 8000
#define TASK_STACK (12 * 1024)     /* a TLS handshake runs on it */
/* The HTTP client's two buffers. The request line has to fit in one; and at
 * this size both come from PSRAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL), not
 * from the internal RAM that Wi-Fi and Bluetooth are short of. */
#define HTTP_BUF_BYTES (MUSE_TTS_URL_MAX + 3072)

static TaskHandle_t s_task;
static StreamBufferHandle_t s_mp3;
static SemaphoreHandle_t s_lock;           /* s_text, s_lang */
static char *s_text;                       /* the reply to speak, cleaned */
static char s_lang[8];
/* The task's own, in PSRAM with s_text. */
static struct work {
    char text[TEXT_MAX];
    char chunk[MUSE_TTS_CHUNK_CHARS * 4 + 1];
    char url[MUSE_TTS_URL_MAX];
    char buf[READ_BYTES];
} *s_work;
static atomic_uint s_wanted;               /* the newest ticket */
static atomic_uint s_serving;              /* the ticket whose MP3 is in s_mp3, or 0 */
static atomic_int s_state;                 /* of s_serving */
static atomic_bool s_writing;              /* the task is between two writes to s_mp3 */

static bool stale(uint32_t ticket)
{
    return atomic_load(&s_wanted) != ticket;
}

/* One piece of the reply. Returns the bytes of MP3 passed on, or -1. */
static int fetch(const char *chunk, const char *lang, uint32_t ticket)
{
    char *url = s_work->url, *buf = s_work->buf;
    if (!muse_tts_url(chunk, lang, url, sizeof(s_work->url))) {
        return -1;
    }
    const esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = HTTP_BUF_BYTES,
        .buffer_size_tx = HTTP_BUF_BYTES,
        /* The endpoint turns away clients that don't look like a browser. */
        .user_agent = "Mozilla/5.0",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return -1;
    }
    int total = -1;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        ESP_LOGW(TAG, "couldn't connect");
    } else if (esp_http_client_fetch_headers(c) < 0 || esp_http_client_get_status_code(c) != 200) {
        ESP_LOGW(TAG, "HTTP %d", esp_http_client_get_status_code(c));
    } else {
        total = 0;
        while (!stale(ticket)) {
            int n = esp_http_client_read(c, buf, READ_BYTES);
            if (n <= 0) {
                if (n < 0) {
                    ESP_LOGW(TAG, "read failed after %d bytes", total);
                }
                break;
            }
            /* The reader takes what it has room for; wait for it, but not
             * past a newer request. */
            for (int sent = 0; sent < n && !stale(ticket);) {
                sent += (int)xStreamBufferSend(s_mp3, buf + sent, (size_t)(n - sent), pdMS_TO_TICKS(50));
            }
            total += n;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return total;
}

static void tts_task(void *arg)
{
    (void)arg;
    char *text = s_work->text, *chunk = s_work->chunk;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        char lang[sizeof(s_lang)];
        /* The ticket and its text together: muse_tts_speak() changes both under the lock. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t ticket = atomic_load(&s_wanted);
        strlcpy(text, s_text, TEXT_MAX);
        strlcpy(lang, s_lang, sizeof(lang));
        xSemaphoreGive(s_lock);
        if (!text[0]) {
            continue;   /* cancelled */
        }

        /* Nothing reads s_mp3 while s_serving is 0, so it can be emptied. */
        atomic_store(&s_serving, 0);
        xStreamBufferReset(s_mp3);
        atomic_store(&s_state, MUSE_TTS_FETCHING);
        atomic_store(&s_writing, true);
        atomic_store(&s_serving, ticket);

        int bytes = 0, pieces = 0;
        bool failed = false;
        const char *p = text;
        while (!stale(ticket) && muse_tts_next_chunk(&p, chunk, sizeof(s_work->chunk))) {
            int n = fetch(chunk, lang, ticket);
            if (n <= 0) {
                failed = true;
                break;
            }
            bytes += n;
            pieces++;
        }
        if (!stale(ticket)) {
            ESP_LOGI(TAG, "%d bytes of speech in %d piece%s (%s)%s", bytes, pieces, pieces == 1 ? "" : "s", lang,
                     failed ? ", then a failure" : "");
            /* A reply that lost its later pieces still has its start to say. */
            atomic_store(&s_state, bytes ? MUSE_TTS_DONE : MUSE_TTS_FAILED);
        }
        atomic_store(&s_writing, false);
    }
}

bool muse_tts_start(void)
{
    if (s_task) {
        return true;
    }
    s_lock = xSemaphoreCreateMutex();
    s_mp3 = xStreamBufferCreateWithCaps(MP3_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_text = heap_caps_calloc(1, TEXT_MAX, MALLOC_CAP_SPIRAM);
    s_work = heap_caps_calloc(1, sizeof(*s_work), MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_mp3 || !s_text || !s_work ||
        xTaskCreatePinnedToCoreWithCaps(tts_task, "muse_tts", TASK_STACK, NULL, 4, &s_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
        s_task = NULL;
        return false;
    }
    return true;
}

uint32_t muse_tts_speak(const char *text, const char *lang)
{
    if (!s_task) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!muse_tts_clean(text, s_text, TEXT_MAX)) {
        /* Nothing to say (a reply of emoji, say). The task takes the empty
         * text as a cancel, which drops what it was fetching. */
        atomic_fetch_add(&s_wanted, 1);
        xSemaphoreGive(s_lock);
        xTaskNotifyGive(s_task);
        return 0;
    }
    strlcpy(s_lang, lang, sizeof(s_lang));
    uint32_t ticket = atomic_fetch_add(&s_wanted, 1) + 1;
    if (!ticket) {
        ticket = atomic_fetch_add(&s_wanted, 1) + 1;   /* 0 means no ticket */
    }
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return ticket;
}

void muse_tts_cancel(void)
{
    if (!s_task) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_text[0] = '\0';
    atomic_fetch_add(&s_wanted, 1);
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

size_t muse_tts_read(uint32_t ticket, uint8_t *buf, size_t cap)
{
    if (!ticket || atomic_load(&s_serving) != ticket) {
        return 0;
    }
    return xStreamBufferReceive(s_mp3, buf, cap, 0);
}

muse_tts_state_t muse_tts_state(uint32_t ticket)
{
    if (!ticket) {
        return MUSE_TTS_FAILED;
    }
    if (atomic_load(&s_serving) != ticket) {
        /* Not picked up yet, unless a newer request has taken its place. */
        return stale(ticket) ? MUSE_TTS_FAILED : MUSE_TTS_FETCHING;
    }
    if (atomic_load(&s_writing) || !xStreamBufferIsEmpty(s_mp3)) {
        return MUSE_TTS_FETCHING;
    }
    return (muse_tts_state_t)atomic_load(&s_state);
}
