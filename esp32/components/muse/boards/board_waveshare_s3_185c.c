/*
 * Copyright (c) 2026 ledienbien-ai
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
 * Waveshare ESP32-S3-Touch-LCD-1.85C: ESP32-S3R8 (16 MB flash, 8 MB octal
 * PSRAM), round 360 px ST77916 LCD on QSPI with a PWM backlight, CST816
 * touch, a TCA9554 expander holding the panel's and touch's reset lines, and
 * the battery on a 1:3 divider. BOOT (GPIO0) is the only button the ESP32
 * sees: RESET resets it and the power switch is a slide switch on the battery.
 *
 * The board comes in two audio versions on the same pins, told apart at boot
 * by whether an ES8311 answers on I2C:
 *   V1: PCM5101 DAC (no control bus) on one I2S port, a digital I2S mic on
 *       another (GPIO2 and 15 are its WS and SCK).
 *   V2 ("Rev2.0" on the PCB): ES8311 DAC and ES7210 ADC with two analog mics
 *       on one duplex I2S port (GPIO2 is MCLK, GPIO15 enables the amplifier).
 *
 * Pins are from Waveshare's wiki (docs.waveshare.com/ESP32-S3-Touch-LCD-1.85C),
 * its ESP-IDF example (github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85C, V2)
 * and xiaozhi-esp32's waveshare/esp32-s3-touch-lcd-1.85c board (both versions).
 * CREDITS.md at the repository root lists these sources and their licenses.
 */
#include <math.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "waveshare_s3_185c_lcd_init.h"

static const char *TAG = "board";

#define LCD_RES 360
#define LCD_HOST SPI2_HOST
#define LCD_PCLK GPIO_NUM_40
#define LCD_D0 GPIO_NUM_46
#define LCD_D1 GPIO_NUM_45
#define LCD_D2 GPIO_NUM_42
#define LCD_D3 GPIO_NUM_41
#define LCD_CS GPIO_NUM_21
#define LCD_BL GPIO_NUM_5
#define LCD_ID_HZ (3 * 1000 * 1000)     /* Waveshare reads the ID register this slowly */
#define LCD_HZ (40 * 1000 * 1000)
#define DRAW_BUF_LINES 90       /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

#define I2C_SDA GPIO_NUM_11
#define I2C_SCL GPIO_NUM_10
#define TP_INT GPIO_NUM_4       /* pulses low with each touch report */
#define TP_ADDR 0x15
#define TP_REG_POINTS 0x02      /* finger count, then X and Y, high byte first */
#define TP_REG_SLEEP 0xE5       /* 0x03: deep sleep until the reset line pulses */
#define TP_REG_AUTO_SLEEP 0xFE  /* 1: stay awake between touches */

/* TCA9554 at 0x20. Waveshare numbers its pins EXIO1 to EXIO8. */
#define EXP_ADDR 0x20
#define EXP_REG_OUTPUT 0x01
#define EXP_REG_CONFIG 0x03     /* 1 = input */
#define EXP_TP_RST BIT(0)       /* EXIO1, active low */
#define EXP_LCD_RST BIT(1)      /* EXIO2, active low */

/* The speaker's I2S lines are the same on both versions. */
#define I2S_BCLK GPIO_NUM_48
#define I2S_WS GPIO_NUM_38
#define I2S_DOUT GPIO_NUM_47
#define I2S_DIN GPIO_NUM_39     /* V1: the mic's data; V2: the ES7210's */
#define V1_MIC_WS GPIO_NUM_2
#define V1_MIC_SCK GPIO_NUM_15
#define V2_MCLK GPIO_NUM_2
#define V2_PA_EN GPIO_NUM_15
#define V1_MIC_SLOT 1           /* the mic answers on the right slot (xiaozhi) */
#define V1_BLOCK 128            /* frames converted per I2S read or write */
#define MIC_GAIN_OFFSET_DB 6    /* Muse's default 30 dB lands on xiaozhi's x16 */

#define BOOT_GPIO GPIO_NUM_0
#define BATT_ADC ADC_CHANNEL_7  /* GPIO8, behind a 1:3 divider */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_exp;
static i2c_master_dev_handle_t s_tp_dev;
static uint8_t s_exp_out = EXP_TP_RST | EXP_LCD_RST;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_boot;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_v2;                   /* ES8311 and ES7210 rather than PCM5101 and an I2S mic */
static i2s_chan_handle_t s_v1_tx, s_v1_rx;
static bool s_v1_tx_on, s_v1_rx_on;
static int s_v1_mic_gain_q8 = 256;
static volatile bool s_tp_irq;      /* the touch controller has a report */
static bool s_tp_down;

