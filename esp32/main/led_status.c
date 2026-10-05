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

#include "led_status.h"
#include "stack_monitor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#if CONFIG_HOMEHUB_LED_BACKEND_PWM_RGB
#include "driver/ledc.h"
#elif CONFIG_HOMEHUB_LED_BACKEND_DEVKIT_GPIO27
#include "led_strip.h"
#include "soc/spi_pins.h"
#elif CONFIG_HOMEHUB_LED_BACKEND_VOICE_RING
#include "driver/gpio.h"
#include "led_strip.h"
#elif CONFIG_HOMEHUB_DISPLAY
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "happy_anim.h"
#include "pixel_font.h"
#if CONFIG_HOMEHUB_LED_BACKEND_IDEASPARK_ST7789
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_st7789.h"
#else
#include "driver/i2c_master.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_rom_sys.h"
#endif
#elif CONFIG_HOMEHUB_LED_BACKEND_MUSE
#include "muse_glue.h"
#include "muse_ui.h"
#endif

static const char *TAG = "link.led";

// Muse Home Link RGB LED: 3 channels, driven via LEDC PWM.
#if CONFIG_HOMEHUB_LED_BACKEND_PWM_RGB
#if CONFIG_HOMEHUB_PRE_DVT_GPIO
#define LED_R_GPIO   24
#define LED_G_GPIO   25
#define LED_B_GPIO   26
#else
#define LED_R_GPIO   2
#define LED_G_GPIO   3
#define LED_B_GPIO   6
#endif
#define LED_PWM_MODE LEDC_LOW_SPEED_MODE
#define LED_PWM_TIMER LEDC_TIMER_0
#define LED_PWM_FREQ  5000
#define LED_PWM_RES   LEDC_TIMER_8_BIT
#elif CONFIG_HOMEHUB_LED_BACKEND_DEVKIT_GPIO27
// Single addressable RGB LED, GPIO27 on the ESP32-C5 DevKitC-1. The DevKitC-1's
// separate red power LED is always on when USB-powered and is not
// firmware-controlled.
#define LED_STRIP_GPIO       CONFIG_HOMEHUB_LED_STRIP_GPIO
// Routing the LED to a flash or PSRAM pin takes that pin off the memory bus the
// chip runs from, so refuse the build. GPIO27, the default, is one on the S3.
#if LED_STRIP_GPIO == MSPI_IOMUX_PIN_NUM_CLK || LED_STRIP_GPIO == MSPI_IOMUX_PIN_NUM_CS0 \
    || LED_STRIP_GPIO == MSPI_IOMUX_PIN_NUM_MISO || LED_STRIP_GPIO == MSPI_IOMUX_PIN_NUM_MOSI \
    || LED_STRIP_GPIO == MSPI_IOMUX_PIN_NUM_HD || LED_STRIP_GPIO == MSPI_IOMUX_PIN_NUM_WP
#error "CONFIG_HOMEHUB_LED_STRIP_GPIO is an SPI flash pin on this chip"
#endif
#if CONFIG_SPIRAM && defined(MSPI_IOMUX_PIN_NUM_CS1) && LED_STRIP_GPIO == MSPI_IOMUX_PIN_NUM_CS1
#error "CONFIG_HOMEHUB_LED_STRIP_GPIO is the PSRAM chip select on this chip"
#endif
#if (CONFIG_SPIRAM_MODE_OCT || CONFIG_ESPTOOLPY_OCT_FLASH) && defined(MSPI_IOMUX_PIN_NUM_D4) \
    && LED_STRIP_GPIO >= MSPI_IOMUX_PIN_NUM_D4 && LED_STRIP_GPIO <= MSPI_IOMUX_PIN_NUM_DQS
#error "CONFIG_HOMEHUB_LED_STRIP_GPIO is an octal flash/PSRAM pin on this chip"
#endif
#define LED_STRIP_LED_COUNT  1
#define LED_STRIP_RMT_RES_HZ (10 * 1000 * 1000)
#elif CONFIG_HOMEHUB_LED_BACKEND_VOICE_RING
// Home Assistant Voice PE: 12 WS2812 LEDs in a ring, index 0 at the top and
// counting clockwise, powered through a switch on GPIO45.
#define RING_GPIO            21
#define RING_POWER_GPIO      45
#define RING_LEDS            12
#define LED_STRIP_RMT_RES_HZ (10 * 1000 * 1000)
// How long the ring stays green after connecting before it goes dark.
#define RING_CONNECTED_MS    3000
#elif CONFIG_HOMEHUB_LED_BACKEND_IDEASPARK_ST7789
// ideaspark ESP32 board: 170x320 ST7789 IPS panel on SPI, no status LED.
#define LCD_NAME         "ideaspark ST7789"
#define LCD_HOST         SPI2_HOST
#define LCD_PIN_SCLK     18
#define LCD_PIN_MOSI     23
#define LCD_PIN_CS       15
#define LCD_PIN_DC       2
#define LCD_PIN_RST      4
#define LCD_PIN_BL       32
#define LCD_PCLK_HZ      (40 * 1000 * 1000)
#define LCD_H_RES        170
#define LCD_V_RES        320
// The 170-column panel sits in the middle of the controller's 240 columns.
#define LCD_X_GAP        35
#define LCD_BAR_ROWS     10
#define LCD_ANIM_SCALE   3
#define LCD_DOT_MARGIN   4
// Draw buffers are sent by SPI DMA.
#define LCD_BUF_CAPS     MALLOC_CAP_DMA
#elif CONFIG_HOMEHUB_LED_BACKEND_SENSECAP_ST7701
// SenseCAP Indicator: 480x480 ST7701S panel on a 16-bit RGB bus, refreshed
// from a frame buffer in PSRAM. The panel is set up once over 3-wire SPI
// (bit-banged), with its chip select and reset on a TCA9535 I/O expander.
// Pins and timings follow Seeed's SenseCAP_Indicator_ESP32 BSP.
#define LCD_NAME         "SenseCAP Indicator ST7701S"
#define LCD_PIN_PCLK     21
#define LCD_PIN_DE       18
#define LCD_PIN_VSYNC    17
#define LCD_PIN_HSYNC    16
#define LCD_PIN_BL       45
#define LCD_PIN_SPI_SCLK 41
#define LCD_PIN_SPI_MOSI 48
#define LCD_PIN_I2C_SDA  39
#define LCD_PIN_I2C_SCL  40
#define LCD_PCLK_HZ      (18 * 1000 * 1000)
#define LCD_H_RES        480
#define LCD_V_RES        480
// Lines per DMA bounce buffer (two, in internal RAM). They keep the panel fed
// while PSRAM is busy, and save a cache flush of the frame buffer per draw.
#define LCD_BOUNCE_LINES 10
#define EXP_ADDR         0x20
#define EXP_ADDR_ALT     0x39  // some boards
#define EXP_REG_OUT0     0x02
#define EXP_REG_CFG0     0x06
// Port 0 bits. The touch reset (bit 7) and, on port 1, the RP2040 reset share
// the expander and are left alone.
#define EXP_LCD_CS       (1 << 4)
#define EXP_LCD_RST      (1 << 5)
#define LCD_BAR_ROWS     24
#define LCD_ANIM_SCALE   5
#define LCD_DOT_MARGIN   10
// Draw buffers are copied into the frame buffer by the CPU.
#define LCD_BUF_CAPS     (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

