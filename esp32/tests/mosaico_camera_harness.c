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

/* boards/mosaico_camera.c for test_mosaico_camera.py, against a scripted slot
 * EEPROM, esp_video device, PPA and JPEG encoder: the slot's pins move only
 * for a Camera Module descriptor, every capture leaves esp_video torn down
 * and the sensor asleep, an SC101IOT gets PCLK delay 0 and the flash only
 * when its auto exposure is near the ceiling (an OV3640 neither), the flash
 * is off again after, and the JPEG is the frame turned upright. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mosaico_camera_fakes.h"
#include "mosaico_camera.h"

#define W 640
#define H 480
#define FD 7
#define PWDN 48
#define FLASH 34

static int level[64], sets[64];
static gpio_mode_t mode[64];
static uint8_t eeprom[256];
static bool eeprom_acks, sensor, frames = true, video_live;
static int buses_made, bus_port = -1, bus_sda = -1, bus_scl = -1;
static int video_inits, video_deinits, opens, closes, ppa_live, encoders_live, rotations;
static uint16_t pid = 0xDA4A;           /* SC101IOT */
static uint8_t regs[256][256], page;     /* its paged registers */
static int64_t now_us, flash_on_at = -1, kept_at;
static uint32_t jpeg_len_at_quality_ten = 1000, jpeg_len_fixed, last_quality;
static uint8_t buffers[2][W * H * 2];
static struct fake_i2c_bus { int unused; } board_bus, own_bus;
static struct fake_i2c_dev { int unused; } eeprom_dev;
static i2c_master_bus_handle_t eeprom_on, sccb_bus = &own_bus;

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "err";
}

int64_t esp_timer_get_time(void)
{
    return now_us;
}

void vTaskDelay(uint32_t ticks)
{
    now_us += ticks * 1000;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t lvl)
{
    assert(pin >= 0 && pin < 64);
    level[pin] = lvl;
    sets[pin]++;
    if (pin == FLASH && lvl == 0 && flash_on_at < 0) {
        assert(mode[FLASH] == GPIO_MODE_OUTPUT_OD);
        flash_on_at = now_us;
    }
    return ESP_OK;
}

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    for (int pin = 0; pin < 64; pin++) {
        if (cfg->pin_bit_mask & BIT64(pin)) {
            mode[pin] = cfg->mode;
        }
    }
    return ESP_OK;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *out)
{
    buses_made++;
    bus_port = cfg->i2c_port;
    bus_sda = cfg->sda_io_num;
    bus_scl = cfg->scl_io_num;
    *out = &own_bus;
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *out)
{
    assert(cfg->device_address == 0x50);
    eeprom_on = bus;
    *out = &eeprom_dev;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int timeout_ms)
{
    (void)timeout_ms;
    assert(bus == eeprom_on && addr == 0x50);
    return eeprom_acks ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t tx_len,
                                      uint8_t *rx, size_t rx_len, int timeout_ms)
{
    (void)timeout_ms;
    assert(dev == &eeprom_dev && tx_len == 1 && tx[0] == 0 && rx_len <= sizeof(eeprom));
    if (!eeprom_acks) return ESP_FAIL;
    memcpy(rx, eeprom, rx_len);
    return ESP_OK;
}

esp_err_t esp_video_init_with_flags(const esp_video_init_config_t *config, uint32_t flags)
{
    static const gpio_num_t data[8] = { 16, 15, 33, 4, 14, 12, 18, 13 };
    const esp_video_init_dvp_config_t *d = config->dvp;
    assert(flags == ESP_VIDEO_INIT_FLAGS_DVP && d && !video_live);
    assert(!d->sccb_config.init_sccb && d->sccb_config.i2c_handle == sccb_bus);
    assert(memcmp(d->dvp_pin.data_io, data, sizeof(data)) == 0 && d->dvp_pin.data_width == 8);
    assert(d->dvp_pin.vsync_io == 55 && d->dvp_pin.de_io == 19 && d->dvp_pin.pclk_io == 17);
    assert(d->dvp_pin.xclk_io == GPIO_NUM_NC && d->reset_pin == 53 && d->pwdn_pin == GPIO_NUM_NC);
    /* The sensor is awake, and the flash dark, before SCCB. */
    assert(mode[PWDN] == GPIO_MODE_OUTPUT && level[PWDN] == 0);
    assert(mode[FLASH] == GPIO_MODE_OUTPUT_OD && level[FLASH] == 1);
    video_live = true;
    video_inits++;
    return ESP_OK;
}

