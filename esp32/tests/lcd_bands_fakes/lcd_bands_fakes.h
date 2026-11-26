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

/* Single-threaded fakes for boards/muse_lcd_bands.c on the host
 * (tests/test_lcd_bands_recovery.py). A wait that would block takes the
 * pieces the fake wire still has in flight first; a timed one then lets its
 * time pass and fails; an unbounded one aborts the harness: it would hang
 * the task for good. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
static inline const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "error"; }
#define IRAM_ATTR

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))   /* 1 ms ticks */
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define portENTER_CRITICAL_SAFE(m) ((void)(m))
#define portEXIT_CRITICAL_SAFE(m) ((void)(m))
#define portYIELD_FROM_ISR() ((void)0)

/* Time, in microseconds. */
extern int64_t fake_now_us;
static inline int64_t esp_timer_get_time(void) { return fake_now_us; }

/* Logs, kept for the test to read. */
extern char fake_log[16384];
void fake_logf(char level, const char *tag, const char *fmt, ...);
#define ESP_LOGI(tag, ...) fake_logf('I', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_logf('W', tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) fake_logf('E', tag, __VA_ARGS__)

/* Semaphores and queues. */
typedef struct fake_sem { int count, max; } *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateCounting(int max, int initial);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken);
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s);
typedef struct fake_queue { size_t item; int n, cap; unsigned char buf[8][64]; } *QueueHandle_t;
QueueHandle_t xQueueCreate(int cap, size_t item);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);

/* Tasks: none run; the test calls the send task's steps itself. */
typedef void *TaskHandle_t;
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)1; }
static inline BaseType_t xTaskGetCoreID(TaskHandle_t t) { (void)t; return 1; }
static inline UBaseType_t uxTaskPriorityGet(TaskHandle_t t) { (void)t; return 5; }
static inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 1024; }
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *out, BaseType_t core);
void vTaskDelay(TickType_t ticks);
#define MUSE_UI_CORE 1
#define MUSE_UI_PRIORITY 5

#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
static inline void *heap_caps_aligned_alloc(size_t align, size_t n, uint32_t caps)
{
    (void)align;
    (void)caps;
    return calloc(1, n);
}

/* GPIO (the TE line; unused here). */
typedef struct { uint64_t pin_bit_mask; int mode, intr_type; } gpio_config_t;
#define GPIO_MODE_INPUT 1
#define GPIO_INTR_ANYEDGE 3
static inline esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
static inline esp_err_t gpio_install_isr_service(int f) { (void)f; return ESP_OK; }
static inline esp_err_t gpio_isr_handler_add(int g, void (*fn)(void *), void *a) { (void)g; (void)fn; (void)a; return ESP_OK; }
static inline int gpio_get_level(int g) { (void)g; return 0; }

/* The panel and its IO: each piece goes on the fake wire. */
typedef struct fake_panel *esp_lcd_panel_handle_t;
typedef struct fake_io *esp_lcd_panel_io_handle_t;
typedef struct { int unused; } esp_lcd_panel_io_event_data_t;
typedef bool (*esp_lcd_panel_io_color_trans_done_cb_t)(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *);
typedef struct { esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done; } esp_lcd_panel_io_callbacks_t;
esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_io_callbacks_t *cbs,
                                                    void *ctx);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x1, int y1, int x2, int y2, const void *data);
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int cmd, const void *param, size_t n);
esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io, int cmd, const void *data, size_t n);

/* LVGL and the adapter. */
typedef struct fake_disp lv_display_t;
typedef struct fake_obj lv_obj_t;
typedef void (*lv_display_flush_wait_cb_t)(lv_display_t *);
typedef void (*lv_async_cb_t)(void *);
#define LV_COLOR_FORMAT_RGB565_SWAPPED 0x1b
void lv_display_set_color_format(lv_display_t *d, int f);
void lv_display_flush_ready(lv_display_t *d);
void lv_display_set_flush_wait_cb(lv_display_t *d, lv_display_flush_wait_cb_t cb);
int lv_async_call(lv_async_cb_t cb, void *arg);
lv_obj_t *lv_screen_active(void);
lv_obj_t *lv_layer_top(void);
lv_obj_t *lv_layer_sys(void);
void lv_obj_invalidate(lv_obj_t *o);
typedef struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t panel_io;
    struct { int buffer_height, ver_res; bool use_psram, require_double_buffer; } profile;
} esp_lv_adapter_display_config_t;
typedef esp_err_t (*esp_lv_adapter_draw_bitmap_cb_t)(lv_display_t *, esp_lcd_panel_handle_t, int, int, int, int,
                                                       const void *, void *);
typedef struct { esp_lv_adapter_draw_bitmap_cb_t custom_draw_bitmap; } esp_lv_adapter_draw_bitmap_callbacks_t;
void esp_lv_adapter_set_default_display_idf_callback_registration_enabled(bool on);
lv_display_t *esp_lv_adapter_register_display(const esp_lv_adapter_display_config_t *cfg);
esp_err_t esp_lv_adapter_set_draw_bitmap_callbacks(lv_display_t *d, const esp_lv_adapter_draw_bitmap_callbacks_t *cbs,
                                                   void *ctx);
bool esp_lv_adapter_display_notify_color_trans_done_from_isr(lv_display_t *d);

static inline void muse_mic_ring_apply(uint8_t *px, int stride, int x, int y, int w, int h)
{
    (void)px; (void)stride; (void)x; (void)y; (void)w; (void)h;
}
static inline bool muse_mic_ring_on(void)
{
    return false;
}
static inline void muse_mic_ring_apply_as(bool on, uint8_t *px, int stride, int x, int y, int w, int h)
{
    (void)on; (void)px; (void)stride; (void)x; (void)y; (void)w; (void)h;
}
/* The turn dot paints like the ring: nothing to check here. */
static inline void muse_turn_dot_apply(uint8_t *px, int stride, int x, int y, int w, int h)
{
    (void)px; (void)stride; (void)x; (void)y; (void)w; (void)h;
}
