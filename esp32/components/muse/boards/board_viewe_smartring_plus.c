/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2026 SHENZHEN VIEWE TECHNOLOGY CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * VIEWE SmartRing-Plus: ESP32-S3-N16R8, 360×360 round ST77916 QSPI LCD with
 * CST816 touch, ES8311 codec (speaker through NS4150B amp, one analog mic) and
 * V1 (GPIO1 ADC) or V2 (AXP2101) battery gauge.
 *
 * BOOT (GPIO0) is inside the case, so the mic icon is an on-screen talk button
 * (touch_talk): hold to talk and let go to send. A tap on the screen confirms
 * Muse app pairing. BOOT still talks if reachable.
 *
 * Pins, the panel's init table, touch reset and the battery handling follow
 * VIEWE's smartring_plus BSP 1.1.2 (board_config.h, lcd_init_seq.h B panel,
 * bsp_battery.c, bsp_axp2101.c), driven directly here without the BSP.
 *
 * Draw buffers: two 10-line internal-RAM buffers, double-buffered, as on the
 * EchoEar-2ST with the same panel. PSRAM buffers would need a fresh 28.8 KB
 * internal DMA bounce buffer per 40-line flush, which may not be available
 * once Wi-Fi, BLE and the Muse session are up.
 *
 * No display_pause: pausing LVGL would stop touch, the only reachable way to
 * wake the screen. No power_off: the GPIO47 soft power-off isn't checked with
 * this port; the switch turns it off.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"
#include "muse_pmu.h"
#include "viewe_smartring_display_init.h"
#include "viewe_smartring_power.h"

static const char *TAG = "smartring";

/* Display (ST77916 QSPI, no DC pin) */
#define LCD_HOST SPI2_HOST
#define LCD_RES 360
#define DRAW_BUF_LINES 10
#define LCD_SCLK 10
#define LCD_D0 12
#define LCD_D1 13
#define LCD_D2 15
#define LCD_D3 14
#define LCD_CS 11
#define LCD_RST 39
#define LCD_BL 46

/* Shared I2C: CST816 touch (0x15), ES8311 (0x18), AXP2101 on V2 (0x34) */
#define I2C_PORT I2C_NUM_0
#define I2C_SDA 8
#define I2C_SCL 9
#define I2C_FREQ_HZ 400000
#define TOUCH_RST 40
#define TOUCH_INT 41

/* Audio */
#define I2S_PORT I2S_NUM_0
#define I2S_MCLK 48
#define I2S_BCLK 21
#define I2S_WS 17
#define I2S_DOUT 16
#define I2S_DIN 18
#define PA_EN 45 /* NS4150B enable, active high */

/* V1 battery: GPIO1 = ADC1 channel 0 */
#define BAT_ADC_UNIT ADC_UNIT_1
#define BAT_ADC_CHAN ADC_CHANNEL_0

#define TALK_GPIO GPIO_NUM_0 /* BOOT, inside the case */

static esp_lcd_panel_handle_t s_panel;
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_axp;    /* V2 charger setup */
static bool s_pmu;                       /* V2: the shared AXP2101 reader */
static adc_oneshot_unit_handle_t s_adc;  /* V1 only */
static adc_cali_handle_t s_cali;
static smartring_v1_t s_v1;
static muse_gpio_button_t s_talk;

static esp_err_t axp_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_axp, &reg, 1, val, 1, 100);
}

static esp_err_t axp_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_axp, buf, sizeof(buf), 100);
}

static esp_err_t axp_set_bits(uint8_t reg, uint8_t bits)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(axp_read(reg, &v), TAG, "axp read %02x", reg);
    return (v & bits) == bits ? ESP_OK : axp_write(reg, v | bits);
}

/* V2: the peripheral rail, then the charger for the 600 mAh cell and the gauge,
 * as VIEWE's BSP does. */
