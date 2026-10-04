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

#include "x3_pages.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_status.h"
#include "nvs.h"
#include "stack_monitor.h"
#include "x3_display.h"

static const char *TAG = "link.pages";

#define MAX_PAGES  6
#define NVS_NS     "x3pages"
#define NVS_KEY    "pages"
#define NVS_FORMAT 1

typedef struct {
    char name[16];
    char title[28];
    char text[468];
} page_t;

typedef struct {
    uint8_t format, count;
    page_t pages[MAX_PAGES];
} store_t;

// What a new device scrolls through until the agent sets its own.
static const page_t s_samples[] = {
    {"priorities", "Priorities",
     "[ ] First thing\n[ ] Second thing\n[x] Pair with Muse\n\n"
     "Sample page. Ask Muse:\n\"Put my priorities on my X3\""},
    {"today", "Today",
     "09:30  An event\n13:00  Another event\n\n"
     "Sample page. Ask Muse:\n\"Show my day on my X3\""},
    {"note", "Note",
     "A page for anything: a shopping list, a reminder, a quote.\n\n"
     "Sample page. Ask Muse:\n\"Write a note on my X3\""},
};

static store_t s_store;
static SemaphoreHandle_t s_lock;
// 0 is the status screen; 1..count are the pages.
static int s_position;

static void save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, NVS_KEY, &s_store, sizeof(s_store)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void load(void) {
    nvs_handle_t h;
    size_t len = sizeof(s_store);
    bool ok = nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK;
    if (ok) {
        ok = nvs_get_blob(h, NVS_KEY, &s_store, &len) == ESP_OK && len == sizeof(s_store)
             && s_store.format == NVS_FORMAT && s_store.count <= MAX_PAGES;
        nvs_close(h);
    }
    if (ok) return;
    memset(&s_store, 0, sizeof(s_store));
    s_store.format = NVS_FORMAT;
    s_store.count = sizeof(s_samples) / sizeof(s_samples[0]);
    memcpy(s_store.pages, s_samples, sizeof(s_samples));
}

// Show the stop at s_position. Caller holds s_lock.
static void show(void) {
    if (s_position == 0) {
        led_status_show_animation();
        return;
    }
    const page_t *p = &s_store.pages[s_position - 1];
    x3_display_show_page(p->title, p->text, s_position, s_store.count);
}

static int find(const char *name) {
    for (int i = 0; i < s_store.count; i++) {
        if (strcasecmp(s_store.pages[i].name, name) == 0) return i;
    }
    return -1;
}

bool x3_pages_set(const char *name, const char *title, const char *text) {
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find(name);
    if (i < 0 && s_store.count < MAX_PAGES) i = s_store.count++;
    if (i >= 0) {
        page_t *p = &s_store.pages[i];
        snprintf(p->name, sizeof(p->name), "%s", name);
        snprintf(p->title, sizeof(p->title), "%s", title && title[0] ? title : name);
        snprintf(p->text, sizeof(p->text), "%s", text);
        save();
        s_position = i + 1;
        show();
    }
    xSemaphoreGive(s_lock);
    return i >= 0;
}

bool x3_pages_clear(const char *name) {
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find(name);
    if (i >= 0) {
        memmove(&s_store.pages[i], &s_store.pages[i + 1],
                (size_t)(s_store.count - 1 - i) * sizeof(page_t));
        s_store.count--;
        memset(&s_store.pages[s_store.count], 0, sizeof(page_t));
        save();
        if (s_position > s_store.count) s_position = s_store.count;
        show();
    }
    xSemaphoreGive(s_lock);
    return i >= 0;
}

// ---- Buttons ----------------------------------------------------------------

// Two resistor ladders, each on one ADC pin: Back, Confirm, Left and Right on
// GPIO1, Up and Down on GPIO2. Thresholds are the FreeInk SDK's
// (InputManager.cpp), in raw 12-bit counts at 12 dB attenuation.
typedef enum { BTN_NONE, BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN } btn_t;

static const char *const s_btn_names[] = {"none", "back", "confirm", "left", "right", "up", "down"};

static btn_t classify(int front, int side) {
    if (side <= 1120) return BTN_DOWN;
    if (side <= 3900) return BTN_UP;
    if (front <= 750) return BTN_RIGHT;
    if (front <= 2090) return BTN_LEFT;
    if (front <= 3100) return BTN_CONFIRM;
    if (front <= 3900) return BTN_BACK;
    return BTN_NONE;
}

static void on_button(btn_t btn) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int stops = s_store.count + 1;
    switch (btn) {
        case BTN_DOWN:
        case BTN_RIGHT: s_position = (s_position + 1) % stops; break;
        case BTN_UP:
        case BTN_LEFT:  s_position = (s_position + stops - 1) % stops; break;
        case BTN_BACK:  s_position = 0; break;
        default: break;
    }
    show();
    xSemaphoreGive(s_lock);
}

static void buttons_task(void *arg) {
    adc_oneshot_unit_handle_t adc = arg;
    stack_monitor_t stack = STACK_MONITOR_INIT;
    btn_t last = BTN_NONE, held = BTN_NONE;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        int front = 4095, side = 4095;
        adc_oneshot_read(adc, ADC_CHANNEL_1, &front);
        adc_oneshot_read(adc, ADC_CHANNEL_2, &side);
        btn_t now = classify(front, side);
        // The same reading twice in a row counts; a press acts once.
        if (now == last && now != held) {
            held = now;
            if (now != BTN_NONE) {
                ESP_LOGI(TAG, "button %s (front=%d side=%d)", s_btn_names[now], front, side);
                on_button(now);
                stack_monitor_poll(&stack);
            }
        }
        last = now;
    }
}

void x3_pages_start(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;
    load();
    adc_oneshot_unit_handle_t adc;
    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    const adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    if (adc_oneshot_new_unit(&unit, &adc) != ESP_OK
        || adc_oneshot_config_channel(adc, ADC_CHANNEL_1, &chan) != ESP_OK
        || adc_oneshot_config_channel(adc, ADC_CHANNEL_2, &chan) != ESP_OK) {
        ESP_LOGE(TAG, "button ADC init failed");
        return;
    }
    if (xTaskCreate(buttons_task, "x3btn", 3072, adc, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the button task");
        return;
    }
    ESP_LOGI(TAG, "%d pages; up/down scroll, back returns to the status screen", s_store.count);
}
