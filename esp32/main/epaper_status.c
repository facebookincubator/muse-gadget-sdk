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

// led_status.h on the Seeed reTerminal E series e-paper, both 800x480 on SPI
// with the same pins:
// - E1001: 7.5 inch, black and white only (1 bit per pixel), a GDEY075T7
//   panel with a UC8179 controller.
// - E1002: 7.3 inch E Ink Spectra 6, six colours (black, white, yellow, red,
//   blue and green, 4 bits per pixel), full refreshes only.
// It replaces led_status.c for these boards: e-paper keeps its picture
// without power but takes seconds to change, so instead of status bars and
// an animation it shows a still status screen (the agent's name, the
// character and a line of status text) and redraws it only when the text
// changes.
//
// Everything is drawn into a PSRAM canvas (8-bit gray on the E1001, RGB565
// on the E1002), then dithered to the panel's inks just before the refresh,
// so photos keep their tones and the inks themselves stay exact.
//
// Pins: Seeed's ESPHome cookbook for the reTerminal E series
// (wiki.seeedstudio.com/reterminal_e10xx_with_esphome). Controller commands
// and timings: GxEPD2's GxEPD2_750_GDEY075T7 (github.com/ZinggJM/GxEPD2) for
// the E1001, ESPHome's epaper_spi Spectra E6 model
// (github.com/esphome/esphome, esphome/components/epaper_spi) for the E1002.

#include "led_status.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

// The E1002's six-colour panel; the E1001's black and white one otherwise.
#if CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6
#define EPD_COLOR 1
#else
#define EPD_COLOR 0
#endif

// ---- Pixels (host-tested) ---------------------------------------------------

// Gray level of a native RGB565 pixel, 0 (black) to 255 (white).
static inline uint8_t luma565(uint16_t px) {
    int r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
    r = r << 3 | r >> 2;
    g = g << 2 | g >> 4;
    b = b << 3 | b >> 2;
    return (uint8_t)((77 * r + 150 * g + 29 * b + 128) >> 8);
}

// Dither w x h gray pixels to 1 bit with Floyd-Steinberg, packed 8 to a byte,
// most significant bit leftmost, 1 for white (the controller's order). `w`
// is a multiple of 8. `err` holds 2 * (w + 2) values.
static inline void dither_frame(const uint8_t *gray, uint8_t *bits, int w, int h, int16_t *err) {
    int16_t *cur = err, *next = err + w + 2;
    memset(err, 0, 2 * (size_t)(w + 2) * sizeof(*err));
    for (int y = 0; y < h; y++) {
        const uint8_t *row = gray + (size_t)y * w;
        uint8_t *out = bits + (size_t)y * (w / 8);
        memset(next, 0, (size_t)(w + 2) * sizeof(*next));
        for (int x = 0; x < w; x += 8) {
            uint8_t byte = 0;
            for (int k = 0; k < 8; k++) {
                // cur and next are offset by one so that x - 1 stays in range.
                int i = x + k;
                int v = row[i] + cur[i + 1];
                bool white = v >= 128;
                int e = v - (white ? 255 : 0);
                cur[i + 2] += (int16_t)(e * 7 / 16);
                next[i] += (int16_t)(e * 3 / 16);
                next[i + 1] += (int16_t)(e * 5 / 16);
                next[i + 2] += (int16_t)(e / 16);
                byte = (uint8_t)(byte << 1 | white);
            }
            out[x / 8] = byte;
        }
        int16_t *t = cur;
        cur = next;
        next = t;
    }
}

#if EPD_COLOR
// The Spectra 6 inks: the controller's 4-bit code and the colour dithered to
// it. Black and white come first and win ties.
static const struct {
    uint8_t code, r, g, b;
} s_inks[] = {
    {0, 0, 0, 0},        // black
    {1, 255, 255, 255},  // white
    {2, 255, 255, 0},    // yellow
    {3, 255, 0, 0},      // red
    {5, 0, 0, 255},      // blue
    {6, 0, 255, 0},      // green
};