static esp_err_t axp_setup(void)
{
    ESP_RETURN_ON_ERROR(axp_write(0x94, SMARTRING_AXP_ALDO3_CODE), TAG, "ALDO3 voltage");
    ESP_RETURN_ON_ERROR(axp_set_bits(0x90, 0x04), TAG, "ALDO3 on");
    ESP_RETURN_ON_ERROR(axp_set_bits(0x18, 0x0A), TAG, "charger and gauge");
    ESP_RETURN_ON_ERROR(axp_set_bits(0x68, 0x01), TAG, "battery detect");
    ESP_RETURN_ON_ERROR(axp_set_bits(0x30, 0x01), TAG, "VBAT ADC");
    ESP_RETURN_ON_ERROR(axp_write(0x64, SMARTRING_AXP_CV_CODE), TAG, "CV");
    ESP_RETURN_ON_ERROR(axp_write(0x61, SMARTRING_AXP_IPRE_CODE), TAG, "precharge");
    ESP_RETURN_ON_ERROR(axp_write(0x62, SMARTRING_AXP_ICC_CODE), TAG, "charge current");
    return axp_write(0x63, SMARTRING_AXP_ITERM_CODE);
}

static bool axp_detect(void)
{
    if (i2c_master_probe(s_i2c, SMARTRING_AXP_ADDR, 100) != ESP_OK) {
        return false;
    }
    const i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                     .device_address = SMARTRING_AXP_ADDR,
                                     .scl_speed_hz = I2C_FREQ_HZ};
    if (i2c_master_bus_add_device(s_i2c, &dev, &s_axp) != ESP_OK) {
        return false;
    }
    uint8_t id = 0;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (axp_read(0x03, &id) == ESP_OK && id == SMARTRING_AXP_CHIP_ID) {
        err = axp_setup();
    }
    i2c_master_bus_rm_device(s_axp);
    s_axp = NULL;
    if (err == ESP_OK) {
        /* Gauge, voltage and charging through the shared reader. Its PWR key
         * hold-off change is moot: power-off here is GPIO47, not the AXP2101. */
        err = muse_pmu_init(s_i2c, false);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "V2: AXP2101 battery gauge");
        s_pmu = true;
    } else if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "AXP2101 setup failed (%s); trying the V1 gauge", esp_err_to_name(err));
    }
    return s_pmu;
}

static esp_err_t adc_setup(void)
{
    adc_oneshot_unit_handle_t adc;
    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = BAT_ADC_UNIT};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &adc), TAG, "ADC unit");
    const adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    esp_err_t err = adc_oneshot_config_channel(adc, BAT_ADC_CHAN, &chan);
    if (err != ESP_OK) {
        adc_oneshot_del_unit(adc);
        return err;
    }
    s_adc = adc; /* read_power uses it only once it's configured */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cali = {
        .unit_id = BAT_ADC_UNIT, .chan = BAT_ADC_CHAN, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_cali) != ESP_OK) {
        s_cali = NULL;
    }
