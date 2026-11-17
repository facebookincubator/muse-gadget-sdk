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

/*
 * A board with no panel, keys or codec: the UI draws into PSRAM and the cable
 * mirror (tools/muse/mirror.py) shows it, gives it touch and the keys, and
 * plays and records its sound on the computer. Any ESP32-S3 with PSRAM runs
 * it, with the size and shape set in menuconfig, to work on the UI for a
 * board that is not at hand. No battery.
 */
#include "sdkconfig.h"
#include <string.h>
#include "muse_board.h"
#include "muse_mem.h"
#include "audio_codec_data_if.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if !CONFIG_MUSE_MIRROR
#error "the virtual display is seen only through CONFIG_MUSE_MIRROR"
#endif

#define W CONFIG_MUSE_VIRTUAL_DISPLAY_WIDTH
#define H CONFIG_MUSE_VIRTUAL_DISPLAY_HEIGHT
#define BUF_LINES 40
#if CONFIG_MUSE_VIRTUAL_DISPLAY_ROUND
#define ROUND true
#else
#define ROUND false   /* a disabled bool is left undefined */
#endif

static const char *TAG = "board";
static SemaphoreHandle_t s_lv_lock;
static lv_display_t *s_disp;

static esp_err_t init(void)
{
    return ESP_OK;
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Nothing to send: the mirror copied the area at LV_EVENT_FLUSH_START. */
static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    lv_display_flush_ready(disp);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTakeRecursive(s_lv_lock, portMAX_DELAY);
        uint32_t ms = lv_timer_handler();
        xSemaphoreGiveRecursive(s_lv_lock);
        ms = ms < 5 ? 5 : ms > 50 ? 50 : ms;
        vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
    }
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    size_t bytes = (size_t)W * BUF_LINES * 2;
    void *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    s_lv_lock = xSemaphoreCreateRecursiveMutex();
    if (!buf || !s_lv_lock) {
        ESP_LOGE(TAG, "virtual display: no memory");
        return NULL;
    }
    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(W, H);
    if (!s_disp) {
        return NULL;
    }
    lv_display_set_buffers(s_disp, buf, NULL, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush);
    if (xTaskCreatePinnedToCoreWithCaps(lvgl_task, "lvgl", 8192, NULL, MUSE_UI_PRIORITY, NULL, MUSE_UI_CORE,
                                        MUSE_BIG_CAPS) != pdPASS) {
        return NULL;
    }
    *touch = NULL;   /* the mirror adds its own pointer */
    return s_disp;
}

static bool display_lock(int timeout_ms)
{
    return xSemaphoreTakeRecursive(s_lv_lock, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lv_lock);
}

static void set_brightness(int pct)
{
    (void)pct;
}

/*
 * Sound with nothing behind it: writes and reads take their real time, so the
 * audio task keeps its pace; what is written goes nowhere and reads are
 * silence. The mirror taps both, so the computer plays and records them.
 */
typedef struct {
    audio_codec_data_if_t base;
    uint32_t bytes_per_s;
    int64_t out_at, in_at;   /* when the next write and read are due, us */
} null_data_t;

static void pace(null_data_t *d, int64_t *at, int size)
{
    int64_t now = esp_timer_get_time();
    if (*at < now - 100000) *at = now;   /* resume from now after a pause, not with a burst */
    *at += (int64_t)size * 1000000 / (d->bytes_per_s ? d->bytes_per_s : 64000);
    if (*at > now) vTaskDelay(pdMS_TO_TICKS((*at - now) / 1000) ? pdMS_TO_TICKS((*at - now) / 1000) : 1);
}

static int null_open(const audio_codec_data_if_t *h, void *cfg, int size) { (void)h; (void)cfg; (void)size; return 0; }
static bool null_is_open(const audio_codec_data_if_t *h) { (void)h; return true; }
static int null_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t t, bool on) { (void)h; (void)t; (void)on; return 0; }
static int null_close(const audio_codec_data_if_t *h) { (void)h; return 0; }

static int null_set_fmt(const audio_codec_data_if_t *h, esp_codec_dev_type_t t, esp_codec_dev_sample_info_t *fs)
{
    (void)t;
    ((null_data_t *)h)->bytes_per_s = fs->sample_rate * fs->channel * (fs->bits_per_sample / 8);
    return 0;
}

static int null_write(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)data;
    null_data_t *d = (null_data_t *)h;
    pace(d, &d->out_at, size);
    return 0;
}

static int null_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    null_data_t *d = (null_data_t *)h;
    memset(data, 0, size);
    pace(d, &d->in_at, size);
    return 0;
}

static null_data_t s_out_if = { .base = { null_open, null_is_open, null_enable, null_set_fmt, null_read, null_write, null_close } };
static null_data_t s_in_if = { .base = { null_open, null_is_open, null_enable, null_set_fmt, null_read, null_write, null_close } };

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    esp_codec_dev_cfg_t out = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .data_if = &s_out_if.base };
    esp_codec_dev_cfg_t in = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .data_if = &s_in_if.base };
    *spk = esp_codec_dev_new(&out);
    *mic = esp_codec_dev_new(&in);
    return *spk && *mic ? ESP_OK : ESP_ERR_NO_MEM;
}

static unsigned poll_buttons(void)
{
    return 0;
}

static const muse_board_t s_board = {
    .name = CONFIG_MUSE_VIRTUAL_DISPLAY_NAME,
    .width = W,
    .height = H,
    .round = ROUND,
    .touch = true,
    .diagonal_in = CONFIG_MUSE_VIRTUAL_DISPLAY_DIAGONAL / 100.0f,
    /* Pressed only from the mirror window; named as the round Waveshare 1.75C names them. */
    .talk_button = "top",
    .aux_button = "bottom",
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = display_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .poll_buttons = poll_buttons,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