#if CONFIG_HOMEHUB_DISPLAY
// Portrait layout: the status colour fills a bar along the top and bottom
// edges and a pixel-art animation plays in between, with the agent's name
// above it. Once connected, the bars give way to a small green dot in the
// top-right corner.

// Status colours are tuned for an LED; this channel level maps to full scale.
#define LCD_FULL_LEVEL   80
#define LCD_ANIM_X       ((LCD_H_RES - HAPPY_ANIM_WIDTH * LCD_ANIM_SCALE) / 2)
#define LCD_ANIM_Y       ((LCD_V_RES - HAPPY_ANIM_HEIGHT * LCD_ANIM_SCALE) / 2)
// Cell rows expanded and sent per panel write.
#define LCD_ANIM_STRIPE_CELLS 8
#define LCD_ANIM_BUF_PIXELS \
    (HAPPY_ANIM_WIDTH * LCD_ANIM_SCALE * LCD_ANIM_STRIPE_CELLS * LCD_ANIM_SCALE)
// Connected dot: a 4x4-cell circle drawn in the animation's pixel size.
#define LCD_DOT_CELLS    4
#define LCD_DOT_SIZE     (LCD_DOT_CELLS * LCD_ANIM_SCALE)
#define LCD_DOT_X        (LCD_H_RES - LCD_DOT_MARGIN - LCD_DOT_SIZE)
#define LCD_DOT_Y        LCD_DOT_MARGIN
// Title: one line of pixel font, in the animation's pixel size when it fits.
#define LCD_TITLE_MAX_SCALE LCD_ANIM_SCALE
#define LCD_TITLE_ROWS   (PIXEL_FONT_HEIGHT * LCD_TITLE_MAX_SCALE)
#define LCD_TITLE_Y      (LCD_ANIM_Y - LCD_TITLE_ROWS - 16)
// Rows rendered per panel write; must fit s_bar_buf.
#define LCD_TITLE_STRIPE_ROWS 8
// Full-width rows that fit s_anim_buf.
#define LCD_IMAGE_MAX_ROWS (LCD_ANIM_BUF_PIXELS / LCD_H_RES)
#endif

static SemaphoreHandle_t s_mutex = NULL;
static led_state_t s_state = LED_STATE_BOOT;
static TaskHandle_t s_task = NULL;
// Title requested by led_status_set_title(); the LED task draws it.
static char s_title[48];
static bool s_title_dirty = false;
#if CONFIG_HOMEHUB_VOICE
static led_voice_t s_voice = LED_VOICE_IDLE;
static float s_level = 0;
static int s_volume = 0;
static int64_t s_volume_until = 0;
#endif

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static const rgb_t COLOR_OFF      = {0,   0,   0};
static const rgb_t COLOR_DIM_ORANGE = {12, 4,   0};
static const rgb_t COLOR_ORANGE   = {80,  25,  0};
static const rgb_t COLOR_BLUE     = {0,   0,   80};
static const rgb_t COLOR_YELLOW   = {80,  50,  0};
static const rgb_t COLOR_GREEN    = {0,   80,  0};
static const rgb_t COLOR_RED      = {80,  0,   0};
static const rgb_t COLOR_PURPLE   = {60,  0,   60};

#if CONFIG_HOMEHUB_LED_BACKEND_PWM_RGB
static void led_hw_set_color(rgb_t c) {
    ledc_set_duty(LED_PWM_MODE, LEDC_CHANNEL_0, c.r);
    ledc_update_duty(LED_PWM_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LED_PWM_MODE, LEDC_CHANNEL_1, c.g);
    ledc_update_duty(LED_PWM_MODE, LEDC_CHANNEL_1);
    ledc_set_duty(LED_PWM_MODE, LEDC_CHANNEL_2, c.b);
    ledc_update_duty(LED_PWM_MODE, LEDC_CHANNEL_2);
}

static esp_err_t init_channel(ledc_channel_t channel, int gpio) {
    ledc_channel_config_t cfg = {
        .gpio_num = gpio,
        .speed_mode = LED_PWM_MODE,
        .channel = channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LED_PWM_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    };
    return ledc_channel_config(&cfg);
}

static bool led_hw_init(void) {
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LED_PWM_MODE,
        .duty_resolution = LED_PWM_RES,
        .timer_num = LED_PWM_TIMER,
        .freq_hz = LED_PWM_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer init failed");
        return false;
    }

    if (init_channel(LEDC_CHANNEL_0, LED_R_GPIO) != ESP_OK ||
        init_channel(LEDC_CHANNEL_1, LED_G_GPIO) != ESP_OK ||
        init_channel(LEDC_CHANNEL_2, LED_B_GPIO) != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel init failed");
        return false;
    }

    ESP_LOGI(TAG, "LED status ready: " CONFIG_GADGET_PRODUCT_NAME " PWM RGB (R=%d G=%d B=%d)",
             LED_R_GPIO, LED_G_GPIO, LED_B_GPIO);
    return true;
}
#elif CONFIG_HOMEHUB_LED_BACKEND_DEVKIT_GPIO27
static led_strip_handle_t s_strip = NULL;

