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

// led_status.h on the Xteink X3's 3.7 inch e-paper: 792x528, black and white,
// a UC8253 controller on SPI. Like epaper_status.c it shows a still status
// screen (the agent's name, the character and a line of status text) and
// redraws it only when the text changes. It also shows a text message
// (x3_display_show_text, the display.show_text command).
//
// The X3 is held upright, so everything is drawn 528 wide and 792 tall and
// turned into the panel's rows. There is no PSRAM: the only frame is 1 bit per
// pixel (52 KB), images are dithered with an ordered pattern as they arrive,
// and the controller itself holds the last frame for fast refreshes, so no copy
// of it is kept.
//
// Pins, controller init, the full-refresh waveform and the controller probe:
// the FreeInk SDK (github.com/Free-Ink/freeink-sdk, MIT), BoardConfig.h
// XTEINK_X3, Uc8253X3Driver.cpp, Uc8253X3Luts.h and XteinkDetect.cpp.
// Units made after about July 2026 carry a UC8279d instead, which this does
// not drive yet; the probe reports it and the screen is left alone.

#include "led_status.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "happy_anim.h"
#include "pixel_font.h"
#include "sdkconfig.h"
#include "stack_monitor.h"
#include "x3_display.h"
#include "x3_pages.h"

static const char *TAG = "link.led";

#define EPD_HOST        SPI2_HOST
#define EPD_PIN_SCLK    8
#define EPD_PIN_MOSI    10
#define EPD_PIN_CS      21
#define EPD_PIN_DC      4
#define EPD_PIN_RST     5
// Drops low when a command starts and returns high when it is done.
#define EPD_PIN_BUSY    6
#define EPD_SPI_HZ      (10 * 1000 * 1000)
// The panel's own rows and columns.
#define EPD_W           792
#define EPD_H           528
#define EPD_ROW_BYTES   (EPD_W / 8)
#define EPD_FRAME_BYTES (EPD_ROW_BYTES * EPD_H)
// What is drawn on: the panel turned upright.
#define SCR_W           EPD_H
#define SCR_H           EPD_W
// Frame data goes out through a DMA buffer of whole rows.
#define EPD_CHUNK_ROWS  40
#define EPD_CHUNK_BYTES (EPD_CHUNK_ROWS * EPD_ROW_BYTES)
#define EPD_BUSY_TIMEOUT_MS 10000

// A status change waits this long for the next, so that the burst while
// connecting costs one refresh.
#define STATUS_SETTLE_MS 1500

#define TITLE_Y          56
#define TITLE_MAX_SCALE  6
#define ANIM_SCALE       6
#define ANIM_W           (HAPPY_ANIM_WIDTH * ANIM_SCALE)
#define ANIM_X           ((SCR_W - ANIM_W) / 2)
#define ANIM_Y           190
#define STATUS_Y         660
#define STATUS_SCALE     3
#define TEXT_SCALE       3
#define TEXT_MARGIN      24
#define TEXT_TOP         120
#define FOOTER_Y         (SCR_H - 44)
#define TEXT_LINE_H      ((PIXEL_FONT_HEIGHT + 3) * TEXT_SCALE)

// FreeInk's lut_x3_*_full bank (the OEM full refresh); the controller takes
// 42 bytes of each.
#define LUT_LEN 42
static const uint8_t s_lut_vcom[LUT_LEN] = {0x00, 0x18, 0x04, 0x0E, 0x0A, 0x01, 0x00, 0x0A,
                                            0x00, 0x00, 0x00, 0x01};
static const uint8_t s_lut_ww[LUT_LEN] = {0x4A, 0x18, 0x04, 0x0E, 0x0A, 0x01, 0x00, 0x0A,
                                          0x00, 0x00, 0x00, 0x01};
static const uint8_t s_lut_bw[LUT_LEN] = {0x0A, 0x18, 0x04, 0x0E, 0x0A, 0x01, 0x00, 0x0A,
                                          0x00, 0x00, 0x00, 0x01};
