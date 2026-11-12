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
 * WeatherXM WG1200 Indoor (BMP390) + Outdoor (SX1262 LoRa WS1001/WS1300)
 * Weather Station subsystem and gadget tool handlers.
 */

#include "wg1200_weather.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "boards/board_wxm_wg1200.h"
#include "wg1200_gps.h"
#include "wxm_boot_mgr.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wg1200_weather";

/* External real NVS functions from wxm_boot_mgr.c for read-only factory NVS inspection */
extern esp_err_t __real_nvs_flash_init_partition(const char *partition_label);
extern esp_err_t __real_nvs_open_from_partition(const char *part_name,
                                                const char *namespace_name,
                                                nvs_open_mode_t open_mode,
                                                nvs_handle_t *out_handle);

/* ── BMP390 Registers & Constants ────────────────────────────────────────── */

#define BMP390_I2C_ADDR_PRIMARY     0x77
#define BMP390_I2C_ADDR_SECONDARY   0x76
#define BMP390_I2C_SPEED_HZ         400000

#define BMP3_REG_CHIP_ID            0x00
#define BMP3_REG_ERR                0x02
#define BMP3_REG_STATUS             0x03
#define BMP3_REG_DATA               0x04
#define BMP3_REG_INT_CTRL           0x19
#define BMP3_REG_PWR_CTRL           0x1B
#define BMP3_REG_OSR                0x1C
#define BMP3_REG_ODR                0x1D
#define BMP3_REG_CONFIG             0x1F
#define BMP3_REG_CALIB_DATA         0x31
#define BMP3_REG_CMD                0x7E

#define BMP3_CHIP_ID_BMP388         0x50
#define BMP3_CHIP_ID_BMP390         0x60
#define BMP3_CMD_SOFT_RESET         0xB6
#define BMP3_CALIB_DATA_LEN         21

typedef struct {
    double par_t1;
    double par_t2;
    double par_t3;
    double par_p1;
    double par_p2;
    double par_p3;
    double par_p4;
    double par_p5;
    double par_p6;
    double par_p7;
    double par_p8;
    double par_p9;
    double par_p10;
    double par_p11;
    double t_lin;
} bmp3_quantized_calib_t;

/* ── SX1262 Hardware Pins & Opcodes ──────────────────────────────────────── */

#define WG1200_LORA_SPI_HOST        SPI3_HOST
#define WG1200_LORA_PIN_SCLK        GPIO_NUM_41
#define WG1200_LORA_PIN_MOSI        GPIO_NUM_48
#define WG1200_LORA_PIN_MISO        GPIO_NUM_47
#define WG1200_LORA_PIN_EXP_INT     GPIO_NUM_42
#define WG1200_LORA_SPI_HZ          2000000

#define SX126X_CMD_SET_SLEEP                0x84
#define SX126X_CMD_SET_STANDBY              0x80
#define SX126X_CMD_SET_FS                   0xC1
#define SX126X_CMD_SET_TX                   0x83
#define SX126X_CMD_SET_RX                   0x82
#define SX126X_CMD_STOP_TIMER_ON_PREAMBLE   0x9F
#define SX126X_CMD_SET_LORA_SYMB_TIMEOUT    0xA0
#define SX126X_CMD_SET_PACKET_TYPE          0x8A
#define SX126X_CMD_SET_RF_FREQUENCY         0x86
#define SX126X_CMD_SET_PA_CONFIG            0x95
#define SX126X_CMD_SET_TX_PARAMS            0x8E
#define SX126X_CMD_SET_BUFFER_BASE_ADDR     0x8F
#define SX126X_CMD_SET_MODULATION_PARAMS    0x8B
#define SX126X_CMD_SET_PACKET_PARAMS        0x8C
#define SX126X_CMD_SET_DIO_IRQ_PARAMS       0x08
#define SX126X_CMD_GET_IRQ_STATUS           0x12
#define SX126X_CMD_CLR_IRQ_STATUS           0x02
#define SX126X_CMD_SET_DIO2_AS_RFSWITCH     0x9D
#define SX126X_CMD_SET_REGULATOR_MODE       0x96
#define SX126X_CMD_CALIBRATE                0x89
#define SX126X_CMD_CALIBRATE_IMAGE          0x98
#define SX126X_CMD_GET_STATUS               0xC0
#define SX126X_CMD_GET_RX_BUFFER_STATUS     0x13
#define SX126X_CMD_GET_PACKET_STATUS        0x14
#define SX126X_CMD_WRITE_REGISTER           0x0D
#define SX126X_CMD_READ_REGISTER            0x1D
#define SX126X_CMD_WRITE_BUFFER             0x0E
#define SX126X_CMD_READ_BUFFER              0x1E

#define SX126X_REG_LR_SYNCWORD              0x0740
#define SX126X_REG_IQ_POLARITY              0x0736
#define SX126X_REG_RX_GAIN                  0x08AC
#define SX126X_REG_TX_CLAMP_CFG             0x08D8

#define SX126X_IRQ_TX_DONE                  (1U << 0)
#define SX126X_IRQ_RX_DONE                  (1U << 1)
#define SX126X_IRQ_CRC_ERROR                (1U << 6)
#define SX126X_IRQ_TIMEOUT                  (1U << 9)
#define SX126X_IRQ_ALL                      0x03FFU

/* ── State ───────────────────────────────────────────────────────────────── */

static SemaphoreHandle_t s_state_mutex = NULL;
static wg1200_weather_state_t s_state = {
    .region = WG1200_REGION_EU868,
    .freq_hz = 868000000UL,
};
static volatile bool s_region_dirty = false;

static i2c_master_dev_handle_t s_bmp_dev = NULL;
static bmp3_quantized_calib_t s_bmp_calib = {0};

static spi_device_handle_t s_lora_spi = NULL;

static const uint32_t k_region_freqs[WG1200_REGION_COUNT] = {
    [WG1200_REGION_EU868] = 868000000UL,
    [WG1200_REGION_US915] = 915000000UL,
    [WG1200_REGION_AS923] = 923000000UL,
};

static const char *const k_region_names[WG1200_REGION_COUNT] = {
    [WG1200_REGION_EU868] = "EU 868 MHz",
    [WG1200_REGION_US915] = "US 915 MHz",
    [WG1200_REGION_AS923] = "AS 923 MHz",
};

const char *wg1200_weather_region_name(wg1200_weather_region_t region)
{
    if (region < 0 || region >= WG1200_REGION_COUNT) {
        return "EU 868 MHz";
    }
    return k_region_names[region];
}

/* ── BMP390 Implementation ───────────────────────────────────────────────── */

static esp_err_t bmp3_write_reg(uint8_t reg, uint8_t val)
{
    if (!s_bmp_dev) return ESP_ERR_INVALID_STATE;
    if (!wg1200_i2c_lock(200)) return ESP_ERR_TIMEOUT;
    uint8_t buf[2] = {reg, val};
    esp_err_t err = i2c_master_transmit(s_bmp_dev, buf, sizeof(buf), 100);
    wg1200_i2c_unlock();
    return err;
}

static esp_err_t bmp3_read_regs(uint8_t reg, uint8_t *out, size_t len)
{
    if (!s_bmp_dev) return ESP_ERR_INVALID_STATE;
    if (!wg1200_i2c_lock(200)) return ESP_ERR_TIMEOUT;
    esp_err_t err = i2c_master_transmit_receive(s_bmp_dev, &reg, 1, out, len, 100);
    wg1200_i2c_unlock();
    return err;
}

static void bmp3_parse_calib_data(const uint8_t *reg_data, bmp3_quantized_calib_t *q)
{
    uint16_t par_t1 = ((uint16_t)reg_data[1] << 8) | reg_data[0];
    uint16_t par_t2 = ((uint16_t)reg_data[3] << 8) | reg_data[2];
    int8_t par_t3   = (int8_t)reg_data[4];
    int16_t par_p1  = (int16_t)(((uint16_t)reg_data[6] << 8) | reg_data[5]);
    int16_t par_p2  = (int16_t)(((uint16_t)reg_data[8] << 8) | reg_data[7]);
    int8_t par_p3   = (int8_t)reg_data[9];
    int8_t par_p4   = (int8_t)reg_data[10];
    uint16_t par_p5 = ((uint16_t)reg_data[12] << 8) | reg_data[11];
    uint16_t par_p6 = ((uint16_t)reg_data[14] << 8) | reg_data[13];
    int8_t par_p7   = (int8_t)reg_data[15];
    int8_t par_p8   = (int8_t)reg_data[16];
    int16_t par_p9  = (int16_t)(((uint16_t)reg_data[18] << 8) | reg_data[17]);
    int8_t par_p10  = (int8_t)reg_data[19];
    int8_t par_p11  = (int8_t)reg_data[20];

    q->par_t1  = (double)par_t1 / 0.00390625;
    q->par_t2  = (double)par_t2 / 1073741824.0;
    q->par_t3  = (double)par_t3 / 281474976710656.0;
    q->par_p1  = (double)(par_p1 - 16384) / 1048576.0;
    q->par_p2  = (double)(par_p2 - 16384) / 536870912.0;
    q->par_p3  = (double)par_p3 / 4294967296.0;
    q->par_p4  = (double)par_p4 / 137438953472.0;
    q->par_p5  = (double)par_p5 / 0.125;
    q->par_p6  = (double)par_p6 / 64.0;
    q->par_p7  = (double)par_p7 / 256.0;
    q->par_p8  = (double)par_p8 / 32768.0;
    q->par_p9  = (double)par_p9 / 281474976710656.0;
    q->par_p10 = (double)par_p10 / 281474976710656.0;
    q->par_p11 = (double)par_p11 / 36893488147419103232.0;
}

