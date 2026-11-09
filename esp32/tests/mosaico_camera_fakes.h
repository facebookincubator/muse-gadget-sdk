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

/* The bits of ESP-IDF, esp_video, the PPA and V4L2 that boards/mosaico_camera.c
 * uses, for test_mosaico_camera.py. Every header it includes is this one; the
 * calls go to mosaico_camera_harness.c. */
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

/* FreeRTOS */
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(uint32_t ticks);

/* GPIO */
typedef int gpio_num_t;
#define GPIO_NUM_NC -1
#define GPIO_NUM_0  0
#define GPIO_NUM_1  1
#define GPIO_NUM_4  4
#define GPIO_NUM_12 12
#define GPIO_NUM_13 13
#define GPIO_NUM_14 14
#define GPIO_NUM_15 15
#define GPIO_NUM_16 16
#define GPIO_NUM_17 17
#define GPIO_NUM_18 18
#define GPIO_NUM_19 19
#define GPIO_NUM_33 33
#define GPIO_NUM_34 34
#define GPIO_NUM_48 48
#define GPIO_NUM_53 53
#define GPIO_NUM_55 55
#define BIT64(n) (1ULL << (n))
typedef enum { GPIO_MODE_OUTPUT = 2, GPIO_MODE_OUTPUT_OD = 6 } gpio_mode_t;
typedef struct { uint64_t pin_bit_mask; gpio_mode_t mode; } gpio_config_t;
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
esp_err_t gpio_config(const gpio_config_t *cfg);

/* I2C */
typedef struct fake_i2c_bus *i2c_master_bus_handle_t;
typedef struct fake_i2c_dev *i2c_master_dev_handle_t;
#define I2C_NUM_1 1
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 0
typedef struct {
    int i2c_port, clk_source;
    gpio_num_t sda_io_num, scl_io_num;
    struct { uint32_t enable_internal_pullup : 1; } flags;
} i2c_master_bus_config_t;
typedef struct { int dev_addr_length; uint16_t device_address; uint32_t scl_speed_hz; } i2c_device_config_t;
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *out);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *out);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t tx_len,
                                      uint8_t *rx, size_t rx_len, int timeout_ms);

/* esp_video */
#define ESP_VIDEO_DVP_DEVICE_NAME "/dev/video2"
#define ESP_VIDEO_INIT_FLAGS_DVP  (1 << 1)
#define CAM_CTLR_DATA_WIDTH_8     8
typedef struct {
    int data_width;
    gpio_num_t data_io[8];
    gpio_num_t vsync_io, de_io, pclk_io, xclk_io;
} esp_cam_ctlr_dvp_pin_config_t;
typedef struct {
    struct { bool init_sccb; i2c_master_bus_handle_t i2c_handle; uint32_t freq; } sccb_config;
    gpio_num_t reset_pin, pwdn_pin;
    esp_cam_ctlr_dvp_pin_config_t dvp_pin;
    uint32_t xclk_freq;
} esp_video_init_dvp_config_t;
typedef struct { const esp_video_init_dvp_config_t *dvp; } esp_video_init_config_t;
esp_err_t esp_video_init_with_flags(const esp_video_init_config_t *config, uint32_t flags);
esp_err_t esp_video_deinit(void);

/* V4L2 */
enum {
    VIDIOC_G_FMT = 1, VIDIOC_S_FMT, VIDIOC_REQBUFS, VIDIOC_QUERYBUF, VIDIOC_QBUF,
    VIDIOC_DQBUF, VIDIOC_STREAMON, VIDIOC_STREAMOFF, VIDIOC_S_DQBUF_TIMEOUT,
    VIDIOC_G_EXT_CTRLS, VIDIOC_S_EXT_CTRLS,
};
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define V4L2_MEMORY_MMAP            1
#define V4L2_PIX_FMT_UYVY           0x59565955
#define V4L2_BUF_FLAG_ERROR         0x40
struct v4l2_pix_format { uint32_t width, height, pixelformat; };
struct v4l2_format { uint32_t type; union { struct v4l2_pix_format pix; } fmt; };
struct v4l2_requestbuffers { uint32_t count, type, memory; };
struct v4l2_ext_control { uint32_t id, size; union { int32_t value; uint8_t *p_u8; }; };
struct v4l2_ext_controls { uint32_t ctrl_class, count; struct v4l2_ext_control *controls; };
#define V4L2_CTRL_CLASS_ESP_CAM_IOCTL 0x00a70000