static const uint8_t s_lut_wb[LUT_LEN] = {0x04, 0x18, 0x04, 0x0E, 0x0A, 0x01, 0x40, 0x0A,
                                          0x00, 0x00, 0x00, 0x01};
static const uint8_t s_lut_bb[LUT_LEN] = {0x84, 0x18, 0x04, 0x0E, 0x0A, 0x01, 0x40, 0x0A,
                                          0x00, 0x00, 0x00, 0x01};

// FreeInk's lut_x3_*_fast bank: full voltages for a shorter time, driving only
// the pixels that differ from the old frame in the controller. No flash, and a
// faint ghost that the next full refresh clears.
static const uint8_t s_fast_vcom[LUT_LEN] = {0x00, 0x04, 0x02, 0x04, 0x04, 0x01, 0x00, 0x04,
                                             0x01, 0x00, 0x00, 0x01};
static const uint8_t s_fast_ww[LUT_LEN] = {0x20, 0x04, 0x02, 0x04, 0x04, 0x01, 0x00, 0x04,
                                           0x01, 0x00, 0x00, 0x01};
static const uint8_t s_fast_bw[LUT_LEN] = {0xAA, 0x04, 0x02, 0x04, 0x04, 0x01, 0x80, 0x04,
                                           0x01, 0x00, 0x00, 0x01};
static const uint8_t s_fast_wb[LUT_LEN] = {0x55, 0x04, 0x02, 0x04, 0x04, 0x01, 0x40, 0x04,
                                           0x01, 0x00, 0x00, 0x01};
static const uint8_t s_fast_bb[LUT_LEN] = {0x10, 0x04, 0x02, 0x04, 0x04, 0x01, 0x00, 0x04,
                                           0x01, 0x00, 0x00, 0x01};

// Fast refreshes leave a faint ghost of the old picture; every so often a
// refresh is full, which flashes but clears it.
#define FULL_REFRESH_EVERY 8

static spi_device_handle_t s_spi;
static uint8_t *s_chunk;  // DMA buffer for SPI writes
static uint8_t *s_frame;  // 1 bit per pixel, panel order, 1 for white
static bool s_ready;
// The controller holds the shown frame as its old one, so the next refresh can
// be fast. False until the first full refresh. Guarded by s_panel_lock.
static bool s_synced;
static int s_fast_refreshes;

// Guards the panel, held through a refresh.
static SemaphoreHandle_t s_panel_lock;
// Guards s_frame and the drawn-state below.
static SemaphoreHandle_t s_lock;
static bool s_image_mode;    // an image or a text message replaces the status screen
static bool s_image_dirty;   // drawn since the last refresh
static bool s_status_drawn;  // the status screen shows s_drawn_*
static const char *s_drawn_label;
static char s_drawn_title[48];

// Requested by led_status_set_state() and _set_title(); guarded by s_mutex.
static SemaphoreHandle_t s_mutex;
static led_state_t s_state = LED_STATE_BOOT;
static char s_title[48];
static TaskHandle_t s_task;
// A text screen asked for by x3_display_show_text() or _show_page(), drawn by
// the task so that callers never wait for the panel; guarded by s_mutex. The
// newest request wins.
#define TEXT_MAX 640
static bool s_text_pending;
static bool s_text_footer;
static char s_text_title[48];
static char s_text_body[TEXT_MAX];
static char s_text_foot[24];

// ---- Pixels -----------------------------------------------------------------

// Gray level of a native RGB565 pixel, 0 (black) to 255 (white).
static inline uint8_t luma565(uint16_t px) {
    int r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
    r = r << 3 | r >> 2;
    g = g << 2 | g >> 4;
    b = b << 3 | b >> 2;
    return (uint8_t)((77 * r + 150 * g + 29 * b + 128) >> 8);
}

// Black or white for a gray level at (x, y), by a 4x4 ordered pattern: no
// error rows to keep, so an image can arrive a few rows at a time.
static inline bool dither_white(uint8_t gray, int x, int y) {
    static const uint8_t bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    return gray > bayer[y & 3][x & 3] * 16 + 8;
}

