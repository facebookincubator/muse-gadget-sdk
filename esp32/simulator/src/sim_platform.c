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

#define _POSIX_C_SOURCE 200809L

#include "sim_platform.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

static int64_t monotonic_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * INT64_C(1000000) + now.tv_nsec / 1000;
}

static pthread_mutex_t s_time_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_time_initialized;
static bool s_realtime = true;
static int64_t s_real_epoch_us;
static int64_t s_time_epoch_us;
static int64_t s_fake_time_us;

static void time_init_locked(void)
{
    if (!s_time_initialized) {
        s_real_epoch_us = monotonic_us();
        s_time_epoch_us = 0;
        s_fake_time_us = 0;
        s_time_initialized = true;
    }
}

static int64_t time_now_locked(void)
{
    time_init_locked();
    return s_realtime ? s_time_epoch_us + monotonic_us() - s_real_epoch_us : s_fake_time_us;
}

int64_t esp_timer_get_time(void)
{
    pthread_mutex_lock(&s_time_lock);
    int64_t now = time_now_locked();
    pthread_mutex_unlock(&s_time_lock);
    return now;
}

void sim_time_use_realtime(bool realtime)
{
    pthread_mutex_lock(&s_time_lock);
    int64_t now = time_now_locked();
    if (realtime && !s_realtime) {
        s_time_epoch_us = now;
        s_real_epoch_us = monotonic_us();
    } else if (!realtime && s_realtime) {
        s_fake_time_us = now;
    }
    s_realtime = realtime;
    pthread_mutex_unlock(&s_time_lock);
}

bool sim_time_is_realtime(void)
{
    pthread_mutex_lock(&s_time_lock);
    bool realtime = s_realtime;
    pthread_mutex_unlock(&s_time_lock);
    return realtime;
}

void sim_time_reset(void)
{
    sim_time_set_us(0);
}

void sim_time_set_us(int64_t time_us)
{
    pthread_mutex_lock(&s_time_lock);
    time_init_locked();
    s_fake_time_us = time_us < 0 ? 0 : time_us;
    s_realtime = false;
    pthread_mutex_unlock(&s_time_lock);
}

void sim_time_advance_us(int64_t delta_us)
{
    pthread_mutex_lock(&s_time_lock);
    int64_t now = time_now_locked();
    s_fake_time_us = now + delta_us;
    if (s_fake_time_us < 0) {
        s_fake_time_us = 0;
    }
    s_realtime = false;
    pthread_mutex_unlock(&s_time_lock);
}

uint32_t sim_time_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void sim_delay_ms(uint32_t milliseconds)
{
    if (!sim_time_is_realtime()) {
        sim_time_advance_us((int64_t)milliseconds * 1000);
        return;
    }
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

const char *esp_err_to_name(esp_err_t error)
{
    switch (error) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
        case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
        default: return "ESP_ERR_UNKNOWN";
    }
}

void esp_error_check_failed(esp_err_t error, const char *file, int line, const char *function,
                            const char *expression)
{
    fprintf(stderr, "ESP_ERROR_CHECK failed: %s (%d) at %s:%d in %s: %s\n",
            esp_err_to_name(error), error, file, line, function, expression);
    abort();
}

static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;
static esp_log_level_t s_log_level = ESP_LOG_INFO;

void esp_log_writev(esp_log_level_t level, const char *tag, const char *format, va_list args)
{
    static const char level_names[] = "NEWIDV";
    pthread_mutex_lock(&s_log_lock);
    if (level <= s_log_level && level > ESP_LOG_NONE && level < ESP_LOG_MAX) {
        fprintf(stderr, "%u %c (%s): ", esp_log_timestamp(), level_names[level], tag ? tag : "");
        vfprintf(stderr, format, args);
        fputc('\n', stderr);
    }
    pthread_mutex_unlock(&s_log_lock);
}

void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    esp_log_writev(level, tag, format, args);
    va_end(args);
}