// The ink nearest an RGB colour, by lightness and by the red and blue
// differences from it. Plain RGB distance puts mid grays about as close to
// yellow, red, blue and green as to black and white, and the error that
// picking those spreads soon turns a whole gray area into coloured dots.
static int nearest_ink(int r, int g, int b) {
    int y = (77 * r + 150 * g + 29 * b) >> 8;
    int best = 0, best_d = 0;
    for (int i = 0; i < (int)(sizeof(s_inks) / sizeof(s_inks[0])); i++) {
        int iy = (77 * s_inks[i].r + 150 * s_inks[i].g + 29 * s_inks[i].b) >> 8;
        int dy = y - iy, du = (r - y) - (s_inks[i].r - iy), dv = (b - y) - (s_inks[i].b - iy);
        int d = dy * dy + du * du + dv * dv;
        if (i == 0 || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    return best;
}

// Dither w x h native RGB565 pixels to the inks with Floyd-Steinberg, packed
// 2 to a byte, left pixel in the high nibble (the controller's order). `w` is
// even. `err` holds 6 * (w + 2) values: red, green and blue per pixel.
static void dither_color(const uint16_t *rgb, uint8_t *codes, int w, int h, int16_t *err) {
    int16_t *cur = err, *next = err + 3 * (w + 2);
    memset(err, 0, 6 * (size_t)(w + 2) * sizeof(*err));
    for (int y = 0; y < h; y++) {
        const uint16_t *row = rgb + (size_t)y * w;
        uint8_t *out = codes + (size_t)y * (w / 2);
        memset(next, 0, 3 * (size_t)(w + 2) * sizeof(*next));
        for (int x = 0; x < w; x++) {
            int r = (row[x] >> 11) & 0x1F, g = (row[x] >> 5) & 0x3F, b = row[x] & 0x1F;
            // cur and next are offset by one pixel so that x - 1 stays in range.
            int c[3] = {r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2};
            for (int k = 0; k < 3; k++) {
                c[k] += cur[3 * (x + 1) + k];
                // Bright or dark runs would otherwise pile up error that
                // bleeds far into the next area.
                if (c[k] < -128) c[k] = -128;
                if (c[k] > 383) c[k] = 383;
            }
            int ink = nearest_ink(c[0], c[1], c[2]);
            const int want[3] = {s_inks[ink].r, s_inks[ink].g, s_inks[ink].b};
            for (int k = 0; k < 3; k++) {
                int e = c[k] - want[k];
                cur[3 * (x + 2) + k] += (int16_t)(e * 7 / 16);
                next[3 * x + k] += (int16_t)(e * 3 / 16);
                next[3 * (x + 1) + k] += (int16_t)(e * 5 / 16);
                next[3 * (x + 2) + k] += (int16_t)(e / 16);
            }
            if (x & 1) out[x / 2] |= s_inks[ink].code;
            else out[x / 2] = (uint8_t)(s_inks[ink].code << 4);
        }
        int16_t *t = cur;
        cur = next;
        next = t;
    }
}
#endif

// ---- Panel ------------------------------------------------------------------

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "happy_anim.h"
#include "pixel_font.h"
#include "stack_monitor.h"

static const char *TAG = "link.led";

#define EPD_HOST        SPI2_HOST
#define EPD_PIN_SCLK    7
#define EPD_PIN_MOSI    9
#define EPD_PIN_CS      10
#define EPD_PIN_DC      11
#define EPD_PIN_RST     12
// Low while the controller is busy.
#define EPD_PIN_BUSY    13
#define EPD_SPI_HZ      (10 * 1000 * 1000)
#define EPD_W           800
#define EPD_H           480
#if EPD_COLOR
#define EPD_NAME        "reTerminal E1002 Spectra 6"
#define EPD_DEPTH       "6 colours, 4 bits per pixel"
#define EPD_ROW_BYTES   (EPD_W / 2)
// A refresh takes about 30 s; this is for a stuck or missing panel.
#define EPD_BUSY_TIMEOUT_MS 60000
// What is drawn: native RGB565.
typedef uint16_t canvas_t;
#else
#define EPD_NAME        "reTerminal E1001 UC8179"
#define EPD_DEPTH       "1 bit per pixel"
#define EPD_ROW_BYTES   (EPD_W / 8)
// A full refresh takes about 1.2 s and a fast one 0.5 s; this is for a stuck
// or missing panel.
#define EPD_BUSY_TIMEOUT_MS 10000
// What is drawn: 8-bit gray.
typedef uint8_t canvas_t;
#endif
#define EPD_FRAME_BYTES (EPD_ROW_BYTES * EPD_H)
#define CANVAS_BYTES    ((size_t)EPD_W * EPD_H * sizeof(canvas_t))
// Frame data goes out through a DMA buffer this big.
#define EPD_CHUNK_BYTES 4000

// A status change waits this long for the next, so that the burst while
// connecting costs one refresh.
#define STATUS_SETTLE_MS 1500
// Fast refreshes leave a faint ghost of the old picture; every so often the
// status gets a full refresh, which flashes but clears it.
#define FULL_REFRESH_EVERY 10

// Status screen: the title band on top, the character in the middle, the
// status text below.
#define TITLE_Y          40
#define TITLE_MAX_SCALE  8
#define ANIM_SCALE       4
#define ANIM_W           (HAPPY_ANIM_WIDTH * ANIM_SCALE)
#define ANIM_X           ((EPD_W - ANIM_W) / 2)
#define ANIM_Y           128
#define STATUS_Y         404
#define STATUS_SCALE     4

static spi_device_handle_t s_spi;
static uint8_t *s_chunk;     // DMA buffer for SPI writes
static canvas_t *s_canvas;   // what is being drawn
static uint8_t *s_frame;     // s_canvas dithered, about to be shown
#if !EPD_COLOR
static uint8_t *s_shown;     // what the panel shows, for fast refreshes
#endif
static int16_t *s_err;       // dithering error rows
static bool s_ready;

// Guards the panel, s_frame, s_shown and s_fast_refreshes. Taken before
// s_lock, and held through a refresh, so that drawing waits only for the
// dithering, not the seconds the panel takes.
static SemaphoreHandle_t s_panel_lock;
static int s_fast_refreshes = FULL_REFRESH_EVERY;  // the first is full

// Guards s_canvas and the drawn-state below.
static SemaphoreHandle_t s_lock;
static bool s_image_mode;    // an image replaces the status screen
static bool s_image_dirty;   // drawn since the last refresh
static bool s_status_drawn;  // the status screen shows s_drawn_*
static const char *s_drawn_label;
static char s_drawn_title[48];

// Requested by led_status_set_state() and _set_title(); guarded by s_mutex.
static SemaphoreHandle_t s_mutex;
static led_state_t s_state = LED_STATE_BOOT;
static char s_title[48];
static TaskHandle_t s_task;

// Send `len` bytes, as data or as a command.
static esp_err_t epd_write(bool data, const uint8_t *buf, size_t len) {
    gpio_set_level(EPD_PIN_DC, data);
    while (len) {
        size_t n = len < EPD_CHUNK_BYTES ? len : EPD_CHUNK_BYTES;
        memcpy(s_chunk, buf, n);
        spi_transaction_t t = {.length = n * 8, .tx_buffer = s_chunk};
        esp_err_t err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) return err;
        buf += n;
        len -= n;
    }
    return ESP_OK;
}

static esp_err_t epd_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    esp_err_t err = epd_write(false, &cmd, 1);
    if (err == ESP_OK && len) err = epd_write(true, data, len);
    return err;
}