static void led_hw_set_color(rgb_t c) {
    if (!s_strip) return;
    if (led_strip_set_pixel(s_strip, 0, c.r, c.g, c.b) != ESP_OK) return;
    (void)led_strip_refresh(s_strip);
}

static bool led_hw_init(void) {
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = LED_STRIP_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
#if CONFIG_HOMEHUB_LED_RGB_ORDER
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
#else
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
#endif
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_STRIP_RMT_RES_HZ,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DevKit RGB LED init failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "LED status ready: addressable RGB (GPIO=%d)", LED_STRIP_GPIO);
    return true;
}
#elif CONFIG_HOMEHUB_LED_BACKEND_VOICE_RING
static led_strip_handle_t s_strip = NULL;
// Steady frames shown since the ring last turned green for "connected".
static int s_connected_frames = 0;

static void ring_show(const rgb_t *px) {
    if (!s_strip) return;
    for (int i = 0; i < RING_LEDS; i++) {
        if (led_strip_set_pixel(s_strip, i, px[i].r, px[i].g, px[i].b) != ESP_OK) return;
    }
    (void)led_strip_refresh(s_strip);
}

static void ring_fill(rgb_t c) {
    rgb_t px[RING_LEDS];
    for (int i = 0; i < RING_LEDS; i++) px[i] = c;
    ring_show(px);
}

static void led_hw_set_color(rgb_t c) {
    s_connected_frames = 0;
    ring_fill(c);
}

// Connected: green for a moment, then dark until something happens.
static void led_hw_set_connected(void) {
    ring_fill(s_connected_frames * 200 < RING_CONNECTED_MS ? COLOR_GREEN : COLOR_OFF);
    if (s_connected_frames * 200 < RING_CONNECTED_MS) s_connected_frames++;
}

static bool led_hw_init(void) {
    gpio_config_t power_cfg = {
        .pin_bit_mask = 1ULL << RING_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&power_cfg);
    if (err == ESP_OK) err = gpio_set_level(RING_POWER_GPIO, 1);

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = RING_GPIO,
        .max_leds = RING_LEDS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_STRIP_RMT_RES_HZ,
        // Four RMT memory blocks, so the 12 LEDs go out without refills.
        .mem_block_symbols = 192,
    };
    if (err == ESP_OK) err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED ring init failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "LED status ready: Voice PE %d-LED ring (GPIO=%d)", RING_LEDS, RING_GPIO);
    return true;
}
#elif CONFIG_HOMEHUB_DISPLAY
static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_bar_buf = NULL;
static uint16_t *s_anim_buf = NULL;
// Serializes panel access between the LED task (bars), the animation task and
// image drawing, and guards the drawn-state below and s_anim_buf.
static SemaphoreHandle_t s_lcd_lock = NULL;
static bool s_bars_drawn = false;
static rgb_t s_bar_color;
static uint16_t s_bar_px;
static bool s_dot_drawn = false;
// An image from led_status_draw_rect() replaces the animation and title.
static bool s_image_mode = false;
static const uint8_t s_dot_rows[LCD_DOT_CELLS] = {0x6, 0xf, 0xf, 0x6};

#if CONFIG_HOMEHUB_LED_BACKEND_IDEASPARK_ST7789
static SemaphoreHandle_t s_draw_done = NULL;

static bool lcd_draw_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata, void *ctx) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_draw_done, &woken);
    return woken == pdTRUE;
}

// Draw and wait for DMA so the caller can reuse `buf`. Caller holds s_lcd_lock.
static bool lcd_draw(int x0, int y0, int x1, int y1, const uint16_t *buf) {
    if (esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, buf) != ESP_OK) return false;
    xSemaphoreTake(s_draw_done, portMAX_DELAY);
    return true;
}

// The panel takes RGB565 high byte first, the same as the image wire format
// and the animation palette.
static uint16_t lcd_from_be(uint16_t px) {
    return px;
}
#else
// The copy into the frame buffer is done when this returns. Caller holds
// s_lcd_lock.
static bool lcd_draw(int x0, int y0, int x1, int y1, const uint16_t *buf) {
    return esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, buf) == ESP_OK;
}

// The frame buffer holds native (little-endian) RGB565.
static uint16_t lcd_from_be(uint16_t px) {
    return (uint16_t)((px >> 8) | (px << 8));
}
#endif