static esp_err_t exp_write(uint8_t reg, uint8_t v)
{
    const uint8_t buf[] = { reg, v };
    return i2c_master_transmit(s_exp, buf, sizeof(buf), 50);
}

/* Drives one of the expander's reset lines; they're active low. */
static esp_err_t exp_reset_line(uint8_t lines, bool in_reset)
{
    s_exp_out = in_reset ? s_exp_out & ~lines : s_exp_out | lines;
    return exp_write(EXP_REG_OUTPUT, s_exp_out);
}

static esp_err_t tp_write(uint8_t reg, uint8_t v)
{
    const uint8_t buf[] = { reg, v };
    return i2c_master_transmit(s_tp_dev, buf, sizeof(buf), 20);
}

static esp_err_t init(void)
{
    /* power_off() holds the backlight off through deep sleep. */
    rtc_gpio_hold_dis(LCD_BL);
    rtc_gpio_deinit(LCD_BL);

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    const i2c_device_config_t exp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXP_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &exp_cfg, &s_exp), TAG, "expander");
    const i2c_device_config_t tp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &tp_cfg, &s_tp_dev), TAG, "touch");

    /* Only the two reset lines become outputs, as on xiaozhi: EXIO3 is the SD
     * card's chip select and EXIO4 the RTC's interrupt, and Muse uses neither. */
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_OUTPUT, s_exp_out), TAG, "expander outputs");
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_CONFIG, (uint8_t)~(EXP_TP_RST | EXP_LCD_RST)), TAG, "expander config");

    s_v2 = i2c_master_probe(s_i2c, ES8311_CODEC_DEFAULT_ADDR >> 1, 50) == ESP_OK;
    ESP_LOGI(TAG, "audio: %s", s_v2 ? "V2 (ES8311 + ES7210)" : "V1 (PCM5101 + I2S mic)");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_GPIO), TAG, "boot button");

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg), TAG, "adc channel");
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BATT_ADC,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration: battery level disabled");
    }
    return ESP_OK;
}

static void IRAM_ATTR on_touch_line(void *arg)
{
    (void)arg;
    s_tp_irq = true;
}

/*
 * The CST816 is read only once it has pulsed its interrupt line, and from then
 * until it reports no finger: idle, it may be asleep and not answer on I2C.
 * LVGL's task calls this each time it reads the pointer.
 */
static esp_err_t tp_read(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t *points, uint8_t *count,
                         uint8_t max_count, void *ctx)
{
    (void)tp;
    (void)max_count;
    (void)ctx;
    *count = 0;
    if (!s_tp_irq && !s_tp_down) {
        return ESP_OK;
    }
    s_tp_irq = false;
    const uint8_t reg = TP_REG_POINTS;
    uint8_t d[5];
    if (i2c_master_transmit_receive(s_tp_dev, &reg, 1, d, sizeof(d), 20) != ESP_OK || !(d[0] & 0x0F)) {
        s_tp_down = false;
        return ESP_OK;
    }
    int x = (d[1] & 0x0F) << 8 | d[2];
    int y = (d[3] & 0x0F) << 8 | d[4];
    points[0] = (esp_lcd_touch_point_data_t){
        .x = x < LCD_RES ? x : LCD_RES - 1,
        .y = y < LCD_RES ? y : LCD_RES - 1,
    };
    *count = 1;
    s_tp_down = true;
    return ESP_OK;
}

/* The reset line wakes the CST816 from deep sleep too. Auto-sleep goes off, as
 * in Waveshare's example; a controller that lacks the register ignores it. */