static esp_err_t epd_wait_idle(void) {
    int64_t give_up = esp_timer_get_time() + EPD_BUSY_TIMEOUT_MS * 1000LL;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        if (esp_timer_get_time() > give_up) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

typedef struct {
    uint8_t cmd, len;
    uint8_t data[6];
} epd_init_cmd_t;

#if EPD_COLOR
// ESPHome's Spectra E6 model.
static const epd_init_cmd_t s_init[] = {
    {0xAA, 6, {0x49, 0x55, 0x20, 0x08, 0x09, 0x18}},  // command header
    {0x01, 1, {0x3F}},                                // power setting
    {0x00, 2, {0x5F, 0x69}},                          // panel setting
    {0x03, 4, {0x00, 0x54, 0x00, 0x44}},              // power off sequence
    {0x05, 4, {0x40, 0x1F, 0x1F, 0x2C}},              // booster soft start 1
    {0x06, 4, {0x6F, 0x1F, 0x17, 0x49}},              // booster soft start 2
    {0x08, 4, {0x6F, 0x1F, 0x1F, 0x22}},              // booster soft start 3
    {0x30, 1, {0x03}},                                // PLL
    {0x50, 1, {0x3F}},                                // VCOM and data interval
    {0x60, 2, {0x02, 0x00}},                          // TCON
    {0x61, 4, {EPD_W >> 8, EPD_W & 0xFF, EPD_H >> 8, EPD_H & 0xFF}},
    {0x84, 1, {0x01}},
    {0xE3, 1, {0x2F}},                                // power saving
};
#else
// GxEPD2's _InitDisplay for the GDEY075T7.
static const epd_init_cmd_t s_init[] = {
    {0x00, 1, {0x1F}},                          // panel: B/W, LUT from OTP
    {0x01, 5, {0x07, 0x07, 0x3F, 0x3F, 0x09}},  // power: VGH/VGL 20 V, VDH/VDL 15 V
    {0x06, 4, {0x17, 0x17, 0x28, 0x17}},        // booster soft start
    {0x61, 4, {EPD_W >> 8, EPD_W & 0xFF, EPD_H >> 8, EPD_H & 0xFF}},
    {0x15, 1, {0x00}},                          // dual SPI off
    {0x50, 2, {0x29, 0x07}},                    // VCOM and data interval
    {0x60, 1, {0x22}},                          // TCON
    {0xE3, 1, {0x22}},                          // power saving
    // Force the temperature the OTP waveform is picked by: GxEPD2's fast
    // full refresh and fast refresh.
    {0xE0, 1, {0x02}},
};
#endif

// Wake the controller, send the frame and refresh, then back to deep sleep;
// the picture stays. The E1001 refreshes full (about 1.2 s, flashes black and
// white) or fast (about 0.5 s, may leave a ghost); the E1002 always refreshes
// full, in about 30 s. Caller holds s_panel_lock.
static esp_err_t epd_update(bool full) {
#if EPD_COLOR
    full = true;
#endif
    // Deep sleep ends only with a reset.
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_err_t err = epd_wait_idle();
    for (size_t i = 0; err == ESP_OK && i < sizeof(s_init) / sizeof(s_init[0]); i++) {
        err = epd_cmd(s_init[i].cmd, s_init[i].data, s_init[i].len);
    }
#if EPD_COLOR
    static const uint8_t zero = 0x00;
    if (err == ESP_OK) err = epd_wait_idle();
    if (err == ESP_OK) err = epd_cmd(0x10, s_frame, EPD_FRAME_BYTES);
    if (err == ESP_OK) err = epd_wait_idle();
#else
    uint8_t temp = full ? 0x5A : 0x6E;
    if (err == ESP_OK) err = epd_cmd(0xE5, &temp, 1);
    // Deep sleep loses the controller's copy of the old frame, which the
    // waveform needs.
    if (err == ESP_OK) err = epd_cmd(0x10, s_shown, EPD_FRAME_BYTES);
    if (err == ESP_OK) err = epd_cmd(0x13, s_frame, EPD_FRAME_BYTES);
#endif
    if (err == ESP_OK) err = epd_cmd(0x04, NULL, 0);  // power on
    if (err == ESP_OK) err = epd_wait_idle();
    int64_t start = esp_timer_get_time();
#if EPD_COLOR
    if (err == ESP_OK) err = epd_cmd(0x12, &zero, 1);  // refresh
#else
    if (err == ESP_OK) err = epd_cmd(0x12, NULL, 0);  // refresh
#endif
    if (err == ESP_OK) err = epd_wait_idle();
    int ms = (int)((esp_timer_get_time() - start) / 1000);
#if EPD_COLOR
    if (err == ESP_OK) err = epd_cmd(0x02, &zero, 1);  // power off
#else
    if (err == ESP_OK) err = epd_cmd(0x02, NULL, 0);  // power off
#endif
    if (err == ESP_OK) err = epd_wait_idle();
    static const uint8_t check = 0xA5;
    if (err == ESP_OK) err = epd_cmd(0x07, &check, 1);  // deep sleep
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "e-paper refresh failed: %s", esp_err_to_name(err));
        return err;
    }
