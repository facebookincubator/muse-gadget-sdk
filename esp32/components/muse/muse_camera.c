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

#include "muse_camera.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "muse_camera";

#define RX_CAP (256 * 1024)     /* a whole reply, image included: a 640x480 JPEG in base64 is ~80 KB */
#define POLL_MS 10
#define BOOT_WAIT_MS 4000       /* power-on to the first answer */
#define EVENTS 2
#define NOT_SUPPORTED 8         /* SSCMA's code for a command it lacks */
#define REPLY_NAME_MAX 24

/* ---- Frames (host-tested) ---- */

/*
 * Finds the first complete "\r{...}\n" frame in buf[0..len). Returns true with
 * the JSON at [*start, *end) (the braces included). *drop is how many leading
 * bytes can be discarded whether or not a frame was found: noise before a
 * frame, or everything but a possible partial marker.
 */
static bool find_frame(const char *buf, size_t len, size_t *start, size_t *end, size_t *drop)
{
    size_t s = 0;
    while (s + 1 < len && !(buf[s] == '\r' && buf[s + 1] == '{')) {
        s++;
    }
    if (s + 1 >= len) {
        *drop = len && buf[len - 1] == '\r' ? len - 1 : len;   /* keep a lone '\r' that may start one */
        return false;
    }
    *drop = s;
    for (size_t e = s + 2; e + 1 < len; e++) {
        if (buf[e] == '}' && buf[e + 1] == '\n') {
            *start = s + 1;
            *end = e + 1;
            return true;
        }
    }
    return false;
}

/* The command's name as the reply carries it: "MODEL=1" -> "MODEL", "ID?" -> "ID?". */
static void command_name(const char *cmd, char *name, size_t cap)
{
    size_t n = strcspn(cmd, "=\r\n");
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(name, cmd, n);
    name[n] = '\0';
}

/* ---- Frames end ---- */

static const muse_sscma_ops_t *s_io;
static SemaphoreHandle_t s_open_lock;
static SemaphoreHandle_t s_req_lock;
static SemaphoreHandle_t s_reply;
static QueueHandle_t s_events;
static portMUX_TYPE s_spin = portMUX_INITIALIZER_UNLOCKED;
static int s_opens;
static volatile bool s_run;
static TaskHandle_t s_reader;
static char *s_rx;
static size_t s_rx_len;
static uint8_t *s_chunk;
static char s_want[REPLY_NAME_MAX];      /* the reply awaited */
static cJSON *s_reply_data;
static int s_reply_code;
static volatile bool s_booted;

bool muse_camera_present(void)
{
    return muse_board && muse_board->sscma;
}

static void dispatch(cJSON *root)
{
    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsNumber(type) || !cJSON_IsString(name)) {
        return;
    }
    int c = cJSON_IsNumber(code) ? code->valueint : -1;
    if (type->valueint == 2) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        const char *text = cJSON_IsString(data) ? data->valuestring : name->valuestring;
        ESP_LOGI(TAG, "log: %s", text);
        /* A command it lacks gets this log rather than a reply: answer it now. */
        if (!strncmp(text, "Unknown command", 15)) {
            taskENTER_CRITICAL(&s_spin);
            bool waiting = s_want[0] != '\0';
            s_want[0] = '\0';
            taskEXIT_CRITICAL(&s_spin);
            if (waiting) {
                s_reply_data = cJSON_CreateString(text);
                s_reply_code = NOT_SUPPORTED;
                xSemaphoreGive(s_reply);
            }
        }
        return;
    }
    if (!strcmp(name->valuestring, "INIT@STAT?")) {
        s_booted = true;
    }
    cJSON *data = cJSON_DetachItemFromObject(root, "data");
    if (type->valueint == 0) {
        taskENTER_CRITICAL(&s_spin);
        bool wanted = s_want[0] && !strcmp(s_want, name->valuestring);
        if (wanted) {
            s_want[0] = '\0';
        }
        taskEXIT_CRITICAL(&s_spin);
        if (wanted) {
            s_reply_data = data;
            s_reply_code = c;
            xSemaphoreGive(s_reply);
            return;
        }
        cJSON_Delete(data);
        return;
    }
    /* An event: tag it with its name, and keep only the newest. */
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "name", name->valuestring);
    cJSON_AddNumberToObject(ev, "code", c);
    if (data) {
        cJSON_AddItemToObject(ev, "data", data);
    }
    if (xQueueSend(s_events, &ev, 0) != pdTRUE) {
        cJSON *old;
        if (xQueueReceive(s_events, &old, 0) == pdTRUE) {
            cJSON_Delete(old);
        }
        if (xQueueSend(s_events, &ev, 0) != pdTRUE) {
            cJSON_Delete(ev);
        }
    }
}

