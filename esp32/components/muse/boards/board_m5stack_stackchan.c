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
 * M5Stack Stack-Chan (CoreS3 + Stack-Chan motorized base).
 * Core unit: 320x240 ILI9342C LCD, FT6336U touch, AW88298 speaker, ES7210 dual mic,
 * AXP2101 PMU and AW9523B IO expander (via espressif/m5stack_core_s3 BSP).
 * Stack-Chan base:
 * - 5V power bus enabled via CoreS3 AW9523B USB_EN (bsp_feature_enable(BSP_FEATURE_USB, true)).
 * - 2x Feetech SCS0009 serial bus servos (Yaw ID 1, Pitch ID 2) over half-duplex UART (1 Mbps).
 *   Connected to Port C (GPIO 17 TX, GPIO 18 RX) and/or internal base lines (GPIO 6/7).
 * - 12x WS2812C RGB LEDs driven by the base PY32L020 MCU at I2C address 0x6F (or direct fallback).
 */

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <string.h>

#include "muse_board.h"
#include "muse_audio.h"
#include "muse_mem.h"
#include "muse_pmu.h"
#include "muse_state.h"

#define TAG "stackchan"

#define PMU_KEY_EVERY 2         /* poll the PMU over I2C every 20 ms */

/* Servo UART settings */
#define SCS_UART_NUM           UART_NUM_1
#define SCS_BAUD_RATE          1000000
#define SCS_TX_PIN             GPIO_NUM_17
#define SCS_RX_PIN             GPIO_NUM_18
#define SCS_ALT_TX_PIN         GPIO_NUM_6
#define SCS_ALT_RX_PIN         GPIO_NUM_7

#define SERVO_ID_YAW           1
#define SERVO_ID_PITCH         2

#define YAW_CENTER             500
#define PITCH_CENTER           580
#define YAW_MIN                200
#define YAW_MAX                800
#define PITCH_MIN              380
#define PITCH_MAX              740

/* PY32L020 Base IO Expander I2C Address and Registers */
#define PY32_I2C_ADDR          0x6F
#define PY32_REG_GPIO_M_L      0x03
#define PY32_REG_GPIO_M_H      0x04
#define PY32_REG_GPIO_O_L      0x05
#define PY32_REG_GPIO_O_H      0x06
#define PY32_REG_GPIO_PU_L     0x09
#define PY32_REG_GPIO_PU_H     0x0A
#define PY32_REG_GPIO_DRV_L    0x13
#define PY32_REG_GPIO_DRV_H    0x14
#define PY32_REG_LED_CFG       0x24
#define PY32_REG_LED_RAM_START 0x30
#define PY32_LED_COUNT         12

static esp_codec_dev_handle_t s_spk, s_mic;
static bool s_pmu = false;
static bool s_servos_ready = false;
static i2c_master_dev_handle_t s_py32_dev = NULL;
static TaskHandle_t s_motion_task = NULL;

/* -------------------------------------------------------------------------
 * Feetech SCS0009 Serial Bus Servo Driver (Lightweight & Non-Blocking)
 * ------------------------------------------------------------------------- */

