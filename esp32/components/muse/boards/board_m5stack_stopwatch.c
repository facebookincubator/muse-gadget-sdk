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
 * M5Stack StopWatch (C152): ESP32-S3R8 (16 MB flash, 8 MB octal PSRAM), round
 * 1.75" 466 px CO5300 AMOLED on QSPI with CST820 touch, ES8311 codec with one
 * mic and an AW8737A speaker amp, two buttons on the rim (yellow upper left,
 * blue upper right) and a power button wired to the power chip.
 *
 * Everything shares one I2C bus (GPIO 47/48). M5's power chip (M5PM1, 0x6E)
 * measures the battery and USB; its GPIO2 is the charger's CHG_STAT and its
 * GPIO1 the IRQ line to GPIO12 that wakes the paused input task on a red key
 * click. M5's IO expander (M5IOE1, 0x4F) switches the rest: IO3 the AU_L3B
 * rail (codec and mic), IO4 touch reset, IO5 panel reset, IO8 the 3V3_L3B
 * rail (the OLED), IO9 the vibration motor, IO10 the amp. The BMI270 (0x68)
 * and RTC are on the always-on 3V3_L1; the IMU is put back in suspend at boot.
 *
 * At rest on battery (display paused, codecs closed) both L3B rails are off,
 * with the lines into them parked low, and the chip light-sleeps until a rim
 * key or the PM1's interrupt: display_pause, codec_power, wait_buttons.
 *
 * Pins and registers are from M5Unified and M5GFX (board_M5StopWatch:
 * M5Unified.inl, Power_Class.inl, M5PM1_Class.inl, M5IOE1_Class.inl; M5GFX.cpp
 * Panel_StopWatch) and M5's factory firmware, M5StopWatch-UserDemo at 6b4aa125
 * (main/hal/). The panel's 466 columns start at 6, as on the Waveshare 1.75C;
 * the factory firmware hands touch coordinates to LVGL unscaled.
 * Dedicated controls also follow factory BtnA -> left, BtnB -> right
 * (app_stopwatch/view/view.cpp) and M5PM1 1.0.6 at 8f1f1a60:
 * src/M5PM1.h/btnGetState(), BTN_CFG_1 and SYS_CMD, not an invented PWR GPIO.
 * Vendor clones read for this change: M5Unified fd40d58, M5GFX c5a3fef.
 * Rest power: the StopWatch schematic (power network sheet 2, PMIC and LDOs
 * sheet 3, ESP32 pins sheet 4, IMU, audio, OLED and expander sheet 5), the
 * M5PM1 user manual (docs/ in M5PM1 be9a545: IRQ Status 1-3, BTN_CFG_1) and
 * M5Unified's ES8311 notes for the StopWatch (M5Unified.inl).
 */
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "soc/spi_periph.h"

static const char *TAG = "board";

#define LCD_RES 466
#define LCD_GAP_X 6                /* the 466 columns in the controller's 480 */
#define LCD_HOST SPI2_HOST
#define LCD_CS GPIO_NUM_39
#define LCD_SCLK GPIO_NUM_40
#define LCD_D0 GPIO_NUM_41
#define LCD_D1 GPIO_NUM_42
#define LCD_D2 GPIO_NUM_46
#define LCD_D3 GPIO_NUM_45
#define DRAW_BUF_LINES 118         /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)
#define LCD_LEVEL_BOOT 0xA0        /* M5GFX's brightness, until the UI sets its own */
#define LCD_POWER_UP_MS 20         /* 3V3_L3B up before the reset is let go; M5GFX waits 10 */
#define LCD_RESET_MS 120           /* the panel's wake from reset, as init() waits */

#define I2C_SDA GPIO_NUM_47
#define I2C_SCL GPIO_NUM_48
#define I2S_MCLK GPIO_NUM_18
#define I2S_BCLK GPIO_NUM_17
#define I2S_WS GPIO_NUM_15
#define I2S_DOUT GPIO_NUM_21
#define I2S_DIN GPIO_NUM_16
#define CODEC_POWER_UP_MS 10       /* AU_L3B up before the ES8311's setup, as M5Unified waits */
#define CODEC_MIN_OFF_US 200000    /* AU_L3B off at least this long: a quickly cycled rail misfires */
#define CODEC_ARM_MS 30            /* M5Unified arms capture at +30/60/90 ms of running I2S clock */
#define ES8311_REG_SYSTEM 0x0D     /* 0x01: analog circuitry powered up */

#define TALK_GPIO GPIO_NUM_1       /* KEYB / BtnB, blue, upper right: PTT */
#define SPEAKER_GPIO GPIO_NUM_2    /* KEYA / BtnA, yellow, upper left: speaker */

#define TP_ADDR 0x15
#define TP_REG_POINTS 0x02         /* finger count, then X and Y, 12 bits each */
#define TP_REG_SLEEP 0xE5          /* 0x03: deep sleep until reset */
#define TP_REG_NO_AUTO_SLEEP 0xFE  /* else it stops answering I2C when idle */

#define PMIC_ADDR 0x6E
#define PMIC_PWR_CFG 0x06
#define PMIC_PWR_LED BIT(4)
#define PMIC_PWR_5V_OUT 0x08       /* 5 V boost to the Grove port */
#define PMIC_I2C_CFG 0x09          /* 0: no I2C idle sleep */
#define PMIC_WDT 0x0A              /* 0: watchdog off */
#define PMIC_GPIO_MODE 0x10        /* 1 = output */
#define PMIC_GPIO_OUT 0x11
#define PMIC_GPIO_IN 0x12
#define PMIC_GPIO_OD 0x13          /* 1 = open drain */
#define PMIC_GPIO_FUNC0 0x16       /* two bits per GPIO 0-3; 0 = GPIO */
#define PMIC_CHG_STAT BIT(2)       /* low while charging */
#define PMIC_CHG_PROG BIT(3)       /* driven low, as M5's firmware does */
#define PMIC_VBAT_MV 0x22          /* 16-bit little-endian millivolts */
#define PMIC_VIN_MV 0x24           /* USB */
/* M5PM1 v1.0.6 src/M5PM1.h and btnGetState(): red PWR is PMIC BTN,
 * not an ESP GPIO. Bit7 records a past press and clears on read; ignore it. */