#if !EPD_COLOR
    memcpy(s_shown, s_frame, EPD_FRAME_BYTES);
#endif
    s_fast_refreshes = full ? 0 : s_fast_refreshes + 1;
    ESP_LOGI(TAG, "e-paper %s refresh in %d ms", full ? "full" : "fast", ms);
    return ESP_OK;
}

// Dither s_canvas into s_frame. Caller holds s_panel_lock and s_lock.
static void epd_dither(void) {
#if EPD_COLOR
    dither_color(s_canvas, s_frame, EPD_W, EPD_H, s_err);
#else
    dither_frame(s_canvas, s_frame, EPD_W, EPD_H, s_err);
#endif
}

// ---- Status screen ----------------------------------------------------------

static const char *status_label(led_state_t state) {
    switch (state) {
        case LED_STATE_BOOT:                     return "Starting";
        case LED_STATE_SETUP_IDLE:               return "Press the green button to set up";
        case LED_STATE_BLE_ADVERTISING:          return "Ready to pair over Bluetooth";
        case LED_STATE_BLE_CONNECTED:            return "Pairing";
        case LED_STATE_PAIRING_CONFIRM_REQUIRED: return "Press the green button to confirm";
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

// Draw `text` in black, centred on the row at `y`, in the largest pixel size
// up to `max_scale` that fits, cutting off what still does not fit at size 2.
// Bytes outside printable ASCII show as '?'.
static void draw_text(const char *text, int y, int max_scale) {
    const int adv = PIXEL_FONT_WIDTH + 1;
    int n = (int)strlen(text);
    int scale = max_scale;
    while (scale > 2 && n * adv * scale - scale > EPD_W) scale--;
    if (n > (EPD_W + scale) / (adv * scale)) n = (EPD_W + scale) / (adv * scale);
    int x0 = (EPD_W - (n * adv * scale - scale)) / 2;
    y += (max_scale - scale) * PIXEL_FONT_HEIGHT / 2;
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < PIXEL_FONT_FIRST || ch > PIXEL_FONT_LAST) ch = '?';
        const uint8_t *glyph = pixel_font[ch - PIXEL_FONT_FIRST];
        for (int gx = 0; gx < PIXEL_FONT_WIDTH; gx++) {
            for (int gy = 0; gy < PIXEL_FONT_HEIGHT; gy++) {
                if (!(glyph[gx] >> gy & 1)) continue;
                int px = x0 + (i * adv + gx) * scale, py = y + gy * scale;
                for (int r = 0; r < scale; r++) {
                    memset(s_canvas + (size_t)(py + r) * EPD_W + px, 0,
                           scale * sizeof(canvas_t));
                }
            }
        }
    }
}

