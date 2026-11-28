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
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
#define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) abort(); } while (0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define IRAM_ATTR
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4

typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef struct test_queue *QueueHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY UINT32_MAX
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueuePeek(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueSendToFront(QueueHandle_t, const void *, TickType_t);
/* muse_input's console post queue (the Nordic UART's !IN): none here, so
 * muse_input_console_post refuses and the input loop skips it. */
typedef struct { int unused; } StaticQueue_t;
static inline QueueHandle_t xQueueCreateStatic(unsigned n, unsigned size, uint8_t *buf, StaticQueue_t *q)
{
    (void)n; (void)size; (void)buf; (void)q;
    return NULL;
}
void vTaskDelay(int);
void vTaskDelete(TaskHandle_t);
void xTaskNotifyGive(TaskHandle_t);
/* The calling task (the harness says which), and a task's state. */
static TaskHandle_t test_current_task __attribute__((unused));
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return test_current_task; }
typedef enum { eRunning, eReady, eBlocked, eSuspended, eDeleted, eInvalid } eTaskState;
static inline eTaskState eTaskGetState(TaskHandle_t t) { (void)t; return eBlocked; }
static inline TaskHandle_t xTaskGetHandle(const char *name) { (void)name; return NULL; }
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#ifndef portTICK_PERIOD_MS
#define portTICK_PERIOD_MS 1
#endif
static inline unsigned uxTaskPriorityGet(TaskHandle_t t) { (void)t; return 5; }
TickType_t xTaskGetTickCount(void);
BaseType_t xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*)(void *), const char *, unsigned, void *, unsigned,
                                       TaskHandle_t *, int, unsigned);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
int64_t esp_timer_get_time(void);
void esp_restart(void);
/* muse_stack.h: plain tasks on the host. */
#define tskNO_AFFINITY 0x7fffffff
typedef struct { int unused; } muse_task_slot_t;
#define muse_task_create(fn, name, stack, arg, prio, task, core) \
    ((void)(core), xTaskCreate((fn), (name), (stack), (arg), (prio), (task)))
#define muse_task_slot_run(slot, fn, name, stack, arg, prio, task, core) \
    ((void)(slot), (void)(core), xTaskCreate((fn), (name), (stack), (arg), (prio), (task)))
#define MUSE_TASK_END() vTaskDelete(NULL)
/* muse_wdt.h: no watchdog on the host. */
typedef enum { MUSE_WDT_BLE_HOST, MUSE_WDT_MG_WORKER, MUSE_WDT_MUSE_INPUT, MUSE_WDT_LINK_SESSION,
               MUSE_WDT_MUSE_VOICE, MUSE_WDT_COUNT } muse_wdt_user_t;
/* Counted, so a test sees the voice task's feeds. */
static int test_wdt_feeds[MUSE_WDT_COUNT] __attribute__((unused));
/* test_wdt_jump set: the next feed ends the input task's loop there (one poll of input_task). */
#include <setjmp.h>
static jmp_buf *test_wdt_jump __attribute__((unused));
static inline void muse_wdt_feed(muse_wdt_user_t u)
{
    test_wdt_feeds[u]++;
    if (test_wdt_jump) longjmp(*test_wdt_jump, 1);
}
/* muse_local_press.h: the update's confirm press, its prompt off unless a test turns it on; presses counted. */
static int test_local_presses __attribute__((unused));
static bool test_update_prompt __attribute__((unused));
static inline bool muse_local_press_prompting(void) { return test_update_prompt; }
static inline bool muse_local_press_note(const char *w)
{
    (void)w;
    if (!test_update_prompt) return false;
    test_local_presses++;
    test_update_prompt = false;
    return true;
}
static inline bool muse_local_press_console(const char *line) { (void)line; return false; }
/* muse_bootguard.h: never SAFE here (SAFE never runs the voice or input code under test); the
 * consoles' SAFE words aren't in these harnesses' scripts. */
typedef enum { MUSE_BOOT_NORMAL = 0, MUSE_BOOT_SAFE, MUSE_BOOT_DOWNLOAD } muse_boot_mode_t;
static inline muse_boot_mode_t muse_bootguard_mode(void) { return MUSE_BOOT_NORMAL; }
static inline bool muse_bootguard_console_word(const char *line, char *out, size_t n)
{
    (void)line, (void)out, (void)n;
    return false;
}

/* muse_restart.h: the console's restart is esp_restart() here. */
typedef enum { MUSE_RESTART_OTHER = 1, MUSE_RESTART_CONSOLE, MUSE_RESTART_OTA, MUSE_RESTART_SETUP } muse_restart_why_t;
#define muse_restart(why) ((void)(why), esp_restart())
static inline bool muse_restart_later(muse_restart_why_t why, int core, unsigned delay_ms)
{
    (void)why;
    (void)core;
    (void)delay_ms;
    return false;
}
static inline bool muse_restart_test_failure(bool panic, unsigned delay_ms)
{
    (void)panic;
    (void)delay_ms;
    return true;
}
void *heap_caps_malloc(size_t, unsigned);
void *heap_caps_realloc(void *, size_t, unsigned);
const char *esp_err_to_name(esp_err_t);
void esp_restart(void);

typedef int gpio_num_t;
typedef void *esp_codec_dev_handle_t;
typedef int lv_align_t;
typedef struct lv_obj lv_obj_t;
typedef struct lv_display lv_display_t;
typedef struct lv_indev lv_indev_t;

typedef int nvs_handle_t;
typedef int nvs_open_mode_t;
#define NVS_READWRITE 1
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
esp_err_t nvs_open(const char *, nvs_open_mode_t, nvs_handle_t *);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_get_u16(nvs_handle_t, const char *, uint16_t *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_set_u16(nvs_handle_t, const char *, uint16_t);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
void nvs_close(nvs_handle_t);
