/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Waveshare ESP32-S3-Touch-LCD-1.85B: ESP32-S3R8 (16 MB flash, 8 MB octal
 * PSRAM), round 1.85" 360 px ST77916 LCD on QSPI with CST816S touch, ES8311
 * speaker codec (amp enable on GPIO9) and ES7210 dual mic. BOOT (GPIO0) is
 * the only readable user button. BQ27220 reports battery percent/voltage;
 * power-off is deep sleep until BOOT.
 *
 * Pins, the panel's two init sequences (waveshare_s3_185b_init.h) and the
 * codec setup are from Waveshare's Apache-2.0 BSP for the board. Reference:
 * https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/tree/7232af4c501c64d34215b73e1d8319376dfd9c19/examples/esp-idf/05_bq27220/components/esp32_s3_touch_lcd_1_85B
 * The panel tables originated in its earlier ESP-IDF-V5.5.3 BSP. This port
 * uses the registry ST77916/CST816S drivers and IDF's master I2C API directly.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "waveshare_s3_185b_init.h"
#include "waveshare_s3_185b_power.h"

static const char *TAG = "board";

#define LCD_RES 360
#define LCD_HOST SPI2_HOST
#define LCD_CS GPIO_NUM_21
#define LCD_SCLK GPIO_NUM_40
#define LCD_D0 GPIO_NUM_46
#define LCD_D1 GPIO_NUM_45
#define LCD_D2 GPIO_NUM_42
#define LCD_D3 GPIO_NUM_41
#define LCD_RST GPIO_NUM_3
#define LCD_BL GPIO_NUM_5
#define LCD_ID_HZ (3 * 1000 * 1000)    /* register reads need a slow clock */
#define LCD_HZ (80 * 1000 * 1000)
#define DRAW_BUF_LINES 90              /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

#define BL_LEDC_TIMER LEDC_TIMER_0
#define BL_LEDC_CH LEDC_CHANNEL_0

#define I2C_SDA GPIO_NUM_11
#define I2C_SCL GPIO_NUM_10
#define TP_RST GPIO_NUM_1
#define TP_INT GPIO_NUM_4
#define I2S_MCLK GPIO_NUM_2
#define I2S_BCLK GPIO_NUM_48
#define I2S_WS GPIO_NUM_38
#define I2S_DOUT GPIO_NUM_47
#define I2S_DIN GPIO_NUM_39
#define PA_GPIO GPIO_NUM_9
#define BOOT_GPIO GPIO_NUM_0

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_gauge;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_tp;
static esp_codec_dev_handle_t s_spk, s_mic;
static muse_gpio_button_t s_boot;

/* Match the ST77916 QSPI update windows used by the 1.85C driver. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static esp_err_t init(void)
{
    /* Release the amp and backlight pins held low by power_off(). */
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(PA_GPIO);
    gpio_hold_dis(LCD_BL);
    rtc_gpio_deinit(BOOT_GPIO);

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");

    /* The backlight's PWM, off until the UI sets a brightness. */
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CH,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_GPIO), TAG, "boot button");
    /* BOOT woke the board from power-off and may still be down; don't count it. */
    s_boot.pressed = gpio_get_level(BOOT_GPIO) == 0;
    return ESP_OK;
}

/* Waveshare's BSP picks the init sequence by the panel's register 0x04. */
static void pick_init(const uint8_t id[4], st77916_vendor_config_t *vendor)
{
    if (id[0] == 0x00 && id[1] == 0x7F && id[2] == 0x7F && id[3] == 0x7F) {
        vendor->init_cmds = s_lcd_init_v1;
        vendor->init_cmds_size = sizeof(s_lcd_init_v1) / sizeof(s_lcd_init_v1[0]);
    } else if (id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F) {
        vendor->init_cmds = s_lcd_init_v2;
        vendor->init_cmds_size = sizeof(s_lcd_init_v2) / sizeof(s_lcd_init_v2[0]);
    }
    ESP_LOGI(TAG, "panel id %02x %02x %02x %02x: %s", id[0], id[1], id[2], id[3],
             vendor->init_cmds == s_lcd_init_v1 ? "init v1" :
             vendor->init_cmds == s_lcd_init_v2 ? "init v2" : "unknown, driver's default init");
}