// Upright (x, y) to the panel: CrossPoint Reader's portrait mapping.
static inline void set_pixel(int x, int y, bool white) {
    int px = y, py = EPD_H - 1 - x;
    uint8_t *byte = s_frame + (size_t)py * EPD_ROW_BYTES + px / 8;
    uint8_t bit = (uint8_t)(0x80 >> (px & 7));
    if (white) *byte |= bit;
    else *byte &= (uint8_t)~bit;
}

static void fill_black(int x, int y, int w, int h) {
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) set_pixel(x + c, y + r, false);
    }
}

// One line of `n` characters in black at (x0, y).
static void draw_chars(const char *text, int n, int x0, int y, int scale) {
    const int adv = PIXEL_FONT_WIDTH + 1;
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < PIXEL_FONT_FIRST || ch > PIXEL_FONT_LAST) ch = '?';
        const uint8_t *glyph = pixel_font[ch - PIXEL_FONT_FIRST];
        for (int gx = 0; gx < PIXEL_FONT_WIDTH; gx++) {
            for (int gy = 0; gy < PIXEL_FONT_HEIGHT; gy++) {
                if (!(glyph[gx] >> gy & 1)) continue;
                fill_black(x0 + (i * adv + gx) * scale, y + gy * scale, scale, scale);
            }
        }
    }
}

// Draw `text` centred on the row at `y`, in the largest pixel size up to
// `max_scale` that fits, cutting off what still does not fit at size 2.
static void draw_text(const char *text, int y, int max_scale) {
    const int adv = PIXEL_FONT_WIDTH + 1;
    int n = (int)strlen(text);
    int scale = max_scale;
    while (scale > 2 && n * adv * scale - scale > SCR_W) scale--;
    if (n > (SCR_W + scale) / (adv * scale)) n = (SCR_W + scale) / (adv * scale);
    int x0 = (SCR_W - (n * adv * scale - scale)) / 2;
    y += (max_scale - scale) * PIXEL_FONT_HEIGHT / 2;
    draw_chars(text, n, x0, y, scale);
}

// Draw `text` from `y` down to `bottom`, left-aligned, breaking lines at spaces
// and at newlines. What does not fit is left out.
static void draw_wrapped(const char *text, int y, int bottom) {
    const int adv = (PIXEL_FONT_WIDTH + 1) * TEXT_SCALE;
    const int cols = (SCR_W - 2 * TEXT_MARGIN) / adv;
    while (*text && y + PIXEL_FONT_HEIGHT * TEXT_SCALE <= bottom) {
        while (*text == ' ') text++;
        int n = 0, last_space = -1;
        while (text[n] && text[n] != '\n' && n < cols) {
            if (text[n] == ' ') last_space = n;
            n++;
        }
        int take = n;
        if (text[n] && text[n] != '\n' && text[n] != ' ' && last_space > 0) take = last_space;
        draw_chars(text, take, TEXT_MARGIN, y, TEXT_SCALE);
        text += take;
        if (*text == '\n') text++;
        y += TEXT_LINE_H;
    }
}

// The character, still: the first frame of the animation, with its black
// background as paper.
static void draw_character(void) {
    const uint8_t *cells = happy_anim_frames[0];
    for (int cy = 0; cy < HAPPY_ANIM_HEIGHT; cy++) {
        for (int cx = 0; cx < HAPPY_ANIM_WIDTH; cx++) {
            uint8_t c = cells[cy * HAPPY_ANIM_WIDTH + cx];
            if (c == 0) continue;
            uint16_t be = happy_anim_palette[c];
            uint8_t gray = luma565((uint16_t)(be >> 8 | be << 8));
            for (int r = 0; r < ANIM_SCALE; r++) {
                for (int k = 0; k < ANIM_SCALE; k++) {
                    int x = ANIM_X + cx * ANIM_SCALE + k, y = ANIM_Y + cy * ANIM_SCALE + r;
                    set_pixel(x, y, dither_white(gray, x, y));
                }
            }
        }
    }
}