void esp_log_level_set(const char *tag, esp_log_level_t level)
{
    (void)tag;
    pthread_mutex_lock(&s_log_lock);
    s_log_level = level;
    pthread_mutex_unlock(&s_log_lock);
}

esp_log_level_t esp_log_level_get(const char *tag)
{
    (void)tag;
    pthread_mutex_lock(&s_log_lock);
    esp_log_level_t level = s_log_level;
    pthread_mutex_unlock(&s_log_lock);
    return level;
}

uint32_t esp_log_timestamp(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

void *heap_caps_calloc(size_t count, size_t size, uint32_t caps)
{
    (void)caps;
    return calloc(count, size);
}

void *heap_caps_realloc(void *memory, size_t size, uint32_t caps)
{
    (void)caps;
    return realloc(memory, size);
}

void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    (void)caps;
    void *memory = NULL;
    return posix_memalign(&memory, alignment, size) == 0 ? memory : NULL;
}

void heap_caps_free(void *memory)
{
    free(memory);
}

size_t heap_caps_get_total_size(uint32_t caps)
{
    (void)caps;
    return 256u * 1024u * 1024u;
}

size_t heap_caps_get_free_size(uint32_t caps)
{
    return heap_caps_get_total_size(caps);
}

#if !defined(__APPLE__)
size_t muse_sim_strlcpy(char *destination, const char *source, size_t size)
{
    size_t source_length = strlen(source);
    if (size) {
        size_t copy_length = source_length < size - 1 ? source_length : size - 1;
        memcpy(destination, source, copy_length);
        destination[copy_length] = '\0';
    }
    return source_length;
}

size_t muse_sim_strlcat(char *destination, const char *source, size_t size)
{
    size_t used = 0;
    while (used < size && destination[used]) used++;
    if (used == size) return size + strlen(source);
    return used + muse_sim_strlcpy(destination + used, source, size - used);
}
#endif

static int cond_wait_ticks(pthread_cond_t *condition, pthread_mutex_t *lock, TickType_t ticks)
{
    if (ticks == 0) {
        return ETIMEDOUT;
    }
    if (ticks == portMAX_DELAY) {
        return pthread_cond_wait(condition, lock);
    }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    uint64_t nanoseconds = (uint64_t)until.tv_nsec + (uint64_t)ticks * 1000000u;
    until.tv_sec += (time_t)(nanoseconds / 1000000000u);
    until.tv_nsec = (long)(nanoseconds % 1000000000u);
    return pthread_cond_timedwait(condition, lock, &until);
}

static SemaphoreHandle_t semaphore_init(StaticSemaphore_t *semaphore, UBaseType_t maximum,
                                        UBaseType_t initial, bool dynamic)
{
    if (!semaphore || maximum == 0 || initial > maximum) {
        return NULL;
    }
    pthread_mutex_init(&semaphore->lock, NULL);
    pthread_cond_init(&semaphore->changed, NULL);
    semaphore->count = initial;
    semaphore->maximum = maximum;
    semaphore->dynamically_allocated = dynamic;
    return semaphore;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    StaticSemaphore_t *semaphore = malloc(sizeof(*semaphore));
    return semaphore_init(semaphore, 1, 1, true);
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    return semaphore_init(storage, 1, 1, false);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    StaticSemaphore_t *semaphore = malloc(sizeof(*semaphore));
    return semaphore_init(semaphore, 1, 0, true);
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    return semaphore_init(storage, 1, 0, false);
}

SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t maximum, UBaseType_t initial)
{
    StaticSemaphore_t *semaphore = malloc(sizeof(*semaphore));
    return semaphore_init(semaphore, maximum, initial, true);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait)
{
    if (!semaphore) {
        return pdFALSE;
    }
    pthread_mutex_lock(&semaphore->lock);
    int error = 0;
    while (semaphore->count == 0 && error == 0) {
        error = cond_wait_ticks(&semaphore->changed, &semaphore->lock, ticks_to_wait);
    }
    BaseType_t taken = semaphore->count != 0;
    if (taken) {
        semaphore->count--;
    }
    pthread_mutex_unlock(&semaphore->lock);
    return taken;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (!semaphore) {
        return pdFALSE;
    }
    pthread_mutex_lock(&semaphore->lock);
    BaseType_t given = semaphore->count < semaphore->maximum;
    if (given) {
        semaphore->count++;
        pthread_cond_signal(&semaphore->changed);
    }
    pthread_mutex_unlock(&semaphore->lock);
    return given;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore, BaseType_t *higher_priority_woken)
{
    if (higher_priority_woken) {
        *higher_priority_woken = pdFALSE;
    }
    return xSemaphoreGive(semaphore);
}

UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t semaphore)
{
    if (!semaphore) {
        return 0;
    }
    pthread_mutex_lock(&semaphore->lock);
    UBaseType_t count = semaphore->count;
    pthread_mutex_unlock(&semaphore->lock);
    return count;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    if (!semaphore) {
        return;
    }
    bool dynamic = semaphore->dynamically_allocated;
    pthread_cond_destroy(&semaphore->changed);
    pthread_mutex_destroy(&semaphore->lock);
    if (dynamic) {
        free(semaphore);
    }
}

static EventGroupHandle_t event_group_init(StaticEventGroup_t *group, bool dynamic)
{
    if (!group) {
        return NULL;
    }
    pthread_mutex_init(&group->lock, NULL);
    pthread_cond_init(&group->changed, NULL);
    group->bits = 0;
    group->dynamically_allocated = dynamic;
    return group;
}

EventGroupHandle_t xEventGroupCreate(void)
{
    StaticEventGroup_t *group = malloc(sizeof(*group));
    return event_group_init(group, true);
}

EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *storage)
{
    return event_group_init(storage, false);
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{
    if (!group) {
        return 0;
    }
    pthread_mutex_lock(&group->lock);
    group->bits |= bits;
    EventBits_t result = group->bits;
    pthread_cond_broadcast(&group->changed);
    pthread_mutex_unlock(&group->lock);
    return result;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits)
{
    if (!group) {
        return 0;
    }
    pthread_mutex_lock(&group->lock);
    EventBits_t before = group->bits;
    group->bits &= ~bits;
    pthread_mutex_unlock(&group->lock);
    return before;
}

EventBits_t xEventGroupGetBits(EventGroupHandle_t group)
{
    if (!group) {
        return 0;
    }
    pthread_mutex_lock(&group->lock);
    EventBits_t bits = group->bits;
    pthread_mutex_unlock(&group->lock);
    return bits;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits_to_wait_for,
                                BaseType_t clear_on_exit, BaseType_t wait_for_all,
                                TickType_t ticks_to_wait)
{
    if (!group) {
        return 0;
    }
    pthread_mutex_lock(&group->lock);
    int error = 0;
    bool ready = false;
    while (!ready && error == 0) {
        EventBits_t matched = group->bits & bits_to_wait_for;
        ready = wait_for_all ? matched == bits_to_wait_for : matched != 0;
        if (!ready) {
            error = cond_wait_ticks(&group->changed, &group->lock, ticks_to_wait);
        }
    }
    EventBits_t result = group->bits;
    if (ready && clear_on_exit) {
        group->bits &= ~bits_to_wait_for;
    }
    pthread_mutex_unlock(&group->lock);
    return result;
}

void vEventGroupDelete(EventGroupHandle_t group)
{
    if (!group) {
        return;
    }
    bool dynamic = group->dynamically_allocated;
    pthread_cond_destroy(&group->changed);
    pthread_mutex_destroy(&group->lock);
    if (dynamic) {
        free(group);
    }
}

static QueueHandle_t queue_init(StaticQueue_t *queue, UBaseType_t length, UBaseType_t item_size,
                                uint8_t *storage, bool dynamic, bool owns_storage)
{
    if (!queue || !storage || length == 0 || item_size == 0) {
        return NULL;
    }
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->readable, NULL);
    pthread_cond_init(&queue->writable, NULL);
    queue->storage = storage;
    queue->length = length;
    queue->item_size = item_size;
    queue->count = 0;
    queue->head = 0;
    queue->dynamically_allocated = dynamic;
    queue->owns_storage = owns_storage;
    return queue;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    if (length == 0 || item_size == 0 || (size_t)length > SIZE_MAX / item_size) {
        return NULL;
    }
    StaticQueue_t *queue = malloc(sizeof(*queue));
    uint8_t *storage = malloc((size_t)length * item_size);
    if (!queue || !storage) {
        free(queue);
        free(storage);
        return NULL;
    }
    return queue_init(queue, length, item_size, storage, true, true);
}

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size, uint8_t *storage,
                                 StaticQueue_t *queue_storage)
{
    return queue_init(queue_storage, length, item_size, storage, false, false);
}

