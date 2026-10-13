/*
 * AXP2101 — SmartRing-Plus V2 电源管理芯片驱动
 *
 * 电芯与 V1 相同：3.7V / 600mAh；充电按 0.5C(300mA)、CV 4.2V。
 * 寄存器取值依据 X-Powers AXP2101 Datasheet。
 * 电量：芯片内置 E-Gauge（REGA4）；未写入 REGA1 厂参时依赖自学习。
 * 软关机不在此实现（板级仍用 GPIO47）。
 *
 * Author : Ayang
 * Company: SHENZHEN VIEWE TECHNOLOGY CO.,LTD
 */
#include "bsp/bsp_axp2101.h"
#include "bsp/board_config.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "axp2101";

#define REG_STATUS1           0x00  /* bit5 VBUS good, bit3 bat present */
#define REG_STATUS2           0x01  /* bit6:5 current dir */
#define REG_CHIP_ID           0x03
#define REG_MODULE_EN         0x18  /* bit3 gauge_en, bit1 chg_en */
#define REG_ADC_CH_EN         0x30  /* bit0 VBAT ADC */
#define REG_VBAT_H            0x34
#define REG_VBAT_L            0x35
#define REG_IPRECHG           0x61
#define REG_ICC               0x62
#define REG_ITERM             0x63
#define REG_CV                0x64
#define REG_BAT_DET           0x68
#define REG_LDO_ONOFF0        0x90
#define REG_ALDO3_VOLT        0x94
#define REG_BAT_PERCENT       0xA4

#define STATUS1_VBUS_GOOD     (1u << 5)
#define STATUS1_BAT_PRESENT   (1u << 3)

#define I2C_TIMEOUT_MS        200
#define FAIL_LOG_INTERVAL_US  5000000LL

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;
static bool s_ready;
static int64_t s_last_fail_log_us;

static esp_err_t axp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t axp_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

static esp_err_t axp_update_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t cur = 0;
    ESP_RETURN_ON_ERROR(axp_read(reg, &cur), TAG, "read 0x%02X failed", reg);
    uint8_t next = (cur & (uint8_t)~mask) | (value & mask);
    if (next == cur) {
        return ESP_OK;
    }
    return axp_write(reg, next);
}

static void axp_cleanup_device(void)
{
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
}

/** ALDO3：0.5~3.5V，100mV/step → code = (mV - 500) / 100 */
static uint8_t aldo3_volt_code(int mv)
{
    if (mv < 500) {
        mv = 500;
    }
    if (mv > 3500) {
        mv = 3500;
    }
    return (uint8_t)((mv - 500) / 100);
}

/**
 * ICC REG62[4:0]：
 *   N<=8: 25*N mA
 *   N>8 : 200+100*(N-8) mA
 * Datasheet：01001b(N=9) = 300mA（本板 0.5C@600mAh）
 */
static uint8_t icc_code_from_ma(int ma)
{
    if (ma <= 0) {
        return 0;
    }
    if (ma <= 200) {
        int n = (ma + 12) / 25; /* 就近档 */
        if (n < 0) {
            n = 0;
        }
        if (n > 8) {
            n = 8;
        }
        return (uint8_t)n;
    }
    int n = 8 + (ma - 200) / 100;
    if (n > 16) {
        n = 16;
    }
    return (uint8_t)n;
}

/** IPRECHG REG61：25*N mA（本板预充 ≈0.1C → 75mA → N=3） */
static uint8_t iprechg_code_from_ma(int ma)
{
    int n = (ma + 12) / 25;
    if (n < 0) {
        n = 0;
    }
    if (n > 15) {
        n = 15;
    }
    return (uint8_t)n;
}

/** ITERM REG63：bit4 使能 + 25*N mA（本板 ≈C/24 → 25mA → 0x11） */
static uint8_t iterm_code_from_ma(int ma)
{
    int n = (ma + 12) / 25;
    if (n < 1) {
        n = 1;
    }
    if (n > 15) {
        n = 15;
    }
    return (uint8_t)(0x10 | n);
}

static void note_i2c_fail(const char *what, esp_err_t err)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_fail_log_us > FAIL_LOG_INTERVAL_US) {
        ESP_LOGW(TAG, "%s failed: %s", what, esp_err_to_name(err));
        s_last_fail_log_us = now;
        if (s_bus) {
            i2c_master_bus_reset(s_bus);
        }
    }
}