// ---- Panel ------------------------------------------------------------------

static esp_err_t epd_send(bool data, const uint8_t *buf, size_t len) {
    gpio_set_level(EPD_PIN_DC, data);
    while (len) {
        size_t n = len < EPD_CHUNK_BYTES ? len : EPD_CHUNK_BYTES;
        if (buf != s_chunk) memcpy(s_chunk, buf, n);
        spi_transaction_t t = {.length = n * 8, .tx_buffer = s_chunk};
        esp_err_t err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) return err;
        buf += n;
        len -= n;
    }
    return ESP_OK;
}

// A command and its data, each in its own chip-select pulse as FreeInk does.
static esp_err_t epd_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    gpio_set_level(EPD_PIN_CS, 0);
    esp_err_t err = epd_send(false, &cmd, 1);
    gpio_set_level(EPD_PIN_CS, 1);
    if (err == ESP_OK && len) {
        gpio_set_level(EPD_PIN_CS, 0);
        err = epd_send(true, data, len);
        gpio_set_level(EPD_PIN_CS, 1);
    }
    return err;
}

// BUSY drops low once the command starts (not at all for a redundant one),
// then returns high.
static esp_err_t epd_wait(void) {
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(EPD_PIN_BUSY) == 1) {
        if (esp_timer_get_time() - start > 1000 * 1000LL) return ESP_OK;
        vTaskDelay(1);
    }
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        if (esp_timer_get_time() - start > EPD_BUSY_TIMEOUT_MS * 1000LL) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    return ESP_OK;
}

// A whole plane in one chip-select burst, bottom row first (the panel is
// mounted that way round): `plane`, or all white without one.
static esp_err_t epd_plane(uint8_t cmd, const uint8_t *plane) {
    esp_err_t err = epd_cmd(cmd, NULL, 0);
    gpio_set_level(EPD_PIN_CS, 0);
    for (int y = EPD_H; err == ESP_OK && y > 0; y -= EPD_CHUNK_ROWS) {
        int rows = y < EPD_CHUNK_ROWS ? y : EPD_CHUNK_ROWS;
        for (int r = 0; r < rows; r++) {
            if (plane) {
                memcpy(s_chunk + (size_t)r * EPD_ROW_BYTES,
                       plane + (size_t)(y - 1 - r) * EPD_ROW_BYTES, EPD_ROW_BYTES);
            } else {
                memset(s_chunk + (size_t)r * EPD_ROW_BYTES, 0xFF, EPD_ROW_BYTES);
            }
        }
        err = epd_send(true, s_chunk, (size_t)rows * EPD_ROW_BYTES);
    }
    gpio_set_level(EPD_PIN_CS, 1);
    return err;
}

typedef struct {
    uint8_t cmd, len;
    uint8_t data[5];
} epd_init_cmd_t;

// FreeInk's Uc8253X3Driver::initController.
static const epd_init_cmd_t s_init[] = {
    {0x00, 2, {0x3F, 0x0A}},                    // panel setting: LUT from registers
    {0x61, 4, {0x03, 0x18, 0x02, 0x58}},        // resolution: 792 x 600 gates
    {0x65, 4, {0x00, 0x00, 0x00, 0x00}},        // gate and source start
    {0x03, 1, {0x20}},                          // power off sequence
    {0x01, 5, {0x07, 0x17, 0x3F, 0x3F, 0x17}},  // power setting
    {0x82, 1, {0x24}},                          // VCOM DC
    {0x06, 4, {0x25, 0x25, 0x3C, 0x37}},        // booster soft start
    {0x30, 1, {0x09}},                          // PLL
    {0xE1, 1, {0x02}},                          // LV selection
    {0x50, 2, {0x29, 0x07}},                    // VCOM and data interval
};