// A colour in the panel's pixel format.
static uint16_t lcd_px(rgb_t c) {
    uint16_t px = ((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3);
    return lcd_from_be((uint16_t)((px >> 8) | (px << 8)));
}

static uint8_t to_full_scale(uint8_t v) {
    int s = v * 255 / LCD_FULL_LEVEL;
    return s > 255 ? 255 : s;
}

static bool dot_cell_set(int x, int y) {
    return s_dot_rows[y / LCD_ANIM_SCALE] >> (x / LCD_ANIM_SCALE) & 1;
}

static uint16_t dot_px(void) {
    return lcd_px((rgb_t){to_full_scale(COLOR_GREEN.r),
                             to_full_scale(COLOR_GREEN.g),
                             to_full_scale(COLOR_GREEN.b)});
}

// Draw the connected dot in `on`, or erase it. Uses s_bar_buf: LED task only.
static void lcd_draw_dot(bool on) {
    uint16_t px = dot_px();
    for (int y = 0; y < LCD_DOT_SIZE; y++) {
        for (int x = 0; x < LCD_DOT_SIZE; x++) {
            s_bar_buf[y * LCD_DOT_SIZE + x] = on && dot_cell_set(x, y) ? px : 0;
        }
    }
    xSemaphoreTake(s_lcd_lock, portMAX_DELAY);
    if (lcd_draw(LCD_DOT_X, LCD_DOT_Y, LCD_DOT_X + LCD_DOT_SIZE,
                 LCD_DOT_Y + LCD_DOT_SIZE, s_bar_buf)) {
        s_dot_drawn = on;
    }
    xSemaphoreGive(s_lcd_lock);
}

// Paint the status (coloured bars, the connected dot) over the w x h pixels of
// `buf` that land at (x0, y0), so it stays on top of whatever else is drawn
// there. Caller holds s_lcd_lock.
static void lcd_overlay_rect(uint16_t *buf, int x0, int y0, int w, int h) {
    bool bars = s_bars_drawn && memcmp(&s_bar_color, &COLOR_OFF, sizeof(rgb_t)) != 0;
    uint16_t dot = dot_px();
    for (int r = 0; r < h; r++) {
        int y = y0 + r;
        uint16_t *line = buf + r * w;
        if (bars && (y < LCD_BAR_ROWS || y >= LCD_V_RES - LCD_BAR_ROWS)) {
            for (int x = 0; x < w; x++) line[x] = s_bar_px;
        }
        int dy = y - LCD_DOT_Y;
        if (s_dot_drawn && dy >= 0 && dy < LCD_DOT_SIZE) {
            for (int dx = 0; dx < LCD_DOT_SIZE; dx++) {
                int x = LCD_DOT_X + dx - x0;
                if (x >= 0 && x < w && dot_cell_set(dx, dy)) line[x] = dot;
            }
        }
    }
}

static void lcd_overlay_status(uint16_t *buf, int y0, int rows) {
    lcd_overlay_rect(buf, 0, y0, LCD_H_RES, rows);
}

// Blank rows [y0, y1), keeping the status on top. Uses s_anim_buf; caller
// holds s_lcd_lock (or runs before the other tasks exist).
static void lcd_clear_rows(int y0, int y1) {
    for (int y = y0; y < y1; y += LCD_IMAGE_MAX_ROWS) {
        int n = y1 - y < LCD_IMAGE_MAX_ROWS ? y1 - y : LCD_IMAGE_MAX_ROWS;
        memset(s_anim_buf, 0, n * LCD_H_RES * sizeof(uint16_t));
        lcd_overlay_status(s_anim_buf, y, n);
        lcd_draw(0, y, LCD_H_RES, y + n, s_anim_buf);
    }
}

static void led_hw_set_color(rgb_t c) {
    if (!s_panel) return;
    if (s_dot_drawn) lcd_draw_dot(false);
    if (s_bars_drawn && memcmp(&c, &s_bar_color, sizeof(c)) == 0) return;
    uint16_t px = lcd_px((rgb_t){to_full_scale(c.r), to_full_scale(c.g),
                                    to_full_scale(c.b)});
    for (int i = 0; i < LCD_H_RES * LCD_BAR_ROWS; i++) s_bar_buf[i] = px;
    xSemaphoreTake(s_lcd_lock, portMAX_DELAY);
    bool ok = lcd_draw(0, 0, LCD_H_RES, LCD_BAR_ROWS, s_bar_buf) &&
              lcd_draw(0, LCD_V_RES - LCD_BAR_ROWS, LCD_H_RES, LCD_V_RES, s_bar_buf);
    s_bar_color = c;
    s_bar_px = px;
    s_bars_drawn = ok;
    xSemaphoreGive(s_lcd_lock);
}

// Connected: blank the bars and show the green dot instead.
static void led_hw_set_connected(void) {
    if (!s_panel || s_dot_drawn) return;
    led_hw_set_color(COLOR_OFF);
    lcd_draw_dot(true);
}

// Draw `text` centred in the title area, shrinking the pixel size for long
// text and cutting off what still does not fit. Bytes outside printable
// ASCII show as '?'. Skipped while an image is shown. Uses s_bar_buf: LED
// task only.
static void led_hw_set_title(const char *text) {
    if (!s_panel) return;
    const int adv = PIXEL_FONT_WIDTH + 1;
    int n = (int)strlen(text);
    int scale = LCD_TITLE_MAX_SCALE;
    while (scale > 2 && n * adv * scale - scale > LCD_H_RES) scale--;
    if (n > (LCD_H_RES + scale) / (adv * scale)) n = (LCD_H_RES + scale) / (adv * scale);
    int w = n > 0 ? n * adv * scale - scale : 0;
    int x0 = (LCD_H_RES - w) / 2;
    int y0 = (LCD_TITLE_ROWS - PIXEL_FONT_HEIGHT * scale) / 2;
    uint16_t fg = lcd_px((rgb_t){0xff, 0xee, 0xde});  // animation's cream

    for (int sy = 0; sy < LCD_TITLE_ROWS; sy += LCD_TITLE_STRIPE_ROWS) {
        for (int r = 0; r < LCD_TITLE_STRIPE_ROWS; r++) {
            uint16_t *line = s_bar_buf + r * LCD_H_RES;
            int fy = sy + r - y0;
            for (int x = 0; x < LCD_H_RES; x++) {
                int fx = x - x0;
                bool on = false;
                if (fy >= 0 && fy < PIXEL_FONT_HEIGHT * scale && fx >= 0 && fx < w) {
                    int col = fx / scale;
                    int gx = col % adv;
                    unsigned char ch = (unsigned char)text[col / adv];
                    if (ch < PIXEL_FONT_FIRST || ch > PIXEL_FONT_LAST) ch = '?';
                    on = gx < PIXEL_FONT_WIDTH &&
                         (pixel_font[ch - PIXEL_FONT_FIRST][gx] >> (fy / scale) & 1);
                }
                line[x] = on ? fg : 0;
            }
        }
        xSemaphoreTake(s_lcd_lock, portMAX_DELAY);
        if (!s_image_mode) {
            lcd_draw(0, LCD_TITLE_Y + sy, LCD_H_RES,
                     LCD_TITLE_Y + sy + LCD_TITLE_STRIPE_ROWS, s_bar_buf);
        }
        xSemaphoreGive(s_lcd_lock);
    }
}

// Skipped while an image is shown. s_anim_buf is shared with image drawing, so
// each stripe is filled and sent under s_lcd_lock.
static void lcd_draw_anim_frame(const uint8_t *cells) {
    const int w = HAPPY_ANIM_WIDTH * LCD_ANIM_SCALE;
    for (int cy = 0; cy < HAPPY_ANIM_HEIGHT; cy += LCD_ANIM_STRIPE_CELLS) {
        int rows = HAPPY_ANIM_HEIGHT - cy;
        if (rows > LCD_ANIM_STRIPE_CELLS) rows = LCD_ANIM_STRIPE_CELLS;
        xSemaphoreTake(s_lcd_lock, portMAX_DELAY);
        if (s_image_mode) {
            xSemaphoreGive(s_lcd_lock);
            return;
        }
        // Expand each cell to a solid LCD_ANIM_SCALE square.
        for (int r = 0; r < rows; r++) {
            uint16_t *line = s_anim_buf + r * LCD_ANIM_SCALE * w;
            const uint8_t *src = cells + (cy + r) * HAPPY_ANIM_WIDTH;
            for (int cx = 0; cx < HAPPY_ANIM_WIDTH; cx++) {
                uint16_t px = lcd_from_be(happy_anim_palette[src[cx]]);
                for (int k = 0; k < LCD_ANIM_SCALE; k++) line[cx * LCD_ANIM_SCALE + k] = px;
            }
            for (int k = 1; k < LCD_ANIM_SCALE; k++) {
                memcpy(line + k * w, line, w * sizeof(uint16_t));
            }
        }
        int y = LCD_ANIM_Y + cy * LCD_ANIM_SCALE;
        lcd_draw(LCD_ANIM_X, y, LCD_ANIM_X + w, y + rows * LCD_ANIM_SCALE, s_anim_buf);
        xSemaphoreGive(s_lcd_lock);
    }
}

static void anim_task(void *arg) {
    TickType_t wake = xTaskGetTickCount();
    for (int frame = 0;; frame = (frame + 1) % HAPPY_ANIM_FRAMES) {
        lcd_draw_anim_frame(happy_anim_frames[frame]);
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(HAPPY_ANIM_FRAME_MS));
    }
}

