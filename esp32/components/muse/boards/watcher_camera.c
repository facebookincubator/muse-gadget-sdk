/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "watcher_camera.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "watcher_camera.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "muse_board.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "rom/tjpgd.h"

/*
 * The Watcher's camera.capture and live preview, on the board's camera
 * (components/camera; board_sensecap_watcher.c registers the Himax). The
 * camera is powered only while a capture or the preview runs.
 */

static const char *TAG = "watcher.camera";
static SemaphoreHandle_t s_lock;     /* s_last */
static uint8_t *s_last;              /* the preview's newest frame, a JPEG */
static size_t s_last_len;
static atomic_bool s_preview_active;
static atomic_bool s_busy;           /* a capture, or the preview starting or stopping */

#define CAMERA_PREVIEW_SIZE 412
#define CAMERA_JPEG_POOL 3100
/* Largest block the decoder emits: a 16x16 MCU. */
#define CAMERA_MCU_PIXELS (16 * 16)

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t offset;
} jpeg_source_t;

static UINT jpeg_read(JDEC *jd, BYTE *buffer, UINT count)
{
    jpeg_source_t *src = jd->device;
    size_t remain = src->length - src->offset;
    if (count > remain) count = remain;
    if (buffer && count) memcpy(buffer, src->data + src->offset, count);
    src->offset += count;
    return count;
}

static UINT jpeg_draw(JDEC *jd, void *bitmap, JRECT *rect)
{
    int left = rect->left;
    int top = rect->top;
    int right = rect->right > CAMERA_PREVIEW_SIZE - 1 ? CAMERA_PREVIEW_SIZE - 1 : rect->right;
    int bottom = rect->bottom > CAMERA_PREVIEW_SIZE - 1 ? CAMERA_PREVIEW_SIZE - 1 : rect->bottom;
    if (left > right || top > bottom) return 1;

    int source_w = rect->right - rect->left + 1;
    int width = right - left + 1;
    int height = bottom - top + 1;
    if (width * height > CAMERA_MCU_PIXELS) return 0;
    uint16_t pixels[CAMERA_MCU_PIXELS];
    uint8_t *pixel_bytes = (uint8_t *)pixels;
    const uint8_t *rgb = bitmap;
    for (int y = top; y <= bottom; y++) {
        for (int x = left; x <= right; x++) {
            size_t si = ((size_t)(y - rect->top) * source_w + (x - rect->left)) * 3;
            size_t di = ((size_t)(y - top) * width + (x - left)) * 2;
            uint16_t px = ((rgb[si] & 0xF8) << 8) | ((rgb[si + 1] & 0xFC) << 3) | (rgb[si + 2] >> 3);
            pixel_bytes[di] = px >> 8;
            pixel_bytes[di + 1] = px & 0xFF;
        }
    }
    muse_ui_image_draw(left, top, width, height, pixels);
    return 1;
}

static void preview_draw(const uint8_t *jpeg, size_t len)
{
    void *pool = heap_caps_malloc(CAMERA_JPEG_POOL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pool) return;
    jpeg_source_t src = { .data = jpeg, .length = len };
    JDEC jd = {0};  /* the ROM decoder doesn't clear it */
    if (jd_prepare(&jd, jpeg_read, pool, CAMERA_JPEG_POOL, &src) == JDR_OK) {
        if (jd.width > 416 || jd.height > 416) {
            muse_state_set_caption("CAMERA FRAME TOO LARGE");
        } else {
            jd_decomp(&jd, jpeg_draw, 0);
        }
    }
    free(pool);
}

/* Each streamed frame: kept for a capture during the preview, and drawn. */
static void on_frame(const camera_frame_t *frame, void *ctx)
{
    (void)ctx;
    if (!s_preview_active) return;
    uint8_t *copy = heap_caps_malloc(frame->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy) {
        memcpy(copy, frame->jpeg, frame->len);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        free(s_last);
        s_last = copy;
        s_last_len = frame->len;
        xSemaphoreGive(s_lock);
    }
    preview_draw(frame->jpeg, frame->len);
}