static esp_err_t epd_luts(bool fast) {
    esp_err_t err = epd_cmd(0x20, fast ? s_fast_vcom : s_lut_vcom, LUT_LEN);
    if (err == ESP_OK) err = epd_cmd(0x21, fast ? s_fast_ww : s_lut_ww, LUT_LEN);
    if (err == ESP_OK) err = epd_cmd(0x22, fast ? s_fast_bw : s_lut_bw, LUT_LEN);
    if (err == ESP_OK) err = epd_cmd(0x23, fast ? s_fast_wb : s_lut_wb, LUT_LEN);
    if (err == ESP_OK) err = epd_cmd(0x24, fast ? s_fast_bb : s_lut_bb, LUT_LEN);
    return err;
}

// Refresh with the loaded waveform, then make the shown frame the controller's
// old one for the next fast refresh.
static esp_err_t epd_refresh(void) {
    esp_err_t err = epd_cmd(0x12, NULL, 0);
    if (err == ESP_OK) err = epd_wait();
    if (err == ESP_OK) err = epd_plane(0x10, s_frame);
    if (err == ESP_OK) err = epd_cmd(0x11, NULL, 0);  // data stop
    return err;
}

// Show s_frame; the picture stays without power. A full refresh resets the
// controller and drives every pixel from a white baseline: it flashes. A fast
// one drives only what changed since the last frame, which the controller
// still holds, so no copy of it is kept here. The first refresh, and one in
// every FULL_REFRESH_EVERY, is full whatever was asked. Caller holds
// s_panel_lock.
static esp_err_t epd_update(bool full) {
    if (!s_synced || s_fast_refreshes >= FULL_REFRESH_EVERY) full = true;
    esp_err_t err = ESP_OK;
    int64_t start = esp_timer_get_time();
    if (full) {
        gpio_set_level(EPD_PIN_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(EPD_PIN_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(EPD_PIN_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(60));
        for (size_t i = 0; err == ESP_OK && i < sizeof(s_init) / sizeof(s_init[0]); i++) {
            err = epd_cmd(s_init[i].cmd, s_init[i].data, s_init[i].len);
        }
        if (err == ESP_OK) err = epd_luts(false);
        if (err == ESP_OK) err = epd_plane(0x10, NULL);     // old frame: white
        if (err == ESP_OK) err = epd_cmd(0x11, NULL, 0);    // data stop
        if (err == ESP_OK) err = epd_plane(0x13, s_frame);  // new frame
        if (err == ESP_OK) err = epd_cmd(0x04, NULL, 0);    // power on
        if (err == ESP_OK) err = epd_wait();
        if (err == ESP_OK) err = epd_refresh();
        // The first fast refresh after a full one comes out garbled on this
        // panel; spend it on the same frame.
        if (err == ESP_OK) err = epd_luts(true);
        if (err == ESP_OK) err = epd_plane(0x13, s_frame);
        if (err == ESP_OK) err = epd_refresh();
    } else {
        err = epd_luts(true);
        if (err == ESP_OK) err = epd_plane(0x13, s_frame);
        if (err == ESP_OK) err = epd_cmd(0x04, NULL, 0);    // power on
        if (err == ESP_OK) err = epd_wait();
        if (err == ESP_OK) err = epd_refresh();
    }
    // Rails off; the controller stays awake to keep its copy of the frame.
    if (err == ESP_OK) err = epd_cmd(0x02, NULL, 0);
    if (err == ESP_OK) err = epd_wait();
    int ms = (int)((esp_timer_get_time() - start) / 1000);
    if (err != ESP_OK) {
        s_synced = false;
        ESP_LOGE(TAG, "e-paper refresh failed: %s", esp_err_to_name(err));
        return err;
    }
    s_synced = true;
    s_fast_refreshes = full ? 0 : s_fast_refreshes + 1;
    ESP_LOGI(TAG, "e-paper %s refresh in %d ms", full ? "full" : "fast", ms);
    return ESP_OK;
}

// Ask the controller which one it is, the way the stock firmware does: bit-bang
// the VER register (0x70) and read 3 bytes back on the data line. The third is
// 0x66 on a UC8279d and 0xFF (nothing drives the line) on a UC8253. Runs
// before the SPI driver takes the pins.
static uint8_t epd_probe(void) {
    gpio_hold_dis(EPD_PIN_RST);
    const gpio_config_t out = {
        .pin_bit_mask = 1ULL << EPD_PIN_CS | 1ULL << EPD_PIN_SCLK | 1ULL << EPD_PIN_DC |
                        1ULL << EPD_PIN_MOSI | 1ULL << EPD_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    const gpio_config_t busy = {.pin_bit_mask = 1ULL << EPD_PIN_BUSY, .mode = GPIO_MODE_INPUT};
    gpio_config(&out);
    gpio_config(&busy);
    gpio_set_level(EPD_PIN_CS, 1);
    gpio_set_level(EPD_PIN_SCLK, 0);
    gpio_set_level(EPD_PIN_DC, 1);
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    for (int i = 0; i < 300 && gpio_get_level(EPD_PIN_BUSY) == 0; i++) vTaskDelay(1);

    gpio_set_level(EPD_PIN_CS, 0);
    gpio_set_level(EPD_PIN_DC, 0);
    for (int bit = 7; bit >= 0; bit--) {
        gpio_set_level(EPD_PIN_MOSI, 0x70 >> bit & 1);
        esp_rom_delay_us(2);
        gpio_set_level(EPD_PIN_SCLK, 1);
        esp_rom_delay_us(2);
        gpio_set_level(EPD_PIN_SCLK, 0);
    }
    gpio_set_level(EPD_PIN_DC, 1);
    esp_rom_delay_us(2);
    gpio_set_direction(EPD_PIN_MOSI, GPIO_MODE_INPUT);
    esp_rom_delay_us(2);
    uint8_t ver[3] = {0};
    for (int i = 0; i < 3; i++) {
        for (int bit = 0; bit < 8; bit++) {
            gpio_set_level(EPD_PIN_SCLK, 0);
            esp_rom_delay_us(2);
            gpio_set_level(EPD_PIN_SCLK, 1);
            esp_rom_delay_us(2);
            ver[i] = (uint8_t)(ver[i] << 1 | gpio_get_level(EPD_PIN_MOSI));
        }
        gpio_set_level(EPD_PIN_SCLK, 0);
        esp_rom_delay_us(2);
    }
    gpio_set_level(EPD_PIN_CS, 1);
    ESP_LOGI(TAG, "X3 panel probe: VER %02x %02x %02x -> %s", ver[0], ver[1], ver[2],
             ver[2] == 0x66 ? "UC8279d" : ver[2] == 0xFF ? "UC8253" : "unknown, assuming UC8253");
    return ver[2];
}

static esp_err_t epd_init(void) {
    const gpio_config_t out = {
        .pin_bit_mask = 1ULL << EPD_PIN_DC | 1ULL << EPD_PIN_RST | 1ULL << EPD_PIN_CS,
        .mode = GPIO_MODE_OUTPUT,
    };
    const gpio_config_t busy = {.pin_bit_mask = 1ULL << EPD_PIN_BUSY, .mode = GPIO_MODE_INPUT};
    esp_err_t err = gpio_config(&out);
    if (err == ESP_OK) err = gpio_config(&busy);
    gpio_set_level(EPD_PIN_CS, 1);
    gpio_set_level(EPD_PIN_RST, 1);
    const spi_bus_config_t bus = {
        .sclk_io_num = EPD_PIN_SCLK,
        .mosi_io_num = EPD_PIN_MOSI,
        // GPIO7 is the microSD card's; the panel only listens.
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_CHUNK_BYTES,
    };
    // Chip select is driven by hand: a plane must go out in one burst.
    const spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = EPD_SPI_HZ,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    if (err == ESP_OK) err = spi_bus_initialize(EPD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) err = spi_bus_add_device(EPD_HOST, &dev, &s_spi);
    return err;
}

// ---- Status screen ----------------------------------------------------------

static const char *status_label(led_state_t state) {
    switch (state) {
        case LED_STATE_BOOT:                     return "Starting";
        case LED_STATE_SETUP_IDLE:               return "Press the power button to set up";
        case LED_STATE_BLE_ADVERTISING:          return "Ready to pair over Bluetooth";
        case LED_STATE_BLE_CONNECTED:            return "Pairing";
        case LED_STATE_PAIRING_CONFIRM_REQUIRED: return "Press the power button to confirm";
        case LED_STATE_WIFI_CONNECTING:
        case LED_STATE_WIFI_CONNECTED:
        case LED_STATE_AUTH_OK:
        case LED_STATE_VM_SWITCHING:
        case LED_STATE_VM_OK:                    return "Connecting";
        case LED_STATE_WS_CONNECTED:             return "Connected";
        case LED_STATE_WS_DISCONNECTED:          return "Reconnecting";
        case LED_STATE_UNPAIRED:                 return "Not paired";
        case LED_STATE_ERROR:                    return "Error";
    }
    return "";
}

static void epd_task(void *arg) {
    (void)arg;
    stack_monitor_t stack = STACK_MONITOR_INIT;
    // The task's own copy of a text request, off its stack.
    static char title[sizeof(s_text_title)], body[TEXT_MAX], foot[sizeof(s_text_foot)];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // A status change waits for the next; a text screen does not.
        for (;;) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            bool text = s_text_pending;
            xSemaphoreGive(s_mutex);
            if (text || !ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STATUS_SETTLE_MS))) break;
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        const char *label = status_label(s_state);
        bool text = s_text_pending, footer = s_text_footer;
        s_text_pending = false;
        if (text) {
            memcpy(title, s_text_title, sizeof(title));
            memcpy(body, s_text_body, sizeof(body));
            memcpy(foot, s_text_foot, sizeof(foot));
        } else {
            memcpy(title, s_title, sizeof(s_title));
        }
        xSemaphoreGive(s_mutex);

        xSemaphoreTake(s_panel_lock, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool redraw;
        if (text) {
            // `title` over a rule, `body` wrapped below it, `foot` centred at
            // the bottom.
            redraw = true;
            s_image_mode = true;
            s_image_dirty = false;
            s_status_drawn = false;
            memset(s_frame, 0xFF, EPD_FRAME_BYTES);
            int cols = (SCR_W - 2 * TEXT_MARGIN) / ((PIXEL_FONT_WIDTH + 1) * 4);
            int len = (int)strlen(title);
            draw_chars(title, len < cols ? len : cols, TEXT_MARGIN, 40, 4);
            fill_black(TEXT_MARGIN, 88, SCR_W - 2 * TEXT_MARGIN, 3);
            draw_wrapped(body, TEXT_TOP, footer ? FOOTER_Y - TEXT_MARGIN : SCR_H - TEXT_MARGIN);
            if (footer) draw_text(foot, FOOTER_Y, 2);
        } else {
            redraw = !s_image_mode && (!s_status_drawn || label != s_drawn_label
                                       || strcmp(title, s_drawn_title) != 0);
            if (redraw) {
                memset(s_frame, 0xFF, EPD_FRAME_BYTES);
                draw_text(title, TITLE_Y, TITLE_MAX_SCALE);
                draw_character();
                draw_text(label, STATUS_Y, STATUS_SCALE);
                // An image drawn during the refresh clears this again.
                s_status_drawn = true;
                s_drawn_label = label;
                snprintf(s_drawn_title, sizeof(s_drawn_title), "%s", title);
            }
        }
        xSemaphoreGive(s_lock);
        if (redraw && epd_update(false) != ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status_drawn = false;
            xSemaphoreGive(s_lock);
        }
        xSemaphoreGive(s_panel_lock);
        stack_monitor_poll(&stack);
    }
}

// ---- led_status.h -----------------------------------------------------------

bool led_status_init(void) {
    if (epd_probe() == 0x66) {
        ESP_LOGE(TAG, "this X3 has a UC8279d panel, which is not supported yet");
        return false;
    }
    s_chunk = heap_caps_malloc(EPD_CHUNK_BYTES, MALLOC_CAP_DMA);
    s_frame = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_INTERNAL);
    s_panel_lock = xSemaphoreCreateMutex();
    s_lock = xSemaphoreCreateMutex();
    s_mutex = xSemaphoreCreateMutex();
    if (!s_chunk || !s_frame || !s_panel_lock || !s_lock || !s_mutex) {
        ESP_LOGE(TAG, "e-paper buffer alloc failed");
        return false;
    }
    esp_err_t err = epd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "X3 UC8253 init failed: %s", esp_err_to_name(err));
        return false;
    }
    if (xTaskCreate(epd_task, "epd", 3072, NULL, 2, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the e-paper task");
        return false;
    }
    s_ready = true;
    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "LED status ready: Xteink X3 UC8253 %dx%d e-paper, 1 bit per pixel",
             SCR_W, SCR_H);
    x3_pages_start();
    return true;
}