#if CONFIG_HOMEHUB_LED_BACKEND_IDEASPARK_ST7789
static esp_err_t lcd_panel_init(void) {
    s_draw_done = xSemaphoreCreateBinary();
    if (!s_draw_done) return ESP_ERR_NO_MEM;
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_ANIM_BUF_PIXELS * sizeof(uint16_t),
    };
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 4,
        .on_color_trans_done = lcd_draw_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    esp_err_t err = spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io);
    }
    if (err == ESP_OK) err = esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_invert_color(s_panel, true);
    if (err == ESP_OK) err = esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, 0);
    return err;
}

static esp_err_t lcd_panel_on(void) {
    return esp_lcd_panel_disp_on_off(s_panel, true);
}
#else
typedef struct {
    uint8_t cmd, len;
    uint16_t delay_ms;
    uint8_t data[16];
} st7701_cmd_t;

// Seeed's ST7701S setup for the SenseCAP Indicator's panel (lcd_panel_config.c,
// "GX" screen): vendor gamma and power settings, 18-bit pixels, inversion on,
// then sleep out and display on.
static const st7701_cmd_t s_st7701_init[] = {
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x10}},
    {0xC0, 2, 0, {0x3B, 0x00}},
    {0xC1, 2, 0, {0x0D, 0x02}},
    {0xC2, 2, 0, {0x31, 0x05}},
    {0xC7, 1, 0, {0x04}},
    {0xCD, 1, 0, {0x08}},
    {0xB0, 16, 0, {0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}},
    {0xB1, 16, 0, {0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x11}},
    {0xB0, 1, 0, {0x60}},
    {0xB1, 1, 0, {0x32}},
    {0xB2, 1, 0, {0x07}},
    {0xB3, 1, 0, {0x80}},
    {0xB5, 1, 0, {0x49}},
    {0xB7, 1, 0, {0x85}},
    {0xB8, 1, 0, {0x21}},
    {0xC1, 1, 0, {0x78}},
    {0xC2, 1, 20, {0x78}},
    {0xE0, 3, 0, {0x00, 0x1B, 0x02}},
    {0xE1, 11, 0, {0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44}},
    {0xE2, 12, 0, {0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00}},
    {0xE3, 4, 0, {0x00, 0x00, 0x11, 0x11}},
    {0xE4, 2, 0, {0x44, 0x44}},
    {0xE5, 16, 0, {0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}},
    {0xE6, 4, 0, {0x00, 0x00, 0x11, 0x11}},
    {0xE7, 2, 0, {0x44, 0x44}},
    {0xE8, 16, 0, {0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}},
    {0xEB, 7, 0, {0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}},
    {0xEC, 2, 0, {0x3C, 0x00}},
    {0xED, 16, 0, {0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}},
    {0x36, 1, 0, {0x10}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x13}},
    {0xE5, 1, 0, {0xE4}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x00}},
    {0x3A, 1, 0, {0x60}},
    {0x21, 0, 0, {0}},
    {0x11, 0, 120, {0}},
    {0x29, 0, 120, {0}},
};

static i2c_master_dev_handle_t s_exp = NULL;
static uint8_t s_exp_out;

static esp_err_t exp_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_exp, buf, sizeof(buf), 100);
}

static esp_err_t exp_read(uint8_t reg, uint8_t *val) {
    return i2c_master_transmit_receive(s_exp, &reg, 1, val, 1, 100);
}

static esp_err_t exp_set(uint8_t bits, bool high) {
    s_exp_out = high ? (s_exp_out | bits) : (s_exp_out & ~bits);
    return exp_write(EXP_REG_OUT0, s_exp_out);
}