#define PMIC_BTN_STATUS 0x48
#define PMIC_BTN_PRESSED BIT(0)
#define PMIC_BTN_CFG1 0x49
#define PMIC_SINGLE_RESET_DISABLE BIT(0)
#define PMIC_LONG_DELAY_MASK 0x18
#define PMIC_LONG_DELAY_4S 0x18
#define PMIC_SYS_CMD 0x0C
#define PMIC_SHUTDOWN 0xA1
#define POWER_DEBOUNCE_US 30000
#define POWER_HOLD_US 1500000
/* The PM1's IRQ output, for wait_buttons(): its GPIO1 (G1_IRQout_ADC) is net
 * G12_PY_IRQ to ESP32 GPIO12, "PM1: Pull IRQ Low" to wake the S3 (schematic
 * sheets 2 and 4). M5PM1 user manual, IRQ Status 1-3 and src/M5PM1.h: the pin
 * is pulled low while an unmasked status bit is set and released once it's
 * cleared (written 0); mask bits are 1 = masked. */
#define PMIC_IRQ_GPIO GPIO_NUM_12
#define PMIC_IRQ_PIN BIT(1)        /* PM1 GPIO1 */
#define PMIC_FUNC_GPIO1_MASK 0x0C  /* GPIO_FUNC0 [3:2] */
#define PMIC_FUNC_GPIO1_IRQ 0x04   /* 01 = IRQ output */
#define PMIC_IRQ_STATUS1 0x40      /* GPIO level changes */
#define PMIC_IRQ_STATUS2 0x41      /* 5VIN/5VINOUT/battery */
#define PMIC_IRQ_STATUS3 0x42      /* the power key */
#define PMIC_IRQ_MASK1 0x43
#define PMIC_IRQ_MASK2 0x44
#define PMIC_IRQ_MASK3 0x45
#define PMIC_IRQ_GPIO_ALL 0x1F
#define PMIC_IRQ_SYS_ALL 0x3F
#define PMIC_IRQ_BTN_CLICK BIT(0)
#define PMIC_IRQ_BTN_WAKE BIT(1)
#define PMIC_IRQ_BTN_DOUBLE BIT(2)
#define PMIC_IRQ_BTN_ALL 0x07
#define PMIC_CLICK_DELAY_MASK 0x06 /* BTN_CFG1 [2:1] */
#define PMIC_CLICK_DELAY_125MS 0x00
#define PMIC_DBL_DELAY_MASK 0x60   /* BTN_CFG1 [6:5], the double-click gap */
#define PMIC_DBL_DELAY_250MS 0x20  /* the chip's reset value (BTN_CFG1 = 0x2A); M5's firmware keeps it */
/* The latest a click can follow its release, as pmic_irq_setup programs the
 * PM1: the 125 ms click delay, plus the 250 ms double-click gap it may wait
 * out for a second press, plus 125 ms for the wake, the I2C read and the
 * poll's 30 ms debounce. A release the poll saw earns a credit for this long
 * (poll_power_button). */
#define CLICK_CREDIT_US ((125 + 250 + 125) * 1000)
#define REST_KEY_POLL_MS 50        /* the red key down or settling while paused: poll it, as before */
#define REST_FALLBACK_MS 200       /* no PM1 interrupt: the longest a paused wait may last */
#define REST_KEY_FIND_MS 1000      /* the longest any paused wait lasts: a held red key is found this soon */
#define IRQ_SETTLE_MS 10           /* the line just fell: the next poll takes the event */

#define IMU_ADDR 0x68              /* BMI270 on 3V3_L1, sheet 5 (the BMM150 isn't fitted) */
#define IMU_REG_CHIP_ID 0x00
#define IMU_CHIP_ID 0x24
#define IMU_REG_CMD 0x7E
#define IMU_SOFT_RESET 0xB6

#define IOE_ADDR 0x4F
#define IOE_MODE 0x03              /* 16 bits, IO1 in bit 0; 1 = output */
#define IOE_OUT 0x05
#define IOE_IN 0x07
#define IOE_OD 0x13                /* 1 = open drain */
#define IOE_PWM1_DUTY 0x1B         /* the motor's PWM; bit 15 enables */
#define IOE_I2C_CFG 0x23           /* 0: no I2C idle sleep */
#define IOE_PIN(n) ((uint16_t)BIT((n) - 1))
#define IOE_AUDIO IOE_PIN(3)
#define IOE_TP_RST IOE_PIN(4)
#define IOE_LCD_RST IOE_PIN(5)
#define IOE_L3B IOE_PIN(8)
#define IOE_MOTOR IOE_PIN(9)
#define IOE_SPK IOE_PIN(10)
#define IOE_OUTPUTS (IOE_AUDIO | IOE_TP_RST | IOE_LCD_RST | IOE_L3B | IOE_MOTOR | IOE_SPK)
#define SPK_PA_PIN 10              /* es8311's pa_pin, routed to the expander by s_ioe_gpio */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_pmic, s_ioe, s_tp;
static SemaphoreHandle_t s_ioe_lock;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static const audio_codec_if_t *s_codec;
static es8311_codec_cfg_t s_es_cfg;     /* the ES8311's setup, run again after a power-up */
static bool s_codec_off;                /* AU_L3B off, the I2S lines parked (codec_power) */
static bool s_codec_arm;                /* powered up since the capture path was last armed */
static int64_t s_codec_off_at;
/* The panel's state, kept on the send task (muse_lcd_bands_run): the CO5300
 * forgets its setup, brightness and sleep with 3V3_L3B (display_pause). */