static esp_err_t bmp390_init_hw(void)
{
    i2c_master_bus_handle_t bus = wg1200_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;

    /* Ensure BMP390 power switch (PCA9535 IO1_2) is ON */
    wg1200_io_set_direction(WG1200_EXP_BMP_PWR, true);
    wg1200_io_set_level(WG1200_EXP_BMP_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(15));

    const uint8_t addrs[2] = {BMP390_I2C_ADDR_PRIMARY, BMP390_I2C_ADDR_SECONDARY};
    uint8_t found_addr = 0;

    for (size_t i = 0; i < 2; i++) {
        if (!wg1200_i2c_lock(200)) continue;
        esp_err_t probe = i2c_master_probe(bus, addrs[i], 50);
        wg1200_i2c_unlock();
        if (probe == ESP_OK) {
            found_addr = addrs[i];
            break;
        }
    }

    if (!found_addr) {
        ESP_LOGW(TAG, "BMP390 not detected on I2C0 (0x77/0x76)");
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = found_addr,
        .scl_speed_hz = BMP390_I2C_SPEED_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_bmp_dev);
    if (err != ESP_OK) return err;

    uint8_t chip_id = 0;
    err = bmp3_read_regs(BMP3_REG_CHIP_ID, &chip_id, 1);
    if (err != ESP_OK || (chip_id != BMP3_CHIP_ID_BMP390 && chip_id != BMP3_CHIP_ID_BMP388)) {
        ESP_LOGW(TAG, "Unexpected BMP3xx chip ID 0x%02X (err=%s)", chip_id, esp_err_to_name(err));
        return ESP_ERR_INVALID_RESPONSE;
    }

    bmp3_write_reg(BMP3_REG_CMD, BMP3_CMD_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t calib_raw[BMP3_CALIB_DATA_LEN] = {0};
    err = bmp3_read_regs(BMP3_REG_CALIB_DATA, calib_raw, sizeof(calib_raw));
    if (err != ESP_OK) return err;
    bmp3_parse_calib_data(calib_raw, &s_bmp_calib);

    /* Configure 2x oversampling for pressure & temp, ODR 50 Hz, DRDY enabled, Normal mode */
    bmp3_write_reg(BMP3_REG_OSR, 0x09);      /* press_os = 2x (001), temp_os = 2x (001) */
    bmp3_write_reg(BMP3_REG_ODR, 0x02);      /* ODR = 50 Hz */
    bmp3_write_reg(BMP3_REG_INT_CTRL, 0x40); /* drdy_en = 1 */
    bmp3_write_reg(BMP3_REG_PWR_CTRL, 0x33); /* press_en=1, temp_en=1, mode=NORMAL(0x30) */

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.bmp390_detected = true;
    s_state.bmp390_chip_id = chip_id;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "BMP390 initialized at I2C 0x%02X (chip_id=0x%02X)", found_addr, chip_id);
    return ESP_OK;
}

static esp_err_t bmp390_poll(float *out_temp_c, float *out_press_hpa)
{
    if (!s_bmp_dev) return ESP_ERR_INVALID_STATE;

    uint8_t status = 0;
    esp_err_t err = bmp3_read_regs(BMP3_REG_STATUS, &status, 1);
    if (err != ESP_OK) return err;

    uint8_t raw[6] = {0};
    err = bmp3_read_regs(BMP3_REG_DATA, raw, sizeof(raw));
    if (err != ESP_OK) return err;

    /* Clear status register */
    (void)bmp3_read_regs(BMP3_REG_STATUS, &status, 1);

    uint32_t uncomp_press = ((uint32_t)raw[2] << 16) | ((uint32_t)raw[1] << 8) | raw[0];
    uint32_t uncomp_temp  = ((uint32_t)raw[5] << 16) | ((uint32_t)raw[4] << 8) | raw[3];

    /* Compensate temperature */
    double pd1 = (double)uncomp_temp - s_bmp_calib.par_t1;
    double pd2 = pd1 * s_bmp_calib.par_t2;
    double t_lin = pd2 + (pd1 * pd1) * s_bmp_calib.par_t3;
    s_bmp_calib.t_lin = t_lin;

    /* Compensate pressure (in Pa) */
    double t2 = t_lin * t_lin;
    double t3 = t2 * t_lin;
    double po1 = s_bmp_calib.par_p5 + s_bmp_calib.par_p6 * t_lin +
                 s_bmp_calib.par_p7 * t2 + s_bmp_calib.par_p8 * t3;
    double po2 = (double)uncomp_press *
                 (s_bmp_calib.par_p1 + s_bmp_calib.par_p2 * t_lin +
                  s_bmp_calib.par_p3 * t2 + s_bmp_calib.par_p4 * t3);
    double up2 = (double)uncomp_press * (double)uncomp_press;
    double up3 = up2 * (double)uncomp_press;
    double pd3 = up2 * (s_bmp_calib.par_p9 + s_bmp_calib.par_p10 * t_lin) +
                 up3 * s_bmp_calib.par_p11;
    double comp_press_pa = po1 + po2 + pd3;

    if (out_temp_c) *out_temp_c = (float)t_lin;
    if (out_press_hpa) *out_press_hpa = (float)(comp_press_pa / 100.0);
    return ESP_OK;
}

/* ── SX1262 LoRa Driver ──────────────────────────────────────────────────── */

static bool sx1262_wait_busy(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        int busy = wg1200_io_get_level(WG1200_EXP_LORA_BUSY);
        if (busy == 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        elapsed += 2;
    }
    ESP_LOGW(TAG, "SX1262 BUSY timeout (%lu ms)", (unsigned long)timeout_ms);
    return false;
}

static bool sx1262_cs_select(uint32_t timeout_ms)
{
    if (!sx1262_wait_busy(timeout_ms)) {
        return false;
    }
    if (wg1200_io_set_level(WG1200_EXP_LORA_NSS, 0) != ESP_OK) {
        ESP_LOGW(TAG, "SX1262 NSS assert failed");
        return false;
    }
    return true;
}

static void sx1262_cs_deselect(bool wait_busy)
{
    (void)wg1200_io_set_level(WG1200_EXP_LORA_NSS, 1);
    if (wait_busy) {
        (void)sx1262_wait_busy(100);
    }
}

static esp_err_t sx1262_spi_tx(const uint8_t *tx, size_t len)
{
    if (!s_lora_spi || len == 0) return ESP_OK;
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = NULL,
    };
    return spi_device_transmit(s_lora_spi, &t);
}

static esp_err_t sx1262_spi_rx(uint8_t *rx, size_t len)
{
    if (!s_lora_spi || len == 0) return ESP_OK;
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = NULL,
        .rx_buffer = rx,
    };
    return spi_device_transmit(s_lora_spi, &t);
}

static void sx1262_write_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    if (!sx1262_cs_select(100)) {
        return;
    }
    sx1262_spi_tx(&cmd, 1);
    if (data && len > 0) {
        sx1262_spi_tx(data, len);
    }
    sx1262_cs_deselect(cmd != SX126X_CMD_SET_SLEEP);
}

static void sx1262_read_cmd(uint8_t cmd, uint8_t *data, size_t len)
{
    if (data && len > 0) {
        memset(data, 0, len);
    }
    if (!sx1262_cs_select(100)) {
        return;
    }
    uint8_t hdr[2] = {cmd, 0x00};
    sx1262_spi_tx(hdr, sizeof(hdr));
    if (data && len > 0) {
        sx1262_spi_rx(data, len);
    }
    sx1262_cs_deselect(true);
}

static void sx1262_write_reg(uint16_t addr, uint8_t val)
{
    if (!sx1262_cs_select(100)) {
        return;
    }
    uint8_t buf[4] = {
        SX126X_CMD_WRITE_REGISTER,
        (uint8_t)(addr >> 8),
        (uint8_t)(addr & 0xFF),
        val,
    };
    sx1262_spi_tx(buf, sizeof(buf));
    sx1262_cs_deselect(true);
}

static uint8_t sx1262_read_reg(uint16_t addr)
{
    uint8_t val = 0;
    if (!sx1262_cs_select(100)) {
        return 0;
    }
    uint8_t hdr[4] = {
        SX126X_CMD_READ_REGISTER,
        (uint8_t)(addr >> 8),
        (uint8_t)(addr & 0xFF),
        0x00,
    };
    sx1262_spi_tx(hdr, sizeof(hdr));
    sx1262_spi_rx(&val, 1);
    sx1262_cs_deselect(true);
    return val;
}

static void sx1262_write_buffer(uint8_t offset, const uint8_t *data, size_t len)
{
    if (!sx1262_cs_select(100)) {
        return;
    }
    uint8_t hdr[2] = {SX126X_CMD_WRITE_BUFFER, offset};
    sx1262_spi_tx(hdr, sizeof(hdr));
    if (data && len > 0) {
        sx1262_spi_tx(data, len);
    }
    sx1262_cs_deselect(true);
}

static void sx1262_read_buffer(uint8_t offset, uint8_t *data, size_t len)
{
    if (data && len > 0) {
        memset(data, 0, len);
    }
    if (!sx1262_cs_select(100)) {
        return;
    }
    uint8_t hdr[3] = {SX126X_CMD_READ_BUFFER, offset, 0x00};
    sx1262_spi_tx(hdr, sizeof(hdr));
    if (data && len > 0) {
        sx1262_spi_rx(data, len);
    }
    sx1262_cs_deselect(true);
}