esp_err_t esp_video_deinit(void)
{
    assert(video_live);
    video_live = false;
    video_deinits++;
    return ESP_OK;
}

int fake_open(const char *path, int flags)
{
    assert(video_live && strcmp(path, "/dev/video2") == 0 && flags == O_RDONLY);
    if (!sensor) return -1;   /* esp_video_init found no sensor */
    opens++;
    return FD;
}

int fake_close(int fd)
{
    assert(fd == FD);
    closes++;
    return 0;
}

int fake_ioctl(int fd, unsigned long request, void *arg)
{
    assert(fd == FD && video_live);
    struct v4l2_buffer *b = arg;
    struct v4l2_format *f = arg;
    switch (request) {
    case VIDIOC_G_FMT:
        f->fmt.pix.width = W;
        f->fmt.pix.height = H;
        f->fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
        return 0;
    case VIDIOC_S_FMT:
        assert(f->fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY);
        return 0;
    case VIDIOC_QUERYBUF:
        assert(b->index < 2);
        b->length = sizeof(buffers[0]);
        b->m.offset = b->index;
        return 0;
    case VIDIOC_DQBUF:
        if (!frames) return -1;   /* the DQBUF timeout */
        now_us += 40000;
        b->index = (uint32_t)(now_us / 40000) % 2;
        b->bytesused = sizeof(buffers[0]);
        b->flags = 0;
        memset(buffers[b->index], level[FLASH] == 0 ? 160 : 80, sizeof(buffers[0]));
        return 0;
    case VIDIOC_G_EXT_CTRLS:
    case VIDIOC_S_EXT_CTRLS: {
        struct v4l2_ext_controls *cs = arg;
        struct v4l2_ext_control *c = cs->controls;
        assert(cs->ctrl_class == V4L2_CTRL_CLASS_ESP_CAM_IOCTL && cs->count == 1);
        esp_cam_sensor_reg_val_t *r = (esp_cam_sensor_reg_val_t *)c->p_u8;
        if (c->id == ESP_CAM_SENSOR_IOC_G_CHIP_ID) {
            assert(request == VIDIOC_G_EXT_CTRLS && c->size == sizeof(esp_cam_sensor_id_t));
            ((esp_cam_sensor_id_t *)c->p_u8)->pid = pid;
        } else if (c->id == ESP_CAM_SENSOR_IOC_S_REG) {
            assert(request == VIDIOC_S_EXT_CTRLS && r->regaddr < 256 && r->value < 256);
            if (r->regaddr == 0xf0) page = r->value; else regs[page][r->regaddr] = r->value;
        } else {
            assert(c->id == ESP_CAM_SENSOR_IOC_G_REG && request == VIDIOC_G_EXT_CTRLS && r->regaddr < 256);
            r->value = r->regaddr == 0xf0 ? page : regs[page][r->regaddr];
        }
        return 0;
    }
    default:
        return 0;
    }
}

void *fake_mmap(void *addr, size_t len, int prot, int flags, int fd, long offset)
{
    (void)addr;
    (void)prot;
    (void)flags;
    assert(fd == FD && len == sizeof(buffers[0]) && offset >= 0 && offset < 2);
    return buffers[offset];
}

esp_err_t ppa_register_client(const ppa_client_config_t *cfg, ppa_client_handle_t *out)
{
    assert(cfg->oper_type == PPA_OPERATION_SRM);
    ppa_live++;
    *out = (ppa_client_handle_t)&ppa_live;
    return ESP_OK;
}

esp_err_t ppa_unregister_client(ppa_client_handle_t client)
{
    assert(client);
    ppa_live--;
    return ESP_OK;
}

esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t client, const ppa_srm_oper_config_t *op)
{
    /* Turned while the frame is still esp_video's, and with the flash off. */
    assert(client && video_live && level[FLASH] == 1);
    assert(op->in.buffer == buffers[0] || op->in.buffer == buffers[1]);
    assert(op->in.pic_w == W && op->in.pic_h == H && op->in.block_w == W && op->in.block_h == H);
    assert(op->in.srm_cm == PPA_SRM_COLOR_MODE_YUV422_UYVY && op->out.srm_cm == PPA_SRM_COLOR_MODE_YUV420);
    assert(op->out.pic_w == H && op->out.pic_h == W && op->out.buffer_size >= W * H * 3 / 2);
    assert(op->rotation_angle == PPA_SRM_ROTATION_ANGLE_90 && !op->mirror_y && !op->mirror_x);
    assert(op->scale_x == 1 && op->scale_y == 1 && op->mode == PPA_TRANS_MODE_BLOCKING);
    rotations++;
    kept_at = now_us;
    return ESP_OK;
}

esp_err_t jpeg_new_encoder_engine(const jpeg_encode_engine_cfg_t *cfg, jpeg_encoder_handle_t *out)
{
    (void)cfg;
    assert(!video_live);   /* the frame buffers go before the JPEG's come */
    encoders_live++;
    *out = (jpeg_encoder_handle_t)&encoders_live;
    return ESP_OK;
}

esp_err_t jpeg_del_encoder_engine(jpeg_encoder_handle_t encoder)
{
    assert(encoder);
    encoders_live--;
    return ESP_OK;
}

void *jpeg_alloc_encoder_mem(size_t size, const jpeg_encode_memory_alloc_cfg_t *cfg, size_t *allocated)
{
    (void)cfg;
    *allocated = size;
    return calloc(1, size);
}

esp_err_t jpeg_encoder_process(jpeg_encoder_handle_t encoder, const jpeg_encode_cfg_t *cfg,
                               const uint8_t *in, uint32_t in_len, uint8_t *out,
                               uint32_t out_cap, uint32_t *out_len)
{
    assert(encoder && in && in_len == W * H * 3 / 2 && cfg->src_type == JPEG_ENCODE_IN_FORMAT_YUV420);
    assert(cfg->width == H && cfg->height == W);   /* upright */
    last_quality = cfg->image_quality;
    uint32_t len = jpeg_len_fixed ? jpeg_len_fixed : jpeg_len_at_quality_ten * cfg->image_quality / 10;
    if (len > out_cap) return ESP_FAIL;
    *out_len = len;
    out[0] = 0xFF;
    out[1] = 0xD8;
    return ESP_OK;
}

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = (slen + 2) / 3 * 4;
    *olen = need;
    if (dlen < need + 1) return -1;
    size_t o = 0;
    for (size_t i = 0; i < slen; i += 3) {
        uint32_t v = (uint32_t)src[i] << 16 | (i + 1 < slen ? src[i + 1] << 8 : 0)
                     | (i + 2 < slen ? src[i + 2] : 0);
        dst[o++] = abc[v >> 18 & 63];
        dst[o++] = abc[v >> 12 & 63];
        dst[o++] = i + 1 < slen ? abc[v >> 6 & 63] : '=';
        dst[o++] = i + 2 < slen ? abc[v & 63] : '=';
    }
    dst[o] = 0;
    return 0;
}

static uint16_t crc16_modbus(const uint8_t *p, size_t n)
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

/* A V1 descriptor as mosaico_module_mgr reads it: "ESP", the board type, ...,
 * the descriptor's CRC16 at 0x34. */
static void program_eeprom(uint8_t type, bool good_crc)
{
    memset(eeprom, 0xFF, sizeof(eeprom));
    memset(eeprom, 0, 0x34);
    memcpy(eeprom, "ESP", 3);
    eeprom[3] = type;
    strcpy((char *)eeprom + 0x14, "Module-Camera");
    uint16_t crc = crc16_modbus(eeprom, 0x34) ^ (good_crc ? 0 : 1);
    eeprom[0x34] = crc & 0xFF;
    eeprom[0x35] = crc >> 8;
}

static void set_exposure(int lines)
{
    regs[0x00][0x8c] = lines & 0xff;
    regs[0x00][0x8d] = lines >> 8;
}

