/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Seeed XIAO 1.47-inch IPS Touch Display with XIAO ESP32-S3 Plus:
 * 16 MB flash, 8 MB octal PSRAM, native USB Serial/JTAG (303a:1001).
 * Pins: https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/
 * https://wiki.seeedstudio.com/getting_started_1.47_inch_touch_display_esp32s3/
 * Panel, touch and PDM reference: https://github.com/Seeed-Studio/Seeed_GFX2
 * src/driver/tft/Driver_JD9853A.cpp, Driver_ST7789.cpp,
 * src/touch/Touch_AXS5106L.cpp and examples/.../0519_DashBoard_147_ESP32.
 * The panel register sequence is adapted from Seeed_GFX2; its MIT notice is
 * retained in devices/licenses/Seeed_GFX2.txt.
 *
 * The PDM microphone uses I2S0. The board has no speaker. This profile does
 * not use SD/IMU.
 */
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_pdm.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"
#include "xiao_147_touch.h"

#define LCD_W 172
#define LCD_H 320
#define DRAW_LINES 32
#define LCD_HOST SPI2_HOST
#define LCD_CS GPIO_NUM_3       /* D2 */
#define LCD_DC GPIO_NUM_4       /* D3 */
#define LCD_SCLK GPIO_NUM_7     /* D8 */
#define LCD_MOSI GPIO_NUM_9     /* D10 */
#define LCD_RST GPIO_NUM_13     /* D17, shared with touch reset */
#define LCD_BL GPIO_NUM_12      /* D18, active high */
#define SD_CS GPIO_NUM_43       /* D6: keep the unused shared-SPI device deselected */
#define I2C_SDA GPIO_NUM_5      /* D4 */
#define I2C_SCL GPIO_NUM_6      /* D5 */
#define TOUCH_IRQ GPIO_NUM_44   /* D7 */
#define MIC_CLK GPIO_NUM_1      /* D0 */
#define MIC_DATA GPIO_NUM_2     /* D1 */
#define TALK_GPIO GPIO_NUM_11   /* D19 / USR1, active low */
#define AUX_GPIO GPIO_NUM_42    /* D15 / USR2, active low */

static const char *TAG = "xiao147";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static i2c_master_dev_handle_t s_touch;
static i2s_chan_handle_t s_mic;
static muse_gpio_button_t s_talk, s_aux;
static bool s_mic_on;
static bool s_mic_settled;
static int32_t s_mic_dc;
static lv_point_t s_touch_point;

static esp_err_t init(void)
{
    const gpio_config_t outputs = {
        .pin_bit_mask = BIT64(LCD_RST) | BIT64(LCD_BL) | BIT64(SD_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "display pins");
    gpio_set_level(LCD_BL, 0);
    gpio_set_level(SD_CS, 1);
    /* Reset both controllers once. Resetting touch later would also reset LCD. */
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "USR1");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_aux, AUX_GPIO), TAG, "USR2");
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;
    s_aux.pressed = gpio_get_level(AUX_GPIO) == 0;
    const gpio_config_t irq = {
        .pin_bit_mask = BIT64(TOUCH_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = true,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&irq), TAG, "touch interrupt");
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "touch bus");
    const i2c_device_config_t touch_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x63,   /* AXS5106L */
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(bus, &touch_cfg, &s_touch);
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint8_t reg = 0x01, report[14];
    uint16_t x, y;
    /* Match the vendor's STOP between selecting the register and reading it.
     * Poll even when INT is high, so a missed edge cannot leave touch stuck. */
    bool pressed = i2c_master_transmit(s_touch, &reg, 1, 20) == ESP_OK &&
                   i2c_master_receive(s_touch, report, sizeof(report), 20) == ESP_OK &&
                   xiao_147_touch_decode(report, sizeof(report), &x, &y);
    if (pressed) {
        s_touch_point.x = x;
        s_touch_point.y = y;
    }
    data->point = s_touch_point;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static esp_err_t panel_configure(void)
{
    /* Seeed's verified ST7789-compatible JD9853A sequence. */
    static const struct {
        uint8_t cmd, length, data[14];
    } commands[] = {
        {0x13, 0, {0}},
        {0xb6, 2, {0x0a, 0x82}},
        {0xb0, 2, {0x00, 0xe0}},
        {0x3a, 1, {0x55}},
        {0xb2, 5, {0x0c, 0x0c, 0x00, 0x33, 0x33}},
        {0xb7, 1, {0x35}},
        {0xbb, 1, {0x28}},
        {0xc0, 1, {0x0c}},
        {0xc2, 2, {0x01, 0xff}},
        {0xc3, 1, {0x10}},
        {0xc4, 1, {0x20}},
        {0xc6, 1, {0x0f}},
        {0xd0, 2, {0xa4, 0xa1}},
        {0xe0, 14, {0xd0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0e, 0x12, 0x14, 0x17}},
        {0xe1, 14, {0xd0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x31, 0x54, 0x47, 0x0e, 0x1c, 0x17, 0x1b, 0x1e}},
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "LCD init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_panel_io, 0x11, NULL, 0), TAG, "sleep out");
    vTaskDelay(pdMS_TO_TICKS(120));
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_panel_io, commands[i].cmd,
                            commands[i].data, commands[i].length), TAG, "LCD register");
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "LCD portrait");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 34, 0), TAG, "LCD offset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, false), TAG, "LCD colours");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "LCD on");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    *touch = NULL;
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t backlight = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&timer) != ESP_OK || ledc_channel_config(&backlight) != ESP_OK) {
        return NULL;
    }
    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_W * DRAW_LINES * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    const esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = GPIO_NUM_NC,   /* LCD_RST, pulsed once in init() */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io, &s_panel_io) != ESP_OK ||
        esp_lcd_new_panel_st7789(s_panel_io, &panel, &s_panel) != ESP_OK ||
        panel_configure() != ESP_OK) {
        return NULL;
    }
    esp_lv_adapter_config_t adapter = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter.task_core_id = MUSE_UI_CORE;
    adapter.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t display = {
        .panel = s_panel,
        .panel_io = s_panel_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_W,
            .ver_res = LCD_H,
            .buffer_height = DRAW_LINES,
            .use_psram = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&display);
    if (!disp || esp_lv_adapter_start() != ESP_OK || esp_lv_adapter_lock(-1) != ESP_OK) {
        return NULL;
    }
    *touch = lv_indev_create();
    if (*touch) {
        lv_indev_set_type(*touch, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(*touch, disp);
        lv_indev_set_read_cb(*touch, touch_read);
    }
    esp_lv_adapter_unlock();
    return *touch ? disp : NULL;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);
}