/* Takes every complete frame out of the buffer. */
static void drain(void)
{
    for (;;) {
        size_t start, end, drop;
        bool found = find_frame(s_rx, s_rx_len, &start, &end, &drop);
        if (found) {
            cJSON *root = cJSON_ParseWithLength(s_rx + start, end - start);
            if (root) {
                dispatch(root);
                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "unreadable frame (%u bytes)", (unsigned)(end - start));
            }
            drop = end + 1;
        }
        if (drop) {
            memmove(s_rx, s_rx + drop, s_rx_len - drop);
            s_rx_len -= drop;
        }
        if (!found) {
            return;
        }
    }
}

static void reader_task(void *arg)
{
    (void)arg;
    while (s_run) {
        size_t n = 0;
        if (s_io->available(&n) != ESP_OK || !n) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }
        while (n && s_run) {
            size_t take = n < MUSE_SSCMA_READ_MAX ? n : MUSE_SSCMA_READ_MAX;
            if (s_io->read(s_chunk, take) != ESP_OK) {
                break;
            }
            n -= take;
            /* The chip pads a short read with zeros; they never occur in a frame. */
            for (size_t i = 0; i < take; i++) {
                if (!s_chunk[i]) {
                    continue;
                }
                if (s_rx_len == RX_CAP) {
                    ESP_LOGW(TAG, "reply too large; dropped");
                    s_rx_len = 0;
                }
                s_rx[s_rx_len++] = (char)s_chunk[i];
            }
        }
        drain();
    }
    s_reader = NULL;
    vTaskSuspend(NULL);   /* muse_camera_close() deletes it, freeing its PSRAM stack */
}

static void flush_events(void)
{
    cJSON *ev;
    while (s_events && xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        cJSON_Delete(ev);
    }
}

static void power_down(void)
{
    s_run = false;
    TaskHandle_t reader = s_reader;
    /* It stops between transfers, none of which waits long for the bus. */
    for (int i = 0; i < 200 && s_reader; i++) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    if (reader) {
        vTaskDeleteWithCaps(reader);
    }
    s_reader = NULL;
    s_io->power(false);
    free(s_rx);
    free(s_chunk);
    s_rx = NULL;
    s_chunk = NULL;
    s_rx_len = 0;
    flush_events();
}