// Take over the LCD's chip select and reset, both driven high, and keep the
// other port 0 pins as they are.
static esp_err_t exp_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = LCD_PIN_I2C_SDA,
        .scl_io_num = LCD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) return err;
    uint16_t addr = i2c_master_probe(bus, EXP_ADDR, 100) == ESP_OK ? EXP_ADDR : EXP_ADDR_ALT;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_exp);
    uint8_t cfg = 0xff;
    if (err == ESP_OK) err = exp_read(EXP_REG_OUT0, &s_exp_out);
    if (err == ESP_OK) err = exp_read(EXP_REG_CFG0, &cfg);
    if (err == ESP_OK) err = exp_set(EXP_LCD_CS | EXP_LCD_RST, true);
    if (err == ESP_OK) err = exp_write(EXP_REG_CFG0, cfg & ~(EXP_LCD_CS | EXP_LCD_RST));
    if (err != ESP_OK) ESP_LOGE(TAG, "TCA9535 at 0x%02x: %s", addr, esp_err_to_name(err));
    return err;
}

// One 9-bit frame, MSB first, sampled on the rising clock edge. The first
// bit is 0 for a command, 1 for a parameter.
static void st7701_send9(bool param, uint8_t byte) {
    uint16_t word = (param ? 0x100 : 0) | byte;
    for (int bit = 8; bit >= 0; bit--) {
        gpio_set_level(LCD_PIN_SPI_MOSI, (word >> bit) & 1);
        esp_rom_delay_us(2);
        gpio_set_level(LCD_PIN_SPI_SCLK, 1);
        esp_rom_delay_us(2);
        gpio_set_level(LCD_PIN_SPI_SCLK, 0);
    }
}

static esp_err_t st7701_setup(void) {
    gpio_config_t spi_cfg = {
        .pin_bit_mask = (1ULL << LCD_PIN_SPI_SCLK) | (1ULL << LCD_PIN_SPI_MOSI),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&spi_cfg);
    if (err != ESP_OK) return err;
    gpio_set_level(LCD_PIN_SPI_SCLK, 0);

    err = exp_set(EXP_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (err == ESP_OK) err = exp_set(EXP_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(120));
    for (size_t i = 0; err == ESP_OK && i < sizeof(s_st7701_init) / sizeof(s_st7701_init[0]); i++) {
        const st7701_cmd_t *c = &s_st7701_init[i];
        err = exp_set(EXP_LCD_CS, false);
        if (err != ESP_OK) break;
        st7701_send9(false, c->cmd);
        for (int k = 0; k < c->len; k++) st7701_send9(true, c->data[k]);
        err = exp_set(EXP_LCD_CS, true);
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }
    return err;
}

static esp_err_t lcd_panel_init(void) {
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_back_porch = 50,
            .hsync_front_porch = 10,
            .hsync_pulse_width = 8,
            .vsync_back_porch = 20,
            .vsync_front_porch = 10,
            .vsync_pulse_width = 8,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .bounce_buffer_size_px = LCD_H_RES * LCD_BOUNCE_LINES,
        .hsync_gpio_num = LCD_PIN_HSYNC,
        .vsync_gpio_num = LCD_PIN_VSYNC,
        .de_gpio_num = LCD_PIN_DE,
        .pclk_gpio_num = LCD_PIN_PCLK,
        .disp_gpio_num = -1,
        // B0-B4, G0-G5, R0-R4.
        .data_gpio_nums = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0},
        .flags.fb_in_psram = 1,
    };
    esp_err_t err = exp_init();
    if (err == ESP_OK) err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(s_panel);
    // Seeed sets up the controller once the RGB clock is running.
    if (err == ESP_OK) err = st7701_setup();
    return err;
}

// The setup sequence already turned the display on.
static esp_err_t lcd_panel_on(void) {
    return ESP_OK;
}
#endif

static bool led_hw_init(void) {
    s_bar_buf = heap_caps_malloc(LCD_H_RES * LCD_BAR_ROWS * sizeof(uint16_t), LCD_BUF_CAPS);
    s_anim_buf = heap_caps_malloc(LCD_ANIM_BUF_PIXELS * sizeof(uint16_t), LCD_BUF_CAPS);
    s_lcd_lock = xSemaphoreCreateMutex();
    if (!s_bar_buf || !s_anim_buf || !s_lcd_lock) {
        ESP_LOGE(TAG, "LCD buffer alloc failed");
        return false;
    }

    esp_err_t err = lcd_panel_init();
    // Black out everything between the bars; the animation covers only part of it.
    if (err == ESP_OK) lcd_clear_rows(LCD_BAR_ROWS, LCD_V_RES - LCD_BAR_ROWS);
    if (err == ESP_OK) err = lcd_panel_on();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, LCD_NAME " init failed: %s", esp_err_to_name(err));
        s_panel = NULL;
        return false;
    }

    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(LCD_PIN_BL, 1);

    // Lower priority than the LED task so status changes are never delayed.
    xTaskCreate(anim_task, "lcd_anim", 2560, NULL, 1, NULL);

    ESP_LOGI(TAG, "LED status ready: " LCD_NAME " %dx%d display (BL=%d)",
             LCD_H_RES, LCD_V_RES, LCD_PIN_BL);
    return true;
}
#elif CONFIG_HOMEHUB_LED_BACKEND_MUSE
// Boards with the full UI show Link's status on screen instead of an LED.
static void led_hw_set_color(rgb_t c) {
    (void)c;
}

static bool led_hw_init(void) {
    ESP_LOGI(TAG, "LED status ready: Muse display");
    return true;
}
#else
static void led_hw_set_color(rgb_t c) {
    (void)c;
}

static bool led_hw_init(void) {
    ESP_LOGI(TAG, "LED status disabled");
    return true;
}
#endif

#if !CONFIG_HOMEHUB_DISPLAY
#if !CONFIG_HOMEHUB_LED_BACKEND_VOICE_RING
static void led_hw_set_connected(void) {
    led_hw_set_color(COLOR_GREEN);
}
#endif

static void led_hw_set_title(const char *text) {
    (void)text;
}
#endif

static void led_hw_set_dimmed(rgb_t c, float level) {
    led_hw_set_color((rgb_t){c.r * level, c.g * level, c.b * level});
}

static void blink(rgb_t c, int on_ms, int off_ms) {
    led_hw_set_color(c);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    led_hw_set_color(COLOR_OFF);
    vTaskDelay(pdMS_TO_TICKS(off_ms));
}

