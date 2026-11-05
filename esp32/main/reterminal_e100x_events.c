// SPDX-License-Identifier: Apache-2.0

#include "reterminal_e100x_events.h"
#include "reterminal_e100x_board.h"
#include "reterminal_e100x.h"

#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define EVENT_CAPACITY 32
#define EVENT_BATCH 16
#define EVENT_WAIT_MAX_MS 30000

typedef struct {
    uint64_t sequence;
    int64_t at_us;
    int key; // -1: SD detect; -2: E1003 touch.
    int gpio;
    bool active;
    uint32_t presses;
    int x, y;
} hardware_event_t;

static hardware_event_t s_history[EVENT_CAPACITY];
static uint64_t s_sequence;
static bool s_waiting;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_wait_signal;

static void record_event(int key, int gpio, bool active, uint32_t presses) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    uint64_t sequence = ++s_sequence;
    s_history[(sequence - 1) % EVENT_CAPACITY] =
        (hardware_event_t){.sequence = sequence, .at_us = now, .key = key,
                           .gpio = gpio, .active = active, .presses = presses};
    portEXIT_CRITICAL(&s_lock);
    if (s_wait_signal) xSemaphoreGive(s_wait_signal);
}

void reterminal_e100x_events_button(int key, int gpio, bool pressed, uint32_t presses) {
    if (key < 0 || key > 2) return;
    record_event(key, gpio, pressed, presses);
}

void reterminal_e100x_events_sd(bool inserted) {
    record_event(-1, E100X_SD_DETECT_GPIO, inserted, 0);
}

void reterminal_e100x_events_touch(bool pressed, int x, int y) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    uint64_t sequence = ++s_sequence;
    s_history[(sequence - 1) % EVENT_CAPACITY] =
        (hardware_event_t){.sequence = sequence, .at_us = now, .key = -2,
                           .gpio = E100X_TOUCH_INT_GPIO, .active = pressed, .x = x, .y = y};
    portEXIT_CRITICAL(&s_lock);
    if (s_wait_signal) xSemaphoreGive(s_wait_signal);
}

static uint64_t latest_sequence(void) {
    portENTER_CRITICAL(&s_lock);
    uint64_t latest = s_sequence;
    portEXIT_CRITICAL(&s_lock);
    return latest;
}

static cJSON *read_events(uint64_t after) {
    hardware_event_t batch[EVENT_BATCH];
    size_t count = 0;
    portENTER_CRITICAL(&s_lock);
    uint64_t latest = s_sequence;
    uint64_t first = latest >= EVENT_CAPACITY ? latest - EVENT_CAPACITY + 1 : 1;
    bool reset = after > latest;
    bool lost = !reset && after < first - 1;
    uint64_t start = reset || lost ? first : after + 1;
    for (uint64_t i = start; i <= latest && count < EVENT_BATCH; i++) {
        batch[count++] = s_history[(i - 1) % EVENT_CAPACITY];
    }
    portEXIT_CRITICAL(&s_lock);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddBoolToObject(result, "history_lost", lost);
    cJSON_AddBoolToObject(result, "cursor_reset", reset);
    uint64_t next = count ? batch[count - 1].sequence : latest;
    cJSON_AddNumberToObject(result, "next_sequence", (double)next);
    cJSON_AddBoolToObject(result, "has_more", next < latest);
    cJSON *events = cJSON_AddArrayToObject(result, "events");
    for (size_t i = 0; i < count; i++) {
        const hardware_event_t *event = &batch[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "sequence", (double)event->sequence);
        cJSON_AddNumberToObject(item, "uptime_ms", (double)(event->at_us / 1000));
        cJSON_AddStringToObject(item, "type", event->key == -2 ? "touch.changed" :
                                event->key == -1 ? "sd.changed" : "button.changed");
        cJSON_AddNumberToObject(item, "gpio", event->gpio);
        if (event->key == -2) {
            cJSON_AddBoolToObject(item, "pressed", event->active);
            cJSON_AddNumberToObject(item, "x", event->x);
            cJSON_AddNumberToObject(item, "y", event->y);
        } else if (event->key == -1) {
            cJSON_AddBoolToObject(item, "inserted", event->active);
        } else {
            static const char *names[] = {"key0", "key1", "key2"};
            cJSON_AddStringToObject(item, "key", names[event->key]);
            cJSON_AddBoolToObject(item, "pressed", event->active);
            cJSON_AddNumberToObject(item, "presses", event->presses);
        }
        cJSON_AddItemToArray(events, item);
    }
    return reterminal_e100x_data_reply(result);
}

