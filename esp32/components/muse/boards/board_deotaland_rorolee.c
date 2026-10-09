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
 * Deotaland RoRoLee (RRL-01): ESP32-S3 with 16 MB flash and 8 MB octal PSRAM,
 * a 120x240 SH8501 AMOLED on 4-wire SPI (no touch), an ES8311 speaker codec
 * and an ES7210 microphone ADC on one duplex I2S bus, a BQ27220 fuel gauge,
 * three buttons and a vibration motor. GPIO14, active low, powers the panel,
 * codecs and gauge. Pins are from Deotaland's RRL-01 schematic and production
 * firmware.
 *
 * The RoRoLee is held in landscape, but the SH8501 can't exchange rows and
 * columns, so the UI draws 240x120 and each flushed area is turned onto a
 * portrait copy of the screen here, which the panel gets whole, as Deotaland's
 * rorolee-muse firmware sends its frames.
 *
 * BOOT is talk. VOL- opens the menu and steps down it, VOL+ steps up. Only
 * BOOT is on an RTC pin (VOL+ and VOL- are GPIO 39 and 40), so only BOOT
 * wakes the board from power-off.
 */
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define PERIPH_EN GPIO_NUM_14       /* low: panel, codecs and gauge powered */
#define VIBRATOR GPIO_NUM_1         /* motor, high runs it; not used yet */

#define LCD_W 120                   /* the panel, portrait */
#define LCD_H 240
#define UI_W LCD_H                  /* what the UI draws: landscape */
#define UI_H LCD_W
#define LCD_HOST SPI2_HOST
#define LCD_SCLK GPIO_NUM_5
#define LCD_MOSI GPIO_NUM_4
#define LCD_CS GPIO_NUM_7
#define LCD_DC GPIO_NUM_46
#define LCD_RST GPIO_NUM_6
#define STRIPE_ROWS 20              /* panel rows per window, as production sends them */
#define STRIPE_PX (LCD_W * STRIPE_ROWS)
#define LK_CHUNK 4092               /* the LK driver's largest transfer */

#define I2C_SDA GPIO_NUM_45
#define I2C_SCL GPIO_NUM_48
#define I2S_MCLK GPIO_NUM_9
#define I2S_BCLK GPIO_NUM_10
#define I2S_WS GPIO_NUM_11
#define I2S_DOUT GPIO_NUM_41        /* to the ES8311 */
#define I2S_DIN GPIO_NUM_12         /* from the ES7210 */
#define PA_EN GPIO_NUM_13

#define TALK_GPIO GPIO_NUM_0        /* BOOT */
#define AUX_GPIO GPIO_NUM_40        /* VOL- */
#define UP_GPIO GPIO_NUM_39         /* VOL+ */

#define GAUGE_ADDR 0x55             /* BQ27220; its registers are little-endian words */
#define GAUGE_VOLTAGE 0x08          /* mV */
#define GAUGE_CURRENT 0x0C          /* mA, signed, positive into the cell */
#define GAUGE_SOC 0x2C              /* percent */
#define CHARGE_MA 10                /* the cell at rest reads a few mA either way */

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[4];
    uint8_t delay_ms;
} lcd_init_cmd_t;

/*
 * The panel maker's short init, as the production firmware sends it: power,
 * timing and gamma come from the panel's OTP, so it's standard commands only.
 * The panel takes no pixels before DISPON. The sequence leaves the panel in
 * idle mode (0x39), which this OTP setup needs: out of it (0x38) it goes dark.
 */
static const lcd_init_cmd_t LCD_INIT[] = {
    { LCD_CMD_SLPOUT, 0, { 0 }, 60 },
    { LCD_CMD_CASET, 4, { 0, 0, 0, LCD_W - 1 }, 0 },
    { LCD_CMD_RASET, 4, { 0, 0, 0, LCD_H - 1 }, 0 },
    { 0x44, 2, { 0x01, 0x27 }, 0 },             /* tearing-effect scan line */
    { LCD_CMD_TEON, 1, { 0x00 }, 0 },
    { LCD_CMD_COLMOD, 1, { 0x55 }, 0 },         /* RGB565 */
    { LCD_CMD_MADCTL, 1, { 0x00 }, 0 },
    { LCD_CMD_WRDISBV, 1, { 0xFF }, 60 },
    { LCD_CMD_DISPON, 0, { 0 }, 120 },
    { LCD_CMD_IDMON, 0, { 0 }, 0 },             /* ends the maker's sequence */
};

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_gauge;
static spi_device_handle_t s_spi;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_frame;           /* the whole screen, portrait, in the panel's byte order */
static uint16_t *s_stripe;          /* a stripe of it on its way to the panel */
static muse_gpio_button_t s_talk, s_aux, s_up;