static void sx1262_set_rf_freq(uint32_t freq_hz)
{
    uint8_t cal[2] = {0xD7, 0xDB}; /* 863..870 MHz default */
    if (freq_hz > 900000000UL) {
        cal[0] = 0xE1;
        cal[1] = 0xE9;
    }
    sx1262_write_cmd(SX126X_CMD_CALIBRATE_IMAGE, cal, sizeof(cal));

    /* PLL steps = freq_hz * 2^25 / 32e6 */
    uint32_t steps = (uint32_t)(((uint64_t)freq_hz << 25) / 32000000ULL);
    uint8_t buf[4] = {
        (uint8_t)((steps >> 24) & 0xFF),
        (uint8_t)((steps >> 16) & 0xFF),
        (uint8_t)((steps >> 8) & 0xFF),
        (uint8_t)(steps & 0xFF),
    };
    sx1262_write_cmd(SX126X_CMD_SET_RF_FREQUENCY, buf, sizeof(buf));
}

static void sx1262_start_rx_continuous(uint32_t freq_hz)
{
    uint8_t stdby_xosc = 0x01;
    sx1262_write_cmd(SX126X_CMD_SET_STANDBY, &stdby_xosc, 1);

    uint8_t pkt_type_lora = 0x01;
    sx1262_write_cmd(SX126X_CMD_SET_PACKET_TYPE, &pkt_type_lora, 1);

    sx1262_set_rf_freq(freq_hz);

    uint8_t stop_tmr = 0x00;
    sx1262_write_cmd(SX126X_CMD_STOP_TIMER_ON_PREAMBLE, &stop_tmr, 1);

    /* WeatherXM WS1001/WS1300 LoRa config: SF7 (0x07), BW_125 (0x04), CR_4_5 (0x01), LDO=0 */
    uint8_t mod_params[4] = {0x07, 0x04, 0x01, 0x00};
    sx1262_write_cmd(SX126X_CMD_SET_MODULATION_PARAMS, mod_params, sizeof(mod_params));

    /* Preamble=12, Variable length (0x00), MaxPayload=0xFF, CRC=OFF (0x00), IQ=Normal (0x00) */
    uint8_t pkt_params[6] = {0x00, 12, 0x00, 0xFF, 0x00, 0x00};
    sx1262_write_cmd(SX126X_CMD_SET_PACKET_PARAMS, pkt_params, sizeof(pkt_params));

    uint8_t symb_timeout = 0x00;
    sx1262_write_cmd(SX126X_CMD_SET_LORA_SYMB_TIMEOUT, &symb_timeout, 1);

    /* Normal IQ polarity workaround (DS_SX1261-2_V1.2 chapter 15.4) */
    sx1262_write_reg(SX126X_REG_IQ_POLARITY, sx1262_read_reg(SX126X_REG_IQ_POLARITY) | (1U << 2));

    /* Clear pending IRQs and enable RX_DONE + CRC_ERROR + TX_DONE on DIO1 */
    uint8_t clr_all[2] = {0x03, 0xFF};
    sx1262_write_cmd(SX126X_CMD_CLR_IRQ_STATUS, clr_all, sizeof(clr_all));

    uint16_t irq_mask = SX126X_IRQ_RX_DONE | SX126X_IRQ_CRC_ERROR | SX126X_IRQ_TX_DONE;
    uint8_t dio_irq[8] = {
        (uint8_t)(irq_mask >> 8), (uint8_t)(irq_mask & 0xFF),
        (uint8_t)(irq_mask >> 8), (uint8_t)(irq_mask & 0xFF),
        0x00, 0x00,
        0x00, 0x00,
    };
    sx1262_write_cmd(SX126X_CMD_SET_DIO_IRQ_PARAMS, dio_irq, sizeof(dio_irq));

    /* Max LNA boosted RX gain */
    sx1262_write_reg(SX126X_REG_RX_GAIN, 0x96);

    /* Continuous RX (0xFFFFFF) */
    uint8_t rx_cont[3] = {0xFF, 0xFF, 0xFF};
    sx1262_write_cmd(SX126X_CMD_SET_RX, rx_cont, sizeof(rx_cont));
}

static void sx1262_tx_packet(const uint8_t *payload, uint8_t len, uint32_t freq_hz)
{
    uint8_t stdby_xosc = 0x01;
    sx1262_write_cmd(SX126X_CMD_SET_STANDBY, &stdby_xosc, 1);

    /* PA config for SX1262 +22 dBm */
    sx1262_write_reg(SX126X_REG_TX_CLAMP_CFG, sx1262_read_reg(SX126X_REG_TX_CLAMP_CFG) | (0x0F << 1));
    uint8_t pa_cfg[4] = {0x04, 0x07, 0x00, 0x01};
    sx1262_write_cmd(SX126X_CMD_SET_PA_CONFIG, pa_cfg, sizeof(pa_cfg));

    uint8_t tx_params[2] = {22, 0x04}; /* +22 dBm, 200 us ramp */
    sx1262_write_cmd(SX126X_CMD_SET_TX_PARAMS, tx_params, sizeof(tx_params));

    uint8_t pkt_params[6] = {0x00, 12, 0x00, len, 0x00, 0x00};
    sx1262_write_cmd(SX126X_CMD_SET_PACKET_PARAMS, pkt_params, sizeof(pkt_params));

    sx1262_write_buffer(0x00, payload, len);

    /* Transmit with ~1s timeout (64000 * 15.625 us) */
    uint8_t tx_cmd[3] = {0x00, 0xFA, 0x00};
    sx1262_write_cmd(SX126X_CMD_SET_TX, tx_cmd, sizeof(tx_cmd));

    /* Wait up to 300 ms for TX_DONE then resume RX continuous */
    for (int i = 0; i < 60; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        uint8_t irq_buf[2] = {0};
        sx1262_read_cmd(SX126X_CMD_GET_IRQ_STATUS, irq_buf, sizeof(irq_buf));
        uint16_t irq = ((uint16_t)irq_buf[0] << 8) | irq_buf[1];
        if (irq & (SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT)) {
            break;
        }
    }
    sx1262_start_rx_continuous(freq_hz);
}