// The character, still: the first frame of the animation, with its black
// background as paper. In colour on the E1002, in gray on the E1001.
static void draw_character(void) {
    const uint8_t *cells = happy_anim_frames[0];
    for (int cy = 0; cy < HAPPY_ANIM_HEIGHT; cy++) {
        canvas_t *line = s_canvas + (size_t)(ANIM_Y + cy * ANIM_SCALE) * EPD_W + ANIM_X;
        for (int cx = 0; cx < HAPPY_ANIM_WIDTH; cx++) {
            uint8_t c = cells[cy * HAPPY_ANIM_WIDTH + cx];
            uint16_t be = happy_anim_palette[c];
            uint16_t px = (uint16_t)(be >> 8 | be << 8);
#if EPD_COLOR
            canvas_t v = c == 0 ? 0xFFFF : px;
#else
            canvas_t v = c == 0 ? 255 : luma565(px);
#endif
            for (int k = 0; k < ANIM_SCALE; k++) line[cx * ANIM_SCALE + k] = v;
        }
        for (int k = 1; k < ANIM_SCALE; k++) {
            memcpy(line + k * EPD_W, line, ANIM_W * sizeof(canvas_t));
        }
    }
}

static void epd_task(void *arg) {
    (void)arg;
    stack_monitor_t stack = STACK_MONITOR_INIT;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STATUS_SETTLE_MS))) {
        }
        char title[sizeof(s_title)];
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        const char *label = status_label(s_state);
        memcpy(title, s_title, sizeof(title));
        xSemaphoreGive(s_mutex);

        xSemaphoreTake(s_panel_lock, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool redraw = !s_image_mode && (!s_status_drawn || label != s_drawn_label
                                        || strcmp(title, s_drawn_title) != 0);
        // A new screen after an image gets a full refresh too.
        bool full = !s_status_drawn || s_fast_refreshes >= FULL_REFRESH_EVERY;
        if (redraw) {
            memset(s_canvas, 255, CANVAS_BYTES);
            draw_text(title, TITLE_Y, TITLE_MAX_SCALE);
            draw_character();
            draw_text(label, STATUS_Y, STATUS_SCALE);
            epd_dither();
            // An image drawn during the refresh clears this again.
            s_status_drawn = true;
            s_drawn_label = label;
            memcpy(s_drawn_title, title, sizeof(s_drawn_title));
        }
        xSemaphoreGive(s_lock);
        if (redraw && epd_update(full) != ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status_drawn = false;
            xSemaphoreGive(s_lock);
        }
        xSemaphoreGive(s_panel_lock);
        stack_monitor_poll(&stack);
    }
}