/*
 * The panel is driven the way its maker's LK driver does it, which is what
 * Deotaland's rorolee-muse firmware uses: an SPI device of its own with DC on
 * a plain GPIO, every transfer polled, the command byte and its parameters
 * each in a transfer of their own, and RAMWR sent twice before the pixels.
 */
static esp_err_t lcd_send(int dc, const void *buf, size_t len, uint32_t flags)
{
    spi_transaction_t t = { .flags = flags, .length = len * 8 };
    if (len <= sizeof(t.tx_data)) {         /* a command or its parameters */
        t.flags |= SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, buf, len);
    } else {
        t.tx_buffer = buf;
    }
    gpio_set_level(LCD_DC, dc);
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t lcd_cmd(uint8_t cmd, const void *data, size_t len)
{
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(s_spi, portMAX_DELAY), TAG, "bus");
    esp_err_t err = lcd_send(0, &cmd, 1, 0);
    if (err == ESP_OK && len) {
        err = lcd_send(1, data, len, 0);
    }
    spi_device_release_bus(s_spi);
    return err;
}

/* Pixels into the window [x1, x2) x [y1, y2), in chunks of the LK driver's
 * size with CS held between them. They're all out when this returns. */
static esp_err_t lcd_window(int x1, int y1, int x2, int y2, const void *pixels)
{
    const uint8_t cmds[] = { LCD_CMD_CASET, LCD_CMD_RASET, LCD_CMD_RAMWR, LCD_CMD_RAMWR };
    const uint8_t args[2][4] = {
        { x1 >> 8, x1 & 0xFF, (x2 - 1) >> 8, (x2 - 1) & 0xFF },
        { y1 >> 8, y1 & 0xFF, (y2 - 1) >> 8, (y2 - 1) & 0xFF },
    };
    const uint8_t *p = pixels;
    size_t left = (size_t)(x2 - x1) * (y2 - y1) * 2;
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(s_spi, portMAX_DELAY), TAG, "bus");
    esp_err_t err = ESP_OK;
    for (int i = 0; i < 4 && err == ESP_OK; i++) {
        err = lcd_send(0, &cmds[i], 1, 0);
        if (err == ESP_OK && i < 2) {
            err = lcd_send(1, args[i], 4, 0);
        }
    }
    while (err == ESP_OK && left) {
        const size_t n = left < LK_CHUNK ? left : LK_CHUNK;
        left -= n;
        err = lcd_send(1, p, n, left ? SPI_TRANS_CS_KEEP_ACTIVE : 0);
        p += n;
    }
    spi_device_release_bus(s_spi);
    return err;
}

/*
 * The whole screen, a stripe at a time from the top through internal RAM, as
 * rorolee-muse sends every frame. Given only the areas that changed, this
 * panel shows an animation in fits and starts, though every pixel arrives.
 */
static esp_err_t send_frame(void)
{
    for (int y = 0; y < LCD_H; y += STRIPE_ROWS) {
        memcpy(s_stripe, s_frame + y * LCD_W, STRIPE_PX * 2);
        ESP_RETURN_ON_ERROR(lcd_window(0, y, LCD_W, y + STRIPE_ROWS, s_stripe), TAG, "stripe %d", y);
    }
    return ESP_OK;
}

/* An esp_lcd panel for the adapter; LVGL's areas go through draw_turned. */
static esp_err_t panel_draw(esp_lcd_panel_t *panel, int x1, int y1, int x2, int y2, const void *pixels)
{
    (void)panel;
    return lcd_window(x1, y1, x2, y2, pixels);
}

static esp_err_t panel_del(esp_lcd_panel_t *panel)
{
    free(panel);
    return ESP_OK;
}

/* The adapter listens on a panel IO for the end of each transfer; these end
 * before draw_turned returns, so there's nothing to tell it. */
static esp_err_t io_callbacks(esp_lcd_panel_io_t *io, const esp_lcd_panel_io_callbacks_t *cbs, void *ctx)
{
    (void)io;
    (void)cbs;
    (void)ctx;
    return ESP_OK;
}

static esp_lcd_panel_io_t s_io = { .register_event_callbacks = io_callbacks };

/*
 * The UI's landscape area onto the portrait copy of the screen, a quarter turn
 * as the production firmware draws it: UI (x, y) lands on panel (y, 239 - x).
 * The adapter has already put the pixels in the panel's byte order. After the
 * last area of a refresh, the panel gets the whole copy.
 */