static void scs_uart_init(void)
{
    const uart_config_t uart_cfg = {
        .baud_rate = SCS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(SCS_UART_NUM, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UART param config failed: %s", esp_err_to_name(err));
        return;
    }

    err = uart_set_pin(SCS_UART_NUM, SCS_TX_PIN, SCS_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UART set pin failed: %s", esp_err_to_name(err));
        return;
    }

    err = uart_driver_install(SCS_UART_NUM, 256, 256, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return;
    }

    s_servos_ready = true;
    ESP_LOGI(TAG, "Servos UART1 initialized on TX=%d, RX=%d @ %d baud", SCS_TX_PIN, SCS_RX_PIN, SCS_BAUD_RATE);
}

static void scs_write_pos(uint8_t id, uint16_t pos, uint16_t time, uint16_t speed)
{
    if (!s_servos_ready) {
        return;
    }

    uint8_t buf[13];
    buf[0] = 0xFF;
    buf[1] = 0xFF;
    buf[2] = id;
    buf[3] = 9;   /* Length: 6 data bytes + 1 MemAddr + 1 Fun + 1 Checksum */
    buf[4] = 0x03;/* INST_WRITE */
    buf[5] = 42;  /* SCSCL_GOAL_POSITION_L */
    buf[6] = (uint8_t)((pos >> 8) & 0xFF);
    buf[7] = (uint8_t)(pos & 0xFF);
    buf[8] = (uint8_t)((time >> 8) & 0xFF);
    buf[9] = (uint8_t)(time & 0xFF);
    buf[10] = (uint8_t)((speed >> 8) & 0xFF);
    buf[11] = (uint8_t)(speed & 0xFF);

    uint8_t sum = 0;
    for (int i = 2; i <= 11; i++) {
        sum += buf[i];
    }
    buf[12] = (uint8_t)(~sum);

    /* Send packet asynchronously; no blocking */
    uart_write_bytes(SCS_UART_NUM, (const char *)buf, sizeof(buf));
}

static void scs_set_torque(uint8_t id, bool enable)
{
    if (!s_servos_ready) {
        return;
    }

    uint8_t buf[8];
    buf[0] = 0xFF;
    buf[1] = 0xFF;
    buf[2] = id;
    buf[3] = 4;   /* Length */
    buf[4] = 0x03;/* INST_WRITE */
    buf[5] = 40;  /* SCSCL_TORQUE_ENABLE */
    buf[6] = enable ? 1 : 0;
    buf[7] = (uint8_t)(~(id + 4 + 0x03 + 40 + (enable ? 1 : 0)));

    uart_write_bytes(SCS_UART_NUM, (const char *)buf, sizeof(buf));
}

/* -------------------------------------------------------------------------
 * PY32L020 Base IO Expander & WS2812C LED Driver
 * ------------------------------------------------------------------------- */

static esp_err_t py32_write_reg(uint8_t reg, const uint8_t *data, size_t len)
{
    if (!s_py32_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[68];
    if (len + 1 > sizeof(buf)) {
        return ESP_ERR_NO_MEM;
    }
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(s_py32_dev, buf, len + 1, 20);
}

static esp_err_t py32_write_reg8(uint8_t reg, uint8_t val)
{
    return py32_write_reg(reg, &val, 1);
}

static void py32_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PY32_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_py32_dev) != ESP_OK) {
        ESP_LOGW(TAG, "PY32 base IO expander add device failed");
        s_py32_dev = NULL;
        return;
    }

    /* Probe device with a quick write */
    uint8_t dummy = 0;
    if (i2c_master_transmit(s_py32_dev, &dummy, 1, 30) != ESP_OK) {
        ESP_LOGI(TAG, "PY32 base not responding at 0x%02X (standalone CoreS3 or custom wiring)", PY32_I2C_ADDR);
        i2c_master_bus_rm_device(s_py32_dev);
        s_py32_dev = NULL;
        return;
    }

    /* Configure Pin 0 (VM_EN - Servo motor power) as output, pull-up, High */
    py32_write_reg8(PY32_REG_GPIO_M_L, 0x01);  /* Pin 0 output */
    py32_write_reg8(PY32_REG_GPIO_PU_L, 0x01); /* Pull-up */
    py32_write_reg8(PY32_REG_GPIO_O_L, 0x01);  /* Set High -> enable servo power */

    /* Configure Pin 13 (RGB LED) as output, pull-up, push-pull */
    py32_write_reg8(PY32_REG_GPIO_M_H, 0x20);  /* Pin 13 (bit 5 of high reg) */
    py32_write_reg8(PY32_REG_GPIO_PU_H, 0x20);
    py32_write_reg8(PY32_REG_GPIO_DRV_H, 0x00);/* Push-pull */

    /* Set LED count to 12 */
    py32_write_reg8(PY32_REG_LED_CFG, PY32_LED_COUNT & 0x3F);

    ESP_LOGI(TAG, "PY32 base IO expander initialized (Servo power enabled, 12 LEDs ready)");
}

static void stackchan_set_leds_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_py32_dev) {
        return;
    }
    /* Convert RGB888 to RGB565 */
    uint16_t c565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    uint8_t ram[PY32_LED_COUNT * 2];
    for (int i = 0; i < PY32_LED_COUNT; i++) {
        ram[i * 2] = (uint8_t)(c565 & 0xFF);
        ram[i * 2 + 1] = (uint8_t)((c565 >> 8) & 0xFF);
    }
    py32_write_reg(PY32_REG_LED_RAM_START, ram, sizeof(ram));
    /* Trigger refresh: bit 6 of REG_LED_CFG */
    py32_write_reg8(PY32_REG_LED_CFG, (PY32_LED_COUNT & 0x3F) | (1 << 6));
}

/* -------------------------------------------------------------------------
 * Background Motion & Expressive State Animation Task
 * ------------------------------------------------------------------------- */

static int clamp_int(int val, int min_val, int max_val)
{
    return val < min_val ? min_val : (val > max_val ? max_val : val);
}