static BaseType_t queue_send(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait,
                             bool front)
{
    if (!queue || !item) {
        return pdFALSE;
    }
    pthread_mutex_lock(&queue->lock);
    int error = 0;
    while (queue->count == queue->length && error == 0) {
        error = cond_wait_ticks(&queue->writable, &queue->lock, ticks_to_wait);
    }
    BaseType_t sent = queue->count != queue->length;
    if (sent) {
        UBaseType_t index;
        if (front) {
            queue->head = (queue->head + queue->length - 1) % queue->length;
            index = queue->head;
        } else {
            index = (queue->head + queue->count) % queue->length;
        }
        memcpy(queue->storage + (size_t)index * queue->item_size, item, queue->item_size);
        queue->count++;
        pthread_cond_signal(&queue->readable);
    }
    pthread_mutex_unlock(&queue->lock);
    return sent;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait)
{
    return queue_send(queue, item, ticks_to_wait, false);
}

BaseType_t xQueueSendToFront(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait)
{
    return queue_send(queue, item, ticks_to_wait, true);
}

static BaseType_t queue_read(QueueHandle_t queue, void *item, TickType_t ticks_to_wait, bool pop)
{
    if (!queue || !item) {
        return pdFALSE;
    }
    pthread_mutex_lock(&queue->lock);
    int error = 0;
    while (queue->count == 0 && error == 0) {
        error = cond_wait_ticks(&queue->readable, &queue->lock, ticks_to_wait);
    }
    BaseType_t received = queue->count != 0;
    if (received) {
        memcpy(item, queue->storage + (size_t)queue->head * queue->item_size, queue->item_size);
        if (pop) {
            queue->head = (queue->head + 1) % queue->length;
            queue->count--;
            pthread_cond_signal(&queue->writable);
        }
    }
    pthread_mutex_unlock(&queue->lock);
    return received;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks_to_wait)
{
    return queue_read(queue, item, ticks_to_wait, true);
}

BaseType_t xQueuePeek(QueueHandle_t queue, void *item, TickType_t ticks_to_wait)
{
    return queue_read(queue, item, ticks_to_wait, false);
}

BaseType_t xQueueReset(QueueHandle_t queue)
{
    if (!queue) {
        return pdFAIL;
    }
    pthread_mutex_lock(&queue->lock);
    queue->count = 0;
    queue->head = 0;
    pthread_cond_broadcast(&queue->writable);
    pthread_mutex_unlock(&queue->lock);
    return pdPASS;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue)
{
    if (!queue) {
        return 0;
    }
    pthread_mutex_lock(&queue->lock);
    UBaseType_t count = queue->count;
    pthread_mutex_unlock(&queue->lock);
    return count;
}

void vQueueDelete(QueueHandle_t queue)
{
    if (!queue) {
        return;
    }
    bool dynamic = queue->dynamically_allocated;
    bool owns_storage = queue->owns_storage;
    uint8_t *storage = queue->storage;
    pthread_cond_destroy(&queue->readable);
    pthread_cond_destroy(&queue->writable);
    pthread_mutex_destroy(&queue->lock);
    if (owns_storage) {
        free(storage);
    }
    if (dynamic) {
        free(queue);
    }
}

