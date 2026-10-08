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
 * An ESP32-P4 MIPI-CSI camera through esp_video: the sensor's raw frames go
 * through the ISP as RGB565, and the P4's JPEG encoder compresses the one a
 * capture keeps. The device is open, which powers the MIPI PHY's LDO, only
 * while a capture runs.
 */
#include "p4_csi_camera.h"
#include "camera.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "driver/jpeg_encode.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"
#include "mbedtls/base64.h"

#define BUFFERS       2
#define SETTLE_US     1500000  /* auto exposure and white balance catch up */
#define FRAME_WAIT_S  2        /* a stream that stops sending frames */
#define GRAB_LIMIT_US (SETTLE_US + 3000000)   /* one that sends only broken ones */
/* The JPEG has to fit in this: a 380 KB one, about 510 KB as base64, made
 * Muse time out. */
#define JPEG_MAX_BYTES (150 * 1024)
static const int QUALITIES[] = { 90, 80, 70, 60, 50, 40, 30, 20 };

static const char *TAG = "p4.camera";
static i2c_master_bus_handle_t s_bus;
static int s_fd = -1;
static uint8_t *s_buf[BUFFERS];
static int s_w, s_h;
static jpeg_encoder_handle_t s_jpeg;
/* A stream that would not stop may still write to its buffers: keep them. */
static bool s_stuck;

/* The ISP controller esp_video_init starts keeps the device open too, so its
 * buffers and the PHY's LDO go only with esp_video_deinit. */
static void close_camera(void)
{
    if (s_jpeg) {
        jpeg_del_encoder_engine(s_jpeg);
        s_jpeg = NULL;
    }
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
    esp_err_t err = esp_video_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_video deinit: %s", esp_err_to_name(err));
    }
}

static esp_err_t open_camera(void)
{
    esp_video_init_csi_config_t csi = {
        .sccb_config = { .init_sccb = false, .i2c_handle = s_bus, .freq = 100000 },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };
    esp_video_init_config_t cfg = { .csi = &csi };
    ESP_RETURN_ON_ERROR(esp_video_init(&cfg), TAG, "esp_video init");
    /* Without a sensor esp_video_init succeeds but registers no device. */
    s_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    ESP_RETURN_ON_FALSE(s_fd >= 0, ESP_ERR_NOT_FOUND, TAG, "no camera sensor");
    esp_err_t ret = ESP_FAIL;
    struct timeval wait = { .tv_sec = FRAME_WAIT_S };
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_S_DQBUF_TIMEOUT, &wait) == 0, ESP_FAIL, fail, TAG, "DQBUF timeout");
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_G_FMT, &fmt) == 0, ESP_FAIL, fail, TAG, "G_FMT");
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_S_FMT, &fmt) == 0, ESP_FAIL, fail, TAG, "S_FMT");
    s_w = fmt.fmt.pix.width;
    s_h = fmt.fmt.pix.height;
    struct v4l2_requestbuffers req = {
        .count = BUFFERS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
    };
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_REQBUFS, &req) == 0, ESP_FAIL, fail, TAG, "REQBUFS");
    for (int i = 0; i < BUFFERS; i++) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
        ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_QUERYBUF, &b) == 0, ESP_FAIL, fail, TAG, "QUERYBUF");
        /* esp_video's MAP_FAILED is NULL */
        s_buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, b.m.offset);
        ESP_GOTO_ON_FALSE(s_buf[i], ESP_ERR_NO_MEM, fail, TAG, "mmap");
    }
    jpeg_encode_engine_cfg_t eng = { .timeout_ms = 1000 };
    ESP_GOTO_ON_ERROR(jpeg_new_encoder_engine(&eng, &s_jpeg), fail, TAG, "JPEG encoder");
    ESP_LOGI(TAG, "%dx%d RGB565", s_w, s_h);
    return ESP_OK;
fail:
    close_camera();
    return ret;
}

static esp_err_t init(void)
{
    return ESP_OK;
}

/* 0-255 over a sparse grid of the RGB565 frame. */
static int mean_luma(const uint8_t *rgb565)
{
    const uint16_t *px = (const uint16_t *)rgb565;
    uint32_t sum = 0, n = 0;
    for (int y = 0; y < s_h; y += 16) {
        for (int x = 0; x < s_w; x += 16, n++) {
            uint16_t p = px[y * s_w + x];
            sum += ((p >> 11) * 8 * 3 + ((p >> 5) & 63) * 4 * 6 + (p & 31) * 8) / 10;
        }
    }
    return n ? sum / n : 0;
}