static void touch_reset(void)
{
    exp_reset_line(EXP_TP_RST, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    exp_reset_line(EXP_TP_RST, false);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (tp_write(TP_REG_AUTO_SLEEP, 1) != ESP_OK) {
        ESP_LOGW(TAG, "touch controller not answering");
    }
    s_tp_irq = false;
    s_tp_down = false;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&bl_timer) != ESP_OK || ledc_channel_config(&bl_ch) != ESP_OK) {
        return NULL;
    }

    /* The panel's reset is on the expander, so the driver gets no reset pin. */
    exp_reset_line(EXP_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    exp_reset_line(EXP_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(120));

    const spi_bus_config_t bus =
        ST77916_PANEL_BUS_QSPI_CONFIG(LCD_PCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    /* Waveshare ships two panels and tells them apart by the ID register,
     * read slowly over an IO of its own (QSPI read: opcode 0Bh). */
    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = LCD_ID_HZ;
    uint8_t id[4] = { 0 };
    esp_lcd_panel_io_handle_t id_io;
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &id_io) != ESP_OK) {
        return NULL;
    }
    esp_err_t id_err = esp_lcd_panel_io_rx_param(id_io, (0x0B << 24) | (0x04 << 8), id, sizeof(id));
    esp_lcd_panel_io_del(id_io);
    bool newer = id_err == ESP_OK && id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F;
    ESP_LOGI(TAG, "panel id %02x %02x %02x %02x: %s setup", id[0], id[1], id[2], id[3],
             newer ? "Waveshare's" : "the driver's");

    io_cfg.pclk_hz = LCD_HZ;
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    st77916_vendor_config_t vendor_cfg = { .flags.use_qspi_interface = 1 };
    if (newer) {
        vendor_cfg.init_cmds = waveshare_185c_st77916_init;
        vendor_cfg.init_cmds_size = sizeof(waveshare_185c_st77916_init) / sizeof(waveshare_185c_st77916_init[0]);
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);

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

    touch_reset();
    const gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << TP_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    /* muse_gpio_button_init() installed the GPIO interrupt service. */
    if (gpio_config(&int_cfg) != ESP_OK || gpio_isr_handler_add(TP_INT, on_touch_line, NULL) != ESP_OK) {
        return NULL;
    }
    /* The adapter wants a touch handle but, given a read of our own, only looks
     * at its interrupt pin: none, so it asks tp_read() on every pointer read. */
    static esp_lcd_touch_t tp = {
        .config = {
            .x_max = LCD_RES,
            .y_max = LCD_RES,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
        },
    };
    esp_lv_adapter_touch_config_t lv_tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, &tp);
    lv_tp_cfg.callbacks.custom_touch_read = tp_read;
    *touch = esp_lv_adapter_register_touch(&lv_tp_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void send_sleep(void *sleep)
{
    /* SLPIN/SLPOUT as a QSPI command write (opcode 02h), as the driver sends its own. */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | ((*(bool *)sleep ? 0x10 : 0x11) << 8), NULL, 0);
}

static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/* Screen off: LVGL stops and the CST816 goes into deep sleep, which only its
 * reset line ends. */
static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
        tp_write(TP_REG_SLEEP, 0x03);
    } else {
        touch_reset();
        esp_lv_adapter_resume();
    }
}

/* ---------- V1: PCM5101 and an I2S mic, no codec to talk to ---------- */

static int v1_enable(i2s_chan_handle_t ch, bool *is_on, bool on)
{
    if (on != *is_on) {
        if ((on ? i2s_channel_enable(ch) : i2s_channel_disable(ch)) != ESP_OK) {
            return ESP_CODEC_DEV_DRV_ERR;
        }
        *is_on = on;
    }
    return ESP_CODEC_DEV_OK;
}

static int v1_spk_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    (void)type;
    return v1_enable(s_v1_tx, &s_v1_tx_on, on);
}

static int v1_mic_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    (void)type;
    return v1_enable(s_v1_rx, &s_v1_rx_on, on);
}

/* muse_audio's 16-bit pairs, widened to the 32-bit slots the PCM5101 is
 * clocked with. esp_codec_dev has already applied the volume in software. */
static int v1_spk_write(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    static int32_t wide[V1_BLOCK * 2];
    const int16_t *in = (const int16_t *)data;
    for (int samples = size / 2; samples > 0;) {
        int n = samples > V1_BLOCK * 2 ? V1_BLOCK * 2 : samples;
        for (int i = 0; i < n; i++) {
            wide[i] = (int32_t)in[i] * 65536;
        }
        size_t wrote;
        if (i2s_channel_write(s_v1_tx, wide, n * sizeof(wide[0]), &wrote, portMAX_DELAY) != ESP_OK) {
            return ESP_CODEC_DEV_WRITE_FAIL;
        }
        in += n;
        samples -= n;
    }
    return ESP_CODEC_DEV_OK;
}

/* The mic's 24 bits arrive at the top of one 32-bit slot. Both of muse_audio's
 * slots get that sample, with the gain applied here. */
static int v1_mic_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    static int32_t wide[V1_BLOCK * 2];
    int16_t *out = (int16_t *)data;
    for (int frames = size / 4; frames > 0;) {
        int n = frames > V1_BLOCK ? V1_BLOCK : frames;
        size_t want = n * 2 * sizeof(wide[0]), got;
        if (i2s_channel_read(s_v1_rx, wide, want, &got, pdMS_TO_TICKS(1000)) != ESP_OK || got != want) {
            return ESP_CODEC_DEV_READ_FAIL;
        }
        for (int i = 0; i < n; i++) {
            int64_t v = (int64_t)wide[2 * i + V1_MIC_SLOT] * s_v1_mic_gain_q8 >> 24;
            int16_t s = v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : (int16_t)v;
            out[2 * i] = out[2 * i + 1] = s;
        }
        out += 2 * n;
        frames -= n;
    }
    return ESP_CODEC_DEV_OK;
}