esp_err_t bsp_axp2101_init(i2c_master_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG, "bus is NULL");
    if (s_ready) {
        return ESP_OK;
    }

    s_bus = bus;
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_ADDR_AXP2101,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t chip_id = 0;
    err = axp_read(REG_CHIP_ID, &chip_id);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "CHIP ID read failed (%s) — no AXP2101", esp_err_to_name(err));
        axp_cleanup_device();
        return ESP_ERR_NOT_FOUND;
    }
    if (chip_id != BOARD_AXP2101_CHIP_ID) {
        ESP_LOGI(TAG, "CHIP ID 0x%02X != 0x%02X — not AXP2101",
                 chip_id, BOARD_AXP2101_CHIP_ID);
        axp_cleanup_device();
        return ESP_ERR_NOT_FOUND;
    }

    /* 配置失败时清理设备，避免留下半初始化句柄后误走 V1 */
    do {
        /* 先开 ALDO3，再配充电（V2 外设轨依赖） */
        err = axp_write(REG_ALDO3_VOLT, aldo3_volt_code(BOARD_AXP2101_ALDO3_MV));
        if (err != ESP_OK) {
            break;
        }
        err = axp_update_bits(REG_LDO_ONOFF0, 0x04, 0x04);
        if (err != ESP_OK) {
            break;
        }

        /* 充电 + fuel gauge 使能（REG18 bit1 / bit3）；不写 REGA1 厂参 */
        err = axp_update_bits(REG_MODULE_EN, 0x0A, 0x0A);
        if (err != ESP_OK) {
            break;
        }

        err = axp_update_bits(REG_BAT_DET, 0x01, 0x01);
        if (err != ESP_OK) {
            break;
        }

        err = axp_update_bits(REG_ADC_CH_EN, 0x01, 0x01);
        if (err != ESP_OK) {
            break;
        }

        err = axp_write(REG_CV, 0x03); /* 4.2V = BOARD_BAT_FULL_V */
        if (err != ESP_OK) {
            break;
        }
        err = axp_write(REG_IPRECHG, iprechg_code_from_ma(BOARD_BAT_PRECHARGE_MA));
        if (err != ESP_OK) {
            break;
        }
        err = axp_write(REG_ICC, icc_code_from_ma(BOARD_BAT_CHARGE_MA));
        if (err != ESP_OK) {
            break;
        }
        err = axp_write(REG_ITERM, iterm_code_from_ma(BOARD_BAT_ITERM_MA));
        if (err != ESP_OK) {
            break;
        }

        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            err = ESP_ERR_NO_MEM;
            break;
        }

        s_ready = true;
        ESP_LOGI(TAG,
                 "ready: bat=%dmAh/%.1fV, ALDO3=%dmV, ICC=%dmA pre=%dmA iterm=%dmA CV=4.2V, CHIP_ID=0x%02X",
                 BOARD_BAT_CAPACITY_MAH, (double)BOARD_BAT_NOMINAL_V,
                 BOARD_AXP2101_ALDO3_MV, BOARD_BAT_CHARGE_MA,
                 BOARD_BAT_PRECHARGE_MA, BOARD_BAT_ITERM_MA, chip_id);
        return ESP_OK;
    } while (0);

    ESP_LOGE(TAG, "config failed: %s", esp_err_to_name(err));
    axp_cleanup_device();
    return err;
}

bool bsp_axp2101_is_ready(void)
{
    return s_ready;
}

esp_err_t bsp_axp2101_get_battery(int *percent, bool *charging, float *voltage_v)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    uint8_t status1 = 0;
    uint8_t status2 = 0;
    uint8_t pct = 0;
    esp_err_t err = axp_read(REG_STATUS1, &status1);
    if (err != ESP_OK) {
        note_i2c_fail("status1", err);
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    err = axp_read(REG_STATUS2, &status2);
    if (err != ESP_OK) {
        note_i2c_fail("status2", err);
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    err = axp_read(REG_BAT_PERCENT, &pct);
    if (err != ESP_OK) {
        note_i2c_fail("percent", err);
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    const bool bat_present = (status1 & STATUS1_BAT_PRESENT) != 0;
    const bool vbus_good = (status1 & STATUS1_VBUS_GOOD) != 0;
    const int dir = (status2 >> 5) & 0x03; /* 00 standby, 01 charge, 10 discharge */

    /* 与 V1 语义对齐：充电中或外供电（VBUS good）均视为 charging */
    bool is_chg = (dir == 1) || vbus_good;

    int pct_out = bat_present ? (int)pct : 0;
    if (pct_out > 100) {
        pct_out = 100;
    }
    if (pct_out < 0) {
        pct_out = 0;
    }

    float volts = 0.0f;
    uint8_t vh = 0;
    uint8_t vl = 0;
    /* 手册：先读高 6 位再读低 8 位；1 LSB = 1mV */
    if (bat_present &&
        axp_read(REG_VBAT_H, &vh) == ESP_OK &&
        axp_read(REG_VBAT_L, &vl) == ESP_OK) {
        uint16_t raw = (uint16_t)(((vh & 0x3F) << 8) | vl);
        volts = raw / 1000.0f;
    }

    if (percent) {
        *percent = pct_out;
    }
    if (charging) {
        *charging = is_chg;
    }
    if (voltage_v) {
        *voltage_v = volts;
    }

    xSemaphoreGive(s_lock);
    return ESP_OK;
}