/* esp_cam_sensor's ID and register commands */
#define ESP_CAM_SENSOR_IOC_G_CHIP_ID 0x106
#define ESP_CAM_SENSOR_IOC_S_REG     0x107
#define ESP_CAM_SENSOR_IOC_G_REG     0x108
typedef struct { uint32_t regaddr, value; } esp_cam_sensor_reg_val_t;
typedef struct { uint8_t midh, midl; uint16_t pid; uint8_t ver; } esp_cam_sensor_id_t;
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

/* The PPA */
typedef struct fake_ppa *ppa_client_handle_t;
typedef enum { PPA_OPERATION_SRM = 0 } ppa_operation_t;
typedef struct { ppa_operation_t oper_type; } ppa_client_config_t;
typedef enum { PPA_SRM_COLOR_MODE_YUV420 = 1, PPA_SRM_COLOR_MODE_YUV422_UYVY = 2 } ppa_srm_color_mode_t;
typedef enum { PPA_COLOR_RANGE_LIMIT = 0 } ppa_color_range_t;
typedef enum { PPA_COLOR_CONV_STD_RGB_YUV_BT601 = 0 } ppa_color_conv_std_rgb_yuv_t;
typedef enum { PPA_SRM_ROTATION_ANGLE_0, PPA_SRM_ROTATION_ANGLE_90, PPA_SRM_ROTATION_ANGLE_180,
               PPA_SRM_ROTATION_ANGLE_270 } ppa_srm_rotation_angle_t;
typedef enum { PPA_TRANS_MODE_BLOCKING = 0 } ppa_trans_mode_t;
typedef struct {
    const void *buffer;
    uint32_t pic_w, pic_h, block_w, block_h;
    ppa_srm_color_mode_t srm_cm;
    ppa_color_range_t yuv_range;
    ppa_color_conv_std_rgb_yuv_t yuv_std;
} ppa_in_pic_blk_config_t;
typedef struct {
    void *buffer;
    uint32_t buffer_size, pic_w, pic_h;
    ppa_srm_color_mode_t srm_cm;
    ppa_color_range_t yuv_range;
    ppa_color_conv_std_rgb_yuv_t yuv_std;
} ppa_out_pic_blk_config_t;
typedef struct {
    ppa_in_pic_blk_config_t in;
    ppa_out_pic_blk_config_t out;
    ppa_srm_rotation_angle_t rotation_angle;
    float scale_x, scale_y;
    bool mirror_x, mirror_y;
    ppa_trans_mode_t mode;
} ppa_srm_oper_config_t;
esp_err_t ppa_register_client(const ppa_client_config_t *cfg, ppa_client_handle_t *out);
esp_err_t ppa_unregister_client(ppa_client_handle_t client);
esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t client, const ppa_srm_oper_config_t *cfg);

/* The JPEG encoder */
typedef struct fake_jpeg_encoder *jpeg_encoder_handle_t;
typedef struct { int timeout_ms; } jpeg_encode_engine_cfg_t;
typedef enum { JPEG_ENC_ALLOC_INPUT_BUFFER = 0, JPEG_ENC_ALLOC_OUTPUT_BUFFER = 1 } jpeg_enc_buffer_alloc_direction_t;
typedef struct { jpeg_enc_buffer_alloc_direction_t buffer_direction; } jpeg_encode_memory_alloc_cfg_t;
typedef enum { JPEG_ENCODE_IN_FORMAT_YUV420 = 1 } jpeg_enc_input_format_t;
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