/* One capture; checks that it left nothing of esp_video behind, the sensor
 * asleep and the flash dark. */
static bool capture(char **b64, const char **error)
{
    *error = NULL;
    flash_on_at = -1;
    bool ok = mosaico_camera_capture(b64, error);
    assert(!video_live && video_inits == video_deinits);
    assert(opens == closes && ppa_live == 0 && encoders_live == 0);
    assert(ok == (*b64 != NULL) && ok == (*error == NULL));
    assert(!sets[PWDN] || level[PWDN] == 1);
    assert(!sets[FLASH] || level[FLASH] == 1);
    return ok;
}

int main(void)
{
    char *b64 = NULL;
    const char *error = NULL;
    assert(crc16_modbus((const uint8_t *)"123456789", 9) == 0x4B37);

    /* v1.2: the slots' own bus, I2C1 on GPIO0 and 1. An empty slot: none of
     * its pins move. */
    mosaico_camera_register(NULL);
    assert(buses_made == 1 && bus_port == 1 && bus_sda == 0 && bus_scl == 1 && eeprom_on == &own_bus);
    assert(!capture(&b64, &error) && strcmp(error, "no camera") == 0);

    /* Another module (a ToF board), or a corrupt descriptor: still none. */
    eeprom_acks = true;
    program_eeprom(0x10, true);
    assert(!capture(&b64, &error) && strcmp(error, "no camera") == 0);
    program_eeprom(0x07, false);
    assert(!capture(&b64, &error) && strcmp(error, "no camera") == 0);
    assert(video_inits == 0 && !sets[PWDN] && !sets[FLASH]);

    /* A Camera Module whose sensor doesn't answer: esp_video comes up and
     * goes, and the sensor sleeps again. */
    program_eeprom(0x07, true);
    assert(!capture(&b64, &error) && strcmp(error, "no camera") == 0);
    assert(video_inits == 1);

    /* A lit scene: PCLK delay 0, the table's other bits kept; no flash; the
     * frame from after 1.5 s, upright, at quality 70 (90 and 80 are over
     * 150 KB). */
    sensor = true;
    regs[0x36][0x40] = 0x07;
    set_exposure(288);
    jpeg_len_at_quality_ten = 20000;
    int64_t start = now_us;
    assert(capture(&b64, &error));
    assert(flash_on_at < 0 && rotations == 1 && kept_at - start >= 1500000 && last_quality == 70);
    assert(regs[0x36][0x40] == 0x04);
    assert(strncmp(b64, "/9", 2) == 0 && strlen(b64) == (140000 + 2) / 3 * 4);
    free(b64);

    /* A dark one: the flash once the scene has settled, and the frame from
     * 0.7 s after it came on. */
    set_exposure(1398);
    start = now_us;
    assert(capture(&b64, &error));
    assert(flash_on_at - start >= 1500000 && kept_at - flash_on_at >= 700000 && rotations == 2);
    free(b64);

    /* Over 150 KB even at quality 20, and a sensor that stops sending. */
    jpeg_len_fixed = 200000;
    assert(!capture(&b64, &error) && strcmp(error, "frame over 150 KB even at low quality") == 0);
    assert(last_quality == 20);
    jpeg_len_fixed = 0;
    frames = false;
    assert(!capture(&b64, &error) && strcmp(error, "camera sent no frame") == 0);
    frames = true;

    /* v1.0: the board's own bus, which the slots share. */
    sccb_bus = &board_bus;
    mosaico_camera_register(&board_bus);
    assert(buses_made == 1 && eeprom_on == &board_bus);
    set_exposure(288);
    assert(capture(&b64, &error));
    free(b64);

    /* An OV3640: its registers aren't the SC101IOT's, so no PCLK write and
     * no flash, whatever those addresses hold. */
    pid = 0x364C;
    regs[0x36][0x40] = 0x07;
    set_exposure(1398);
    start = now_us;
    assert(capture(&b64, &error));
    assert(flash_on_at < 0 && regs[0x36][0x40] == 0x07 && kept_at - start < 2200000);
    free(b64);
    pid = 0xDA4A;

    printf("ok\n");
    return 0;
}