int mbedtls_base64_encode(unsigned char *destination, size_t destination_size,
                          size_t *output_length, const unsigned char *source,
                          size_t source_length)
{
    static const unsigned char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!output_length || (!source && source_length)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (source_length > (SIZE_MAX - 2) / 3) {
        *output_length = SIZE_MAX;
        return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
    }
    size_t required = ((source_length + 2) / 3) * 4;
    *output_length = required;
    if (destination_size < required || (!destination && required)) {
        return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
    }

    size_t input = 0;
    size_t output = 0;
    while (input < source_length) {
        size_t left = source_length - input;
        uint32_t value = (uint32_t)source[input++] << 16;
        if (left > 1) {
            value |= (uint32_t)source[input++] << 8;
        }
        if (left > 2) {
            value |= source[input++];
        }
        destination[output++] = alphabet[(value >> 18) & 0x3f];
        destination[output++] = alphabet[(value >> 12) & 0x3f];
        destination[output++] = left > 1 ? alphabet[(value >> 6) & 0x3f] : '=';
        destination[output++] = left > 2 ? alphabet[value & 0x3f] : '=';
    }
    *output_length = output;
    return 0;
}

struct sim_codec_dev {
    esp_codec_dev_sample_info_t sample_info;
    bool open;
    int volume;
    float gain;
};

esp_codec_dev_handle_t esp_codec_dev_new(const esp_codec_dev_cfg_t *config)
{
    (void)config;
    return calloc(1, sizeof(struct sim_codec_dev));
}