static bool s_lcd_off;                  /* rail off: nothing goes to the panel */
static uint8_t s_lcd_level = LCD_LEVEL_BOOT;   /* the last 0x51 the UI set */
static bool s_lcd_asleep;               /* the UI's panel_sleep() */
static muse_gpio_button_t s_talk, s_speaker;
static muse_gpio_button_t s_irq_line;   /* GPIO12, only ever waited on: idle high */
static bool s_power_pressed, s_power_candidate, s_power_armed, s_power_long;
static int64_t s_power_changed_at, s_power_held_at;
/* Clicks against the releases the poll saw (poll_power_button): one credit
 * per release, spent by the PM1's click for that press; clicks with none to
 * spend wait to be reported as taps. */
static unsigned s_click_credits, s_click_pending;
static int64_t s_click_credit_at;       /* the latest release that earned one */
static bool s_pmic_irq;                 /* the PM1's IRQ output is set up */
static bool s_irq_stuck;                /* the line stayed low after clearing */
static unsigned poll_power_button(void);

/* Both chips are microcontrollers and now and then miss a transfer; retry. */
static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t n)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = i2c_master_transmit_receive(dev, &reg, 1, buf, n, 50);
    }
    return err;
}

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t *val, size_t n)
{
    uint8_t buf[3] = { reg };
    assert(n < sizeof(buf));
    memcpy(buf + 1, val, n);
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = i2c_master_transmit(dev, buf, n + 1, 50);
    }
    return err;
}

static esp_err_t pmic_update(uint8_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(reg_read(s_pmic, reg, &v, 1), TAG, "pmic read %02x", reg);
    uint8_t want = (v & ~mask) | (bits & mask);
    return want == v ? ESP_OK : reg_write(s_pmic, reg, &want, 1);
}

/* M5Unified's M5PM1_Class::setLedEnLevel controls PWR_CFG bit4.
 * StopWatch schematic sheet 2: LED_EN_PP -> R19 -> green LED1 -> GND
 * (active high). Keep the USB indicator, but save its current on battery.
 * Use a checked VIN read, as factory hal_pmic.cpp does; an I2C failure is
 * not a power-source transition. pmic_update preserves charging and rails. */
static esp_err_t read_usb_power(bool *usb)
{
    uint8_t b[2];
    ESP_RETURN_ON_ERROR(reg_read(s_pmic, PMIC_VIN_MV, b, sizeof(b)), TAG, "pmic vin");
    *usb = (b[0] | b[1] << 8) > 4000;
    return pmic_update(PMIC_PWR_CFG, PMIC_PWR_LED, *usb ? PMIC_PWR_LED : 0);
}

static int pmic_mv(uint8_t reg)
{
    uint8_t b[2];
    return reg_read(s_pmic, reg, b, sizeof(b)) == ESP_OK ? b[0] | b[1] << 8 : 0;
}

/* Writing 0 clears every status bit and lets the IRQ line go high again. */
static esp_err_t pmic_irq_clear(void)
{
    esp_err_t err = ESP_OK;
    for (uint8_t reg = PMIC_IRQ_STATUS1; reg <= PMIC_IRQ_STATUS3; reg++) {
        esp_err_t e = reg_write(s_pmic, reg, (uint8_t[]){ 0 }, 1);
        err = err == ESP_OK ? e : err;
    }
    return err;
}

/*
 * The PM1 pulls GPIO12 low for two power key events only, IRQ_STATUS3's
 * SINGLE_CLICK (bit 0) and DOUBLE_CLICK (bit 2):
 * - The manual raises SINGLE_CLICK only with single-click reset disabled
 *   (init does) and DOUBLE_CLICK only with double-click power-off disabled
 *   (it isn't, so a double click still powers off; unmasked anyway).
 * - A click is reported once the key is released and the click delay
 *   (BTN_CFG1 [2:1]) has passed. M5's firmware leaves it at 1 s; 125 ms here,
 *   so a tap at rest wakes the screen about that long after it's let go.
 *   Nothing is reported for a press or a hold: the paused wait's timeout
 *   (at most 1 s, also while napping: wait_buttons) still polls the live
 *   key, and poll_power_button turns a click no poll saw into a press and
 *   release.
 *   The double-click gap is set to the chip's default, 250 ms, so the
 *   longest wait for a click is known (CLICK_CREDIT_US).
 * - WAKEUP (bit 1) stays masked: it's the power-on event, and the manual says
 *   it stays set (holding the line low) until WAKE_SRC is cleared. GPIO level
 *   changes (the charger's CHG_STAT on GPIO2) and 5VIN/battery events stay
 *   masked too; USB power is noticed by the timeout as before.
 * The pin is open drain; GPIO12's internal pull-up (muse_gpio_button_init)
 * holds it high, also in light sleep (PM_SLP_DISABLE_GPIO is off).
 */
static esp_err_t pmic_irq_setup(void)
{
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_IRQ_MASK1, PMIC_IRQ_GPIO_ALL, PMIC_IRQ_GPIO_ALL), TAG, "mask gpio irqs");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_IRQ_MASK2, PMIC_IRQ_SYS_ALL, PMIC_IRQ_SYS_ALL), TAG, "mask system irqs");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_IRQ_MASK3, PMIC_IRQ_BTN_ALL, PMIC_IRQ_BTN_WAKE), TAG, "unmask clicks");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_BTN_CFG1, PMIC_CLICK_DELAY_MASK | PMIC_DBL_DELAY_MASK,
                                   PMIC_CLICK_DELAY_125MS | PMIC_DBL_DELAY_250MS), TAG, "click timing");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_OD, PMIC_IRQ_PIN, PMIC_IRQ_PIN), TAG, "irq open drain");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_FUNC0, PMIC_FUNC_GPIO1_MASK, PMIC_FUNC_GPIO1_IRQ), TAG, "gpio1 irq");
    return pmic_irq_clear();
}