esp_err_t watcher_camera_prepare(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

/* `jpeg` as base64, for camera.capture's result. */
static char *to_base64(const uint8_t *jpeg, size_t len)
{
    size_t cap = (len + 2) / 3 * 4 + 1, out = 0;
    char *b64 = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (b64 && mbedtls_base64_encode((unsigned char *)b64, cap, &out, jpeg, len) != 0) {
        free(b64);
        return NULL;
    }
    return b64;
}

static bool capture_frame(char **jpeg_base64, const char **error)
{
    if (s_preview_active) {
        /* The preview has the camera: the frame it's showing is the photo. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        *jpeg_base64 = s_last ? to_base64(s_last, s_last_len) : NULL;
        bool had = s_last != NULL;
        xSemaphoreGive(s_lock);
        *error = !had ? "no camera frame yet" : *jpeg_base64 ? NULL : "camera memory allocation failed";
        return *jpeg_base64 != NULL;
    }
    camera_frame_t frame;
    esp_err_t err = camera_capture(&frame);
    if (err != ESP_OK) {
        *error = err == ESP_ERR_TIMEOUT ? "Himax camera did not respond"
                 : err == ESP_ERR_NO_MEM ? "camera memory allocation failed"
                 : err == ESP_ERR_NOT_SUPPORTED ? "camera unavailable"
                                                 : "camera capture failed";
        return false;
    }
    *jpeg_base64 = to_base64(frame.jpeg, frame.len);
    camera_release(&frame);
    *error = *jpeg_base64 ? NULL : "camera memory allocation failed";
    return *jpeg_base64 != NULL;
}

bool watcher_camera_capture(char **jpeg_base64, const char **error)
{
    if (!jpeg_base64 || !error) return false;
    *jpeg_base64 = NULL;
    if (watcher_camera_prepare() != ESP_OK) {
        *error = "camera memory allocation failed";
        return false;
    }
    if (atomic_exchange(&s_busy, true)) {
        *error = "camera busy";
        return false;
    }
    bool ok = capture_frame(jpeg_base64, error);
    atomic_store(&s_busy, false);
    return ok;
}

static void preview_toggle_task(void *arg)
{
    (void)arg;
    if (!s_preview_active) {
        /* The view first, so the stream's first frames land on it. */
        s_preview_active = true;
        muse_state_set_caption("LIVE VIEW • TAP IMAGE TO TAKE PHOTO");
        muse_ui_image_hide();
        muse_ui_camera_hint(true);
        muse_board->display_lock(-1);
        muse_ui_show_face();
        muse_board->display_unlock();
        esp_err_t err = camera_stream_start(on_frame, NULL);
        if (err != ESP_OK) {
            s_preview_active = false;
            muse_ui_camera_hint(false);
            ESP_LOGW(TAG, "preview didn't start: %s", esp_err_to_name(err));
            muse_state_set_caption(err == ESP_ERR_TIMEOUT ? "HIMAX CAMERA NOT READY" : "CAMERA PREVIEW FAILED");
        }
    } else {
        s_preview_active = false;
        camera_stream_stop();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool have_frame = s_last != NULL;
        free(s_last);
        s_last = NULL;
        xSemaphoreGive(s_lock);
        muse_ui_camera_hint(false);
        muse_state_set_caption(have_frame ? "PHOTO CAPTURED" : "NO CAMERA FRAME • TRY AGAIN");
    }
    atomic_store(&s_busy, false);
    vTaskDeleteWithCaps(NULL);
}

bool watcher_camera_preview_active(void)
{
    return s_preview_active;
}

void watcher_camera_preview_close(void)
{
    if (!s_preview_active || atomic_exchange(&s_busy, true)) return;
    s_preview_active = false;
    camera_stream_stop();   /* no frame draws after this */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_last);
    s_last = NULL;
    xSemaphoreGive(s_lock);
    muse_ui_camera_hint(false);
    muse_ui_image_hide();
    atomic_store(&s_busy, false);
}

void watcher_camera_preview_toggle(void)
{
    if (watcher_camera_prepare() != ESP_OK) {
        muse_state_set_caption("CAMERA MEMORY ERROR");
        return;
    }
    if (atomic_exchange(&s_busy, true)) return;
    if (xTaskCreateWithCaps(preview_toggle_task, "camera_view", 8192, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        != pdPASS) {
        ESP_LOGE(TAG, "could not start camera preview task");
        atomic_store(&s_busy, false);
    }
}