int esp_codec_dev_delete(esp_codec_dev_handle_t device)
{
    free(device);
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_open(esp_codec_dev_handle_t device, const esp_codec_dev_sample_info_t *info)
{
    if (!device || !info) {
        return ESP_FAIL;
    }
    device->sample_info = *info;
    device->open = true;
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_close(esp_codec_dev_handle_t device)
{
    if (!device) {
        return ESP_FAIL;
    }
    device->open = false;
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_read(esp_codec_dev_handle_t device, void *data, int size)
{
    if (!device || !device->open || !data || size < 0) {
        return ESP_FAIL;
    }
    memset(data, 0, (size_t)size);
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_write(esp_codec_dev_handle_t device, const void *data, int size)
{
    return device && device->open && (data || size == 0) && size >= 0 ? ESP_CODEC_DEV_OK : ESP_FAIL;
}

int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t device, float gain)
{
    if (!device) {
        return ESP_FAIL;
    }
    device->gain = gain;
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t device, int volume)
{
    if (!device) {
        return ESP_FAIL;
    }
    device->volume = volume;
    return ESP_CODEC_DEV_OK;
}

typedef struct {
    int level;
    gpio_mode_t mode;
    gpio_int_type_t interrupt_type;
    bool interrupt_enabled;
    gpio_isr_t handler;
    void *handler_argument;
} sim_gpio_t;

static pthread_mutex_t s_gpio_lock = PTHREAD_MUTEX_INITIALIZER;
static sim_gpio_t s_gpio[GPIO_NUM_MAX];

static bool gpio_valid(gpio_num_t gpio)
{
    return GPIO_IS_VALID_GPIO(gpio);
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    for (gpio_num_t gpio = GPIO_NUM_0; gpio < GPIO_NUM_MAX; gpio++) {
        if (config->pin_bit_mask & (UINT64_C(1) << gpio)) {
            gpio_set_direction(gpio, config->mode);
            gpio_set_intr_type(gpio, config->intr_type);
            if (config->pull_up_en == GPIO_PULLUP_ENABLE) {
                gpio_set_level(gpio, 1);
            } else if (config->pull_down_en == GPIO_PULLDOWN_ENABLE) {
                gpio_set_level(gpio, 0);
            }
        }
    }
    return ESP_OK;
}

esp_err_t gpio_reset_pin(gpio_num_t gpio)
{
    if (!gpio_valid(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_gpio_lock);
    memset(&s_gpio[gpio], 0, sizeof(s_gpio[gpio]));
    pthread_mutex_unlock(&s_gpio_lock);
    return ESP_OK;
}

esp_err_t gpio_set_direction(gpio_num_t gpio, gpio_mode_t mode)
{
    if (!gpio_valid(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_gpio_lock);
    s_gpio[gpio].mode = mode;
    pthread_mutex_unlock(&s_gpio_lock);
    return ESP_OK;
}

static void gpio_drive(gpio_num_t gpio, int level, bool simulated_input)
{
    (void)simulated_input;
    gpio_isr_t handler = NULL;
    void *argument = NULL;
    pthread_mutex_lock(&s_gpio_lock);
    int old = s_gpio[gpio].level;
    int next = !!level;
    s_gpio[gpio].level = next;
    bool rising = old == 0 && next == 1;
    bool falling = old == 1 && next == 0;
    gpio_int_type_t type = s_gpio[gpio].interrupt_type;
    bool triggered = (type == GPIO_INTR_POSEDGE && rising) ||
                     (type == GPIO_INTR_NEGEDGE && falling) ||
                     (type == GPIO_INTR_ANYEDGE && old != next) ||
                     (type == GPIO_INTR_LOW_LEVEL && next == 0) ||
                     (type == GPIO_INTR_HIGH_LEVEL && next == 1);
    if (s_gpio[gpio].interrupt_enabled && triggered) {
        handler = s_gpio[gpio].handler;
        argument = s_gpio[gpio].handler_argument;
    }
    pthread_mutex_unlock(&s_gpio_lock);
    if (handler) {
        handler(argument);
    }
}

esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level)
{
    if (!gpio_valid(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_drive(gpio, level, false);
    return ESP_OK;
}

void sim_gpio_set_input_level(gpio_num_t gpio, int level)
{
    if (gpio_valid(gpio)) {
        gpio_drive(gpio, level, true);
    }
}

int gpio_get_level(gpio_num_t gpio)
{
    if (!gpio_valid(gpio)) {
        return 0;
    }
    pthread_mutex_lock(&s_gpio_lock);
    int level = s_gpio[gpio].level;
    pthread_mutex_unlock(&s_gpio_lock);
    return level;
}

esp_err_t gpio_set_intr_type(gpio_num_t gpio, gpio_int_type_t type)
{
    if (!gpio_valid(gpio) || type >= GPIO_INTR_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_gpio_lock);
    s_gpio[gpio].interrupt_type = type;
    pthread_mutex_unlock(&s_gpio_lock);
    return ESP_OK;
}

esp_err_t gpio_intr_enable(gpio_num_t gpio)
{
    if (!gpio_valid(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_gpio_lock);
    s_gpio[gpio].interrupt_enabled = true;
    pthread_mutex_unlock(&s_gpio_lock);
    return ESP_OK;
}

esp_err_t gpio_intr_disable(gpio_num_t gpio)
{
    if (!gpio_valid(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_gpio_lock);
    s_gpio[gpio].interrupt_enabled = false;
    pthread_mutex_unlock(&s_gpio_lock);
    return ESP_OK;
}

esp_err_t gpio_install_isr_service(int flags)
{
    (void)flags;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t gpio, gpio_isr_t handler, void *argument)
{
    if (!gpio_valid(gpio) || !handler) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_gpio_lock);
    s_gpio[gpio].handler = handler;
    s_gpio[gpio].handler_argument = argument;
    pthread_mutex_unlock(&s_gpio_lock);
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t gpio)
{
    if (!gpio_valid(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_gpio_lock);
    s_gpio[gpio].handler = NULL;
    s_gpio[gpio].handler_argument = NULL;
    pthread_mutex_unlock(&s_gpio_lock);
    return ESP_OK;
}

esp_err_t gpio_wakeup_enable(gpio_num_t gpio, gpio_int_type_t type)
{
    return gpio_set_intr_type(gpio, type);
}

esp_err_t gpio_wakeup_disable(gpio_num_t gpio)
{
    return gpio_valid(gpio) ? ESP_OK : ESP_ERR_INVALID_ARG;
}