static esp_err_t v1_audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* 32-bit slots, as xiaozhi runs both: the PCM5101 locks its PLL to a bit
     * clock of 64 per frame, and the mic needs all 64 to send its 24 bits. */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    i2s_chan_config_t spk_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    spk_chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&spk_chan, &s_v1_tx, NULL), TAG, "speaker channel");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_v1_tx, &std_cfg), TAG, "speaker i2s");

    std_cfg.gpio_cfg.bclk = V1_MIC_SCK;
    std_cfg.gpio_cfg.ws = V1_MIC_WS;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = I2S_DIN;
    i2s_chan_config_t mic_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&mic_chan, NULL, &s_v1_rx), TAG, "mic channel");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_v1_rx, &std_cfg), TAG, "mic i2s");

    static const audio_codec_data_if_t spk_if = { .enable = v1_spk_enable, .write = v1_spk_write };
    static const audio_codec_data_if_t mic_if = { .enable = v1_mic_enable, .read = v1_mic_read };
    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .data_if = &spk_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .data_if = &mic_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

/* ---------- V2: ES8311 out, ES7210 in, one duplex bus ---------- */

static esp_err_t v2_audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = V2_MCLK,
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
    audio_codec_i2c_cfg_t dac_i2c = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    audio_codec_i2c_cfg_t adc_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    /* As Waveshare's BSP sets up the same pair on its AMOLED 1.75C. */
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = V2_PA_EN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    /* The two mics are inputs 1 and 2, which a 2-slot bus carries. */
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = adc_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *adc = es7210_codec_new(&es7210_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    return s_v2 ? v2_audio_init(spk, mic) : v1_audio_init(spk, mic);
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    if (s_v2) {
        /* ES7210 PGA steps are 3 dB; snap so the UI shows what's applied.
         * esp_codec_dev rounds 33 dB down to 30; the next real step up is 34.5. */
        db = (db / 3) * 3;
        esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
    } else {
        s_v1_mic_gain_q8 = (int)(256.0f * powf(10.0f, (db - MIC_GAIN_OFFSET_DB) / 20.0f));
    }
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_boot);
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_boot }, 1, timeout_ms);
}

/*
 * Battery voltage through the 1:3 divider on GPIO8, with Waveshare's
 * correction (BAT_Driver.c). The charger's status reaches no pin, so the board
 * counts as charging while a USB host is attached and the battery isn't full;
 * a plain USB charger looks like running on battery.
 */
static esp_err_t read_power(muse_power_t *out)
{
    out->usb = usb_serial_jtag_is_connected();
    out->charging = false;
    out->battery_pct = -1;
    out->battery_mv = 0;
    ESP_RETURN_ON_FALSE(s_cali, ESP_ERR_INVALID_STATE, TAG, "no ADC calibration");
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &raw), TAG, "adc read");
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, raw, &mv), TAG, "adc cali");
        sum += mv;
    }
    int v = sum / 8 * 30000 / 9945;
    if (v < 2500) {
        return ESP_OK;   /* no battery on the connector */
    }
    out->battery_mv = v;
    /* The Watcher's LiPo curve, as on the M5Stack boards. */
    int pct = (-v * v + 9016 * v - 19189000) / 10000;
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    out->charging = out->usb && out->battery_pct < 100;
    return ESP_OK;
}

static void panel_off(void *arg)
{
    (void)arg;
    esp_lcd_panel_disp_on_off(s_panel, false);
}

/*
 * Nothing here can cut the power: that's the slide switch. The screen and
 * touch go off and the chip deep-sleeps until BOOT is pressed, which restarts
 * it. The backlight pin is held low, or it would float while asleep.
 */
static esp_err_t power_off(void)
{
    set_brightness(0);
    muse_lcd_bands_run(panel_off, NULL);
    tp_write(TP_REG_SLEEP, 0x03);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    rtc_gpio_init(LCD_BL);
    rtc_gpio_set_direction(LCD_BL, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(LCD_BL, 0);
    rtc_gpio_hold_en(LCD_BL);
    while (gpio_get_level(BOOT_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(BOOT_GPIO, 0), TAG, "boot button wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-1.85C",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.85f,
    .talk_button = "boot",
    .aux_button = "boot",
    /* BOOT is the only button, so there's no aux_hint: the screen sleeps on its
     * timer and Settings has the power page. The mic icon mirrors the speaker
     * button, under the state word on the right: at 360 px the Watcher's spot
     * beside the state word runs into the longer words. */
    .talk_hint = { LV_ALIGN_CENTER, 99, -84 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = -1,             /* V2: two mics, mixed; V1 puts its one mic on both slots */
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