static esp_err_t mic_init(void)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 4;
    channel.dma_frame_num = MUSE_AUDIO_CHUNK;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, NULL, &s_mic), TAG, "PDM channel");
    const i2s_pdm_rx_config_t pdm = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_CLK,
            .din = MIC_DATA,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_mic, &pdm), TAG, "PDM mode");
    gpio_set_drive_capability(MIC_CLK, GPIO_DRIVE_CAP_0);
    ESP_LOGI(TAG, "PDM mic on I2S0, %d Hz; no speaker", MUSE_AUDIO_RATE);
    return ESP_OK;
}

static esp_err_t mic_power(bool on)
{
    if (on == s_mic_on) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(on ? i2s_channel_enable(s_mic) : i2s_channel_disable(s_mic), TAG, "PDM %s",
                        on ? "start" : "stop");
    s_mic_on = on;
    s_mic_settled = false;
    return ESP_OK;
}

/* Mono PDM samples with the PDM filter's DC offset removed; muse_audio applies the gain. */
static esp_err_t mic_read(int16_t *mono, size_t frames)
{
    size_t bytes = 0;
    ESP_RETURN_ON_ERROR(i2s_channel_read(s_mic, mono, frames * sizeof(*mono), &bytes, 1000), TAG, "PDM read");
    if (bytes != frames * sizeof(*mono)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_mic_settled) {
        int32_t sum = 0;
        for (size_t i = 0; i < frames; i++) {
            sum += mono[i];
        }
        s_mic_dc = sum / (int32_t)frames * 256;
        s_mic_settled = true;
    }
    for (size_t i = 0; i < frames; i++) {
        s_mic_dc += (mono[i] * 256 - s_mic_dc) >> 8;
        int value = mono[i] - (s_mic_dc >> 8);
        mono[i] = value < INT16_MIN ? INT16_MIN : (value > INT16_MAX ? INT16_MAX : value);
    }
    return ESP_OK;
}

/* USR1 talks; USR2 is the aux button, which sleeps and wakes the screen. */
static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk) | (muse_gpio_button_poll(&s_aux) << 2);
}

/* No charger status reaches the chip, so the battery reads as absent; USB is
 * known when a host enumerates the chip's own USB. */
static esp_err_t read_power(muse_power_t *out)
{
    *out = (muse_power_t){
        .usb = usb_serial_jtag_is_connected(),
        .battery_pct = -1,
    };
    return ESP_OK;
}

/* Panel off, then deep sleep until USR1 is pressed. */
static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    while (gpio_get_level(TALK_GPIO) == 0 || gpio_get_level(AUX_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    rtc_gpio_init(TALK_GPIO);
    rtc_gpio_set_direction(TALK_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(TALK_GPIO);
    rtc_gpio_pulldown_dis(TALK_GPIO);
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext1_wakeup_io(BIT64(TALK_GPIO), ESP_EXT1_WAKEUP_ANY_LOW), TAG,
                        "USR1 wake");
    esp_deep_sleep_start();
    return ESP_FAIL;   /* still running: deep sleep didn't start */
}

static const muse_board_t s_board = {
    .name = "Seeed XIAO ESP32-S3 1.47 Touch Display",
    .width = LCD_W,
    .height = LCD_H,
    .touch = true,
    .diagonal_in = 1.47f,
    .talk_button = "USR1",
    .aux_button = "USR2",
    .talk_hint = { LV_ALIGN_BOTTOM_LEFT, 4, -4 },
    .aux_hint = { LV_ALIGN_BOTTOM_RIGHT, -4, -4 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .mic_init = mic_init,
    .mic_power = mic_power,
    .mic_read = mic_read,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
