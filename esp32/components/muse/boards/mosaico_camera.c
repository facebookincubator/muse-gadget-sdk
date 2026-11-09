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
 * The ESP-Mosaico's Camera Module in the left expansion slot, an OV3640 or a
 * SC101IOT on 8-bit DVP, through esp_video. The PPA turns the UYVY frame
 * upright into YUV420 and the S31's JPEG encoder compresses it. On an
 * SC101IOT the module's flash fires when the auto exposure value is high.
 * Pins, PWDN, the flash and the slot's
 * EEPROM descriptor come from Espressif's esp-mosaico-bsp
 * (components/mosaico_module_camera, components/mosaico_module_mgr) and the
 * module's user guide. The sensor is awake, and esp_video holds its buffers,
 * only while a capture runs.
 */
#include "mosaico_camera.h"
#include "camera.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "esp_cam_sensor_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "mbedtls/base64.h"

/* GPIO33 (D2) and GPIO34 (the flash) are the USB Serial/JTAG pads, which
 * reach only this slot: setting them up as GPIOs turns that port off. */
static const gpio_num_t DATA[8] = {
    GPIO_NUM_16, GPIO_NUM_15, GPIO_NUM_33, GPIO_NUM_4,
    GPIO_NUM_14, GPIO_NUM_12, GPIO_NUM_18, GPIO_NUM_13,
};
#define VSYNC     GPIO_NUM_55
#define DE        GPIO_NUM_19
#define PCLK      GPIO_NUM_17
#define RESET     GPIO_NUM_53
#define PWDN      GPIO_NUM_48   /* high: asleep */
#define FLASH     GPIO_NUM_34   /* open drain, low: both LEDs on */
#define SLOT_PORT I2C_NUM_1     /* v1.2's slot bus */
#define SLOT_SDA  GPIO_NUM_0
#define SLOT_SCL  GPIO_NUM_1

#define EEPROM_ADDR 0x50        /* fixed on the camera, whatever GPIO14 (D4) does */
#define EEPROM_CRC  0x34        /* "ESP", the board type, ..., then a CRC-16/Modbus */
#define TYPE_CAMERA 0x07

#define BUFFERS        2
#define WAKE_MS        20       /* out of PWDN, the module's oscillator running, before SCCB */
#define SETTLE_US      1500000  /* auto exposure and white balance catch up */
#define FLASH_US       700000   /* and again with the flash on */
/* The SC101IOT's auto exposure value, as two registers: 288 in a lit room
 * and 1398 with the lens covered by a hand, on a v1.4 module. From this up
 * the flash fires. */
#define DARK_EXPOSURE  1300
#define FRAME_WAIT_S   2        /* a stream that stops sending frames */
#define GRAB_LIMIT_US  (SETTLE_US + FLASH_US + 3000000)   /* one that sends only broken ones */
/* The JPEG has to fit in this: a 380 KB one, about 510 KB as base64, made
 * Muse time out. */
#define JPEG_MAX_BYTES (150 * 1024)
static const int QUALITIES[] = { 90, 80, 70, 60, 50, 40, 30, 20 };

#define SC101IOT_PID      0xDA4A
#define SC_PAGE           0xf0
#define SC_PCLK_DELAY     0x3640  /* bits 1:0 */
#define SC_EXPOSURE_L     0x008c
#define SC_EXPOSURE_H     0x008d

static const char *TAG = "mosaico.camera";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_eeprom;
static int s_fd = -1;
static uint8_t *s_buf[BUFFERS];
static int s_w, s_h;            /* the sensor's frame, on its side */
static ppa_client_handle_t s_ppa;
static bool s_sc101iot;
/* esp_video could not be torn down (a stream that would not stop, a deinit
 * that kept failing): its buffers stay and the camera stays off. */
static bool s_stuck;

static esp_err_t output(gpio_num_t pin, int level, bool open_drain)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(pin),
        .mode = open_drain ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, level), TAG, "GPIO%d level", pin);
    return gpio_config(&cfg);
}

static void flash(bool on)
{
    gpio_set_level(FLASH, on ? 0 : 1);
}

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = crc & 1 ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}