typedef struct {
    uint64_t after;
    int timeout_ms;
    noise_ctrl_session_generation_t generation;
    char request_id[64];
} wait_args_t;

static void wait_task(void *arg) {
    wait_args_t *args = arg;
    int64_t deadline = esp_timer_get_time() + args->timeout_ms * 1000LL;
    while (latest_sequence() == args->after && esp_timer_get_time() < deadline) {
        if (!noise_ctrl_is_connected()) break;
        int64_t remaining_ms = (deadline - esp_timer_get_time() + 999) / 1000;
        int wait_ms = remaining_ms < 1000 ? (int)remaining_ms : 1000;
        if (wait_ms > 0) xSemaphoreTake(s_wait_signal, pdMS_TO_TICKS(wait_ms));
    }
    cJSON *result = read_events(args->after);
    noise_ctrl_send_command_result(args->generation, args->request_id, result);
    free(args);
    portENTER_CRITICAL(&s_lock);
    s_waiting = false;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

cJSON *reterminal_e100x_events_command(const char *command, const cJSON *params,
                            const char *request_id,
                            noise_ctrl_session_generation_t generation) {
    const cJSON *cursor = cJSON_GetObjectItemCaseSensitive(params, "after");
    uint64_t after = latest_sequence();
    if (cursor) {
        if (!cJSON_IsNumber(cursor) || !(cursor->valuedouble >= 0)
            || cursor->valuedouble > 9007199254740991.0
            || (double)(uint64_t)cursor->valuedouble != cursor->valuedouble) {
            return reterminal_e100x_error("invalid_param", "after must be a nonnegative integer cursor");
        }
        after = (uint64_t)cursor->valuedouble;
    }
    if (strcmp(command, "events.read") == 0) return read_events(after);
    if (strcmp(command, "events.wait") != 0) return reterminal_e100x_error("unsupported", "unknown event command");
    int timeout_ms = 25000;
    const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(params, "timeout_ms");
    if (timeout) {
        if (!cJSON_IsNumber(timeout) || timeout->valuedouble != timeout->valueint
            || timeout->valueint < 1 || timeout->valueint > EVENT_WAIT_MAX_MS) {
            return reterminal_e100x_error("invalid_param", "timeout_ms must be 1..30000");
        }
        timeout_ms = timeout->valueint;
    }
    if (latest_sequence() != after) return read_events(after);
    if (!generation || !request_id || strlen(request_id) >= 64) {
        return reterminal_e100x_error("unavailable", "events.wait requires a cloud command request");
    }
    if (!s_wait_signal) {
        SemaphoreHandle_t created = xSemaphoreCreateBinary();
        if (created) {
            portENTER_CRITICAL(&s_lock);
            if (!s_wait_signal) s_wait_signal = created;
            portEXIT_CRITICAL(&s_lock);
            if (s_wait_signal != created) vSemaphoreDelete(created);
        }
    }
    if (!s_wait_signal) return reterminal_e100x_error("out_of_memory", "cannot allocate event wait signal");
    wait_args_t *args = calloc(1, sizeof(*args));
    cJSON *result = cJSON_CreateObject();
    if (!args || !result || !cJSON_AddBoolToObject(result, "_async", true)) {
        free(args); cJSON_Delete(result);
        return reterminal_e100x_error("out_of_memory", "cannot allocate event wait");
    }
    portENTER_CRITICAL(&s_lock);
    bool busy = s_waiting;
    if (!busy) s_waiting = true;
    portEXIT_CRITICAL(&s_lock);
    if (busy) {
        free(args);
        cJSON_Delete(result);
        return reterminal_e100x_error("busy", "one events.wait is already pending");
    }
    args->after = after;
    args->timeout_ms = timeout_ms;
    args->generation = generation;
    strcpy(args->request_id, request_id);
    if (xTaskCreate(wait_task, "e100x_events", 4096, args, 2, NULL) != pdPASS) {
        free(args);
        cJSON_Delete(result);
        portENTER_CRITICAL(&s_lock);
        s_waiting = false;
        portEXIT_CRITICAL(&s_lock);
        return reterminal_e100x_error("out_of_memory", "cannot start event wait");
    }
    return result;
}