void led_status_set_state(led_state_t state) {
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = s_state != state;
    s_state = state;
    xSemaphoreGive(s_mutex);
    if (changed) xTaskNotifyGive(s_task);
}

void led_status_set_title(const char *title) {
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
    xSemaphoreGive(s_mutex);
    xTaskNotifyGive(s_task);
}

bool led_status_display_info(int *width, int *height) {
    if (!s_ready) return false;
    *width = SCR_W;
    *height = SCR_H;
    return true;
}

int led_status_display_bits(void) { return s_ready ? 1 : 0; }

bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels) {
    if (!s_ready || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > SCR_W || y + h > SCR_H) {
        return false;
    }
    const uint8_t *src = (const uint8_t *)pixels;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_image_mode) {
        s_image_mode = true;
        s_status_drawn = false;
        memset(s_frame, 0xFF, EPD_FRAME_BYTES);
    }
    for (int r = 0; r < h; r++) {
        for (int i = 0; i < w; i++, src += 2) {
            uint8_t gray = luma565((uint16_t)(src[0] << 8 | src[1]));
            set_pixel(x + i, y + r, dither_white(gray, x + i, y + r));
        }
    }
    s_image_dirty = true;
    xSemaphoreGive(s_lock);
    return true;
}

void led_status_draw_done(void) {
    if (!s_ready) return;
    xSemaphoreTake(s_panel_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool show = s_image_mode && s_image_dirty;
    s_image_dirty = false;
    xSemaphoreGive(s_lock);
    if (show) epd_update(true);
    xSemaphoreGive(s_panel_lock);
}

// Hand a text screen to the task.
static bool request_text(const char *title, const char *text, const char *footer) {
    if (!s_ready) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_text_title, sizeof(s_text_title), "%s", title ? title : s_title);
    snprintf(s_text_body, sizeof(s_text_body), "%s", text);
    snprintf(s_text_foot, sizeof(s_text_foot), "%s", footer ? footer : "");
    s_text_footer = footer != NULL;
    s_text_pending = true;
    xSemaphoreGive(s_mutex);
    xTaskNotifyGive(s_task);
    return true;
}

bool x3_display_show_text(const char *text) { return request_text(NULL, text, NULL); }

bool x3_display_show_page(const char *title, const char *text, int number, int count) {
    char footer[sizeof(s_text_foot)];
    snprintf(footer, sizeof(footer), "%d / %d", number, count);
    return request_text(title, text, footer);
}

void led_status_show_animation(void) {
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_text_pending = false;
    xSemaphoreGive(s_mutex);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_image = s_image_mode;
    s_image_mode = false;
    s_image_dirty = false;
    xSemaphoreGive(s_lock);
    if (was_image) xTaskNotifyGive(s_task);
}