static esp_err_t draw_turned(lv_display_t *disp, esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2,
                             const void *color_map, void *ctx)
{
    (void)panel;
    (void)ctx;
    const int w = x2 - x1, h = y2 - y1;
    const uint16_t *src = color_map;
    for (int r = 0; r < w; r++) {               /* panel rows: UI columns, right to left */
        const uint16_t *col = src + (w - 1 - r);
        uint16_t *dst = s_frame + (LCD_H - x2 + r) * LCD_W + y1;
        for (int c = 0; c < h; c++) {           /* panel columns: UI rows */
            dst[c] = col[c * w];
        }
    }
    const esp_err_t err = lv_display_flush_is_last(disp) ? send_frame() : ESP_OK;
    lv_display_flush_ready(disp);           /* the transfers are over */
    return err;
}

static esp_err_t gauge_read(uint8_t reg, uint16_t *out)
{
    uint8_t b[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_gauge, &reg, 1, b, sizeof(b), 50), TAG, "gauge %02x", reg);
    *out = b[0] | b[1] << 8;
    return ESP_OK;
}

static esp_err_t init(void)
{
    /* power_off() held these through deep sleep. */
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(PERIPH_EN);
    gpio_hold_dis(PA_EN);
    gpio_hold_dis(VIBRATOR);
    rtc_gpio_deinit(TALK_GPIO);

    const gpio_config_t out = {
        .pin_bit_mask = BIT64(PERIPH_EN) | BIT64(PA_EN) | BIT64(VIBRATOR),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "power pins");
    gpio_set_level(VIBRATOR, 0);
    gpio_set_level(PA_EN, 0);               /* amp off until the speaker opens */
    gpio_set_level(PERIPH_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(50));          /* the rail settles before anything talks to it */

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    const i2c_device_config_t gauge_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GAUGE_ADDR,
        .scl_speed_hz = 100000,
    };
    uint16_t mv;
    if (i2c_master_bus_add_device(s_i2c, &gauge_cfg, &s_gauge) != ESP_OK || gauge_read(GAUGE_VOLTAGE, &mv) != ESP_OK) {
        ESP_LOGW(TAG, "no fuel gauge: battery unknown");
        if (s_gauge) {
            i2c_master_bus_rm_device(s_gauge);
            s_gauge = NULL;
        }
    }

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_aux, AUX_GPIO), TAG, "aux button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_up, UP_GPIO), TAG, "up button");
    /* BOOT, if it woke the board from power-off, is still down; don't count it. */
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    (void)touch;
    /* A pad hold left on CS or RST by other firmware outlives a chip reset
     * and keeps every command from the panel. */
    gpio_hold_dis(LCD_CS);
    gpio_hold_dis(LCD_RST);
    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = STRIPE_PX * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    /* Mode 0 at 40 MHz with one transfer at a time, as the LK driver sets it
     * up: some panels miss init writes at 30 MHz. */
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = LCD_CS,
        .queue_size = 1,
    };
    const gpio_config_t out = { .pin_bit_mask = BIT64(LCD_DC) | BIT64(LCD_RST), .mode = GPIO_MODE_OUTPUT };
    if (spi_bus_add_device(LCD_HOST, &dev, &s_spi) != ESP_OK || gpio_config(&out) != ESP_OK) {
        return NULL;
    }

    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    for (size_t i = 0; i < sizeof(LCD_INIT) / sizeof(LCD_INIT[0]); i++) {
        if (lcd_cmd(LCD_INIT[i].cmd, LCD_INIT[i].data, LCD_INIT[i].len) != ESP_OK) {
            ESP_LOGE(TAG, "panel init %02x", LCD_INIT[i].cmd);
            return NULL;
        }
        vTaskDelay(pdMS_TO_TICKS(LCD_INIT[i].delay_ms));
    }

    esp_lcd_panel_t *panel = calloc(1, sizeof(*panel));
    s_frame = heap_caps_calloc(LCD_W * LCD_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_stripe = heap_caps_malloc(STRIPE_PX * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!panel || !s_frame || !s_stripe) {
        return NULL;
    }
    panel->draw_bitmap = panel_draw;
    panel->del = panel_del;
    s_panel = panel;

    /* The sequence ends lit, showing what the panel powered up with: dark it,
     * clear it to black (the copy starts black) and light it again, as
     * production does. */
    lcd_cmd(LCD_CMD_WRDISBV, (uint8_t[]){ 0 }, 1);
    send_frame();
    lcd_cmd(LCD_CMD_WRDISBV, (uint8_t[]){ 0xFF }, 1);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    /* Two whole-screen buffers in PSRAM, as the production firmware has: LVGL
     * draws each changed area in one pass into them. */
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = &s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = UI_W,
            .ver_res = UI_H,
            .buffer_height = UI_H,
            .use_psram = true,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    const esp_lv_adapter_draw_bitmap_callbacks_t turn = { .custom_draw_bitmap = draw_turned };
    if (!disp || esp_lv_adapter_set_draw_bitmap_callbacks(disp, &turn, NULL) != ESP_OK
        || esp_lv_adapter_start() != ESP_OK) {
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
    lcd_cmd(LCD_CMD_WRDISBV, (uint8_t[]){ pct * 255 / 100 }, 1);
}

static void panel_sleep(bool sleep)
{
    /* DISPOFF and DISPON only, as production does: SLPIN resets the panel's
     * brightness and idle mode, and out of idle mode it stays dark. */
    lcd_cmd(sleep ? LCD_CMD_DISPOFF : LCD_CMD_DISPON, NULL, 0);
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

/* The ES8311 plays and the ES7210 records over one duplex I2S bus, both
 * clocked from MCLK. The microphone is on the ES7210's MIC1, the left slot. */
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
    audio_codec_i2c_cfg_t dac_i2c = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    audio_codec_i2c_cfg_t adc_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PA_EN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 3.3, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&dac_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    es7210_codec_cfg_t adc_cfg = { .ctrl_if = adc_ctrl, .mic_selected = ES7210_SEL_MIC1 };
    const audio_codec_if_t *adc = es7210_codec_new(&adc_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    unsigned up = (muse_gpio_button_poll(&s_up) & MUSE_BTN_TALK_PRESS) ? MUSE_BTN_UP : 0;
    return muse_gpio_button_poll(&s_talk) | muse_gpio_button_poll(&s_aux) << 2 | up;
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_talk, &s_aux, &s_up }, 3, timeout_ms);
}