/*
 * The line is low: read which key event it was, then clear every status.
 * Returns the presses it reported: 1 for a single click, 2 for a double.
 * Still low afterwards (a failed transfer, or an event that won't clear), the
 * paused wait stops trusting the line and polls every REST_FALLBACK_MS until
 * it reads high again.
 */
static unsigned take_power_irq(void)
{
    if (!s_pmic_irq) {
        return 0;
    }
    if (gpio_get_level(PMIC_IRQ_GPIO)) {
        s_irq_stuck = false;
        return 0;
    }
    uint8_t st = 0;
    bool read = reg_read(s_pmic, PMIC_IRQ_STATUS3, &st, 1) == ESP_OK;
    pmic_irq_clear();
    s_irq_stuck = gpio_get_level(PMIC_IRQ_GPIO) == 0;
    if (!read) {
        return 0;
    }
    return (st & PMIC_IRQ_BTN_CLICK ? 1 : 0) + (st & PMIC_IRQ_BTN_DOUBLE ? 2 : 0);
}

/* The expander's registers come in pairs, IO1-8 then IO9-14. The amp (audio
 * task), touch reset (input task) and power-off all change them, so one at a
 * time. */
static esp_err_t ioe_update(uint8_t reg, uint16_t mask, uint16_t bits)
{
    xSemaphoreTake(s_ioe_lock, portMAX_DELAY);
    uint8_t b[2];
    esp_err_t err = reg_read(s_ioe, reg, b, sizeof(b));
    if (err == ESP_OK) {
        uint16_t v = b[0] | b[1] << 8;
        uint16_t want = (v & ~mask) | (bits & mask);
        if (want != v) {
            b[0] = want;
            b[1] = want >> 8;
            err = reg_write(s_ioe, reg, b, sizeof(b));
        }
    }
    xSemaphoreGive(s_ioe_lock);
    return err;
}

static esp_err_t ioe_set(uint16_t pins, bool high)
{
    return ioe_update(IOE_OUT, pins, high ? pins : 0);
}

/* The codec driver switches the amp through this as the speaker opens and closes. */
static int ioe_gpio_setup(int16_t gpio, audio_gpio_dir_t dir, audio_gpio_mode_t mode)
{
    (void)gpio;
    (void)dir;
    (void)mode;
    return ESP_CODEC_DEV_OK;
}

static int ioe_gpio_set(int16_t gpio, bool high)
{
    (void)gpio;
    return ioe_set(IOE_SPK, high) == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

static bool ioe_gpio_get(int16_t gpio)
{
    (void)gpio;
    uint8_t b[2] = { 0 };
    reg_read(s_ioe, IOE_OUT, b, sizeof(b));
    return (b[0] | b[1] << 8) & IOE_SPK;
}

static const audio_codec_gpio_if_t s_ioe_gpio = {
    .setup = ioe_gpio_setup,
    .set = ioe_gpio_set,
    .get = ioe_gpio_get,
};

static esp_err_t add_device(uint8_t addr, uint32_t hz, i2c_master_dev_handle_t *dev)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = hz,
    };
    return i2c_master_bus_add_device(s_i2c, &cfg, dev);
}