#endif
    if (!s_cali) {
        ESP_LOGW(TAG, "ADC calibration unavailable; battery mV is approximate");
    }
    ESP_LOGI(TAG, "V1: GPIO1 battery gauge");
    return ESP_OK;
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t cfg = {.i2c_port = I2C_PORT,
                                         .scl_io_num = I2C_SCL,
                                         .sda_io_num = I2C_SDA,
                                         .clk_source = I2C_CLK_SRC_DEFAULT,
                                         .glitch_ignore_cnt = 7,
                                         .flags.enable_internal_pullup = true};
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c), TAG, "I2C bus");

    /* V2's ALDO3 rail comes up before the peripherals. A missing gauge only
     * costs battery status. */
    if (!axp_detect() && adc_setup() != ESP_OK) {
        ESP_LOGW(TAG, "battery gauge unavailable");
    }
    return muse_gpio_button_init(&s_talk, TALK_GPIO);
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    *touch = NULL;

    /* Backlight PWM, dark until Muse sets the brightness. */
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 4000,
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

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .data0_io_num = LCD_D0,
        .data1_io_num = LCD_D1,
        .data2_io_num = LCD_D2,
        .data3_io_num = LCD_D3,
        .max_transfer_sz = LCD_RES * DRAW_BUF_LINES * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }

    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io) != ESP_OK) {
        return NULL;
    }

    /* The component's default init table doesn't suit this panel (vertical
     * stripes); VIEWE's B-panel table ends with INVON and MADCTL 0, and the
     * BSP adds nothing after it. */
    st77916_vendor_config_t vendor = {
        .init_cmds = smartring_init_cmds,
        .init_cmds_size = sizeof(smartring_init_cmds) / sizeof(smartring_init_cmds[0]),
        .flags = {.use_qspi_interface = true}};
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    if (esp_lcd_new_panel_st77916(io, &panel_cfg, &s_panel) != ESP_OK ||
        esp_lcd_panel_reset(s_panel) != ESP_OK || esp_lcd_panel_init(s_panel) != ESP_OK ||
        esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) {
        return NULL;
    }

    /* The touch driver pulses the CST816's reset line, so it answers after a
     * soft restart too. */
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io_cfg.scl_speed_hz = I2C_FREQ_HZ;
    esp_lcd_touch_handle_t tp;
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_RES,
        .y_max = LCD_RES,
        .rst_gpio_num = TOUCH_RST,
        .int_gpio_num = TOUCH_INT,
        .levels = {.reset = 0, .interrupt = 0},
    };
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK ||
        esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp) != ESP_OK) {
        ESP_LOGE(TAG, "CST816 touch");
        return NULL;
    }

    /* This runs on Muse's boot task, pinned to MUSE_UI_CORE, so the panel's
     * SPI interrupt lands there; keep LVGL's task on the same core. */
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(s_panel, io, LCD_RES, LCD_RES, ESP_LV_ADAPTER_ROTATE_0);
    disp_cfg.profile.buffer_height = DRAW_BUF_LINES;
    disp_cfg.profile.use_psram = false;
    disp_cfg.profile.require_double_buffer = true;
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) {
        return NULL;
    }
    const esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    *touch = esp_lv_adapter_register_touch(&touch_cfg);
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
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)pct * 255 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk);
}

/* ES8311 at 0x18, duplex I2S at 16 kHz (MUSE_AUDIO_RATE), MCLK = 256 x Fs.
 * Not the BSP's bsp_audio_init(): it opens the codec itself and leaves the
 * speaker muted, while Muse wants unopened handles. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &tx, &rx), TAG, "I2S channels");
    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_MCLK, .bclk = I2S_BCLK, .ws = I2S_WS, .dout = I2S_DOUT, .din = I2S_DIN},
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std), TAG, "I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std), TAG, "I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "I2S TX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "I2S RX enable");

    audio_codec_i2s_cfg_t data_cfg = {.port = I2S_PORT, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&data_cfg);
    audio_codec_i2c_cfg_t ctrl_cfg = {.port = I2C_PORT, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c};
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data && ctrl && gpio, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {.ctrl_if = ctrl,
                                 .gpio_if = gpio,
                                 .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
                                 .pa_pin = PA_EN,
                                 .use_mclk = true,
                                 .hw_gain = {.pa_voltage = 3.3, .codec_dac_voltage = 3.3}};
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");

    esp_codec_dev_cfg_t out_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data};
    esp_codec_dev_cfg_t in_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data};
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static esp_err_t read_power(muse_power_t *out)
{
    if (s_pmu) {
        return muse_pmu_read_power(out);
    }
    if (!s_adc) {
        return ESP_ERR_INVALID_STATE;
    }
    int sum = 0;
    for (int i = 0; i < 16; i++) {
        int raw;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BAT_ADC_CHAN, &raw), TAG, "ADC read");
        sum += raw;
    }
    int raw = sum / 16, mv = 0;
    if (!s_cali || adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
        mv = raw * 3100 / 4095; /* 12 dB attenuation, roughly 3.1 V full scale */
    }
    if (!smartring_v1_update(&s_v1, mv * SMARTRING_V1_DIVIDER, &out->battery_mv, &out->battery_pct,
                             &out->charging)) {
        return ESP_ERR_INVALID_STATE;
    }
    out->usb = out->charging; /* the rail only tells "USB in" from the battery by charging */
    return ESP_OK;
}

static const muse_board_t s_board = {
    .name = "VIEWE SmartRing-Plus",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .touch_talk = true,
    .diagonal_in = 1.85f,
    .talk_button = "screen microphone",
    /* The talk button's centre: under the caption, above the page dots. */
    .talk_hint = {LV_ALIGN_CENTER, 0, 132},
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = 0, /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    /* No .power_off: the switch controls power, so Settings leaves Power off out. */
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