static void stackchan_motion_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Stack-Chan motion & LED task started");

    /* Center servos at startup */
    vTaskDelay(pdMS_TO_TICKS(300));
    scs_set_torque(SERVO_ID_YAW, true);
    scs_set_torque(SERVO_ID_PITCH, true);
    scs_write_pos(SERVO_ID_YAW, YAW_CENTER, 60, 0);
    scs_write_pos(SERVO_ID_PITCH, PITCH_CENTER, 60, 0);
    stackchan_set_leds_rgb(20, 20, 20);

    uint32_t tick = 0;
    muse_mode_t prev_mode = MUSE_MODE_BOOT;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(40)); /* 25 Hz loop */
        tick++;

        float secs_in_mode = 0.0f;
        muse_mode_t mode = muse_state_mode(&secs_in_mode);
        float level = muse_state_level();
        float t = (float)(esp_timer_get_time() % 31415926) / 1000000.0f;

        if (mode != prev_mode) {
            ESP_LOGD(TAG, "Mode changed: %d -> %d", prev_mode, mode);
            prev_mode = mode;
        }

        int target_yaw = YAW_CENTER;
        int target_pitch = PITCH_CENTER;

        switch (mode) {
        case MUSE_MODE_BOOT: {
            target_yaw = YAW_CENTER;
            target_pitch = PITCH_CENTER;
            float pulse = 0.5f + 0.5f * sinf(t * 4.0f);
            stackchan_set_leds_rgb((uint8_t)(100 * pulse), (uint8_t)(60 * pulse), (uint8_t)(10 * pulse));
            break;
        }

        case MUSE_MODE_IDLE: {
            /* Gentle relaxed breathing motion */
            float breath = sinf(t * 1.5f);
            float sway = sinf(t * 0.5f);
            target_pitch = PITCH_CENTER + (int)(breath * 10.0f);
            target_yaw = YAW_CENTER + (int)(sway * 18.0f);

            /* Subtle low-power dim LED breathing */
            float led_p = 0.2f + 0.15f * (1.0f + sinf(t * 1.5f));
            stackchan_set_leds_rgb((uint8_t)(30 * led_p), (uint8_t)(35 * led_p), (uint8_t)(45 * led_p));
            break;
        }

        case MUSE_MODE_LISTENING: {
            /* Attentive posture: tilt head slightly forward/up towards user */
            float attentive_nod = sinf(t * 2.5f) * 6.0f;
            target_pitch = PITCH_CENTER - 35 + (int)attentive_nod;
            target_yaw = YAW_CENTER;

            /* Soft glowing blue LEDs indicating active listening */
            float pulse = 0.6f + 0.4f * sinf(t * 3.5f);
            stackchan_set_leds_rgb(0, (uint8_t)(60 * pulse), (uint8_t)(220 * pulse));
            break;
        }

        case MUSE_MODE_THINKING: {
            /* Curious tilted posture while reasoning / thinking */
            float tilt = sinf(t * 2.8f) * 25.0f;
            float nod = cosf(t * 2.8f) * 15.0f;
            target_yaw = YAW_CENTER + (int)tilt;
            target_pitch = PITCH_CENTER - 15 + (int)nod;

            /* Soft purple / magenta shimmer */
            float pulse = 0.5f + 0.5f * sinf(t * 4.0f);
            stackchan_set_leds_rgb((uint8_t)(180 * pulse), (uint8_t)(30 * pulse), (uint8_t)(190 * pulse));
            break;
        }

        case MUSE_MODE_SPEAKING: {
            /* Expressive conversational micro-movements synced with voice level */
            float nod = sinf(t * 7.5f) * (14.0f + 35.0f * level);
            float tilt = sinf(t * 3.2f) * (12.0f + 28.0f * level);
            target_pitch = PITCH_CENTER + (int)nod;
            target_yaw = YAW_CENTER + (int)tilt;

            /* Cheerful friendly turquoise LEDs reacting to speech level */
            uint8_t brightness = (uint8_t)(80 + 175.0f * level);
            stackchan_set_leds_rgb(0, brightness, (uint8_t)(brightness * 0.75f));
            break;
        }

        case MUSE_MODE_OFF: {
            target_yaw = YAW_CENTER;
            target_pitch = PITCH_CENTER;
            stackchan_set_leds_rgb(0, 0, 0);
            scs_set_torque(SERVO_ID_YAW, false);
            scs_set_torque(SERVO_ID_PITCH, false);
            break;
        }

        default:
            target_yaw = YAW_CENTER;
            target_pitch = PITCH_CENTER;
            stackchan_set_leds_rgb(20, 20, 20);
            break;
        }

        /* Apply safety clamps */
        target_yaw = clamp_int(target_yaw, YAW_MIN, YAW_MAX);
        target_pitch = clamp_int(target_pitch, PITCH_MIN, PITCH_MAX);

        /* Update servo positions smoothly every 2 ticks (50 Hz / 40 ms) */
        if (mode != MUSE_MODE_OFF && (tick % 2 == 0)) {
            scs_write_pos(SERVO_ID_YAW, (uint16_t)target_yaw, 35, 0);
            scs_write_pos(SERVO_ID_PITCH, (uint16_t)target_pitch, 35, 0);
        }
    }
}