/* The BQ27220 gauges the cell. The charger has no line of its own, so USB
 * counts as present while the cell charges or a computer holds the port. */
static esp_err_t read_power(muse_power_t *out)
{
    if (!s_gauge) {
        return ESP_ERR_NOT_FOUND;
    }
    uint16_t mv, ma, pct;
    ESP_RETURN_ON_ERROR(gauge_read(GAUGE_VOLTAGE, &mv), TAG, "voltage");
    ESP_RETURN_ON_ERROR(gauge_read(GAUGE_CURRENT, &ma), TAG, "current");
    ESP_RETURN_ON_ERROR(gauge_read(GAUGE_SOC, &pct), TAG, "charge");
    out->battery_mv = mv;
    out->battery_pct = pct <= 100 ? pct : -1;
    out->charging = (int16_t)ma > CHARGE_MA;
    out->usb = out->charging || usb_serial_jtag_is_connected();
    return ESP_OK;
}

/*
 * The panel off, then the rail with the codecs, amp and gauge on it, and deep
 * sleep until BOOT is pressed. The pads hold their levels through it, so the
 * rail, amp and motor stay off.
 */
static esp_err_t power_off(void)
{
    display_lock(-1);   /* nothing on the wire from LVGL; never released */
    set_brightness(0);
    lcd_cmd(LCD_CMD_DISPOFF, NULL, 0);
    lcd_cmd(LCD_CMD_SLPIN, NULL, 0);
    gpio_set_level(PA_EN, 0);
    gpio_set_level(VIBRATOR, 0);
    gpio_set_level(PERIPH_EN, 1);
    gpio_hold_en(PA_EN);
    gpio_hold_en(VIBRATOR);
    gpio_hold_en(PERIPH_EN);
    gpio_deep_sleep_hold_en();
    while (gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0), TAG, "wake button");
    rtc_gpio_pullup_en(TALK_GPIO);
    rtc_gpio_pulldown_dis(TALK_GPIO);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Deotaland RoRoLee",
    .width = UI_W,
    .height = UI_H,
    .round = false,
    .touch = false,
    .talk_button = "boot",
    .aux_button = "vol-",
    .wake_button = "boot",
    .talk_hint = { LV_ALIGN_BOTTOM_RIGHT, -4, -4 },
    .aux_hint = { LV_ALIGN_BOTTOM_LEFT, 4, -4 },
    .frame_ms = 50,             /* rorolee-muse's 20 fps: each frame is the whole screen */
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,              /* MIC1, on the left slot */
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