/* The slot's pins are the camera's to drive only once its EEPROM says so. */
static bool module_present(void)
{
    uint8_t d[EEPROM_CRC + 2], at = 0;
    if (!s_eeprom || i2c_master_probe(s_bus, EEPROM_ADDR, 50) != ESP_OK
        || i2c_master_transmit_receive(s_eeprom, &at, 1, d, sizeof(d), 50) != ESP_OK) {
        return false;
    }
    if (memcmp(d, "ESP", 3) != 0 || d[3] != TYPE_CAMERA
        || crc16(d, EEPROM_CRC) != (d[EEPROM_CRC] | d[EEPROM_CRC + 1] << 8)) {
        ESP_LOGW(TAG, "left slot: not a Camera Module descriptor (type 0x%02x)", d[3]);
        return false;
    }
    return true;
}

static int sensor_ctrl(unsigned long request, uint32_t id, void *arg, uint32_t size)
{
    struct v4l2_ext_control c = { .id = id, .size = size, .p_u8 = arg };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL, .count = 1, .controls = &c };
    return ioctl(s_fd, request, &cs);
}

/* Writes value to an SC101IOT register, or with value < 0 reads it; -1 on
 * failure. esp_cam_sensor's register commands send the low byte only: the
 * sensor takes the high byte in its page register first. */
static int sc_reg(uint16_t reg, int value)
{
    esp_cam_sensor_reg_val_t r = { .regaddr = SC_PAGE, .value = reg >> 8 };
    if (sensor_ctrl(VIDIOC_S_EXT_CTRLS, ESP_CAM_SENSOR_IOC_S_REG, &r, sizeof(r)) != 0) {
        return -1;
    }
    r = (esp_cam_sensor_reg_val_t){ .regaddr = reg & 0xff, .value = value };
    bool set = value >= 0;
    if (sensor_ctrl(set ? VIDIOC_S_EXT_CTRLS : VIDIOC_G_EXT_CTRLS,
                    set ? ESP_CAM_SENSOR_IOC_S_REG : ESP_CAM_SENSOR_IOC_G_REG, &r, sizeof(r)) != 0) {
        return -1;
    }
    return set ? 0 : (int)(r.value & 0xff);
}

/* The SC101IOT's auto exposure value; 0 for another sensor or a failed read. */
static int exposure(void)
{
    int l = s_sc101iot ? sc_reg(SC_EXPOSURE_L, -1) : -1;
    int h = l >= 0 ? sc_reg(SC_EXPOSURE_H, -1) : -1;
    return h >= 0 ? h << 8 | l : 0;
}

/* Only esp_video_deinit frees the frame buffers. */
static void close_camera(void)
{
    if (s_ppa) {
        ppa_unregister_client(s_ppa);
        s_ppa = NULL;
    }
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
    /* Removing the SCCB device fails while another transaction holds the
     * bus, which v1.0 shares with the board's own devices. */
    esp_err_t err = esp_video_deinit();
    for (int i = 0; err == ESP_ERR_INVALID_STATE && i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        err = esp_video_deinit();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video deinit: %s", esp_err_to_name(err));
        s_stuck = true;
    }
    gpio_set_level(PWDN, 1);
}

