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

/* The bits of ESP-IDF, esp_video and V4L2 that boards/p4_csi_camera.c uses,
 * for test_p4_csi_camera.py. Every header it includes is this one; the calls
 * go to p4_csi_camera_harness.c. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

typedef int esp_err_t;
#define ESP_OK                   0
#define ESP_FAIL                 -1
#define ESP_ERR_NO_MEM           0x101
#define ESP_ERR_INVALID_ARG      0x102
#define ESP_ERR_INVALID_STATE    0x103
#define ESP_ERR_INVALID_SIZE     0x104
#define ESP_ERR_NOT_FOUND        0x105
#define ESP_ERR_NOT_SUPPORTED    0x106
#define ESP_ERR_TIMEOUT          0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
const char *esp_err_to_name(esp_err_t err);

#define ESP_LOGE(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)

#define ESP_RETURN_ON_ERROR(x, tag, ...) do { esp_err_t err_rc_ = (x); \
        if (err_rc_ != ESP_OK) return err_rc_; } while (0)
#define ESP_RETURN_ON_FALSE(a, err, tag, ...) do { if (!(a)) return (err); } while (0)
#define ESP_GOTO_ON_ERROR(x, goto_tag, tag, ...) do { esp_err_t err_rc_ = (x); \
        if (err_rc_ != ESP_OK) { ret = err_rc_; goto goto_tag; } } while (0)
#define ESP_GOTO_ON_FALSE(a, err, goto_tag, tag, ...) do { if (!(a)) { ret = (err); goto goto_tag; } } while (0)

#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT   2
#define heap_caps_malloc(size, caps) malloc(size)

int64_t esp_timer_get_time(void);

typedef struct fake_i2c_bus *i2c_master_bus_handle_t;

/* esp_video */
#define ESP_VIDEO_MIPI_CSI_DEVICE_NAME "/dev/video0"
typedef struct {
    struct { bool init_sccb; i2c_master_bus_handle_t i2c_handle; uint32_t freq; } sccb_config;
    int reset_pin, pwdn_pin;
} esp_video_init_csi_config_t;
typedef struct { const esp_video_init_csi_config_t *csi; } esp_video_init_config_t;
esp_err_t esp_video_init(const esp_video_init_config_t *config);
esp_err_t esp_video_deinit(void);

/* V4L2 */
enum {
    VIDIOC_G_FMT = 1, VIDIOC_S_FMT, VIDIOC_REQBUFS, VIDIOC_QUERYBUF, VIDIOC_QBUF,
    VIDIOC_DQBUF, VIDIOC_STREAMON, VIDIOC_STREAMOFF, VIDIOC_S_DQBUF_TIMEOUT,
};
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define V4L2_MEMORY_MMAP            1
#define V4L2_PIX_FMT_RGB565         0x50424752
#define V4L2_BUF_FLAG_ERROR         0x40
struct v4l2_pix_format { uint32_t width, height, pixelformat; };
struct v4l2_format { uint32_t type; union { struct v4l2_pix_format pix; } fmt; };
struct v4l2_requestbuffers { uint32_t count, type, memory; };
struct v4l2_buffer {
    uint32_t index, type, bytesused, flags, memory;
    union { uint32_t offset; } m;
    uint32_t length;
};

/* POSIX calls the driver makes on the video device. */
#define O_RDONLY   0
#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_SHARED 1
int fake_open(const char *path, int flags);
int fake_close(int fd);
int fake_ioctl(int fd, unsigned long request, void *arg);
void *fake_mmap(void *addr, size_t len, int prot, int flags, int fd, long offset);
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define mmap fake_mmap

/* The JPEG encoder */
typedef struct fake_jpeg_encoder *jpeg_encoder_handle_t;
typedef struct { int timeout_ms; } jpeg_encode_engine_cfg_t;
typedef enum { JPEG_ENC_ALLOC_OUTPUT_BUFFER = 1 } jpeg_enc_buffer_alloc_direction_t;
typedef struct { jpeg_enc_buffer_alloc_direction_t buffer_direction; } jpeg_encode_memory_alloc_cfg_t;
typedef enum { JPEG_ENCODE_IN_FORMAT_RGB565 = 1 } jpeg_enc_input_format_t;
typedef enum { JPEG_DOWN_SAMPLING_YUV420 = 2 } jpeg_down_sampling_type_t;
typedef struct {
    uint32_t width, height;
    jpeg_enc_input_format_t src_type;
    jpeg_down_sampling_type_t sub_sample;
    uint32_t image_quality;
} jpeg_encode_cfg_t;
esp_err_t jpeg_new_encoder_engine(const jpeg_encode_engine_cfg_t *cfg, jpeg_encoder_handle_t *out);
esp_err_t jpeg_del_encoder_engine(jpeg_encoder_handle_t encoder);
void *jpeg_alloc_encoder_mem(size_t size, const jpeg_encode_memory_alloc_cfg_t *cfg, size_t *allocated);
esp_err_t jpeg_encoder_process(jpeg_encoder_handle_t encoder, const jpeg_encode_cfg_t *cfg,
                               const uint8_t *in, uint32_t in_len, uint8_t *out,
                               uint32_t out_cap, uint32_t *out_len);

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen);