static esp_err_t sx1262_init_hw(uint32_t freq_hz)
{
    /* Configure PCA9535 LoRa control lines */
    wg1200_io_set_level(WG1200_EXP_LORA_NSS, 1);
    wg1200_io_set_direction(WG1200_EXP_LORA_NSS, true);
    wg1200_io_set_level(WG1200_EXP_LORA_RST, 1);
    wg1200_io_set_direction(WG1200_EXP_LORA_RST, true);
    wg1200_io_set_direction(WG1200_EXP_LORA_BUSY, false);
    wg1200_io_set_direction(WG1200_EXP_LORA_DIO1, false);

    /* Configure GPIO42 as input with pullup for PCA9535 interrupt */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << WG1200_LORA_PIN_EXP_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* Initialize SPI3 bus (shared pins GPIO41/48 were released by ST7701S init) */
    spi_bus_config_t buscfg = {
        .mosi_io_num = WG1200_LORA_PIN_MOSI,
        .miso_io_num = WG1200_LORA_PIN_MISO,
        .sclk_io_num = WG1200_LORA_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };
    esp_err_t err = spi_bus_initialize(WG1200_LORA_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI3 init failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = WG1200_LORA_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1, /* Software CS via PCA9535 IO0_0 */
        .queue_size = 4,
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    err = spi_bus_add_device(WG1200_LORA_SPI_HOST, &devcfg, &s_lora_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SX1262 SPI device add failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Hardware reset SX1262 */
    vTaskDelay(pdMS_TO_TICKS(10));
    wg1200_io_set_level(WG1200_EXP_LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    wg1200_io_set_level(WG1200_EXP_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    sx1262_wait_busy(100);

    /* Read LoRa SyncWord (0x0740..0x0741) to verify SX1262 presence (default 0x1424) */
    uint8_t sw_msb = sx1262_read_reg(SX126X_REG_LR_SYNCWORD);
    uint8_t sw_lsb = sx1262_read_reg(SX126X_REG_LR_SYNCWORD + 1);
    uint16_t sync_word = ((uint16_t)sw_msb << 8) | sw_lsb;

    if (sync_word == 0x0000 || sync_word == 0xFFFF) {
        ESP_LOGW(TAG, "SX1262 not responding on SPI3 (sync_word=0x%04X)", sync_word);
        return ESP_ERR_NOT_FOUND;
    }

    /* Standby RC, calibrate all blocks, enable DC-DC, DIO2 RF switch, base addr 0 */
    uint8_t stdby_rc = 0x00;
    sx1262_write_cmd(SX126X_CMD_SET_STANDBY, &stdby_rc, 1);
    uint8_t cal_all = 0x7F;
    sx1262_write_cmd(SX126X_CMD_CALIBRATE, &cal_all, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t dcdc = 0x01;
    sx1262_write_cmd(SX126X_CMD_SET_REGULATOR_MODE, &dcdc, 1);
    uint8_t rf_sw = 0x01;
    sx1262_write_cmd(SX126X_CMD_SET_DIO2_AS_RFSWITCH, &rf_sw, 1);
    uint8_t base_addr[2] = {0x00, 0x00};
    sx1262_write_cmd(SX126X_CMD_SET_BUFFER_BASE_ADDR, base_addr, sizeof(base_addr));

    sx1262_start_rx_continuous(freq_hz);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.sx1262_detected = true;
    s_state.sx1262_sync_word = sync_word;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "SX1262 initialized on SPI3 (sync_word=0x%04X, listening at %lu Hz)",
             sync_word, (unsigned long)freq_hz);
    return ESP_OK;
}

/* ── Rain Rate & Daily Rain Tracking (ported from wg1200-firmware ws_data.cpp) ─ */

#define WS_DATA_PRECIP_RATE_DECAY_TIMEOUT_SEC   (15 * 60) /* 15 minutes */
#define MAX_VALID_PRECIP_RATE_MM_H              300.0f
#define MAX_SINGLE_PACKET_PRECIP_DELTA_MM       25.4f     /* 100 bucket tips in 16s */
#define DAILY_PRECIP_NVS_MAGIC                  0x57584432U /* 'WXD2' */

typedef struct {
    uint32_t magic;
    int32_t day_idx;
    uint32_t station_id;
    float baseline_cml;
    float accum_rollover;
    float cached;
} daily_precip_nvs_blob_t;

/* The station's last valid GPS fix. The station reports one only when asked,
 * and asking takes WeatherXM's signed ack, so keep whatever arrives. */
#define STATION_GPS_NVS_MAGIC                   0x57584750U /* 'WXGP' */

typedef struct {
    uint32_t magic;
    uint32_t station_id;
    double lat;
    double lon;
} station_gps_nvs_blob_t;

/* Retain daily rain + rain rate across warm reboots / OTA / dual-boot switches */
typedef struct {
    uint32_t magic;
    int32_t daily_current_day_idx;
    uint32_t daily_station_id;
    float daily_baseline_cml;
    float daily_accum_rollover;
    float daily_precip_cached;
    uint32_t rate_station_id;
    float init_tip_cml;
    int64_t init_tip_ts;
    float oldest_tip_cml;
    int64_t oldest_tip_ts;
    float newest_tip_cml;
    int64_t newest_tip_ts;
} wg1200_rain_rtc_t;

static RTC_NOINIT_ATTR wg1200_rain_rtc_t s_rain_rtc;

/* Paired station ID & timezone offset inherited from factory WeatherXM NVS */
static uint32_t s_paired_station_id = 0;
static int64_t s_paired_station_last_seen_us = 0;
static int32_t s_tz_offset_hours = 0;
static bool s_ws1300_native_rate = false;

static void reset_precip_rate_params_locked(void)
{
    s_rain_rtc.init_tip_cml = 0.0f;
    s_rain_rtc.init_tip_ts = 0;
    s_rain_rtc.oldest_tip_cml = 0.0f;
    s_rain_rtc.oldest_tip_ts = 0;
    s_rain_rtc.newest_tip_cml = 0.0f;
    s_rain_rtc.newest_tip_ts = 0;
}

static void update_precip_rate_params_locked(float precip_cml, uint32_t pkt_sta_id)
{
    int64_t now_sec = esp_timer_get_time() / 1000000LL;
    if (now_sec <= 0) {
        now_sec = 1;
    }

    if (pkt_sta_id != 0 && s_rain_rtc.rate_station_id != 0 &&
        pkt_sta_id != s_rain_rtc.rate_station_id) {
        reset_precip_rate_params_locked();
    }
    if (pkt_sta_id != 0) {
        s_rain_rtc.rate_station_id = pkt_sta_id;
    }

    /* Reset if uninitialized or if cumulative rain wrapped/decreased */
    if (s_rain_rtc.init_tip_ts == 0 ||
        precip_cml < s_rain_rtc.init_tip_cml ||
        precip_cml < s_rain_rtc.oldest_tip_cml ||
        precip_cml < s_rain_rtc.newest_tip_cml) {
        reset_precip_rate_params_locked();
        s_rain_rtc.init_tip_cml = precip_cml;
        s_rain_rtc.init_tip_ts = now_sec;
        return;
    }

    /* Reject unphysical single-packet cumulative jump (> 25.4 mm) */
    float ref_cml = (s_rain_rtc.newest_tip_ts != 0) ? s_rain_rtc.newest_tip_cml
                  : ((s_rain_rtc.oldest_tip_ts != 0) ? s_rain_rtc.oldest_tip_cml
                                                     : s_rain_rtc.init_tip_cml);
    if ((precip_cml - ref_cml) > MAX_SINGLE_PACKET_PRECIP_DELTA_MM) {
        reset_precip_rate_params_locked();
        s_rain_rtc.init_tip_cml = precip_cml;
        s_rain_rtc.init_tip_ts = now_sec;
        return;
    }

    /* If previous tip events decayed (>15 min with no tips), promote last cml as dry baseline */
    int64_t last_tip_ts = (s_rain_rtc.newest_tip_ts != 0) ? s_rain_rtc.newest_tip_ts
                                                          : s_rain_rtc.oldest_tip_ts;
    if (last_tip_ts != 0 && (now_sec - last_tip_ts) >= WS_DATA_PRECIP_RATE_DECAY_TIMEOUT_SEC) {
        reset_precip_rate_params_locked();
        s_rain_rtc.init_tip_cml = ref_cml;
        s_rain_rtc.init_tip_ts = last_tip_ts;
    }

    if (precip_cml == s_rain_rtc.init_tip_cml) {
        return;
    }

    if (s_rain_rtc.oldest_tip_ts == 0) {
        s_rain_rtc.oldest_tip_cml = precip_cml;
        s_rain_rtc.oldest_tip_ts = now_sec;
        return;
    }
    if (precip_cml == s_rain_rtc.oldest_tip_cml) {
        return;
    }

    if (s_rain_rtc.newest_tip_ts == 0) {
        s_rain_rtc.newest_tip_cml = precip_cml;
        s_rain_rtc.newest_tip_ts = now_sec;
        return;
    }
    if (precip_cml == s_rain_rtc.newest_tip_cml) {
        return;
    }

    s_rain_rtc.oldest_tip_cml = s_rain_rtc.newest_tip_cml;
    s_rain_rtc.oldest_tip_ts = s_rain_rtc.newest_tip_ts;
    s_rain_rtc.newest_tip_cml = precip_cml;
    s_rain_rtc.newest_tip_ts = now_sec;
}

static float get_precip_rate_locked(void)
{
    int64_t now_sec = esp_timer_get_time() / 1000000LL;

    /* Case 1: Only one bucket tip event since initial/dry baseline */
    if (s_rain_rtc.oldest_tip_ts != 0 && s_rain_rtc.newest_tip_ts == 0) {
        int64_t elapsed_since_tip = now_sec - s_rain_rtc.oldest_tip_ts;
        if (elapsed_since_tip >= WS_DATA_PRECIP_RATE_DECAY_TIMEOUT_SEC) {
            return 0.0f;
        }
        float tip_delta = s_rain_rtc.oldest_tip_cml - s_rain_rtc.init_tip_cml;
        if (tip_delta <= 0.0f || tip_delta > MAX_SINGLE_PACKET_PRECIP_DELTA_MM) {
            tip_delta = 0.254f;
        }
        int64_t init_interval = (s_rain_rtc.oldest_tip_ts > s_rain_rtc.init_tip_ts)
                                    ? (s_rain_rtc.oldest_tip_ts - s_rain_rtc.init_tip_ts)
                                    : 0;
        int64_t elapsed_sec = (init_interval > 0 && init_interval < WS_DATA_PRECIP_RATE_DECAY_TIMEOUT_SEC)
                                  ? (init_interval + elapsed_since_tip)
                                  : elapsed_since_tip;
        int64_t effective_window = (elapsed_sec > 120) ? elapsed_sec : 120;
        float rate = (3600.0f * tip_delta) / (float)effective_window;
        if (!isfinite(rate) || rate < 0.0f) return 0.0f;
        if (rate > MAX_VALID_PRECIP_RATE_MM_H) return MAX_VALID_PRECIP_RATE_MM_H;
        return rate;
    }

    /* Case 2: Two or more bucket tip events */
    if (s_rain_rtc.oldest_tip_ts != 0 && s_rain_rtc.newest_tip_ts != 0) {
        if ((now_sec - s_rain_rtc.newest_tip_ts) >= WS_DATA_PRECIP_RATE_DECAY_TIMEOUT_SEC) {
            return 0.0f;
        }
        float delta_cml = s_rain_rtc.newest_tip_cml - s_rain_rtc.oldest_tip_cml;
        int64_t elapsed_sec = (s_rain_rtc.newest_tip_ts - s_rain_rtc.oldest_tip_ts) +
                              (now_sec - s_rain_rtc.newest_tip_ts);
        if (elapsed_sec <= 0) {
            return 0.0f;
        }
        float rate = (3600.0f * delta_cml) / (float)elapsed_sec;
        if (!isfinite(rate) || rate < 0.0f) return 0.0f;
        if (rate > MAX_VALID_PRECIP_RATE_MM_H) return MAX_VALID_PRECIP_RATE_MM_H;
        return rate;
    }

    return 0.0f;
}

static int32_t current_local_day_idx(void)
{
    time_t now = time(NULL);
    if (now > 86400) {
        int32_t tz_offset_sec = s_tz_offset_hours * 3600;
        return (int32_t)((now + tz_offset_sec) / 86400);
    }
    return -1;
}

static void save_daily_precip_muse_nvs(void)
{
    if (s_rain_rtc.daily_current_day_idx < 0 || isnan(s_rain_rtc.daily_baseline_cml)) {
        return;
    }
    daily_precip_nvs_blob_t blob = {
        .magic = DAILY_PRECIP_NVS_MAGIC,
        .day_idx = s_rain_rtc.daily_current_day_idx,
        .station_id = s_rain_rtc.daily_station_id,
        .baseline_cml = s_rain_rtc.daily_baseline_cml,
        .accum_rollover = s_rain_rtc.daily_accum_rollover,
        .cached = s_rain_rtc.daily_precip_cached,
    };
    nvs_handle_t h = 0;
    if (nvs_open("wxm_weather", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "wx_daily", &blob, sizeof(blob));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_station_gps_muse_nvs(void)
{
    station_gps_nvs_blob_t blob = {0};
    size_t sz = sizeof(blob);
    nvs_handle_t h = 0;
    if (nvs_open("wxm_weather", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, "wx_gps", &blob, &sz) == ESP_OK && sz == sizeof(blob) &&
        blob.magic == STATION_GPS_NVS_MAGIC) {
        s_state.gps_valid = true;
        s_state.gps_lat = blob.lat;
        s_state.gps_lon = blob.lon;
        ESP_LOGI(TAG, "Station GPS from muse_nvs: %.4f, %.4f", blob.lat, blob.lon);
    }
    nvs_close(h);
}

/* Decodes the RMC sentence in a WEATHER_GPS_SIGNED packet: pl[25] is its
 * length and it starts at pl[26], before the CRC and checksum. */
static void handle_ws1001_gps(const uint8_t *pl, uint8_t bare_len, uint32_t station_id)
{
    uint8_t gnss_len = pl[25];
    if (26U + gnss_len + 2U > bare_len) {
        return;
    }
    double lat = 0.0, lon = 0.0;
    if (!wg1200_gps_parse_rmc((const char *)&pl[26], gnss_len, &lat, &lon)) {
        ESP_LOGI(TAG, "WS1001[0x%08lX] GPS packet without a valid fix", (unsigned long)station_id);
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    /* Only write flash when the fix moves (~10 m): the station doesn't. */
    bool moved = !s_state.gps_valid || fabs(lat - s_state.gps_lat) > 1e-4 ||
                 fabs(lon - s_state.gps_lon) > 1e-4;
    s_state.gps_valid = true;
    s_state.gps_lat = lat;
    s_state.gps_lon = lon;
    xSemaphoreGive(s_state_mutex);
    ESP_LOGI(TAG, "WS1001[0x%08lX] GPS fix: %.4f, %.4f", (unsigned long)station_id, lat, lon);

    if (moved) {
        station_gps_nvs_blob_t blob = {
            .magic = STATION_GPS_NVS_MAGIC,
            .station_id = station_id,
            .lat = lat,
            .lon = lon,
        };
        nvs_handle_t h = 0;
        if (nvs_open("wxm_weather", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "wx_gps", &blob, sizeof(blob));
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

static void update_daily_precip_locked(float cml_val, uint32_t pkt_sta_id)
{
    if (isnan(cml_val) || cml_val < 0.0f) {
        return;
    }

    float prev_cached = s_rain_rtc.daily_precip_cached;
    int32_t prev_day_idx = s_rain_rtc.daily_current_day_idx;
    bool was_uninit = isnan(s_rain_rtc.daily_baseline_cml);

    /* Guard against station ID change */
    if (pkt_sta_id != 0 && s_rain_rtc.daily_station_id != 0 &&
        pkt_sta_id != s_rain_rtc.daily_station_id) {
        ESP_LOGW(TAG, "Station ID changed (0x%08lX -> 0x%08lX) — re-anchoring daily rain baseline to %.2f mm",
                 (unsigned long)s_rain_rtc.daily_station_id, (unsigned long)pkt_sta_id, cml_val);
        s_rain_rtc.daily_accum_rollover = s_rain_rtc.daily_precip_cached;
        s_rain_rtc.daily_baseline_cml = cml_val;
        s_rain_rtc.daily_station_id = pkt_sta_id;
    } else if (pkt_sta_id != 0 && s_rain_rtc.daily_station_id == 0) {
        s_rain_rtc.daily_station_id = pkt_sta_id;
    }

    int32_t day_idx = current_local_day_idx();
    if (day_idx >= 0) {
        if (s_rain_rtc.daily_current_day_idx == -1) {
            s_rain_rtc.daily_current_day_idx = day_idx;
            if (isnan(s_rain_rtc.daily_baseline_cml)) {
                s_rain_rtc.daily_baseline_cml = cml_val;
                s_rain_rtc.daily_accum_rollover = 0.0f;
                s_rain_rtc.daily_precip_cached = 0.0f;
                save_daily_precip_muse_nvs();
                return;
            }
        } else if (s_rain_rtc.daily_current_day_idx != day_idx) {
            /* Local midnight rollover */
            ESP_LOGI(TAG, "Daily rain reset for new local day %ld (baseline=%.2f mm)",
                     (long)day_idx, cml_val);
            s_rain_rtc.daily_current_day_idx = day_idx;
            s_rain_rtc.daily_baseline_cml = cml_val;
            s_rain_rtc.daily_accum_rollover = 0.0f;
            s_rain_rtc.daily_precip_cached = 0.0f;
            save_daily_precip_muse_nvs();
            return;
        }
    }

    if (isnan(s_rain_rtc.daily_baseline_cml)) {
        s_rain_rtc.daily_baseline_cml = cml_val;
        s_rain_rtc.daily_accum_rollover = 0.0f;
        s_rain_rtc.daily_precip_cached = 0.0f;
        return;
    }

    if (cml_val >= s_rain_rtc.daily_baseline_cml) {
        float new_delta = (cml_val - s_rain_rtc.daily_baseline_cml) -
                          (s_rain_rtc.daily_precip_cached - s_rain_rtc.daily_accum_rollover);
        if (new_delta > MAX_SINGLE_PACKET_PRECIP_DELTA_MM) {
            s_rain_rtc.daily_accum_rollover = s_rain_rtc.daily_precip_cached;
            s_rain_rtc.daily_baseline_cml = cml_val;
        } else {
            s_rain_rtc.daily_precip_cached =
                (cml_val - s_rain_rtc.daily_baseline_cml) + s_rain_rtc.daily_accum_rollover;
        }
    } else {
        /* Station rebooted or 16-bit counter wrapped */
        s_rain_rtc.daily_accum_rollover = s_rain_rtc.daily_precip_cached + cml_val;
        s_rain_rtc.daily_baseline_cml = cml_val;
        s_rain_rtc.daily_precip_cached = s_rain_rtc.daily_accum_rollover;
    }

    if (was_uninit || prev_day_idx != s_rain_rtc.daily_current_day_idx ||
        fabsf(s_rain_rtc.daily_precip_cached - prev_cached) >= 0.1f) {
        save_daily_precip_muse_nvs();
    }
}

static bool should_accept_station(uint32_t station_id, int16_t rssi)
{
    int64_t now_us = esp_timer_get_time();
    if (s_paired_station_id != 0) {
        if (station_id == s_paired_station_id) {
            s_paired_station_last_seen_us = now_us;
            return true;
        }
        /* If paired station is actively transmitting (seen within 180s), ignore neighbor stations */
        if (s_paired_station_last_seen_us != 0 &&
            (now_us - s_paired_station_last_seen_us) < 180LL * 1000000LL) {
            return false;
        }
    }
    if (s_state.outdoor_valid && s_state.station_id != 0 && s_state.station_id != station_id) {
        /* Keep locked onto the current station while active (<120s), unless the new station
         * is significantly closer/stronger (> 6 dB stronger RSSI). */
        if ((now_us - s_state.outdoor_updated_us) < 120LL * 1000000LL &&
            rssi <= (s_state.rssi_dbm + 6)) {
            return false;
        }
    }
    return true;
}

/* ── WS1001 & WS1300 Packet Decoders ─────────────────────────────────────── */

static uint8_t ws1001_crc8(const uint8_t *buf, int len)
{
    uint8_t rem = 0;
    for (int i = 0; i < len; i++) {
        rem ^= buf[i];
        for (int b = 0; b < 8; b++) {
            rem = (rem & 0x80) ? (uint8_t)((rem << 1) ^ 0x31) : (uint8_t)(rem << 1);
        }
    }
    return rem;
}

static uint8_t ws1001_checksum(const uint8_t *buf, int len)
{
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    return sum;
}

static bool ws1001_try_decode(const uint8_t *pl, uint8_t len, int16_t rssi, int8_t snr, uint32_t freq_hz)
{
    /* WS1001 (FineOffset WL69) signed packet:
     * pl[0] = bare packet size (27 for weather-only + 16-byte signature = 43 bytes)
     * pl[2] = 0x69 station code
     * pl[12] = 0x02 (WEATHER_SIGNED) or 0x04 (WEATHER_GPS_SIGNED)
     */
    if (len < 27 || pl[2] != 0x69) {
        return false;
    }
    uint8_t bare_len = pl[0];
    if (bare_len < 27 || bare_len > len) {
        return false;
    }
    uint8_t pkt_type = pl[12];
    if (pkt_type != 0x02 && pkt_type != 0x04) {
        return false;
    }

    uint8_t expected_crc = (pkt_type == 0x04) ? pl[bare_len - 2] : pl[25];
    uint8_t expected_sum = (pkt_type == 0x04) ? pl[bare_len - 1] : pl[26];
    uint8_t calc_crc = ws1001_crc8(pl, bare_len - 2);
    uint8_t calc_sum = ws1001_checksum(pl, bare_len - 1);
    if (calc_crc != expected_crc || calc_sum != expected_sum) {
        ESP_LOGW(TAG, "WS1001 CRC mismatch (calc=%02X/%02X exp=%02X/%02X)",
                 calc_crc, calc_sum, expected_crc, expected_sum);
        return false;
    }

    uint32_t station_id = ((uint32_t)pl[3] << 24) | ((uint32_t)pl[4] << 16) |
                          ((uint32_t)pl[5] << 8) | pl[6];
    if (!should_accept_station(station_id, rssi)) {
        ESP_LOGD(TAG, "Ignoring neighbor WS1001[0x%08lX] (RSSI=%ddBm)",
                 (unsigned long)station_id, rssi);
        return true;
    }

    if (pkt_type == 0x04) {
        handle_ws1001_gps(pl, bare_len, station_id);
    }

    uint32_t pkt_count = ((uint32_t)pl[7] << 8) | pl[8];
    uint8_t tx_power = pl[9] & 0x7F;
    uint16_t batt_mv = (uint16_t)(pl[11] * 20U);

    uint16_t temp_raw = (uint16_t)(((uint16_t)pl[14] << 8) | pl[15]) >> 4;
    float temp_c = NAN;
    if (temp_raw != 0x07FF) {
        float temp_f = (temp_raw & 0x0800)
                           ? ((float)((int16_t)(temp_raw | 0xF000)) / 10.0f)
                           : ((float)temp_raw / 10.0f);
        temp_c = (temp_f - 32.0f) * (5.0f / 9.0f);
    }

    uint16_t hum_raw = (((uint16_t)pl[15] << 8) | pl[16]) & 0x03FF;
    float hum_pct = (hum_raw != 0x03FF) ? (hum_raw / 10.0f) : NAN;

    uint16_t ws_raw = (uint16_t)(((pl[15] & 0x08) << 5) | pl[17]);
    float wind_speed = (ws_raw != 0x01FF) ? (ws_raw / 10.0f) : 0.0f;

    uint16_t wg_raw = (uint16_t)(((pl[15] & 0x04) << 6) | pl[18]);
    float wind_gust = (wg_raw != 0x01FF) ? (wg_raw / 10.0f) : 0.0f;

    uint16_t wind_dir = (uint16_t)(pl[19] | ((pl[20] & 0x80) << 1));
    if (wind_dir == 0x01FF) wind_dir = 0;

    uint16_t illum_raw = (uint16_t)(((pl[20] & 0x7F) << 8) | pl[21]);
    uint32_t illum_lux = (illum_raw != 0x7FFF) ? ((uint32_t)illum_raw * 10U) : 0;
    float solar_rad = (float)illum_lux * 0.007892f;

    float uv_index = (float)pl[22] * 0.1f;
    uint16_t rain_raw = ((uint16_t)pl[23] << 8) | pl[24];
    float rain_cml_mm = (rain_raw != 0xFFFF) ? ((float)rain_raw * 0.254f) : NAN;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_ws1300_native_rate = false;
    if (!isnan(rain_cml_mm)) {
        update_precip_rate_params_locked(rain_cml_mm, station_id);
        update_daily_precip_locked(rain_cml_mm, station_id);
        s_state.rain_accum_mm = rain_cml_mm;
    }
    float rain_rate = get_precip_rate_locked();
    float rain_daily = s_rain_rtc.daily_precip_cached;
    s_state.rain_rate_mm_h = rain_rate;
    s_state.rain_daily_mm = rain_daily;
    s_state.outdoor_valid = true;
    snprintf(s_state.station_model, sizeof(s_state.station_model), "WS1001");
    s_state.station_id = station_id;
    s_state.packet_count = pkt_count;
    s_state.fw_version = pl[1];
    if (!isnan(temp_c)) s_state.outdoor_temp_c = temp_c;
    if (!isnan(hum_pct)) s_state.outdoor_humidity_pct = hum_pct;
    s_state.wind_speed_ms = wind_speed;
    s_state.wind_gust_ms = wind_gust;
    s_state.wind_dir_deg = wind_dir;
    s_state.uv_index = uv_index;
    s_state.illuminance_lux = illum_lux;
    s_state.solar_rad_wm2 = solar_rad;
    s_state.battery_mv = batt_mv;
    s_state.battery_low = (batt_mv < 2600);
    s_state.rssi_dbm = rssi;
    s_state.snr_db = snr;
    s_state.outdoor_updated_us = esp_timer_get_time();
    s_state.rx_packets_valid++;
    xSemaphoreGive(s_state_mutex);

    /* Send standard 9-byte WS1001 ACK so the outdoor station keeps its 20s cadence */
    uint8_t ack[8] = {
        8,                     /* sizeof(AckPacket) */
        (uint8_t)(20U / 10U),  /* interval = 20s, reboot=0, req_gps=0 */
        (uint8_t)(tx_power & 0x7F),
        0x69,                  /* sensor_type */
        expected_crc,
        expected_sum,
        0,
        0,
    };
    ack[6] = ws1001_crc8(ack, 6);
    ack[7] = ws1001_checksum(ack, 7);
    sx1262_tx_packet(ack, sizeof(ack), freq_hz);

    ESP_LOGI(TAG,
             "WS1001[0x%08lX] #%lu: T=%.1fC RH=%.1f%% Wind=%.1fm/s (Gust=%.1fm/s) @%u° "
             "RainRate=%.1fmm/h Daily=%.1fmm (Cml=%.1fmm) UV=%.1f RSSI=%ddBm",
             (unsigned long)station_id, (unsigned long)pkt_count,
             temp_c, hum_pct, wind_speed, wind_gust, wind_dir,
             rain_rate, rain_daily, rain_cml_mm, uv_index, rssi);
    return true;
}

static bool ws1300_try_decode(const uint8_t *pl, uint8_t len, int16_t rssi, int8_t snr)
{
    /* WS1300 StaSensors packet: 15-byte StaHeader + 90-byte StaSensors = 105 bytes
     * pl[0] = 0x01 (MODEL_ID), pl[3] = 0x03 (StaPacketType::SENSORS)
     */
    if (len != 105 || pl[0] != 0x01 || pl[3] != 0x03) {
        return false;
    }

    uint32_t station_id = ((uint32_t)pl[4] << 24) | ((uint32_t)pl[5] << 16) |
                          ((uint32_t)pl[6] << 8) | pl[7];
    if (!should_accept_station(station_id, rssi)) {
        return true;
    }

    uint32_t pkt_id = ((uint32_t)pl[8] << 8) | pl[9];
    uint16_t batt_mv = (pl[13] != 0xFF) ? (uint16_t)(pl[13] * 20U) : 0;
    bool batt_low = (pl[14] & 0x04) != 0;

    /* StaSensors payload starts at offset 15:
     * 15..18: ts (u32 BE)
     * 19: sample_cnt (u8)
     * 20..21: temp (i16 BE, 0.1 C)
     * 22: hum (u8, %)
     * 23..24: wind_speed (u16 BE, 0.1 m/s)
     * 25..26: wind_gust (u16 BE, 0.1 m/s)
     * 27..28: wind_dir (u16 BE, deg)
     * 29..32: precip_cml (float BE, mm)
     * 33..34: precip_rate (u16 BE, 0.1 mm/h)
     * 35: uv_index (u8)
     * 36..38: illum (u24 BE, lux)
     * 39..40: press (u16 BE, 0.1 hPa)
     */
    int16_t temp_raw = (int16_t)(((uint16_t)pl[20] << 8) | pl[21]);
    float temp_c = (temp_raw != 0x7FFF) ? (temp_raw / 10.0f) : 0.0f;
    float hum_pct = (pl[22] != 0xFF) ? (float)pl[22] : 0.0f;
    uint16_t ws_raw = ((uint16_t)pl[23] << 8) | pl[24];
    uint16_t wg_raw = ((uint16_t)pl[25] << 8) | pl[26];
    uint16_t wd_raw = ((uint16_t)pl[27] << 8) | pl[28];

    union {
        uint8_t b[4];
        float f;
    } precip_u = {.b = {pl[32], pl[31], pl[30], pl[29]}};
    float rain_cml_mm = (isfinite(precip_u.f) && precip_u.f >= 0.0f) ? precip_u.f : NAN;
    uint16_t pr_raw = ((uint16_t)pl[33] << 8) | pl[34];

    uint32_t illum_lux = ((uint32_t)pl[36] << 16) | ((uint32_t)pl[37] << 8) | pl[38];
    if (illum_lux == 0xFFFFFF) illum_lux = 0;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!isnan(rain_cml_mm)) {
        update_precip_rate_params_locked(rain_cml_mm, station_id);
        update_daily_precip_locked(rain_cml_mm, station_id);
        s_state.rain_accum_mm = rain_cml_mm;
    }
    if (pr_raw != 0xFFFF) {
        s_ws1300_native_rate = true;
        s_state.rain_rate_mm_h = (float)pr_raw / 10.0f;
    } else {
        s_ws1300_native_rate = false;
        s_state.rain_rate_mm_h = get_precip_rate_locked();
    }
    s_state.rain_daily_mm = s_rain_rtc.daily_precip_cached;
    s_state.outdoor_valid = true;
    snprintf(s_state.station_model, sizeof(s_state.station_model), "WS1300");
    s_state.station_id = station_id;
    s_state.packet_count = pkt_id;
    s_state.fw_version = pl[1];
    s_state.outdoor_temp_c = temp_c;
    s_state.outdoor_humidity_pct = hum_pct;
    s_state.wind_speed_ms = (ws_raw != 0xFFFF) ? (ws_raw / 10.0f) : 0.0f;
    s_state.wind_gust_ms = (wg_raw != 0xFFFF) ? (wg_raw / 10.0f) : 0.0f;
    s_state.wind_dir_deg = (wd_raw != 0xFFFF) ? wd_raw : 0;
    s_state.uv_index = (pl[35] != 0xFF) ? (float)pl[35] : 0.0f;
    s_state.illuminance_lux = illum_lux;
    s_state.solar_rad_wm2 = (float)illum_lux * 0.007892f;
    s_state.battery_mv = batt_mv;
    s_state.battery_low = batt_low;
    s_state.rssi_dbm = rssi;
    s_state.snr_db = snr;
    s_state.outdoor_updated_us = esp_timer_get_time();
    s_state.rx_packets_valid++;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG,
             "WS1300[0x%08lX] #%lu: T=%.1fC RH=%.0f%% Wind=%.1fm/s (Gust=%.1fm/s) "
             "RainRate=%.1fmm/h Daily=%.1fmm RSSI=%ddBm",
             (unsigned long)station_id, (unsigned long)pkt_id,
             temp_c, hum_pct, s_state.wind_speed_ms, s_state.wind_gust_ms,
             s_state.rain_rate_mm_h, s_state.rain_daily_mm, rssi);
    return true;
}

/* ── Weather Background Task ─────────────────────────────────────────────── */

static void wg1200_weather_task(void *arg)
{
    (void)arg;
    uint32_t tick_count = 0;

    while (1) {
        /* 1. Poll indoor BMP390 every ~5 seconds (initial poll immediately) */
        if (s_state.bmp390_detected && (tick_count % 50 == 0)) {
            float temp_c = 0.0f;
            float press_hpa = 0.0f;
            if (bmp390_poll(&temp_c, &press_hpa) == ESP_OK && press_hpa > 300.0f && press_hpa < 1250.0f) {
                bool first = false;
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                first = !s_state.bmp390_valid;
                s_state.bmp390_valid = true;
                s_state.indoor_temp_c = temp_c;
                s_state.indoor_pressure_hpa = press_hpa;
                s_state.indoor_updated_us = esp_timer_get_time();
                xSemaphoreGive(s_state_mutex);
                if (first) {
                    ESP_LOGI(TAG, "BMP390 first reading: T=%.2f C, P=%.2f hPa", temp_c, press_hpa);
                }
            }
        }

        /* 2. Apply region change if requested from UI */
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        bool sx_ok = s_state.sx1262_detected;
        bool retune = sx_ok && s_region_dirty;
        uint32_t new_freq = s_state.freq_hz;
        wg1200_weather_region_t new_region = s_state.region;
        if (retune) {
            s_region_dirty = false;
        }
        xSemaphoreGive(s_state_mutex);
        if (retune) {
            ESP_LOGI(TAG, "Re-tuning SX1262 to %s (%lu Hz)",
                     wg1200_weather_region_name(new_region), (unsigned long)new_freq);
            sx1262_start_rx_continuous(new_freq);
        }

        /* 3. Poll SX1262 IRQ status when DIO1 / GPIO42 is asserted or every 200 ms */
        if (sx_ok &&
            (gpio_get_level(WG1200_LORA_PIN_EXP_INT) == 0 || (tick_count % 2 == 0))) {
            uint8_t irq_buf[2] = {0};
            sx1262_read_cmd(SX126X_CMD_GET_IRQ_STATUS, irq_buf, sizeof(irq_buf));
            uint16_t irq = ((uint16_t)irq_buf[0] << 8) | irq_buf[1];

            if (irq != 0) {
                uint8_t clr_buf[2] = {irq_buf[0], irq_buf[1]};
                sx1262_write_cmd(SX126X_CMD_CLR_IRQ_STATUS, clr_buf, sizeof(clr_buf));

                if ((irq & SX126X_IRQ_RX_DONE) && !(irq & SX126X_IRQ_CRC_ERROR)) {
                    uint8_t rx_stat[2] = {0};
                    sx1262_read_cmd(SX126X_CMD_GET_RX_BUFFER_STATUS, rx_stat, sizeof(rx_stat));
                    uint8_t pl_len = rx_stat[0];
                    uint8_t offset = rx_stat[1];

                    uint8_t pkt_stat[3] = {0};
                    sx1262_read_cmd(SX126X_CMD_GET_PACKET_STATUS, pkt_stat, sizeof(pkt_stat));
                    int16_t rssi = -((int16_t)pkt_stat[0]) / 2;
                    int8_t snr = (int8_t)(((int8_t)pkt_stat[1] + 2) >> 2);

                    if (pl_len > 0) {
                        uint8_t rx_buf[255] = {0};
                        sx1262_read_buffer(offset, rx_buf, pl_len);

                        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                        s_state.rx_packets_total++;
                        s_state.rssi_dbm = rssi;
                        s_state.snr_db = snr;
                        uint32_t cur_freq = s_state.freq_hz;
                        xSemaphoreGive(s_state_mutex);

                        if (!ws1001_try_decode(rx_buf, pl_len, rssi, snr, cur_freq)) {
                            (void)ws1300_try_decode(rx_buf, pl_len, rssi, snr);
                        }
                    }
                }
            }
        }

        tick_count++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ── Region & Rain Baseline Persistence & Init ───────────────────────────── */

static void init_rain_tracking_state(bool stock_nvs_mounted)
{
    if (s_rain_rtc.magic == DAILY_PRECIP_NVS_MAGIC) {
        ESP_LOGI(TAG, "Restored rain tracking from RTC RAM (day=%ld, sta=0x%08lX, baseline=%.2f, daily=%.2f mm)",
                 (long)s_rain_rtc.daily_current_day_idx,
                 (unsigned long)s_rain_rtc.daily_station_id,
                 s_rain_rtc.daily_baseline_cml,
                 s_rain_rtc.daily_precip_cached);
        return;
    }

    memset(&s_rain_rtc, 0, sizeof(s_rain_rtc));
    s_rain_rtc.magic = DAILY_PRECIP_NVS_MAGIC;
    s_rain_rtc.daily_current_day_idx = -1;
    s_rain_rtc.daily_baseline_cml = NAN;

    /* Try restoring daily rain baseline from muse_nvs first, then factory WeatherXM NVS */
    daily_precip_nvs_blob_t blob = {0};
    size_t sz = sizeof(blob);
    bool loaded = false;

    nvs_handle_t h = 0;
    if (nvs_open("wxm_weather", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "wx_daily", &blob, &sz) == ESP_OK &&
            sz == sizeof(blob) && blob.magic == DAILY_PRECIP_NVS_MAGIC &&
            !isnan(blob.baseline_cml) && blob.baseline_cml >= 0.0f) {
            loaded = true;
        }
        nvs_close(h);
    }

    if (!loaded && stock_nvs_mounted) {
        nvs_handle_t fh = 0;
        sz = sizeof(blob);
        if (__real_nvs_open_from_partition("nvs", "settings", NVS_READONLY, &fh) == ESP_OK) {
            if (nvs_get_blob(fh, "wx_daily", &blob, &sz) == ESP_OK &&
                sz == sizeof(blob) && blob.magic == DAILY_PRECIP_NVS_MAGIC &&
                !isnan(blob.baseline_cml) && blob.baseline_cml >= 0.0f) {
                loaded = true;
            }
            nvs_close(fh);
        }
    }

    if (loaded) {
        s_rain_rtc.daily_current_day_idx = blob.day_idx;
        s_rain_rtc.daily_station_id = blob.station_id;
        s_rain_rtc.daily_baseline_cml = blob.baseline_cml;
        s_rain_rtc.daily_accum_rollover = blob.accum_rollover;
        s_rain_rtc.daily_precip_cached = blob.cached;
        ESP_LOGI(TAG, "Restored daily rain from NVS (day=%ld, sta=0x%08lX, baseline=%.2f, daily=%.2f mm)",
                 (long)blob.day_idx, (unsigned long)blob.station_id,
                 blob.baseline_cml, blob.cached);
    }
}

static void load_initial_region(bool stock_nvs_mounted)
{
    wg1200_weather_region_t region = WG1200_REGION_EU868;
    bool loaded_from_muse = false;

    /* 1. Check muse_nvs first (via wrapped nvs_open) */
    nvs_handle_t h = 0;
    if (nvs_open("wxm_weather", NVS_READONLY, &h) == ESP_OK) {
        uint8_t reg_u8 = 0;
        if (nvs_get_u8(h, "region", &reg_u8) == ESP_OK && reg_u8 < WG1200_REGION_COUNT) {
            region = (wg1200_weather_region_t)reg_u8;
            loaded_from_muse = true;
        }
        nvs_close(h);
    }

    /* 2. Inspect factory WeatherXM NVS ("nvs", namespace "settings") read-only for region, paired ws_id, tz_offset */
    nvs_handle_t fh = 0;
    if (stock_nvs_mounted &&
        __real_nvs_open_from_partition("nvs", "settings", NVS_READONLY, &fh) == ESP_OK) {
        if (!loaded_from_muse) {
            char freq_str[16] = {0};
            size_t len = sizeof(freq_str);
            if (nvs_get_str(fh, "freq_name", freq_str, &len) == ESP_OK) {
                if (strstr(freq_str, "915") != NULL) {
                    region = WG1200_REGION_US915;
                } else if (strstr(freq_str, "923") != NULL) {
                    region = WG1200_REGION_AS923;
                } else {
                    region = WG1200_REGION_EU868;
                }
                ESP_LOGI(TAG, "Inherited WeatherXM factory NVS freq_name='%s' -> %s",
                         freq_str, wg1200_weather_region_name(region));
            }
        }

        int64_t ws_id = -1;
        if (nvs_get_i64(fh, "ws_id", &ws_id) != ESP_OK || ws_id <= 0) {
            (void)nvs_get_i64(fh, "c_ws_id", &ws_id);
        }
        if (ws_id > 0 && ws_id <= 0xFFFFFFFFLL) {
            s_paired_station_id = (uint32_t)ws_id;
            ESP_LOGI(TAG, "Inherited WeatherXM paired station ID: 0x%08lX",
                     (unsigned long)s_paired_station_id);
        }

        uint32_t tz_u32 = 0;
        if (nvs_get_u32(fh, "tz_offset", &tz_u32) == ESP_OK) {
            int32_t tz_i32 = (int32_t)tz_u32;
            if (tz_i32 >= -12 && tz_i32 <= 14) {
                s_tz_offset_hours = tz_i32;
            }
        }
        nvs_close(fh);
    }

    s_state.region = region;
    s_state.freq_hz = k_region_freqs[region];
}

static void sync_factory_nvs_readonly(void)
{
    /* Temporarily mount stock WeatherXM "nvs" without erasing on error, read
     * region/station/rain settings read-only, then immediately unmount it. */
    bool stock_nvs_mounted = (__real_nvs_flash_init_partition("nvs") == ESP_OK);
    load_initial_region(stock_nvs_mounted);
    init_rain_tracking_state(stock_nvs_mounted);
    if (stock_nvs_mounted) {
        (void)nvs_flash_deinit_partition("nvs");
    }
}

esp_err_t wg1200_weather_init(void)
{
    if (!s_state_mutex) {
        s_state_mutex = xSemaphoreCreateMutex();
    }

    sync_factory_nvs_readonly();
    load_station_gps_muse_nvs();

    esp_err_t bmp_err = bmp390_init_hw();
    if (bmp_err == ESP_OK) {
        /* Perform an immediate warm-up measurement after 25 ms */
        vTaskDelay(pdMS_TO_TICKS(25));
        float t = 0.0f, p = 0.0f;
        if (bmp390_poll(&t, &p) == ESP_OK && p > 300.0f && p < 1250.0f) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_state.bmp390_valid = true;
            s_state.indoor_temp_c = t;
            s_state.indoor_pressure_hpa = p;
            s_state.indoor_updated_us = esp_timer_get_time();
            xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "BMP390 initial reading: %.2f C, %.2f hPa", t, p);
        }
    }

    esp_err_t lora_err = sx1262_init_hw(s_state.freq_hz);
    if (bmp_err != ESP_OK && lora_err != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    xTaskCreatePinnedToCore(wg1200_weather_task, "wg1200_wx", 4096, NULL, 4, NULL, 1);
    return ESP_OK;
}

void wg1200_weather_get_state(wg1200_weather_state_t *out)
{
    if (!out) return;
    if (!s_state_mutex) {
        *out = s_state;
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_state.outdoor_valid && !s_ws1300_native_rate) {
        s_state.rain_rate_mm_h = get_precip_rate_locked();
    }
    *out = s_state;
    xSemaphoreGive(s_state_mutex);
}

esp_err_t wg1200_weather_set_region(wg1200_weather_region_t region)
{
    if (region < 0 || region >= WG1200_REGION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state_mutex) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    }
    s_state.region = region;
    s_state.freq_hz = k_region_freqs[region];
    s_region_dirty = true;
    if (s_state_mutex) {
        xSemaphoreGive(s_state_mutex);
    }

    nvs_handle_t h = 0;
    if (nvs_open("wxm_weather", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "region", (uint8_t)region);
        nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
}

/* ── Gadget Commands: sensors.read & device.boot_weatherxm ───────────────── */

static void add_sensor_metric(cJSON *payload, const char *name, double val, const char *unit, double age_s)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "value", round(val * 100.0) / 100.0);
    cJSON_AddStringToObject(m, "unit", unit);
    cJSON_AddNumberToObject(m, "age_s", round(age_s * 10.0) / 10.0);
    cJSON_AddItemToObject(payload, name, m);
}

cJSON *wg1200_weather_sensors_command(void)
{
    wg1200_weather_state_t st;
    wg1200_weather_get_state(&st);
    int64_t now_us = esp_timer_get_time();

    cJSON *resp = cJSON_CreateObject();
    if (!st.bmp390_valid && !st.outdoor_valid) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON *error = cJSON_AddObjectToObject(resp, "error");
        cJSON_AddStringToObject(error, "code", "no_readings");
        cJSON_AddStringToObject(error, "message", "no sensor readings available yet");
        return resp;
    }
    cJSON_AddBoolToObject(resp, "ok", true);

    cJSON *payload = cJSON_CreateObject();
    if (st.bmp390_valid) {
        double age_s = (double)(now_us - st.indoor_updated_us) / 1e6;
        /* Note: BMP390 sits inside the WG1200 enclosure behind the 480x480 LCD,
         * so its temperature reflects gateway PCB/enclosure heat rather than ambient room air. */
        add_sensor_metric(payload, "gateway_temperature", st.indoor_temp_c, "C", age_s);
        add_sensor_metric(payload, "barometric_pressure", st.indoor_pressure_hpa, "hPa", age_s);
        add_sensor_metric(payload, "pressure", st.indoor_pressure_hpa, "hPa", age_s);
    }

    if (st.outdoor_valid) {
        double age_s = (double)(now_us - st.outdoor_updated_us) / 1e6;
        add_sensor_metric(payload, "outdoor_temperature", st.outdoor_temp_c, "C", age_s);
        add_sensor_metric(payload, "temperature", st.outdoor_temp_c, "C", age_s);
        add_sensor_metric(payload, "outdoor_humidity", st.outdoor_humidity_pct, "%", age_s);
        add_sensor_metric(payload, "humidity", st.outdoor_humidity_pct, "%", age_s);
        add_sensor_metric(payload, "wind_speed", st.wind_speed_ms, "m/s", age_s);
        add_sensor_metric(payload, "wind_gust", st.wind_gust_ms, "m/s", age_s);
        add_sensor_metric(payload, "wind_direction", st.wind_dir_deg, "deg", age_s);
        add_sensor_metric(payload, "rain_rate", st.rain_rate_mm_h, "mm/h", age_s);
        add_sensor_metric(payload, "rain_daily", st.rain_daily_mm, "mm", age_s);
        add_sensor_metric(payload, "rain_accumulated", st.rain_accum_mm, "mm", age_s);
        add_sensor_metric(payload, "uv_index", st.uv_index, "UVI", age_s);
        add_sensor_metric(payload, "illuminance", st.illuminance_lux, "lx", age_s);
        add_sensor_metric(payload, "solar_irradiance", st.solar_rad_wm2, "W/m2", age_s);
        add_sensor_metric(payload, "station_battery", st.battery_mv, "mV", age_s);
        add_sensor_metric(payload, "lora_rssi", st.rssi_dbm, "dBm", age_s);
        add_sensor_metric(payload, "lora_snr", st.snr_db, "dB", age_s);
    }

    cJSON *radio = cJSON_CreateObject();
    cJSON_AddBoolToObject(radio, "sx1262_detected", st.sx1262_detected);
    cJSON_AddStringToObject(radio, "region", wg1200_weather_region_name(st.region));
    cJSON_AddNumberToObject(radio, "freq_hz", st.freq_hz);
    cJSON_AddNumberToObject(radio, "rx_packets_total", st.rx_packets_total);
    cJSON_AddNumberToObject(radio, "rx_packets_valid", st.rx_packets_valid);
    if (st.outdoor_valid) {
        cJSON_AddStringToObject(radio, "station_model", st.station_model);
        char id_hex[16];
        snprintf(id_hex, sizeof(id_hex), "0x%08lX", (unsigned long)st.station_id);
        cJSON_AddStringToObject(radio, "station_id", id_hex);
    }
    cJSON_AddItemToObject(payload, "lora_radio", radio);

    cJSON_AddItemToObject(resp, "payload", payload);
    return resp;
}

static cJSON *boot_cmd_error(const char *code, const char *message)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return resp;
}

cJSON *wg1200_boot_weatherxm_command(const cJSON *params)
{
    int requested_slot = 0; /* 0 = auto, 1 = factory, 2 = ota_0 */
    const cJSON *slot_item = params ? cJSON_GetObjectItemCaseSensitive(params, "slot") : NULL;
    if (slot_item && !cJSON_IsNull(slot_item)) {
        if (!cJSON_IsString(slot_item) || !slot_item->valuestring) {
            return boot_cmd_error("invalid_params", "slot must be 'ota_0' or 'factory'");
        }
        if (strcmp(slot_item->valuestring, "factory") == 0) {
            requested_slot = 1;
        } else if (strcmp(slot_item->valuestring, "ota_0") == 0) {
            requested_slot = 2;
        } else {
            return boot_cmd_error("invalid_params", "slot must be 'ota_0' or 'factory'");
        }
    }

    wxm_boot_info_t info;
    wxm_boot_get_info(&info);

    const esp_partition_t *target_part = NULL;
    const char *target_proj = "";
    const char *target_ver = "";
    if (requested_slot == 1) {
        target_part = info.factory_part;
        target_proj = info.factory_project;
        target_ver = info.factory_version;
    } else if (requested_slot == 2) {
        target_part = info.ota0_part;
        target_proj = info.ota0_project;
        target_ver = info.ota0_version;
    } else {
        target_part = info.wxm_part;
        target_proj = info.wxm_project;
        target_ver = info.wxm_version;
    }

    if (!target_part) {
        return boot_cmd_error("unavailable", "target WeatherXM partition has no bootable app image");
    }
    if (wxm_boot_to_partition(target_part) != ESP_OK) {
        return boot_cmd_error("unavailable", "failed to schedule WeatherXM reboot");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "boot_target", target_part->label);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "status", "rebooting_to_weatherxm");
    cJSON_AddStringToObject(payload, "boot_target", target_part->label);
    cJSON_AddStringToObject(payload, "wxm_label", target_part->label);
    cJSON_AddStringToObject(payload, "wxm_project", target_proj);
    cJSON_AddStringToObject(payload, "wxm_version", target_ver);
    cJSON_AddStringToObject(payload, "alt_label", info.alt_label);
    cJSON_AddStringToObject(payload, "alt_project", info.alt_project);
    cJSON_AddStringToObject(payload, "alt_version", info.alt_version);
    cJSON_AddItemToObject(resp, "payload", payload);
    return resp;
}