/* The frame after SETTLE_US, encoded at the first of QUALITIES that fits the cap. */
static esp_err_t grab(struct v4l2_buffer *b, uint8_t **jpeg, uint32_t *len)
{
    int64_t start = esp_timer_get_time(), until = start + SETTLE_US;
    for (int frames = 0;; frames++) {
        if (ioctl(s_fd, VIDIOC_DQBUF, b) != 0 || esp_timer_get_time() - start > GRAB_LIMIT_US) {
            return ESP_ERR_TIMEOUT;
        }
        bool whole = !(b->flags & V4L2_BUF_FLAG_ERROR) && b->bytesused;
        if (whole && esp_timer_get_time() >= until) {
            ESP_LOGI(TAG, "kept frame %d, mean luma %d", frames, mean_luma(s_buf[b->index]));
            break;
        }
        if (ioctl(s_fd, VIDIOC_QBUF, b) != 0) {
            return ESP_FAIL;
        }
    }
    jpeg_encode_memory_alloc_cfg_t mem = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t cap = 0;
    *jpeg = jpeg_alloc_encoder_mem(s_w * s_h, &mem, &cap);
    if (!*jpeg) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < sizeof(QUALITIES) / sizeof(QUALITIES[0]); i++) {
        jpeg_encode_cfg_t enc = {
            .width = s_w, .height = s_h,
            .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
            .image_quality = QUALITIES[i],
        };
        esp_err_t err = jpeg_encoder_process(s_jpeg, &enc, s_buf[b->index], b->bytesused, *jpeg, cap, len);
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGI(TAG, "JPEG quality %d: %u bytes", QUALITIES[i], (unsigned)*len);
        if (*len <= JPEG_MAX_BYTES) {
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_SIZE;
}

static esp_err_t capture(camera_frame_t *out)
{
    if (s_stuck) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_RETURN_ON_ERROR(open_camera(), TAG, "open");
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < BUFFERS && err == ESP_OK; i++) {
        struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
        err = ioctl(s_fd, VIDIOC_QBUF, &b) == 0 ? ESP_OK : ESP_FAIL;
    }
    bool streaming = err == ESP_OK && ioctl(s_fd, VIDIOC_STREAMON, &type) == 0;
    uint8_t *jpeg = NULL;
    uint32_t len = 0;
    struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP };
    if (!streaming) {
        err = ESP_FAIL;
    } else {
        err = grab(&b, &jpeg, &len);
        if (ioctl(s_fd, VIDIOC_STREAMOFF, &type) != 0 && ioctl(s_fd, VIDIOC_STREAMOFF, &type) != 0) {
            s_stuck = true;
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (!s_stuck) {
        close_camera();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "capture failed: %s", esp_err_to_name(err));
        free(jpeg);
        return err;
    }
    *out = (camera_frame_t){ .jpeg = jpeg, .len = len, .width = s_w, .height = s_h, .priv = jpeg };
    return ESP_OK;
}

static void release(camera_frame_t *frame)
{
    free(frame->priv);
}

static const camera_driver_t s_driver = {
    .name = "OV5647 (MIPI-CSI)",
    .max_width = 1280,
    .max_height = 960,
    .init = init,
    .capture = capture,
    .release = release,
};

void p4_csi_camera_register(i2c_master_bus_handle_t bus)
{
    s_bus = bus;
    camera_register(&s_driver);
}

bool p4_csi_camera_capture(char **jpeg_base64, const char **error)
{
    *jpeg_base64 = NULL;
    camera_frame_t frame;
    esp_err_t err = camera_capture(&frame);
    if (err != ESP_OK) {
        *error = err == ESP_ERR_NO_MEM ? "camera memory allocation failed"
               : err == ESP_ERR_INVALID_STATE ? "camera busy"
               : err == ESP_ERR_NOT_FOUND ? "no camera"
               : err == ESP_ERR_TIMEOUT ? "camera sent no frame"
               : err == ESP_ERR_INVALID_SIZE ? "frame over 150 KB even at low quality"
               : err == ESP_ERR_INVALID_RESPONSE ? "camera stream stuck: restart the device"
                                             : "camera capture failed";
        return false;
    }
    size_t cap = (frame.len + 2) / 3 * 4 + 1, len = 0;
    *jpeg_base64 = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (*jpeg_base64 && mbedtls_base64_encode((unsigned char *)*jpeg_base64, cap, &len,
                                              frame.jpeg, frame.len) != 0) {
        free(*jpeg_base64);
        *jpeg_base64 = NULL;
    }
    camera_release(&frame);
    *error = *jpeg_base64 ? NULL : "camera memory allocation failed";
    return *jpeg_base64 != NULL;
}