static esp_err_t open_camera(void)
{
    ESP_RETURN_ON_FALSE(module_present(), ESP_ERR_NOT_FOUND, TAG, "no Camera Module in the left slot");
    ESP_RETURN_ON_ERROR(output(FLASH, 1, true), TAG, "flash");
    /* Low runs both sensors; esp_video doesn't get it, as Espressif's driver
     * doesn't give it. */
    ESP_RETURN_ON_ERROR(output(PWDN, 0, false), TAG, "PWDN");
    vTaskDelay(pdMS_TO_TICKS(WAKE_MS));
    esp_video_init_dvp_config_t dvp = {
        .sccb_config = { .init_sccb = false, .i2c_handle = s_bus, .freq = 100000 },
        .reset_pin = RESET,
        .pwdn_pin = GPIO_NUM_NC,
        .dvp_pin = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = { DATA[0], DATA[1], DATA[2], DATA[3], DATA[4], DATA[5], DATA[6], DATA[7] },
            .vsync_io = VSYNC,
            .de_io = DE,
            .pclk_io = PCLK,
            .xclk_io = GPIO_NUM_NC,   /* the module has its own oscillator */
        },
    };
    esp_video_init_config_t cfg = { .dvp = &dvp };
    esp_err_t ret = esp_video_init_with_flags(&cfg, ESP_VIDEO_INIT_FLAGS_DVP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video init: %s", esp_err_to_name(ret));
        gpio_set_level(PWDN, 1);
        return ret;
    }
    ret = ESP_FAIL;
    /* Without a sensor esp_video_init succeeds but registers no device. */
    s_fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDONLY);
    ESP_GOTO_ON_FALSE(s_fd >= 0, ESP_ERR_NOT_FOUND, fail, TAG, "no camera sensor");
    struct timeval wait = { .tv_sec = FRAME_WAIT_S };
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_S_DQBUF_TIMEOUT, &wait) == 0, ESP_FAIL, fail, TAG, "DQBUF timeout");
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_G_FMT, &fmt) == 0, ESP_FAIL, fail, TAG, "G_FMT");
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
    ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_S_FMT, &fmt) == 0, ESP_FAIL, fail, TAG, "S_FMT");
    ESP_GOTO_ON_FALSE(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY && fmt.fmt.pix.width % 2 == 0,
                      ESP_ERR_NOT_SUPPORTED, fail, TAG, "no UYVY");
    esp_cam_sensor_id_t id = { 0 };
    s_sc101iot = sensor_ctrl(VIDIOC_G_EXT_CTRLS, ESP_CAM_SENSOR_IOC_G_CHIP_ID, &id, sizeof(id)) == 0
                 && id.pid == SC101IOT_PID;
    if (s_sc101iot) {
        /* PCLK delay 0, as Espressif's mosaico_module_camera sets it: with the
         * init table's 3, the stream stalled after the flash in the dark. */
        int delay = sc_reg(SC_PCLK_DELAY, -1);
        ESP_GOTO_ON_FALSE(delay >= 0 && sc_reg(SC_PCLK_DELAY, delay & ~3) == 0, ESP_FAIL, fail, TAG, "PCLK delay");
    }
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
    ppa_client_config_t ppa = { .oper_type = PPA_OPERATION_SRM };
    ESP_GOTO_ON_ERROR(ppa_register_client(&ppa, &s_ppa), fail, TAG, "PPA");
    ESP_LOGI(TAG, "%dx%d UYVY", s_w, s_h);
    return ESP_OK;
fail:
    close_camera();
    return ret;
}

/* The first whole frame after SETTLE_US; if the auto exposure is near its
 * ceiling by then, the first after FLASH_US more with the flash on. */
static esp_err_t grab(struct v4l2_buffer *b)
{
    int64_t start = esp_timer_get_time(), until = start + SETTLE_US;
    bool lit = false;
    for (int frames = 0;; frames++) {
        if (ioctl(s_fd, VIDIOC_DQBUF, b) != 0 || esp_timer_get_time() - start > GRAB_LIMIT_US) {
            return ESP_ERR_TIMEOUT;
        }
        bool whole = !(b->flags & V4L2_BUF_FLAG_ERROR) && b->bytesused;
        if (whole && esp_timer_get_time() >= until) {
            int exp = lit ? 0 : exposure();
            if (lit || exp < DARK_EXPOSURE) {
                ESP_LOGI(TAG, "kept frame %d (exposure %d)%s", frames, exp, lit ? " with the flash" : "");
                return ESP_OK;
            }
            ESP_LOGI(TAG, "exposure %d: flash", exp);
            flash(true);
            lit = true;
            until = esp_timer_get_time() + FLASH_US;
        }
        if (ioctl(s_fd, VIDIOC_QBUF, b) != 0) {
            return ESP_FAIL;
        }
    }
}

/* The sensor lies on its side in the module: turned as Espressif's Claw
 * firmware turns its preview by default, into YUV420 for the JPEG encoder. */
static esp_err_t upright(const uint8_t *uyvy, uint8_t **yuv)
{
    jpeg_encode_memory_alloc_cfg_t mem = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    size_t cap = 0;
    /* Aligned to the cache lines, as the PPA's output has to be. */
    *yuv = jpeg_alloc_encoder_mem(s_w * s_h * 3 / 2, &mem, &cap);
    if (!*yuv) {
        return ESP_ERR_NO_MEM;
    }
    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer = uyvy, .pic_w = s_w, .pic_h = s_h, .block_w = s_w, .block_h = s_h,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY,
            .yuv_range = PPA_COLOR_RANGE_LIMIT, .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = *yuv, .buffer_size = cap, .pic_w = s_h, .pic_h = s_w, .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
            .yuv_range = PPA_COLOR_RANGE_LIMIT, .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 1,
        .scale_y = 1,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_ppa, &op);
}

