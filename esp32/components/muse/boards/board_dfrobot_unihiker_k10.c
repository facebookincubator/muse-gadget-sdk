/* UNIHIKER K10 N16R8. Hardware sources:
 * DFRobot K10 schematic (V1.2 download), pages 1/3/5/6;
 * DFRobot/framework-arduinounihiker: libraries/unihiker_k10/src,
 * libraries/TFT_eSPI/User_Setup.h, variants/unihiker_k10/pins_arduino.h.
 * XL9535 @ 0x20: P00 backlight, P01 camera reset, P02 B, P14 A.
 * ES7243E stereo ADC + NS4168 amplifier, no touch controller.
 * ESP-IDF 6.0.1 port, using the same display adapter as board_aipi.c.
 */
#include <math.h>
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "led_strip.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_link.h"
#include "muse_mem.h"

static const char *TAG = "k10";
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static led_strip_handle_t s_rgb;
static i2s_chan_handle_t s_tx, s_rx;
static SemaphoreHandle_t s_io_lock;
static uint16_t s_outputs; /* Hold the unused camera in reset. */

static esp_err_t io_write(uint8_t reg, uint16_t value)
{
    uint8_t data[] = {reg, value & 255, value >> 8};
    return i2c_master_transmit(s_io, data, sizeof(data), 100);
}

static void set_brightness(int pct)
{
    /* XL9535 has binary outputs; positive brightness means backlight on. */
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    s_outputs = (s_outputs & ~1u) | (pct > 0);
    esp_err_t err = io_write(2, s_outputs);
    xSemaphoreGive(s_io_lock);
    if (err != ESP_OK) ESP_LOGW(TAG, "backlight: %s", esp_err_to_name(err));
}


static void rgb_task(void *arg)
{
    (void)arg;
    for (;;) {
        unsigned r = 0, g = 0, b = 0;
        bool pulse = false;
        muse_link_state_t link = muse_link_state();
        muse_mode_t mode = muse_state_mode(NULL);
        if (muse_state_asleep() || mode == MUSE_MODE_OFF) {
            /* off */
        } else if (mode == MUSE_MODE_ERROR || link == MUSE_LINK_ERROR) {
            r = 40; pulse = true;
        } else if (link == MUSE_LINK_ONLINE) {
            if (mode == MUSE_MODE_LISTENING) { b = 40; }
            else if (mode == MUSE_MODE_THINKING) { r = 24; b = 36; pulse = true; }
            else { g = 20; }
        } else if (link == MUSE_LINK_PAIRING || link == MUSE_LINK_CONFIRM || link == MUSE_LINK_CONNECTING) {
            b = 32; pulse = true;
        } else if (link == MUSE_LINK_OFFLINE) {
            r = 32; g = 20; pulse = true;
        } else {
            r = 36; g = 12; pulse = true;
        }
        float level = pulse ? 0.15f + 0.85f * (1.0f + sinf(esp_timer_get_time() / 250000.0f)) / 2.0f : 1.0f;
        for (int i = 0; i < 3; ++i) led_strip_set_pixel(s_rgb, i, r * level, g * level, b * level);
        led_strip_refresh(s_rgb);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_NUM_47, .scl_io_num = GPIO_NUM_48,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c), TAG, "I2C");
    const i2c_device_config_t io = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x20, .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &io, &s_io), TAG, "XL9535");
    s_io_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_io_lock, ESP_ERR_NO_MEM, TAG, "IO mutex");
    /* Keep expansion pins as inputs: only LCD and camera reset are outputs. */
    ESP_RETURN_ON_ERROR(io_write(2, s_outputs), TAG, "IO outputs");
    ESP_RETURN_ON_ERROR(io_write(4, 0), TAG, "IO polarity");
    ESP_RETURN_ON_ERROR(io_write(6, 0xfffc), TAG, "IO direction");
    const led_strip_config_t strip = {
        .strip_gpio_num = GPIO_NUM_46, .max_leds = 3, .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt = { .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10000000 };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip, &rmt, &s_rgb), TAG, "RGB");
    ESP_LOGI(TAG, "XL9535 ready; A=P14, B=P02; RGB GPIO46 x3");
    /* State is initialized immediately after board init; first update comes later. */
    /* The LED task does not run in interrupt context; keep its stack in PSRAM. */
    ESP_RETURN_ON_FALSE(xTaskCreateWithCaps(rgb_task, "k10_rgb", 3072, NULL, 2, NULL,
                                          MUSE_BIG_CAPS) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "RGB task");
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    *touch = NULL;
    const spi_bus_config_t bus = {
        .sclk_io_num = GPIO_NUM_12, .mosi_io_num = GPIO_NUM_21, .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC, .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 240 * 24 * 2,
    };
    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return NULL;
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = GPIO_NUM_14, .dc_gpio_num = GPIO_NUM_13, .spi_mode = 0,
        .pclk_hz = 40000000, .trans_queue_depth = 10, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi(SPI3_HOST, &io_cfg, &io) != ESP_OK) return NULL;
    const esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = GPIO_NUM_NC, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_ili9341(io, &panel, &s_panel) != ESP_OK) return NULL;
    if (esp_lcd_panel_reset(s_panel) != ESP_OK || esp_lcd_panel_init(s_panel) != ESP_OK ||
        esp_lcd_panel_invert_color(s_panel, false) != ESP_OK ||
        esp_lcd_panel_mirror(s_panel, false, true) != ESP_OK ||
        esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) return NULL;
    esp_lv_adapter_config_t adapter = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter.task_core_id = MUSE_UI_CORE;
    adapter.task_priority = MUSE_UI_PRIORITY;
    /* Use the adapter's supported PSRAM stack option to preserve internal RAM. */
    adapter.stack_in_psram = true;
    if (esp_lv_adapter_init(&adapter) != ESP_OK) return NULL;
    const esp_lv_adapter_display_config_t disp = {
        .panel = s_panel, .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER, .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = 240, .ver_res = 320, .buffer_height = 24,
            .use_psram = false, .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *display = esp_lv_adapter_register_display(&disp);
    if (!display || esp_lv_adapter_start() != ESP_OK) return NULL;
    ESP_LOGI(TAG, "ILI9341 ready: 240x320");
    return display;
}