static void breathe(rgb_t c, int period_ms) {
    int steps = 30;
    int step_ms = period_ms / (steps * 2);
    for (int i = 0; i < steps; i++) {
        led_hw_set_dimmed(c, (float)i / steps);
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
    for (int i = steps; i > 0; i--) {
        led_hw_set_dimmed(c, (float)i / steps);
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
}

// Wait between steady frames. A voice state change cuts the wait short.
static void led_wait(int ms) {
#if CONFIG_HOMEHUB_VOICE
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
#else
    vTaskDelay(pdMS_TO_TICKS(ms));
#endif
}

#if CONFIG_HOMEHUB_VOICE
#define VOICE_FRAME_MS 40
// Three 200 ms flashes.
#define VOICE_ERROR_FRAMES 30
// How long the volume stays up after the dial stops.
#define VOICE_VOLUME_MS 1500

static rgb_t scaled(rgb_t c, float level) {
    return (rgb_t){c.r * level, c.g * level, c.b * level};
}

// A bright head going clockwise, one LED per two frames, with a fading tail.
static void ring_comet(rgb_t *px, int frame, rgb_t c, int tail) {
    int head = (frame / 2) % RING_LEDS;
    for (int k = 0; k < tail; k++) {
        px[(head - k + RING_LEDS) % RING_LEDS] = scaled(c, (float)(tail - k) / tail);
    }
}

// The volume as a white arc growing clockwise from the top, over a faint ring;
// the last LED of the arc lights partly.
static void ring_volume(rgb_t *px, int percent) {
    static const rgb_t white = {50, 50, 50};
    float lit = percent * RING_LEDS / 100.0f;
    for (int i = 0; i < RING_LEDS; i++) {
        float f = lit - i;
        px[i] = scaled(white, f >= 1 ? 1 : f > 0 ? f : 0.06f);
    }
}

// Draw one frame of the voice state, if there is one. Returns false when the
// ring should show the connection status instead.
static bool voice_render(void) {
    static led_voice_t last = LED_VOICE_IDLE;
    static int frame = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_voice_t voice = s_voice;
    float level = s_level;
    int volume = s_volume;
    bool show_volume = esp_timer_get_time() < s_volume_until;
    xSemaphoreGive(s_mutex);
    if (show_volume) {
        // Over any state; the state's animation picks up where it was.
        rgb_t px[RING_LEDS];
        ring_volume(px, volume);
        ring_show(px);
        led_wait(VOICE_FRAME_MS);
        return true;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (voice == LED_VOICE_ERROR && last == LED_VOICE_ERROR && frame >= VOICE_ERROR_FRAMES) {
        voice = s_voice = LED_VOICE_IDLE;
    }
    xSemaphoreGive(s_mutex);
    if (voice != last) frame = 0;
    last = voice;
    if (voice == LED_VOICE_IDLE) return false;

    rgb_t px[RING_LEDS] = {0};
    switch (voice) {
        case LED_VOICE_LISTENING:
            // A level meter growing from the top down both sides, over a
            // faint ring that shows the microphone is open.
            for (int i = 0; i < RING_LEDS; i++) {
                int from_top = i <= RING_LEDS / 2 ? i : RING_LEDS - i;
                px[i] = from_top < level * (RING_LEDS / 2 + 1) ? COLOR_BLUE
                                                                : scaled(COLOR_BLUE, 0.08f);
            }
            break;
        case LED_VOICE_TRANSCRIBING:
            ring_comet(px, frame, (rgb_t){80, 44, 0}, 5);
            break;
        case LED_VOICE_THINKING:
            ring_comet(px, frame, (rgb_t){47, 0, 80}, 7);
            break;
        case LED_VOICE_BUFFERING:
            ring_comet(px, frame, COLOR_GREEN, 5);
            break;
        case LED_VOICE_SPEAKING: {
            float breath = 0.5f - 0.5f * cosf(2 * (float)M_PI * frame / 50);
            for (int i = 0; i < RING_LEDS; i++) {
                px[i] = scaled(COLOR_GREEN, 0.2f + 0.8f * breath);
            }
            break;
        }
        case LED_VOICE_ERROR:
            if ((frame / 5) % 2 == 0) {
                for (int i = 0; i < RING_LEDS; i++) px[i] = COLOR_RED;
            }
            break;
        default:
            break;
    }
    ring_show(px);
    frame++;
    vTaskDelay(pdMS_TO_TICKS(VOICE_FRAME_MS));
    return true;
}
#endif

static void led_task(void *arg) {
    stack_monitor_t stack = STACK_MONITOR_INIT;
    while (1) {
        stack_monitor_poll(&stack);
        char title[sizeof(s_title)];
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        led_state_t state = s_state;
        bool title_dirty = s_title_dirty;
        if (title_dirty) memcpy(title, s_title, sizeof(title));
        s_title_dirty = false;
        xSemaphoreGive(s_mutex);
        if (title_dirty) led_hw_set_title(title);
#if CONFIG_HOMEHUB_VOICE
        if (voice_render()) continue;
#endif

        switch (state) {
            case LED_STATE_BOOT:
                // Orange at boot — waiting for connectivity.
                led_hw_set_color(COLOR_ORANGE);
                led_wait(200);
                break;
            case LED_STATE_SETUP_IDLE:
                led_hw_set_color(COLOR_DIM_ORANGE);
                led_wait(200);
                break;
            case LED_STATE_BLE_ADVERTISING:
                breathe(COLOR_ORANGE, 2000);
                break;
            case LED_STATE_BLE_CONNECTED:
                led_hw_set_color(COLOR_ORANGE);
                led_wait(200);
                break;
            case LED_STATE_PAIRING_CONFIRM_REQUIRED:
                breathe(COLOR_BLUE, 2000);
                break;
            case LED_STATE_WIFI_CONNECTING:
            case LED_STATE_WIFI_CONNECTED:
                // Solid blue — confirmed and working through Wi-Fi/auth/VM
                // setup. confirm_required pulses; green means the tunnel is up.
                led_hw_set_color(COLOR_BLUE);
                led_wait(200);
                break;
            case LED_STATE_AUTH_OK:
            case LED_STATE_VM_OK:
                // Solid blue — WiFi/auth is accepted, VM/control path is coming up.
                led_hw_set_color(COLOR_BLUE);
                led_wait(200);
                break;
            case LED_STATE_WS_CONNECTED:
                // Green — tunnel is live (a green dot on the display).
                led_hw_set_connected();
                led_wait(200);
                break;
            case LED_STATE_VM_SWITCHING:
                blink(COLOR_YELLOW, 150, 150);
                break;
            case LED_STATE_WS_DISCONNECTED:
                blink(COLOR_YELLOW, 500, 500);
                break;
            case LED_STATE_UNPAIRED:
                led_hw_set_color(COLOR_PURPLE);
                led_wait(200);
                break;
            case LED_STATE_ERROR:
                blink(COLOR_RED, 200, 200);
                break;
        }
    }
}

bool led_status_init(void) {
    if (!led_hw_init()) {
        return false;
    }

#if CONFIG_HOMEHUB_LED_BACKEND_NONE || CONFIG_HOMEHUB_LED_BACKEND_MUSE
    return true;
#endif

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return false;

    led_hw_set_color(COLOR_ORANGE);
    // 2026-09-20: increase margin; measured only 904 bytes free with a 2048-byte stack.
#if CONFIG_HOMEHUB_LED_BACKEND_VOICE_RING
    // The RMT ring driver takes about 2 KB of the task's stack per frame.
    const uint32_t stack = 4096;
#else
    const uint32_t stack = 3072;
#endif
    xTaskCreate(led_task, "led", stack, NULL, 2, &s_task);
    return true;
}

void led_status_set_state(led_state_t state) {
#if CONFIG_HOMEHUB_LED_BACKEND_MUSE
    muse_glue_led_state(state);
    return;
#endif
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = state;
    xSemaphoreGive(s_mutex);
}

void led_status_set_title(const char *title) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
    s_title_dirty = true;
    xSemaphoreGive(s_mutex);
}

#if CONFIG_HOMEHUB_VOICE
void led_status_set_voice(led_voice_t voice) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_voice = voice;
    xSemaphoreGive(s_mutex);
    if (s_task) xTaskNotifyGive(s_task);
}

void led_status_set_level(float level) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_level = level < 0 ? 0 : level > 1 ? 1 : level;
    xSemaphoreGive(s_mutex);
}