/* Reset pulse on the touch controller, then keep it answering while idle. */
static void tp_reset(void)
{
    ioe_set(IOE_TP_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    ioe_set(IOE_TP_RST, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (reg_write(s_tp, TP_REG_NO_AUTO_SLEEP, (uint8_t[]){ 0x01 }, 1) != ESP_OK) {
        ESP_LOGW(TAG, "touch not answering");
    }
}

/*
 * The BMI270 is on the always-on 3V3_L1 rail and M5's factory firmware turns
 * its accelerometer on. Muse doesn't use it, so after a reflash it could
 * keep running; a soft reset puts it back in its power-on state (sensors
 * suspended, advanced power save on). Absent, nothing happens.
 */
static void imu_rest(void)
{
    i2c_master_dev_handle_t imu;
    if (i2c_master_probe(s_i2c, IMU_ADDR, 50) != ESP_OK || add_device(IMU_ADDR, 400000, &imu) != ESP_OK) {
        return;
    }
    uint8_t id = 0;
    if (reg_read(imu, IMU_REG_CHIP_ID, &id, 1) == ESP_OK && id == IMU_CHIP_ID) {
        esp_err_t err = reg_write(imu, IMU_REG_CMD, (uint8_t[]){ IMU_SOFT_RESET }, 1);
        ESP_LOGI(TAG, "BMI270: %s", err == ESP_OK ? "soft reset, sensors suspended" : "soft reset failed");
    }
    i2c_master_bus_rm_device(imu);
}

static esp_err_t init(void)
{
    s_ioe_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ioe_lock, ESP_ERR_NO_MEM, TAG, "ioe lock");
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    ESP_RETURN_ON_ERROR(add_device(PMIC_ADDR, 100000, &s_pmic), TAG, "pmic");
    ESP_RETURN_ON_ERROR(add_device(IOE_ADDR, 100000, &s_ioe), TAG, "ioe");
    ESP_RETURN_ON_ERROR(add_device(TP_ADDR, 400000, &s_tp), TAG, "touch");

    /* The power chip keeps its settings while the ESP32 resets, and M5's
     * firmware may have left its I2C sleep or watchdog on; M5GFX clears both
     * on every boot. CHG_STAT is an input, CHG_PROG low as M5's firmware has
     * it, and the 5 V boost off: nothing Muse uses hangs off the Grove port. */
    ESP_RETURN_ON_ERROR(reg_write(s_pmic, PMIC_I2C_CFG, (uint8_t[]){ 0 }, 1), TAG, "pmic i2c sleep off");
    ESP_RETURN_ON_ERROR(reg_write(s_pmic, PMIC_WDT, (uint8_t[]){ 0 }, 1), TAG, "pmic watchdog off");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_FUNC0, 0xF0, 0), TAG, "pmic gpio2-3 function");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_OD, PMIC_CHG_PROG, 0), TAG, "pmic gpio3 push-pull");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_OUT, PMIC_CHG_PROG, 0), TAG, "pmic gpio3 low");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_MODE, PMIC_CHG_STAT | PMIC_CHG_PROG, PMIC_CHG_PROG), TAG, "pmic gpio mode");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_PWR_CFG, PMIC_PWR_5V_OUT, 0), TAG, "5 V out off");
    bool usb;
    ESP_RETURN_ON_ERROR(read_usb_power(&usb), TAG, "power LED");
    /* Factory hal_pmic.cpp disables single-click reset. Keep double-click
     * emergency shutdown and download access; delay the PMIC's long action
     * to 4 s so Muse's 1.5 s hold plus goodbye can shut down cleanly first. */
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_BTN_CFG1, PMIC_SINGLE_RESET_DISABLE | PMIC_LONG_DELAY_MASK,
                                   PMIC_SINGLE_RESET_DISABLE | PMIC_LONG_DELAY_4S), TAG, "power key config");

    /* Motor and amp off, audio and the L3B rail on, and both resets released
     * (driven push-pull). Then reset the panel and touch together, as M5GFX
     * does. The expander's own MUX select (IO1) is left as it is. */
    ESP_RETURN_ON_ERROR(reg_write(s_ioe, IOE_I2C_CFG, (uint8_t[]){ 0 }, 1), TAG, "ioe i2c sleep off");
    ESP_RETURN_ON_ERROR(reg_write(s_ioe, IOE_PWM1_DUTY, (uint8_t[]){ 0, 0 }, 2), TAG, "motor pwm off");
    ESP_RETURN_ON_ERROR(ioe_update(IOE_OD, IOE_OUTPUTS, 0), TAG, "ioe push-pull");
    ESP_RETURN_ON_ERROR(ioe_update(IOE_OUT, IOE_OUTPUTS, IOE_AUDIO | IOE_TP_RST | IOE_LCD_RST | IOE_L3B), TAG,
                        "ioe levels");
    ESP_RETURN_ON_ERROR(ioe_update(IOE_MODE, IOE_OUTPUTS, IOE_OUTPUTS), TAG, "ioe outputs");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(ioe_set(IOE_TP_RST | IOE_LCD_RST, false), TAG, "panel reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(ioe_set(IOE_TP_RST | IOE_LCD_RST, true), TAG, "panel reset");
    vTaskDelay(pdMS_TO_TICKS(120));   /* the panel's wake from reset */
    imu_rest();

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_speaker, SPEAKER_GPIO), TAG, "speaker button");
    /* A button that woke the board from power-off is still down; don't count it.
     * The PMIC key arms only after a debounced release, even if its first read fails. */
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;
    s_speaker.pressed = gpio_get_level(SPEAKER_GPIO) == 0;
    s_power_pressed = s_power_candidate = true;
    s_power_changed_at = esp_timer_get_time();

    /* Without the PM1's interrupt, wait_buttons() polls the red key every
     * REST_FALLBACK_MS instead (the two GPIO keys still interrupt). */
    s_pmic_irq = muse_gpio_button_init(&s_irq_line, PMIC_IRQ_GPIO) == ESP_OK && pmic_irq_setup() == ESP_OK;
    if (!s_pmic_irq) {
        ESP_LOGW(TAG, "power key interrupt not set up: polling the power key at rest");
    }
    return ESP_OK;
}

/* M5GFX's Panel_StopWatch init, less the tearing line: COLMOD and MADCTL come
 * from the driver first. Brightness (0x51) and display on (0x29) follow in
 * panel_setup(), once the panel's memory is black. */
static const co5300_lcd_init_cmd_t s_lcd_init[] = {
    { 0x11, NULL, 0, 150 },                     /* sleep out */
    { 0xC4, (uint8_t[]){ 0x80 }, 1, 0 },
    { 0x53, (uint8_t[]){ 0x20 }, 1, 0 },        /* brightness control on */
};

/* A CO5300 command over QSPI. */
static void lcd_cmd(uint8_t cmd, const void *param, size_t n)
{
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (cmd << 8), param, n);
}

/*
 * At boot, and powered again after a pause: the panel has lost its setup and
 * what it showed. A software reset (its reset line is the expander's), the
 * init (ending with SLPOUT), then black over its random memory before the
 * display goes on, so nothing shows before LVGL's redraw reaches it. Then the
 * last brightness and sleep state the UI set. After registration this runs
 * on the send task between bands, so the chunk buffers are idle; the
 * clearing takes a buffer of its own for a moment.
 */