// ---- led_status.h -----------------------------------------------------------

static esp_err_t epd_init(void) {
    const gpio_config_t out = {
        .pin_bit_mask = 1ULL << EPD_PIN_DC | 1ULL << EPD_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    const gpio_config_t busy = {
        .pin_bit_mask = 1ULL << EPD_PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&out);
    if (err == ESP_OK) err = gpio_config(&busy);
    gpio_set_level(EPD_PIN_RST, 1);
    const spi_bus_config_t bus = {
        .sclk_io_num = EPD_PIN_SCLK,
        .mosi_io_num = EPD_PIN_MOSI,
        // GPIO8 is the microSD card's; the panel only listens.
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_CHUNK_BYTES,
    };
    const spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = EPD_SPI_HZ,
        .spics_io_num = EPD_PIN_CS,
        .queue_size = 1,
    };
    if (err == ESP_OK) err = spi_bus_initialize(EPD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) err = spi_bus_add_device(EPD_HOST, &dev, &s_spi);
    return err;
}

bool led_status_init(void) {
    s_chunk = heap_caps_malloc(EPD_CHUNK_BYTES, MALLOC_CAP_DMA);
    s_canvas = heap_caps_malloc(CANVAS_BYTES, MALLOC_CAP_SPIRAM);
    s_frame = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
#if EPD_COLOR
    size_t err_values = 6 * (EPD_W + 2);
#else
    // What the panel shows at boot is unknown; GxEPD2 sends zeros too.
    s_shown = heap_caps_calloc(1, EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    size_t err_values = 2 * (EPD_W + 2);
#endif
    s_err = heap_caps_malloc(err_values * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    s_panel_lock = xSemaphoreCreateMutex();
    s_lock = xSemaphoreCreateMutex();
    s_mutex = xSemaphoreCreateMutex();
    bool ok = s_chunk && s_canvas && s_frame && s_err && s_panel_lock && s_lock && s_mutex;
#if !EPD_COLOR
    ok = ok && s_shown;
#endif
    if (!ok) {
        ESP_LOGE(TAG, "e-paper buffer alloc failed");
        return false;
    }
    esp_err_t err = epd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, EPD_NAME " init failed: %s", esp_err_to_name(err));
        return false;
    }
    if (xTaskCreate(epd_task, "epd", 3072, NULL, 2, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the e-paper task");
        return false;
    }
    s_ready = true;
    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "LED status ready: " EPD_NAME " %dx%d e-paper, " EPD_DEPTH,
             EPD_W, EPD_H);
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
    *width = EPD_W;
    *height = EPD_H;
    return true;
}

int led_status_display_bits(void) {
    if (!s_ready) return 0;
    return EPD_COLOR ? 4 : 1;
}

bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels) {
    if (!s_ready || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > EPD_W || y + h > EPD_H) {
        return false;
    }
    const uint8_t *src = (const uint8_t *)pixels;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_image_mode) {
        s_image_mode = true;
        s_status_drawn = false;
        memset(s_canvas, 255, CANVAS_BYTES);
    }
    for (int r = 0; r < h; r++) {
        canvas_t *line = s_canvas + (size_t)(y + r) * EPD_W + x;
        for (int i = 0; i < w; i++, src += 2) {
            uint16_t px = (uint16_t)(src[0] << 8 | src[1]);
#if EPD_COLOR
            line[i] = px;
#else
            line[i] = luma565(px);
#endif
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
    if (show) {
        s_image_dirty = false;
        epd_dither();
    }
    xSemaphoreGive(s_lock);
    if (show) epd_update(true);
    xSemaphoreGive(s_panel_lock);
}

void led_status_show_animation(void) {
    if (!s_ready) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_image = s_image_mode;
    s_image_mode = false;
    s_image_dirty = false;
    xSemaphoreGive(s_lock);
    if (was_image) xTaskNotifyGive(s_task);
}
