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
 * What makes the gadget an ESP-Mosaico application: it starts ESP-Iris and the
 * Recovery adapter ahead of the gadget's own app_main(), so that Vibe Mode can
 * always be reached over USB, and it gives ESP-Iris the screen and the
 * speaker tests, which the gadget has on a serial console this board's USB
 * port doesn't carry.
 */
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_err.h"
#include "esp_iris.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "iris_ota_support.h"
#include "lvgl.h"
#include "muse_voice.h"

#define SCREEN_LOCK_MS 500
#define SCREEN_DRAW_MS 2000
#define SPEAKER_TEST_SERVICE 0x4D47   /* "MG" */
#define SPEAKER_TEST_SAMPLE 1
#define SPEAKER_TEST_TONES 2

/* The screen for `mosaico.py iris screenshot` and the Gateway's mirror. LVGL
 * redraws it into s_frame at the start of each frame ESP-Iris reads; s_frame
 * changes hands under LVGL's lock. */
static lv_draw_buf_t *s_frame;
static SemaphoreHandle_t s_drawn;
static lv_result_t s_draw_result;

static uint32_t frame_bytes(void)
{
    return s_frame ? s_frame->header.stride * s_frame->header.h : 0;
}

/* Runs in LVGL's task: ESP-Iris's own stack is too small to draw from. */
static void draw_frame(void *arg)
{
    (void)arg;
    if (s_frame) {
        s_draw_result = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, s_frame);
    }
    xSemaphoreGive(s_drawn);
}

static esp_err_t request_frame(void)
{
    xSemaphoreTake(s_drawn, 0);   /* left by a draw that came after its request gave up */
    if (!bsp_display_lock(SCREEN_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_result_t queued = lv_async_call(draw_frame, NULL);
    bsp_display_unlock();
    if (queued != LV_RESULT_OK || xSemaphoreTake(s_drawn, pdMS_TO_TICKS(SCREEN_DRAW_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return s_draw_result == LV_RESULT_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t screen_begin(const esp_iris_media_desc_t *requested, esp_iris_media_desc_t *actual,
                              uint32_t *total_size, void *user_ctx)
{
    (void)requested;
    (void)user_ctx;
    if (!bsp_display_get()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bsp_display_lock(SCREEN_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_frame) {   /* still there if the last screen_end() couldn't take the lock */
        /* Not lv_snapshot_create_draw_buf(): it lays the screen out first,
         * which is too deep for this stack. */
        s_frame = lv_draw_buf_create(BSP_LCD_H_RES, BSP_LCD_V_RES, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        if (s_frame) {
            /* A read may start past offset 0, before anything is drawn, and
             * must not get what the heap held before. */
            lv_draw_buf_clear(s_frame, NULL);
        }
    }
    bsp_display_unlock();
    if (!s_frame) {
        return ESP_ERR_NO_MEM;
    }
    *actual = (esp_iris_media_desc_t){
        .width = s_frame->header.w,
        .height = s_frame->header.h,
        .stride = s_frame->header.stride,
        .format = ESP_IRIS_PIXEL_FORMAT_RGB565,
    };
    *total_size = frame_bytes();
    return ESP_OK;
}

static esp_err_t screen_read(uint32_t offset, uint8_t *out, size_t capacity, size_t *out_size, void *user_ctx)
{
    (void)user_ctx;
    if (offset >= frame_bytes()) {
        return ESP_ERR_INVALID_ARG;
    }
    /* A mirror reads frame after frame, each from offset 0. */
    if (offset == 0) {
        esp_err_t err = request_frame();
        if (err != ESP_OK) {
            return err;
        }
    }
    size_t size = frame_bytes() - offset;
    if (size > capacity) {
        size = capacity;
    }
    memcpy(out, s_frame->data + offset, size);
    *out_size = size;
    return ESP_OK;
}

static void screen_end(void *user_ctx)
{
    (void)user_ctx;
    /* Freed only under the lock: a draw still queued in LVGL's task would
     * otherwise land in freed memory. */
    if (s_frame && bsp_display_lock(SCREEN_LOCK_MS)) {
        lv_draw_buf_destroy(s_frame);
        s_frame = NULL;
        bsp_display_unlock();
    }
}

static void screen_register(void)
{
    s_drawn = xSemaphoreCreateBinary();
    if (!s_drawn) {
        return;
    }
    const esp_iris_screen_backend_t screen = {
        .begin = screen_begin,
        .read = screen_read,
        .end = screen_end,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_screen_register(&screen));
}

/* `mosaico.py iris rpc 0x4D47 1` plays the gadget's built-in sample through its
 * reply decoder and speaker. Method 2 plays test tones for half a minute and
 * logs how loud the microphone hears each. */
static esp_err_t speaker_test(const esp_iris_rpc_request_t *request, uint8_t *response, size_t response_capacity,
                              size_t *response_size, void *user_ctx)
{
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    if (!bsp_display_get()) {
        return ESP_ERR_INVALID_STATE;   /* the gadget is still starting */
    }
    if (request->method_id == SPEAKER_TEST_SAMPLE) {
        muse_voice_request_mp3test();
    } else {
        muse_voice_request_loopback();
    }
    *response_size = 0;
    return ESP_OK;
}

/* The gadget's main component defines app_main(). The linker's --wrap option
 * (CMakeLists.txt) sends the startup call here first, so Vibe Mode stays
 * reachable whatever the gadget does afterwards. */
void __real_app_main(void);

void __wrap_app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    screen_register();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_rpc_register(SPEAKER_TEST_SERVICE, SPEAKER_TEST_SAMPLE, speaker_test, NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_rpc_register(SPEAKER_TEST_SERVICE, SPEAKER_TEST_TONES, speaker_test, NULL));
    iris_ota_support_start();
    __real_app_main();
}