static bool display_lock(int timeout_ms) { return esp_lv_adapter_lock(timeout_ms) == ESP_OK; }
static void display_pause(bool pause) { if (pause) esp_lv_adapter_pause(-1); else esp_lv_adapter_resume(); }
static void panel_sleep(bool sleep) { esp_lcd_panel_disp_sleep(s_panel, sleep); }

/* No speaker codec: NS4168 is an I2S amplifier. Codec device applies software volume. */
static int data_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{ (void)h; (void)type; (void)on; return ESP_CODEC_DEV_OK; }
static int spk_write(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h; size_t wrote = 0;
    return i2s_channel_write(s_tx, data, size, &wrote, pdMS_TO_TICKS(1000)) == ESP_OK && wrote == (size_t)size
        ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &s_tx, &s_rx), TAG, "I2S channels");
    const i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = GPIO_NUM_3, .bclk = GPIO_NUM_0, .ws = GPIO_NUM_38,
                      .dout = GPIO_NUM_45, .din = GPIO_NUM_39 },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &standard), TAG, "I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &standard), TAG, "I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "TX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "RX enable");
    /* Board variants/documentation disagree on the ADC address. Detect on the
     * dedicated 0x10..0x17 ES7243E address range instead of hard-coding one. */
    int address = -1;
    for (int a = 0x10; a <= 0x17; ++a) {
        if (i2c_master_probe(s_i2c, a, 50) == ESP_OK) { address = a; break; }
    }
    ESP_RETURN_ON_FALSE(address >= 0, ESP_ERR_NOT_FOUND, TAG, "ES7243E not detected");
    audio_codec_i2c_cfg_t ctrl_cfg = { .port = I2C_NUM_0, .addr = address << 1, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    ESP_RETURN_ON_FALSE(ctrl, ESP_ERR_NO_MEM, TAG, "ADC control");
    es7243e_codec_cfg_t adc_cfg = { .ctrl_if = ctrl };
    const audio_codec_if_t *adc = es7243e_codec_new(&adc_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ADC init");
    audio_codec_i2s_cfg_t data_cfg = { .port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&data_cfg);
    static const audio_codec_data_if_t speaker_data = { .enable = data_enable, .write = spk_write };
    ESP_RETURN_ON_FALSE(data, ESP_ERR_NO_MEM, TAG, "ADC data");
    esp_codec_dev_cfg_t input = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data };
    esp_codec_dev_cfg_t output = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .data_if = &speaker_data };
    *mic = esp_codec_dev_new(&input);
    *spk = esp_codec_dev_new(&output);
    ESP_LOGI(TAG, "ES7243E detected at 0x%02x, stereo %d Hz", address, MUSE_AUDIO_RATE);
    return *mic && *spk ? ESP_OK : ESP_ERR_NO_MEM;
}

static unsigned poll_buttons(void)
{
    static bool pressed[2];
    static uint8_t stable[2];
    uint8_t reg = 0, data[2];
    if (i2c_master_transmit_receive(s_io, &reg, 1, data, 2, 20) != ESP_OK) return 0;
    uint16_t inputs = data[0] | (data[1] << 8);
    const unsigned pins[] = {12, 2}; /* A then B, from DFRobot's schematic. */
    unsigned edges = 0;
    for (int i = 0; i < 2; ++i) {
        bool down = !(inputs & (1u << pins[i]));
        if (down == pressed[i]) stable[i] = 0;
        else if (++stable[i] >= 3) {
            pressed[i] = down; stable[i] = 0;
            edges |= (down ? MUSE_BTN_TALK_PRESS : MUSE_BTN_TALK_RELEASE) << (i * 2);
            ESP_LOGI(TAG, "button %c %s", 'A' + i, down ? "pressed" : "released");
        }
    }
    return edges;
}

static esp_err_t read_power(muse_power_t *out)
{
    out->battery_pct = -1; out->battery_mv = 0; out->charging = false;
    out->usb = usb_serial_jtag_is_connected();
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    led_strip_clear(s_rgb);
    /* Reset wakes the first-version firmware; BOOT/GPIO0 belongs to I2S. */
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "DFRobot UNIHIKER K10", .width = 240, .height = 320, .round = false,
    .touch = false, .diagonal_in = 2.8f, .talk_button = "A", .aux_button = "B",
    .talk_hint = { LV_ALIGN_BOTTOM_LEFT, 12, -8 }, .aux_hint = { LV_ALIGN_BOTTOM_RIGHT, -12, -8 },
    .flat_menu_hints = true, .frame_ms = 40,
    .init = init, .display_start = display_start, .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock, .set_brightness = set_brightness,
    .panel_sleep = panel_sleep, .display_pause = display_pause,
    .audio_init = audio_init, .mic_slot = 0, .poll_buttons = poll_buttons,
    .read_power = read_power, .power_off = power_off,
};
const muse_board_t *muse_board_get(void) { return &s_board; }