/* As on the Waveshare 1.75C, the bands go out through fixed internal buffers. */
static lv_display_t *display_start(lv_indev_t **touch)
{
    *touch = NULL;
    /* Reset the panel first so its ID reads back, as the BSP does. */
    const gpio_config_t rst = { .pin_bit_mask = BIT64(LCD_RST), .mode = GPIO_MODE_OUTPUT };
    if (gpio_config(&rst) != ESP_OK || gpio_set_level(LCD_RST, 0) != ESP_OK) {
        ESP_LOGE(TAG, "panel reset low");
        return NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    if (gpio_set_level(LCD_RST, 1) != ESP_OK) {
        ESP_LOGE(TAG, "panel reset high");
        return NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    const spi_bus_config_t bus =
        ST77916_PANEL_BUS_QSPI_CONFIG(LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = LCD_ID_HZ;
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    uint8_t id[4] = { 0 };
    if (esp_lcd_panel_io_rx_param(s_io, (0x0B << 24) | (0x04 << 8), id, sizeof(id)) != ESP_OK) {
        ESP_LOGW(TAG, "panel id unreadable");
    }
    if (esp_lcd_panel_io_del(s_io) != ESP_OK) {
        ESP_LOGE(TAG, "panel ID IO cleanup");
        return NULL;
    }
    s_io = NULL;
    io_cfg.pclk_hz = LCD_HZ;
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    st77916_vendor_config_t vendor_cfg = { .flags.use_qspi_interface = 1 };
    pick_init(id, &vendor_cfg);
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    if (esp_lcd_panel_reset(s_panel) != ESP_OK ||
        esp_lcd_panel_init(s_panel) != ESP_OK ||
        esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) {
        ESP_LOGE(TAG, "panel initialization");
        return NULL;
    }

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

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_RES,
        .y_max = LCD_RES,
        .rst_gpio_num = TP_RST,
        .int_gpio_num = TP_INT,
    };
    if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &s_tp) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_tp);
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
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CH, 1023 * pct / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CH);
}

static void send_sleep(void *sleep)
{
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | ((*(bool *)sleep ? 0x10 : 0x11) << 8), NULL, 0);
}

/* Plain SLPIN/SLPOUT over the QSPI command path, as on the Waveshare 1.75C. */
static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/* ES8311 plays and ES7210 records, over one duplex I2S bus clocked from MCLK. */
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
    audio_codec_i2c_cfg_t spk_i2c = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    audio_codec_i2c_cfg_t mic_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *spk_ctrl = audio_codec_new_i2c_ctrl(&spk_i2c);
    const audio_codec_ctrl_if_t *mic_ctrl = audio_codec_new_i2c_ctrl(&mic_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && spk_ctrl && mic_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = spk_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PA_GPIO,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *es8311 = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(es8311, ESP_FAIL, TAG, "ES8311 not responding");
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = mic_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *es7210 = es7210_codec_new(&es7210_cfg);
    ESP_RETURN_ON_FALSE(es7210, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es7210, .data_if = data_if };
    s_spk = *spk = esp_codec_dev_new(&out_cfg);
    s_mic = *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    /* ES7210 PGA steps are 3 dB; snap so the UI shows what's applied. */
    db = (db / 3) * 3;
    /* esp_codec_dev rounds 33 dB down to 30; the next real step up is 34.5. */
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_boot);
}

static esp_err_t read_power(muse_power_t *out)
{
    /* Native USB detects a host, not VBUS from a charge-only adapter. */
    *out = (muse_power_t){ .battery_pct = -1, .usb = usb_serial_jtag_is_connected() };
    esp_err_t err = ESP_OK;
    if (!s_gauge) {
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x55,
            .scl_speed_hz = 100000,
        };
        /* Optional gauge: a failed allocation/read must not prevent the UI
         * from starting. Retry on the next normal power poll. */
        err = i2c_master_bus_add_device(s_i2c, &cfg, &s_gauge);
    }
    if (err == ESP_OK) {
        err = waveshare_185b_read_power(s_gauge, out->usb, out);
    }
    static esp_err_t last_err;
    static bool logged;
    if (!logged || err != last_err) {
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "BQ27220: battery %d%%, %d mV, charging=%d", out->battery_pct,
                     out->battery_mv, out->charging);
        } else {
            ESP_LOGW(TAG, "BQ27220 reading unavailable: %s", esp_err_to_name(err));
        }
        last_err = err;
        logged = true;
    }
    /* Publish unknown on failure instead of leaving a stale percentage/USB
     * flag in shared state. The helper initializes every output field. */
    return ESP_OK;
}

static void panel_off(void *arg)
{
    (void)arg;
    esp_lcd_panel_disp_on_off(s_panel, false);
}

/* No verified software power latch: screen, amp and codecs off, then sleep until BOOT. */
static esp_err_t power_off(void)
{
    set_brightness(0);
    muse_lcd_bands_run(panel_off, NULL);
    if (s_spk) {
        esp_codec_dev_close(s_spk);
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
    }
    ledc_stop(LEDC_LOW_SPEED_MODE, BL_LEDC_CH, 0);
    gpio_set_level(PA_GPIO, 0);
    while (gpio_get_level(BOOT_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(BOOT_GPIO, 0), TAG, "wake button");
    rtc_gpio_pullup_en(BOOT_GPIO);
    rtc_gpio_pulldown_dis(BOOT_GPIO);
    gpio_hold_en(PA_GPIO);
    gpio_hold_en(LCD_BL);
    gpio_deep_sleep_hold_en();
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-1.85B",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.85f,
    .talk_button = "boot",
    .talk_hint = { LV_ALIGN_BOTTOM_MID, 0, -24 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = -1,
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