/* At the first of QUALITIES that fits the cap. */
static esp_err_t encode(const uint8_t *yuv, uint8_t **jpeg, uint32_t *len)
{
    jpeg_encoder_handle_t enc;
    jpeg_encode_engine_cfg_t eng = { .timeout_ms = 1000 };
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&eng, &enc), TAG, "JPEG encoder");
    jpeg_encode_memory_alloc_cfg_t mem = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t cap = 0;
    *jpeg = jpeg_alloc_encoder_mem(s_w * s_h, &mem, &cap);
    esp_err_t err = *jpeg ? ESP_ERR_INVALID_SIZE : ESP_ERR_NO_MEM;
    for (int i = 0; *jpeg && i < sizeof(QUALITIES) / sizeof(QUALITIES[0]); i++) {
        jpeg_encode_cfg_t cfg = {
            .width = s_h, .height = s_w,
            .src_type = JPEG_ENCODE_IN_FORMAT_YUV420,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
            .image_quality = QUALITIES[i],
        };
        esp_err_t e = jpeg_encoder_process(enc, &cfg, yuv, s_w * s_h * 3 / 2, *jpeg, cap, len);
        if (e != ESP_OK) {
            /* The encoder's own faults come back as INVALID_STATE, camera.c's "busy". */
            err = e == ESP_ERR_INVALID_STATE ? ESP_FAIL : e;
            break;
        }
        ESP_LOGI(TAG, "JPEG quality %d: %u bytes", QUALITIES[i], (unsigned)*len);
        if (*len <= JPEG_MAX_BYTES) {
            err = ESP_OK;
            break;
        }
    }
    jpeg_del_encoder_engine(enc);
    return err;
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
    uint8_t *yuv = NULL, *jpeg = NULL;
    uint32_t len = 0;
    if (!streaming) {
        err = ESP_FAIL;
    } else {
        struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP };
        err = grab(&b);
        flash(false);
        if (err == ESP_OK) {
            err = upright(s_buf[b.index], &yuv);
        }
        if (ioctl(s_fd, VIDIOC_STREAMOFF, &type) != 0 && ioctl(s_fd, VIDIOC_STREAMOFF, &type) != 0) {
            s_stuck = true;
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (s_stuck) {
        gpio_set_level(PWDN, 1);
    } else {
        close_camera();
    }
    /* The frame buffers are gone before the JPEG's are allocated. */
    if (err == ESP_OK) {
        err = encode(yuv, &jpeg, &len);
    }
    free(yuv);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "capture failed: %s", esp_err_to_name(err));
        free(jpeg);
        return err;
    }
    *out = (camera_frame_t){ .jpeg = jpeg, .len = len, .width = s_h, .height = s_w, .priv = jpeg };
    return ESP_OK;
}

static void release(camera_frame_t *frame)
{
    free(frame->priv);
}

static const camera_driver_t s_driver = {
    .name = "ESP-Mosaico Camera Module (DVP)",
    .max_width = 768,
    .max_height = 1280,
    .capture = capture,
    .release = release,
};

void mosaico_camera_register(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        const i2c_master_bus_config_t cfg = {
            .i2c_port = SLOT_PORT,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .sda_io_num = SLOT_SDA,
            .scl_io_num = SLOT_SCL,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
            bus = NULL;
        }
    }
    s_bus = bus;
    const i2c_device_config_t eeprom = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EEPROM_ADDR,
        .scl_speed_hz = 100000,
    };
    if (!s_bus || i2c_master_bus_add_device(s_bus, &eeprom, &s_eeprom) != ESP_OK) {
        s_eeprom = NULL;
        ESP_LOGW(TAG, "no slot I2C bus: no camera");
    } else if (module_present()) {
        /* Asleep and dark until a capture. */
        output(PWDN, 1, false);
        output(FLASH, 1, true);
    }
    camera_register(&s_driver);
}

bool mosaico_camera_capture(char **jpeg_base64, const char **error)
{
    *jpeg_base64 = NULL;
    camera_frame_t frame;
    esp_err_t err = camera_capture(&frame);
    if (err != ESP_OK) {
        *error = err == ESP_ERR_NO_MEM ? "camera memory allocation failed"
               : err == ESP_ERR_INVALID_STATE ? "camera busy"
               : err == ESP_ERR_NOT_FOUND ? "no camera"
               : err == ESP_ERR_NOT_SUPPORTED ? "camera has no UYVY format"
               : err == ESP_ERR_TIMEOUT ? "camera sent no frame"
               : err == ESP_ERR_INVALID_SIZE ? "frame over 150 KB even at low quality"
               : err == ESP_ERR_INVALID_RESPONSE ? "camera stuck: restart the device"
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