esp_err_t muse_camera_open(void)
{
    if (!muse_camera_present()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_open_lock) {
        s_open_lock = xSemaphoreCreateMutex();
        s_req_lock = xSemaphoreCreateMutex();
        s_reply = xSemaphoreCreateBinary();
        s_events = xQueueCreate(EVENTS, sizeof(cJSON *));
        if (!s_open_lock || !s_req_lock || !s_reply || !s_events) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_open_lock, portMAX_DELAY);
    if (s_opens++) {
        xSemaphoreGive(s_open_lock);
        return ESP_OK;
    }
    s_io = muse_board->sscma;
    s_rx = heap_caps_malloc(RX_CAP, MUSE_BIG_CAPS);
    s_chunk = heap_caps_malloc(MUSE_SSCMA_READ_MAX, MUSE_BIG_CAPS);
    s_rx_len = 0;
    s_booted = false;
    esp_err_t err = s_rx && s_chunk ? s_io->power(true) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        s_run = true;
        /* Stack in PSRAM where there is some: the reader never writes flash. */
        if (xTaskCreatePinnedToCoreWithCaps(reader_task, "muse_camera", 6144, NULL, 5, &s_reader, tskNO_AFFINITY,
                                            MUSE_BIG_CAPS) != pdPASS) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        /* It announces itself when booted; ask too, in case that went by. */
        int64_t deadline = esp_timer_get_time() + BOOT_WAIT_MS * 1000LL;
        err = ESP_ERR_TIMEOUT;
        while (esp_timer_get_time() < deadline) {
            xSemaphoreGive(s_open_lock);   /* requests don't need it */
            esp_err_t r = muse_camera_request("ID?", NULL, NULL, 500);
            xSemaphoreTake(s_open_lock, portMAX_DELAY);
            if (r == ESP_OK) {
                err = ESP_OK;
                break;
            }
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera didn't start: %s", esp_err_to_name(err));
        power_down();
        s_opens = 0;
    } else {
        ESP_LOGI(TAG, "camera up%s", s_booted ? " (boot seen)" : "");
    }
    xSemaphoreGive(s_open_lock);
    return err;
}

void muse_camera_close(void)
{
    if (!s_open_lock) {
        return;
    }
    xSemaphoreTake(s_open_lock, portMAX_DELAY);
    if (s_opens > 0 && --s_opens == 0) {
        power_down();
        ESP_LOGI(TAG, "camera off");
    }
    xSemaphoreGive(s_open_lock);
}

esp_err_t muse_camera_request(const char *cmd, cJSON **data, int *code, int timeout_ms)
{
    if (data) {
        *data = NULL;
    }
    if (!s_run) {
        return ESP_ERR_INVALID_STATE;
    }
    char line[256];
    int n = snprintf(line, sizeof(line), "AT+%s\r\n", cmd);
    if (n < 0 || n >= (int)sizeof(line)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    xSemaphoreTake(s_reply, 0);
    taskENTER_CRITICAL(&s_spin);
    command_name(cmd, s_want, sizeof(s_want));
    taskEXIT_CRITICAL(&s_spin);
    esp_err_t err = s_io->write(line, (size_t)n);
    if (err == ESP_OK) {
        err = xSemaphoreTake(s_reply, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    taskENTER_CRITICAL(&s_spin);
    s_want[0] = '\0';
    taskEXIT_CRITICAL(&s_spin);
    if (err == ESP_OK) {
        if (code) {
            *code = s_reply_code;
        }
        if (data) {
            *data = s_reply_data;
        } else {
            cJSON_Delete(s_reply_data);
        }
        s_reply_data = NULL;
        if (s_reply_code != 0) {
            err = ESP_FAIL;
        }
    }
    xSemaphoreGive(s_req_lock);
    return err;
}

esp_err_t muse_camera_event(const char *name, cJSON **data, int timeout_ms)
{
    *data = NULL;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
        cJSON *ev;
        if (left_ms <= 0 || xQueueReceive(s_events, &ev, pdMS_TO_TICKS(left_ms)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        cJSON *n = cJSON_GetObjectItem(ev, "name");
        if (cJSON_IsString(n) && !strcmp(n->valuestring, name)) {
            cJSON *code = cJSON_GetObjectItem(ev, "code");
            bool ok = !cJSON_IsNumber(code) || code->valueint == 0;
            *data = cJSON_DetachItemFromObject(ev, "data");
            cJSON_Delete(ev);
            return ok ? ESP_OK : ESP_FAIL;
        }
        cJSON_Delete(ev);
    }
}

void muse_camera_flush_events(void)
{
    flush_events();
}