static void panel_setup(void *arg)
{
    (void)arg;
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    uint8_t *black = heap_caps_calloc(1, LCD_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (black) {
        for (int y = 0; y < LCD_RES; y += 8) {
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_RES, y + 8 < LCD_RES ? y + 8 : LCD_RES, black);
        }
    }
    lcd_cmd(0x51, &s_lcd_level, 1);             /* waits for the queued draws first */
    esp_lcd_panel_disp_on_off(s_panel, true);
    free(black);
    if (s_lcd_asleep) {
        lcd_cmd(0x10, NULL, 0);                 /* the screen is off: as the UI left it */
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    s_lcd_off = false;
}

/* The CO5300 needs even-aligned update windows. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/* LVGL polls this from its own task. The CST820 reports one point. */
static void tp_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint8_t b[5];
    if (reg_read(s_tp, TP_REG_POINTS, b, sizeof(b)) != ESP_OK || (b[0] & 0x0F) == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    int x = (b[1] & 0x0F) << 8 | b[2];
    int y = (b[3] & 0x0F) << 8 | b[4];
    data->point.x = x < LCD_RES ? x : LCD_RES - 1;
    data->point.y = y < LCD_RES ? y : LCD_RES - 1;
    data->state = LV_INDEV_STATE_PRESSED;
}

/* As on the Waveshare 1.75C, the bands go out through fixed internal buffers. */
static lv_display_t *display_start(lv_indev_t **touch)
{
    const spi_bus_config_t bus =
        CO5300_PANEL_BUS_QSPI_CONFIG(LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    co5300_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_init,
        .init_cmds_size = sizeof(s_lcd_init) / sizeof(s_lcd_init[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,      /* the expander's IO5, pulsed in init() */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, 0);
    muse_lcd_bands_set_gap(LCD_GAP_X, 0);   /* video draws to the panel's memory directly */
    panel_setup(NULL);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_RES,
            .ver_res = LCD_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    tp_reset();
    *touch = lv_indev_create();
    if (!*touch) {
        return NULL;
    }
    lv_indev_set_type(*touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(*touch, disp);
    lv_indev_set_read_cb(*touch, tp_read);
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

/* CO5300 "write display brightness" (0x51), kept for the next power-up. */
static void send_brightness(void *level)
{
    s_lcd_level = *(uint8_t *)level;
    if (!s_lcd_off) {
        lcd_cmd(0x51, &s_lcd_level, 1);
    }
}

static void set_brightness(int pct)
{
    uint8_t level = (uint8_t)(pct * 255 / 100);
    muse_lcd_bands_run(send_brightness, &level);
}

static void send_sleep(void *sleep)
{
    s_lcd_asleep = *(bool *)sleep;
    if (!s_lcd_off) {
        lcd_cmd(s_lcd_asleep ? 0x10 : 0x11, NULL, 0);
    }
}

/* Plain SLPIN/SLPOUT over the QSPI command path, as on the Waveshare 1.75C. */
static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/*
 * 3V3_L3B (the OLED, and the panel's VBAT through R56; schematic sheets 2, 3
 * and 5) is off while paused, switched by the expander's IO8 (PYB_L3B_EN).
 * Idle, the SPI holds CS high, which would feed the unpowered panel, so the
 * QSPI lines go to plain GPIOs driven low first, as M5's design for "L3A core
 * active" leaves the L3B side's lines (and the SenseCAP Watcher's lcd_bus).
 */
static void lcd_bus(void *spi)
{
    const spi_signal_conn_t *sig = &spi_periph_signal[LCD_HOST];
    const struct {
        gpio_num_t pin;
        int in, out;
    } lines[] = {
        { LCD_SCLK, -1, sig->spiclk_out },
        { LCD_D0, sig->spid_in, sig->spid_out },
        { LCD_D1, sig->spiq_in, sig->spiq_out },
        { LCD_D2, sig->spiwp_in, sig->spiwp_out },
        { LCD_D3, sig->spihd_in, sig->spihd_out },
        { LCD_CS, -1, sig->spics_out[0] },
    };
    for (int i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        if (*(bool *)spi) {
            gpio_set_direction(lines[i].pin, lines[i].in >= 0 ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_OUTPUT);
            if (lines[i].in >= 0) {
                esp_rom_gpio_connect_in_signal(lines[i].pin, lines[i].in, false);
            }
            esp_rom_gpio_connect_out_signal(lines[i].pin, lines[i].out, false, false);
        } else {
            gpio_set_level(lines[i].pin, 0);
            gpio_set_direction(lines[i].pin, GPIO_MODE_OUTPUT);
        }
    }
}

/* On the send task: no command reaches the panel from here until panel_setup(). */
static void panel_off(void *arg)
{
    (void)arg;
    if (!s_lcd_asleep) {
        lcd_cmd(0x10, NULL, 0);                 /* the UI sleeps it first; just in case */
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    s_lcd_off = true;
    bool spi = false;
    lcd_bus(&spi);
}

/*
 * Off: the lines low, the panel held in reset (IO5), then the rail. On: the
 * rail, its power-up time with reset still low, the SPI lines back (CS idles
 * high before the panel leaves reset), the reset let go, and the panel set up
 * again with the brightness the UI last set (set_brightness and panel_sleep
 * only record theirs while the rail is off).
 */
static void lcd_power(bool on)
{
    if (!on) {
        muse_lcd_bands_run(panel_off, NULL);
        ioe_set(IOE_LCD_RST, false);
        ioe_set(IOE_L3B, false);
        ESP_LOGI(TAG, "OLED rail off");
        return;
    }
    ioe_set(IOE_L3B, true);
    vTaskDelay(pdMS_TO_TICKS(LCD_POWER_UP_MS));
    bool spi = true;
    muse_lcd_bands_run(lcd_bus, &spi);
    ioe_set(IOE_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(LCD_RESET_MS));
    muse_lcd_bands_run(panel_setup, NULL);
    ESP_LOGI(TAG, "OLED rail on, panel set up");
}

/*
 * Screen off: LVGL stops, the CST820 (on 3V3_L2) sleeps until its reset line
 * wakes it, and the OLED loses its rail. LVGL redraws the whole screen on
 * resume, since the panel forgot it.
 */
static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
        reg_write(s_tp, TP_REG_SLEEP, (uint8_t[]){ 0x03 }, 1);
        lcd_power(false);
    } else {
        lcd_power(true);
        tp_reset();
        esp_lv_adapter_resume();
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            lv_obj_invalidate(lv_screen_active());
            esp_lv_adapter_unlock();
        }
    }
}

/*
 * AU_L3B feeds the ES8311 and the LMA3729 mic and is switched by the
 * expander's IO3 (schematic sheets 2, 3 and 5). The codec's I2C goes through
 * 2N7002 hot-plug isolation, so the shared bus survives it off; M5Unified
 * keeps it off until audio is used. Unpowered, the codec mustn't be fed
 * through its I2S pins, so MCLK, BCLK, WS and DOUT go to plain GPIOs driven
 * low and DIN to an input without pulls. Their routing to the duplex master
 * channels is saved first and put back as the I2S driver set it.
 */
static const gpio_num_t s_i2s_pins[] = { I2S_MCLK, I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN };
static gpio_io_config_t s_i2s_saved[5];

static void i2s_park(bool park)
{
    for (int i = 0; i < 5; i++) {
        gpio_num_t pin = s_i2s_pins[i];
        bool input = pin == I2S_DIN;
        if (park) {
            gpio_get_io_config(pin, &s_i2s_saved[i]);
            if (!input) {
                gpio_set_level(pin, 0);
            }
            gpio_set_direction(pin, input ? GPIO_MODE_INPUT : GPIO_MODE_OUTPUT);
            gpio_set_pull_mode(pin, GPIO_FLOATING);
            continue;
        }
        const gpio_io_config_t *io = &s_i2s_saved[i];
        gpio_set_pull_mode(pin, io->pu && io->pd ? GPIO_PULLUP_PULLDOWN
                                : io->pu         ? GPIO_PULLUP_ONLY
                                : io->pd         ? GPIO_PULLDOWN_ONLY
                                                 : GPIO_FLOATING);
        if (!input) {
            /* As i2s_gpio_check_and_set() routes an output (no inversion). */
            gpio_set_direction(pin, io->ie ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_OUTPUT);
            esp_rom_gpio_connect_out_signal(pin, io->sig_out, false, false);
        }
    }
}

/*
 * muse_audio calls this on before the codecs open and off once both have
 * closed to rest; the amp (IOE10) is already off by then (es8311 pa_pin). A
 * power-on reset loses es8311_open()'s setup, which esp_codec_dev_open()
 * doesn't repeat, so it's run again here. M5Unified's notes on this codec:
 * a quickly cycled rail misfires (so the rail stays off CODEC_MIN_OFF_US at
 * least), capture has to be armed again with the I2S clock running
 * (audio_power), and the analog stage takes ~1 s to settle after power-up.
 */
static void codec_power(bool on)
{
    if (on != s_codec_off) {
        return;
    }
    if (!on) {
        i2s_park(true);
        if (ioe_set(IOE_AUDIO, false) != ESP_OK) {
            ESP_LOGW(TAG, "audio rail off failed");
        }
        s_codec_off = true;
        s_codec_off_at = esp_timer_get_time();
        ESP_LOGI(TAG, "audio rail off");
        return;
    }
    int64_t off_us = esp_timer_get_time() - s_codec_off_at;
    if (off_us < CODEC_MIN_OFF_US) {
        vTaskDelay(pdMS_TO_TICKS((CODEC_MIN_OFF_US - off_us) / 1000 + 1));
    }
    if (ioe_set(IOE_AUDIO, true) != ESP_OK) {
        ESP_LOGW(TAG, "audio rail on failed");   /* the codecs won't open; retried on the next */
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(CODEC_POWER_UP_MS));
    i2s_park(false);
    s_codec_off = false;
    s_codec_arm = true;
    if (s_codec && s_codec->open(s_codec, &s_es_cfg, sizeof(s_es_cfg)) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "ES8311 setup after power-up failed");
    }
    ESP_LOGI(TAG, "audio rail on");
}

/*
 * Codecs open and the I2S clock running: after a power-up, arm the ES8311's
 * capture path. M5Unified (_microphone_post_start_cb_stopwatch): a SYSTEM
 * (0x0D) write arms it only with the clock running, the earliest seen at
 * ~25 ms, so three writes at 30 ms steps. es8311_start()'s own write comes
 * a few ms after the clock starts, which can be too soon. At boot the codec
 * is set up with the clock already running, as before.
 */
static void audio_power(bool on)
{
    if (!on || !s_codec_arm || !s_codec) {
        return;
    }
    bool ok = false;
    for (int i = 0; i < 3; i++) {
        vTaskDelay(pdMS_TO_TICKS(CODEC_ARM_MS));
        ok = s_codec->set_reg(s_codec, ES8311_REG_SYSTEM, 0x01) == ESP_CODEC_DEV_OK;
    }
    s_codec_arm = !ok;   /* the last write acknowledged, or try again next open */
}

/* One ES8311 does both directions over a duplex I2S bus, clocked from MCLK. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(data_if && ctrl_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    s_es_cfg = (es8311_codec_cfg_t){
        .ctrl_if = ctrl_if,
        .gpio_if = &s_ioe_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = SPK_PA_PIN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&s_es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");
    s_codec = codec;

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

/* Read the live PMIC key: its IRQ click flag has no held state. A click
 * only stands in for a tap the poll missed (pmic_irq_setup). Failed
 * transfers never create edges or count towards a shutdown hold. */
static unsigned poll_power_button(void)
{
    int64_t now = esp_timer_get_time();
    s_click_pending += take_power_irq();
    uint8_t status;
    if (reg_read(s_pmic, PMIC_BTN_STATUS, &status, 1) != ESP_OK) {
        s_power_changed_at = now;
        s_power_held_at = 0;
        return 0;
    }
    bool pressed = (status & PMIC_BTN_PRESSED) != 0;
    if (pressed != s_power_candidate) {
        s_power_candidate = pressed;
        s_power_changed_at = now;
        s_power_held_at = 0;
    }
    unsigned ev = 0;
    if (pressed != s_power_pressed && now - s_power_changed_at >= POWER_DEBOUNCE_US) {
        s_power_pressed = pressed;
        s_power_long = false;
        if (!pressed) {
            s_click_credits++;      /* the PM1's click for this press is coming */
            s_click_credit_at = now;
        }
        if (!pressed && !s_power_armed) {
            s_power_armed = true;   /* release after boot: not an input event */
        } else if (s_power_armed) {
            ev = pressed ? MUSE_BTN_POWER_PRESS : MUSE_BTN_POWER_RELEASE;
        }
    }
    if (pressed && s_power_pressed && s_power_armed && !s_power_long) {
        if (!s_power_held_at) s_power_held_at = now;
        if (now - s_power_held_at >= POWER_HOLD_US) {
            s_power_long = true;
            ev |= MUSE_BTN_POWER_LONG;
        }
    }
    /* A tap at rest: the PM1 reports it once the key is up again, so the
     * poll it woke sees no press. Each click spends a credit from a release
     * the poll saw (that press, already reported); a click with none left
     * is a tap the poll missed. Report it as a press and release together
     * (a latched short, as the 1.75's PMU key gives), one a poll, once the
     * key is up and this poll has reported nothing else. A credit the PM1
     * never spends (a long press: no click) runs out after CLICK_CREDIT_US,
     * so it can't swallow a later tap. Clicks before the arming release
     * belong to the press that turned the board on. */
    if (s_click_credits && now - s_click_credit_at >= CLICK_CREDIT_US) {
        s_click_credits = 0;
    }
    while (s_click_pending && s_click_credits) {
        s_click_pending--;
        s_click_credits--;
    }
    if (!s_power_armed) {
        s_click_pending = 0;
    } else if (s_click_pending && !pressed && !s_power_pressed && !ev) {
        s_click_pending--;
        ev = MUSE_BTN_POWER_PRESS | MUSE_BTN_POWER_RELEASE;
    }
    return ev;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk) |
           muse_gpio_button_poll(&s_speaker) << 13 | poll_power_button();
}

/*
 * Display paused: sleep until a rim key changes or the PM1 pulls GPIO12 low
 * for a power key click (pmic_irq_setup), so the chip light-sleeps instead
 * of waking every 50 ms for an I2C read. poll_buttons() takes and clears the
 * PM1's event after each wake. The timeout still polls the live key, which
 * is how a held key (no click yet) is found. While the red key is down or
 * settling it's polled every REST_KEY_POLL_MS as before, so its debounce and
 * 1.5 s hold work unchanged. Without a working interrupt line, waits are
 * capped at REST_FALLBACK_MS.
 *
 * The PM1 has no interrupt for a press, only for a click once the key is up
 * again (its BTN_STATUS bit 7 latches a press but raises no IRQ, and WAKEUP
 * is the power-on event), so every wait is capped at REST_KEY_FIND_MS, the
 * input task's own rest timeout: a held red key is found within about a
 * second while Wi-Fi naps too, not after muse_input's 10 s NAP_WAIT_MS.
 * Estimated cost, napping only (not measured): about 0.9 more wakes a
 * second (every 1 s instead of 10 s; the battery run averaged 1.33 a second
 * in all), each a GPIO12 check and one 100 kHz I2C read of the key, roughly
 * 2 ms awake at the crystal clock. That is about 0.2% more time awake, some
 * 0.05 mA on average at ~25 mA awake.
 */
static void wait_buttons(int timeout_ms)
{
    if (s_power_pressed || s_power_candidate) {
        vTaskDelay(pdMS_TO_TICKS(REST_KEY_POLL_MS));
        return;
    }
    if (s_click_pending) {
        vTaskDelay(pdMS_TO_TICKS(IRQ_SETTLE_MS));   /* a tap still to report */
        return;
    }
    muse_gpio_button_t *const lines[] = { &s_talk, &s_speaker, &s_irq_line };
    int n = 3;
    if (!s_pmic_irq || s_irq_stuck) {
        n = 2;
    } else if (!gpio_get_level(PMIC_IRQ_GPIO)) {
        vTaskDelay(pdMS_TO_TICKS(IRQ_SETTLE_MS));   /* fell since the poll: take it next */
        return;
    }
    if (n == 2 && timeout_ms > REST_FALLBACK_MS) {
        timeout_ms = REST_FALLBACK_MS;
    } else if (timeout_ms > REST_KEY_FIND_MS) {
        timeout_ms = REST_KEY_FIND_MS;
    }
    muse_gpio_buttons_wait(lines, n, timeout_ms);
}

/* The power chip measures the battery and USB; the charger's CHG_STAT reaches its GPIO2. */
static esp_err_t read_power(muse_power_t *out)
{
    uint8_t in;
    ESP_RETURN_ON_ERROR(reg_read(s_pmic, PMIC_GPIO_IN, &in, 1), TAG, "pmic read");
    ESP_RETURN_ON_ERROR(read_usb_power(&out->usb), TAG, "power LED");
    int v = pmic_mv(PMIC_VBAT_MV);
    out->charging = out->usb && !(in & PMIC_CHG_STAT);
    out->battery_mv = v;
    if (v < 2500) {
        out->battery_pct = -1;
        return ESP_OK;
    }
    /* The Watcher's LiPo curve, as on the StickS3. */
    int pct = (-v * v + 9016 * v - 19189000) / 10000;
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    return ESP_OK;
}

/* The documented PMIC shutdown command cuts the main rail; the red POWER
 * button turns it back on. Do not substitute an ESP GPIO or deep-sleep wake. */
static esp_err_t power_off(void)
{
    uint8_t cmd = PMIC_SHUTDOWN;
    /* Don't turn off peripheral rails first: on a failed command the shared
     * input/UI must be able to display COULDN'T POWER OFF and remain usable. */
    ESP_RETURN_ON_ERROR(reg_write(s_pmic, PMIC_SYS_CMD, &cmd, 1), TAG, "pmic shutdown");
    vTaskDelay(pdMS_TO_TICKS(500));
    return ESP_FAIL;   /* still running: the PMIC didn't cut power */
}

static const muse_board_t s_board = {
    .name = "M5Stack StopWatch",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.75f,
    .talk_button = "top-right",
    .power_button = "red bottom-left",
    .speaker_button = "top-left",
    .rim_controls = true,
    /* Physical positions with USB down: blue ~2 o'clock, yellow ~10,
     * red ~7:30. All hint corners fit inside the 223 px inner rim. */
    .talk_hint = { LV_ALIGN_CENTER, 144, -110 },
    .speaker_hint = { LV_ALIGN_CENTER, -144, -110 },
    .power_hint = { LV_ALIGN_CENTER, -144, 110 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .draw_frame = muse_lcd_bands_frame,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .audio_power = audio_power,
    .codec_power = codec_power,
    .poll_buttons = poll_buttons,
    /* Red POWER is on the PMIC: its click interrupt (GPIO12) wakes the wait. */
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