void led_status_show_volume(int percent) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    s_volume_until = esp_timer_get_time() + VOICE_VOLUME_MS * 1000LL;
    xSemaphoreGive(s_mutex);
    if (s_task) xTaskNotifyGive(s_task);
}
#endif

bool led_status_display_info(int *width, int *height) {
#if CONFIG_HOMEHUB_DISPLAY
    if (!s_panel) return false;
    *width = LCD_H_RES;
    *height = LCD_V_RES;
    return true;
#elif CONFIG_HOMEHUB_LED_BACKEND_MUSE && CONFIG_HOMEHUB_DISPLAY_COMMANDS
    return muse_ui_image_size(width, height);
#else
    (void)width;
    (void)height;
    return false;
#endif
}

int led_status_display_bits(void) {
#if CONFIG_HOMEHUB_DISPLAY
    return s_panel ? 16 : 0;
#elif CONFIG_HOMEHUB_LED_BACKEND_MUSE && CONFIG_HOMEHUB_DISPLAY_COMMANDS
    int w, h;
    return muse_ui_image_size(&w, &h) ? 16 : 0;
#else
    return 0;
#endif
}

void led_status_draw_done(void) {
}

#if CONFIG_HOMEHUB_DISPLAY
// Draw w x h image pixels (RGB565, high byte first) at (x, y) with the status
// on top. The first draw replaces the animation and title with a blank screen.
static bool lcd_draw_image_rect(int x, int y, int w, int h, const void *pixels) {
    xSemaphoreTake(s_lcd_lock, portMAX_DELAY);
    if (!s_image_mode) {
        s_image_mode = true;
        lcd_clear_rows(0, LCD_V_RES);
    }
#if CONFIG_HOMEHUB_LED_BACKEND_IDEASPARK_ST7789
    // Already in the panel's format; copied only to reach DMA memory.
    memcpy(s_anim_buf, pixels, (size_t)w * h * sizeof(uint16_t));
#else
    const uint8_t *src = pixels;
    for (int i = 0; i < w * h; i++) s_anim_buf[i] = (uint16_t)(src[2 * i] << 8 | src[2 * i + 1]);
#endif
    lcd_overlay_rect(s_anim_buf, x, y, w, h);
    bool ok = lcd_draw(x, y, x + w, y + h, s_anim_buf);
    xSemaphoreGive(s_lcd_lock);
    return ok;
}
#endif

bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels) {
#if CONFIG_HOMEHUB_DISPLAY
    if (!s_panel || x < 0 || y < 0 || w <= 0 || h <= 0
        || x + w > LCD_H_RES || y + h > LCD_V_RES || w * h > LCD_ANIM_BUF_PIXELS) {
        return false;
    }
    return lcd_draw_image_rect(x, y, w, h, pixels);
#elif CONFIG_HOMEHUB_LED_BACKEND_MUSE && CONFIG_HOMEHUB_DISPLAY_COMMANDS
    return muse_ui_image_draw(x, y, w, h, pixels);
#else
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)pixels;
    return false;
#endif
}

void led_status_show_animation(void) {
#if CONFIG_HOMEHUB_DISPLAY
    if (!s_panel || !s_mutex) return;
    xSemaphoreTake(s_lcd_lock, portMAX_DELAY);
    bool was_image = s_image_mode;
    if (was_image) {
        s_image_mode = false;
        lcd_clear_rows(0, LCD_V_RES);
    }
    xSemaphoreGive(s_lcd_lock);
    if (!was_image) return;
    // The animation resumes by itself; the title needs a redraw.
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_title_dirty = true;
    xSemaphoreGive(s_mutex);
#elif CONFIG_HOMEHUB_LED_BACKEND_MUSE && CONFIG_HOMEHUB_DISPLAY_COMMANDS
    muse_ui_image_hide();
#endif
}