/* -------------------------------------------------------------------------
 * Muse Board Interface Implementation
 * ------------------------------------------------------------------------- */

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");

    /* Enable CoreS3 5V base rail (AW9523 USB_EN / 5V boost) so the Stack-Chan base powers up */
    esp_err_t err = bsp_feature_enable(BSP_FEATURE_USB, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BSP_FEATURE_USB enable failed: %s", esp_err_to_name(err));
    }

    /* Initialize AXP2101 PMU for power sensing and power key */
    err = muse_pmu_init(bsp_i2c_get_handle(), true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMU unavailable (%s): no talk button or battery status", esp_err_to_name(err));
    } else {
        s_pmu = true;
        (void)muse_pmu_set_key_shutdown(true);
    }

    /* Give the base hardware 150 ms to stabilize power rail */
    vTaskDelay(pdMS_TO_TICKS(150));

    /* Initialize PY32L020 Base IO Expander & WS2812C LEDs */
    py32_init(bsp_i2c_get_handle());

    /* Initialize Servos UART (1 Mbps) */
    scs_uart_init();

    /* Spawn background motion & LED task */
    BaseType_t res = xTaskCreateWithCaps(stackchan_motion_task, "stk_act", 4096, NULL, 2, &s_motion_task, MUSE_BIG_CAPS);
    if (res != pdPASS) {
        res = xTaskCreate(stackchan_motion_task, "stk_act", 4096, NULL, 2, &s_motion_task);
        if (res != pdPASS) {
            ESP_LOGW(TAG, "Failed to create Stack-Chan motion task");
        }
    }

    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = true,
        .flags = { .buff_dma = true, .buff_spiram = false },
    };
    cfg.lvgl_port_cfg.task_affinity = MUSE_UI_CORE;
    cfg.lvgl_port_cfg.task_priority = MUSE_UI_PRIORITY;
    cfg.lvgl_port_cfg.task_stack = 10240;
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    if (!disp) {
        return NULL;
    }
    *touch = bsp_display_get_input_dev();
    return *touch ? disp : NULL;
}

static bool display_lock(int timeout_ms)
{
    return bsp_display_lock(timeout_ms < 0 ? 0 : (uint32_t)timeout_ms);
}

static void set_brightness(int pct)
{
    bsp_display_brightness_set(pct);
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    const i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    ESP_RETURN_ON_ERROR(bsp_audio_init(&cfg), TAG, "duplex audio init");
    *spk = s_spk = bsp_audio_codec_speaker_init();
    *mic = s_mic = bsp_audio_codec_microphone_init();
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    db = (db / 3) * 3;
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

static unsigned poll_buttons(void)
{
    static unsigned tick;
    if (!s_pmu || tick++ % PMU_KEY_EVERY) {
        return 0;
    }
    unsigned key = muse_pmu_poll_key();
    bool press = key & MUSE_PMU_KEY_PRESS, release = key & MUSE_PMU_KEY_RELEASE;
    if (press != release) {
        (void)muse_pmu_set_key_shutdown(release);
    }
    return (press ? MUSE_BTN_TALK_PRESS : 0) | (release ? MUSE_BTN_TALK_RELEASE : 0);
}

static esp_err_t read_power(muse_power_t *out)
{
    return s_pmu ? muse_pmu_read_power(out) : ESP_ERR_INVALID_STATE;
}

static esp_err_t power_off(void)
{
    ESP_RETURN_ON_FALSE(s_pmu, ESP_ERR_INVALID_STATE, TAG, "no PMU");
    ESP_RETURN_ON_ERROR(bsp_display_backlight_off(), TAG, "backlight off");
    bsp_display_lock(0);
    esp_err_t err = lvgl_port_stop();
    bsp_display_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "display stop");
    if (s_spk) {
        esp_codec_dev_close(s_spk);
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
    }
    scs_set_torque(SERVO_ID_YAW, false);
    scs_set_torque(SERVO_ID_PITCH, false);
    stackchan_set_leds_rgb(0, 0, 0);
    return muse_pmu_power_off();
}

static const muse_board_t s_board = {
    .name = "M5Stack Stack-Chan",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 2.0f,
    .talk_button = "power",
    .talk_hint = { LV_ALIGN_TOP_LEFT, 8, 8 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = bsp_display_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = -1,
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
