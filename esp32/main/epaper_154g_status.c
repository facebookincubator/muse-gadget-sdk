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

// Waveshare ESP32-S3-ePaper-1.54G status and image screen, 200x200 four inks.
// Panel pins and commands follow Waveshare's 09_E_Paper_Test and panel manual.

#include "led_status.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#if CONFIG_HOMEHUB_VOICE
#include "voice_epaper_154g.h"
#endif

#include "epaper_154g_window.h"
#include "epaper_154g_rotation.h"
#include "nvs.h"

#define EPD_COLOR 1
#define EPD_INK_BITS 2

// ---- Pixels (host-tested) ---------------------------------------------------

// The 1.54G's inks: the controller's 2-bit code and the colour mapped to
// it. Black and white come first and win ties.
static const struct {
    uint8_t code, r, g, b;
} s_inks[] = {
    {0, 0, 0, 0},        // black
    {1, 255, 255, 255},  // white
    {2, 255, 255, 0},    // yellow
    {3, 255, 0, 0},      // red
};

// The nearest ink by lightness and red/blue differences.
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

// No error diffusion on the small panel: shrink/JPEG edge colours stay solid.
static void map_frame(const uint16_t *rgb, uint8_t *codes, int w, int h) {
    for (int y = 0; y < h; y++) {
        const uint16_t *row = rgb + (size_t)y * w;
        uint8_t *out = codes + (size_t)y * (w / 4);
        for (int x = 0; x < w; x++) {
            int r = (row[x] >> 11) & 31, g = (row[x] >> 5) & 63, b = row[x] & 31;
            int ink = nearest_ink(r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2);
            if (x % 4 == 0) out[x / 4] = 0;
            out[x / 4] |= (uint8_t)(s_inks[ink].code << (6 - 2 * (x % 4)));
        }
    }
}

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
#include "epaper_154g_caption_text.h"
#if CONFIG_HOMEHUB_EPD154G_BATTERY
#include "epaper_154g_battery.h"
#endif
#include "stack_monitor.h"

static const char *TAG = "link.led";

#define EPD_HOST        SPI2_HOST
#define EPD_SPI_HZ      (10 * 1000 * 1000)
#define EPD_PIN_SCLK    12
#define EPD_PIN_MOSI    13
#define EPD_PIN_CS      11
#define EPD_PIN_DC      10
#define EPD_PIN_RST     9
// Low while the controller is busy.
#define EPD_PIN_BUSY    8
// The panel's 3.3 V supply switch, on while low.
#define EPD_PIN_PWR     6
// High keeps the battery connected once PWR is let go; low cuts it.
#define BOARD_PIN_LATCH 17
// The ES8311's supply switch, on while low; voice enables it after screen init.
#define BOARD_PIN_AUDIO_PWR 42
#define BOARD_PIN_AMP   46
#define EPD_W           200
#define EPD_H           200
#define EPD_NAME        "Waveshare 1.54G"
#define EPD_DEPTH       "4 colours, 2 bits per pixel"
// A refresh takes about 20 s; this is for a stuck or missing panel.
#define EPD_BUSY_TIMEOUT_MS 45000
#define EPD_ROW_BYTES   (EPD_W * EPD_INK_BITS / 8)
// What is drawn: native RGB565.
typedef uint16_t canvas_t;
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

static spi_device_handle_t s_spi;
static uint8_t *s_chunk;     // DMA buffer for SPI writes
static canvas_t *s_canvas;   // what is being drawn
static uint8_t *s_frame;     // s_canvas dithered, about to be shown
static uint8_t *s_shown;     // what the panel shows, for fast refreshes
static bool s_shown_valid;
static uint8_t *s_logical_frame;
static unsigned s_rotation, s_drawn_rotation; // guarded by s_panel_lock
static unsigned s_requested_rotation; // guarded by s_mutex

static int s_status_band_y, s_status_band_height;
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
#if CONFIG_HOMEHUB_VOICE
#define VOICE_TEXT_BYTES EPAPER_154G_CAPTION_BYTES
static char s_voice_text[VOICE_TEXT_BYTES]; // guarded by s_mutex
static epaper_154g_caption_buffers_t *s_voice_buffers;
static unsigned s_voice_generation, s_drawn_voice_generation;
static unsigned s_voice_page, s_drawn_voice_page;
#endif
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
    // The 1.54G pulls BUSY low only a moment after a command; Waveshare's
    // driver waits this long before it looks.
    vTaskDelay(pdMS_TO_TICKS(100));
    int64_t give_up = esp_timer_get_time() + EPD_BUSY_TIMEOUT_MS * 1000LL;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        if (esp_timer_get_time() > give_up) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

typedef struct {
    uint8_t cmd, len;
    uint8_t data[7];
} epd_init_cmd_t;

// Waveshare's epaper_port_init() for the 1.54G, less the power on.
static const epd_init_cmd_t s_init[] = {
    {0x4D, 1, {0x78}},
    {0x00, 2, {0x0F, 0x29}},                                // panel setting
    {0x06, 7, {0x0D, 0x12, 0x30, 0x20, 0x19, 0x2A, 0x22}},  // booster soft start
    {0x50, 1, {0x37}},                                      // VCOM and data interval
    {0x61, 4, {EPD_W >> 8, EPD_W & 0xFF, EPD_H >> 8, EPD_H & 0xFF}},
    {0xE9, 1, {0x01}},
    {0x30, 1, {0x08}},                                      // PLL
};

// Wake the controller, send the frame and refresh, then back to deep sleep;
// the picture stays. Both full and window refreshes take about 20 seconds.
// Caller holds s_panel_lock.
static esp_err_t epd_update(bool full) {
    epaper_154g_window_t window = epaper_154g_window_plan_rotated(s_shown, s_frame, s_shown_valid, full,
                                                       s_status_band_y, s_status_band_height, s_rotation);
    if (!window.changed) return ESP_OK;
    full = window.full;
    // A failed transfer leaves the visible pixels unknown; retry with a full frame.
    s_shown_valid = false;
    // Deep sleep ends only with a reset; Waveshare's timings. The frame goes
    // in after the power on, as in Waveshare's driver.
    static const uint8_t zero = 0x00;
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t err = ESP_OK;
    for (size_t i = 0; err == ESP_OK && i < sizeof(s_init) / sizeof(s_init[0]); i++) {
        err = epd_cmd(s_init[i].cmd, s_init[i].data, s_init[i].len);
    }
    if (err == ESP_OK) err = epd_cmd(0x04, NULL, 0);  // power on
    if (err == ESP_OK) err = epd_wait_idle();
    // Deep sleep loses RAM: load the whole frame before selecting the refresh area.
    uint8_t area[9];
    epaper_154g_window_encode((epaper_154g_window_t){0, 0, EPD_W, EPD_H, true, true, 0}, area);
    if (err == ESP_OK) err = epd_cmd(0x83, area, sizeof(area));
    if (err == ESP_OK) err = epd_cmd(0x10, s_frame, EPD_FRAME_BYTES);
    epaper_154g_window_encode(window, area);
    if (err == ESP_OK) err = epd_cmd(0x83, area, sizeof(area));
    // A local refresh leaves the panel border floating (R50H).
    uint8_t border = full ? 0x37 : 0x97;
    if (err == ESP_OK) err = epd_cmd(0x50, &border, 1);
    int64_t start = esp_timer_get_time();
    if (err == ESP_OK) err = epd_cmd(0x12, &zero, 1);  // refresh
    // Do not commit a shadow when the controller never starts the refresh.
    if (err == ESP_OK) {
        int64_t busy_deadline = esp_timer_get_time() + 1000000;
        while (gpio_get_level(EPD_PIN_BUSY) != 0 && esp_timer_get_time() < busy_deadline) {
            vTaskDelay(pdMS_TO_TICKS(1) + 1);
        }
        if (gpio_get_level(EPD_PIN_BUSY) != 0) err = ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) err = epd_wait_idle();
    int ms = (int)((esp_timer_get_time() - start) / 1000);
    if (err == ESP_OK) err = epd_cmd(0x02, &zero, 1);  // power off
    if (err == ESP_OK) err = epd_wait_idle();
    static const uint8_t check = 0xA5;
    if (err == ESP_OK) err = epd_cmd(0x07, &check, 1);  // deep sleep
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "e-paper refresh failed: %s", esp_err_to_name(err));
        return err;
    }
    epaper_154g_window_commit(s_shown, s_frame, window);
    s_shown_valid = true;
    ESP_LOGI(TAG, "e-paper %s x=%u y=%u w=%u h=%u changed=%lu in %d ms",
             full ? "full" : "window", window.x, window.y, window.width, window.height,
             (unsigned long)window.changed_pixels, ms);
    s_fast_refreshes = full ? 0 : s_fast_refreshes + 1;
    return ESP_OK;
}

// Convert s_canvas into s_frame. Caller holds s_panel_lock and s_lock.
static void epd_dither(void) {
    map_frame(s_canvas, s_logical_frame, EPD_W, EPD_H);
    epaper_154g_rotation_frame(s_logical_frame, s_frame, s_rotation);
}

// ---- Status screen ----------------------------------------------------------

// Status screen: the title band on top, the character in the middle, the
// status text below, wrapped onto up to STATUS_LINES lines.
#if CONFIG_HOMEHUB_VOICE
// Keep the same compact background for statuses and replies, so captions stay local.
#define TITLE_Y          5
#define TITLE_MAX_SCALE  1
#define ANIM_SCALE       1
#define ANIM_Y           23
#define STATUS_Y         82
#define STATUS_SCALE     1
#define STATUS_LINES     EPAPER_154G_CAPTION_LINES
#else
#define TITLE_Y          5
#define TITLE_MAX_SCALE  1
#define ANIM_SCALE       2
#define ANIM_Y           32
#define STATUS_Y         158
#define STATUS_SCALE     2
#define STATUS_LINES     2
#endif
#define ANIM_W           (HAPPY_ANIM_WIDTH * ANIM_SCALE)
#define ANIM_X           ((EPD_W - ANIM_W) / 2)
// Between wrapped lines, in font pixels.
#define LINE_GAP         2

// The button that sets up and confirms pairing, as the status names it.
#define EPD_BUTTON       "BOOT"

// Ink for text. The 1.54G's status screen is drawn in its four inks alone,
// one to a pixel, so the dither passes it through: dithered, the character's
// creams came out as sparse yellow and red dots on white, and looked pale.
#define INK_BLACK        0
#define INK_WHITE        0xFFFF
#define INK_YELLOW       0xFFE0
#define INK_RED          0xF800
// The name sits on a yellow band across the top, over a red rule.
#define TITLE_BAND_H     20
#define TITLE_RULE_H     2
#define BATTERY_STRIP_H  3
static int s_battery_step = -1; // guarded by s_mutex
static int s_drawn_battery_step = -1;
static bool s_battery_retry; // guarded by s_mutex
#if CONFIG_HOMEHUB_VOICE
static bool s_drawn_caption_layout;
#endif

static const char *status_label(led_state_t state) {
    switch (state) {
        case LED_STATE_BOOT:                     return "Starting";
        case LED_STATE_SETUP_IDLE:               return "Press " EPD_BUTTON " to set up";
        case LED_STATE_BLE_ADVERTISING:          return "Ready to pair over Bluetooth";
        case LED_STATE_BLE_CONNECTED:            return "Pairing";
        case LED_STATE_PAIRING_CONFIRM_REQUIRED: return "Press " EPD_BUTTON " to confirm";
        case LED_STATE_WIFI_CONNECTING:
        case LED_STATE_WIFI_CONNECTED:
        case LED_STATE_AUTH_OK:
        case LED_STATE_VM_SWITCHING:
        case LED_STATE_VM_OK:                    return "Connecting";
        case LED_STATE_WS_CONNECTED:
#if CONFIG_HOMEHUB_VOICE
            return "Hold BOOT to speak";
#else
            return "Connected";
#endif
        case LED_STATE_WS_DISCONNECTED:          return "Reconnecting";
        case LED_STATE_UNPAIRED:                 return "Not paired";
        case LED_STATE_ERROR:                    return "Error";
    }
    return "";
}

// Split `text` at spaces into lines of at most `width` characters, cutting a
// longer word. Returns how many lines it takes and fills in the first `max`
// lines' starts and lengths, without their outer spaces.
static int wrap_text(const char *text, int width, int max, int *start, int *len) {
    int total = (int)strlen(text), lines = 0, i = 0;
    while (i < total) {
        while (text[i] == ' ') i++;
        if (i >= total) break;
        int cut = i + width;
        if (cut >= total) {
            cut = total;
        } else {
            int space = cut;
            while (space > i && text[space] != ' ') space--;
            if (space > i) cut = space;
        }
        int end = cut;
        while (end > i && text[end - 1] == ' ') end--;
        if (lines < max) {
            start[lines] = i;
            len[lines] = end - i;
        }
        lines++;
        i = cut;
    }
    return lines;
}

#define VOICE_TEXT_LINES EPAPER_154G_CAPTION_LINES
#define VOICE_TEXT_COLS EPAPER_154G_CAPTION_COLS

static void draw_caption_glyphs(const char *text, int bytes, int x, int y, canvas_t ink) {
    const char *p = text, *end = text + bytes;
    for (; p < end; x += EPAPER_154G_CAPTION_WIDTH) {
        const uint8_t *glyph = epaper_154g_caption_glyph(epaper_154g_caption_decode(&p));
        if (!glyph) glyph = epaper_154g_caption_glyph('?');
        for (int row = 0; row < EPAPER_154G_CAPTION_HEIGHT; row++) {
            for (int col = 0; col < EPAPER_154G_CAPTION_WIDTH; col++) {
                if (glyph[row] & (0x80 >> col)) s_canvas[(y + row) * EPD_W + x + col] = ink;
            }
        }
    }
}

static void draw_caption_text(const char *text, int text_y, int max_lines, canvas_t ink) {
    int start[VOICE_TEXT_LINES], len[VOICE_TEXT_LINES];
    int lines = epaper_154g_caption_wrap(text, VOICE_TEXT_COLS, max_lines, start, len);
    if (lines > max_lines) lines = max_lines;
    for (int line = 0; line < lines; line++) {
        const char *p = text + start[line];
        int cells = epaper_154g_caption_cells(p, len[line]);
        int x = (EPD_W - cells * EPAPER_154G_CAPTION_WIDTH) / 2;
        int y = text_y + line * (EPAPER_154G_CAPTION_HEIGHT + EPAPER_154G_CAPTION_GAP);
        draw_caption_glyphs(p, len[line], x, y, ink);
    }
}

// Draw `text` in black, centred in the band from the row at `y` that
// `max_lines` lines at `max_scale` take, in the largest pixel size up to
// `max_scale` at which it fits on those lines, wrapped at spaces. What still
// does not fit at size 2 is cut off. Bytes outside printable ASCII show as
// '?'.
static void draw_text(const char *text, int y, int max_scale, int max_lines, canvas_t ink) {
    enum { MAX_LINES = 4 };
    const int adv = PIXEL_FONT_WIDTH + 1;
    if (max_lines > MAX_LINES) max_lines = MAX_LINES;
    int start[MAX_LINES], len[MAX_LINES];
    int scale = max_scale, lines;
    for (;;) {
        lines = wrap_text(text, (EPD_W + scale) / (adv * scale), max_lines, start, len);
        if (lines <= max_lines || scale <= 2) break;
        scale--;
    }
    if (lines > max_lines) lines = max_lines;
    int band = max_lines * max_scale * PIXEL_FONT_HEIGHT + (max_lines - 1) * LINE_GAP * max_scale;
    int used = lines * scale * PIXEL_FONT_HEIGHT + (lines - 1) * LINE_GAP * scale;
    y += (band - used) / 2;
    for (int l = 0; l < lines; l++, y += (PIXEL_FONT_HEIGHT + LINE_GAP) * scale) {
        int x0 = (EPD_W - (len[l] * adv * scale - scale)) / 2;
        for (int i = 0; i < len[l]; i++) {
            unsigned char ch = (unsigned char)text[start[l] + i];
            if (ch < PIXEL_FONT_FIRST || ch > PIXEL_FONT_LAST) ch = '?';
            const uint8_t *glyph = pixel_font[ch - PIXEL_FONT_FIRST];
            for (int gx = 0; gx < PIXEL_FONT_WIDTH; gx++) {
                for (int gy = 0; gy < PIXEL_FONT_HEIGHT; gy++) {
                    if (!(glyph[gx] >> gy & 1)) continue;
                    int px = x0 + (i * adv + gx) * scale, py = y + gy * scale;
                    for (int r = 0; r < scale; r++) {
                        canvas_t *dot = s_canvas + (size_t)(py + r) * EPD_W + px;
                        for (int k = 0; k < scale; k++) dot[k] = ink;
                    }
                }
            }
        }
    }
}

// The ink for one of the character's colours, by what it draws: dark lines,
// eyes and mouth black; pinks and purples (cheeks, sparkles) red; pure white
// (the eyes' shine) white; the face yellow and the body paper white.
static canvas_t character_ink(uint16_t px) {
    int r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
    r = r << 3 | r >> 2;
    g = g << 2 | g >> 4;
    b = b << 3 | b >> 2;
    int y = (77 * r + 150 * g + 29 * b) >> 8;
    if (y < 128) return INK_BLACK;
    if (r > 240 && g > 240 && b > 240) return INK_WHITE;
    if (r - g > 48 || b - g > 60) return INK_RED;
    if (y > 235) return INK_WHITE;
    if (r - b > 55) return INK_YELLOW;
    return INK_WHITE;
}

// A cell with nothing around it: a dot of the glow around the character,
// which in flat ink is only a speck.
static bool lone_cell(const uint8_t *cells, int cx, int cy) {
    static const int dx[] = {-1, 1, 0, 0}, dy[] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        int x = cx + dx[i], y = cy + dy[i];
        if (x >= 0 && x < HAPPY_ANIM_WIDTH && y >= 0 && y < HAPPY_ANIM_HEIGHT
            && cells[y * HAPPY_ANIM_WIDTH + x] != 0) {
            return false;
        }
    }
    return true;
}

// Statuses that want the user, shown in red.
static bool status_alert(led_state_t state) {
    return state == LED_STATE_SETUP_IDLE || state == LED_STATE_PAIRING_CONFIRM_REQUIRED
           || state == LED_STATE_ERROR;
}

// The character, still: the first frame of the animation, with its black
// background as paper. In colour on the colour panels (in the 1.54G's four
// inks, flat).
static void draw_character_at(int scale, int top) {
    static canvas_t ink[HAPPY_ANIM_HEIGHT][HAPPY_ANIM_WIDTH];
    const uint8_t *cells = happy_anim_frames[0];
    for (int cy = 0; cy < HAPPY_ANIM_HEIGHT; cy++) {
        for (int cx = 0; cx < HAPPY_ANIM_WIDTH; cx++) {
            uint8_t c = cells[cy * HAPPY_ANIM_WIDTH + cx];
            uint16_t be = happy_anim_palette[c];
            uint16_t px = (uint16_t)(be >> 8 | be << 8);
            ink[cy][cx] = c == 0 || lone_cell(cells, cx, cy) ? INK_WHITE : character_ink(px);
            // Recolour only the existing pale pixels of the two paws.
            if (cy >= 54 && cx >= 12 && cx <= 43 && c != 0
                && ink[cy][cx] == INK_WHITE && px != 0xffff)
                ink[cy][cx] = INK_YELLOW;
        }
    }
    for (int cy = 1; cy < HAPPY_ANIM_HEIGHT - 1; cy++) {
        for (int cx = 1; cx < HAPPY_ANIM_WIDTH - 1; cx++) {
            if (ink[cy][cx] != INK_WHITE || cells[cy * HAPPY_ANIM_WIDTH + cx] == 0) continue;
            canvas_t n[4] = {ink[cy][cx - 1], ink[cy][cx + 1], ink[cy - 1][cx], ink[cy + 1][cx]};
            for (int i = 0; i < 4; i++) {
                int same = 0;
                for (int j = 0; j < 4; j++) same += n[j] == n[i];
                if (n[i] != INK_WHITE && n[i] != INK_BLACK && same >= 3) {
                    ink[cy][cx] = n[i];
                    break;
                }
            }
        }
    }
    for (int cy = 0; cy < HAPPY_ANIM_HEIGHT; cy++) {
        canvas_t *line = s_canvas + (size_t)(top + cy * scale) * EPD_W + (EPD_W - HAPPY_ANIM_WIDTH * scale) / 2;
        for (int cx = 0; cx < HAPPY_ANIM_WIDTH; cx++) {
            for (int k = 0; k < scale; k++) line[cx * scale + k] = ink[cy][cx];
        }
        for (int k = 1; k < scale; k++) memcpy(line + k * EPD_W, line, HAPPY_ANIM_WIDTH * scale * sizeof(canvas_t));
    }
}

#if !CONFIG_HOMEHUB_VOICE
static void draw_character(void) { draw_character_at(ANIM_SCALE, ANIM_Y); }
#endif


// The whole status screen, on paper.
static void draw_status(const char *title, led_state_t state
#if CONFIG_HOMEHUB_VOICE
                        , const char *voice_text, unsigned voice_page
#endif
                        ) {
    memset(s_canvas, 255, (size_t)EPD_W * EPD_H * sizeof(canvas_t));
    for (int y = BATTERY_STRIP_H; y < BATTERY_STRIP_H + TITLE_BAND_H; y++) {
        canvas_t ink = y < BATTERY_STRIP_H + TITLE_BAND_H - TITLE_RULE_H ? INK_YELLOW : INK_RED;
        for (int x = 0; x < EPD_W; x++) s_canvas[y * EPD_W + x] = ink;
    }
    canvas_t status_ink = status_alert(state) ? INK_RED : INK_BLACK;
#if CONFIG_HOMEHUB_VOICE
    // The header is drawn before the page, so they can share scratch space.
    char *title_text = s_voice_buffers->page;
#else
    char title_text[EPAPER_154G_CAPTION_PAGE_BYTES];
#endif
    epaper_154g_caption_normalize(title, title_text, EPAPER_154G_CAPTION_PAGE_BYTES);
    draw_caption_text(title_text, TITLE_Y, 1, INK_BLACK);
#if CONFIG_HOMEHUB_VOICE
    draw_character_at(voice_text[0] ? 1 : 2, voice_text[0] ? ANIM_Y : 32);
    if (!voice_text[0]) draw_text(status_label(state), 158, 2, 2, status_ink);
    else {
        char *text = s_voice_buffers->page;
        unsigned pages = epaper_154g_caption_page(voice_text, voice_page, text);
        draw_caption_text(text, STATUS_Y, VOICE_TEXT_LINES, INK_BLACK);
        if (pages > 1) {
            char marker[16];
            snprintf(marker, sizeof(marker), "%u/%u", voice_page + 1, pages);
            int x = EPD_W - 4 - (int)strlen(marker) * EPAPER_154G_CAPTION_WIDTH;
            draw_caption_glyphs(marker, (int)strlen(marker), x, STATUS_Y - 18, INK_BLACK);
        }
    }
#else
    draw_character();
    draw_text(status_label(state), STATUS_Y, STATUS_SCALE, STATUS_LINES, status_ink);
#endif
}

// Ten-percent steps, with two percent of hysteresis at each midpoint.
static int battery_step(int previous, int percent) {
    if (percent < 0) return -1;
    if (percent > 100) percent = 100;
    if (previous >= 0 && percent < previous * 10 + 7 && percent > previous * 10 - 7)
        return previous;
    return (percent + 5) / 10;
}

static void draw_battery_strip(int step, uint8_t *frame) {
    int width = step < 0 ? 0 : step * EPD_W / 10;
    for (int y = 0; y < BATTERY_STRIP_H; y++) {
        for (int x = 0; x < EPD_W; x++) {
            s_canvas[y * EPD_W + x] = x < width ? INK_RED : INK_WHITE;
            if (frame) {
                int px, py;
                epaper_154g_rotation_xy(s_rotation, x, y, &px, &py);
                epaper_154g_rotation_put(frame, px, py, x < width ? 3 : 1);
            }
        }
    }
}

// ---- Status task ------------------------------------------------------------

static void epd_task(void *arg) {
    (void)arg;
    stack_monitor_t stack = STACK_MONITOR_INIT;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STATUS_SETTLE_MS))) {
        }
        char title[sizeof(s_title)];
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        led_state_t state = s_state;
        const char *label = status_label(state);
        memcpy(title, s_title, sizeof(title));
        unsigned rotation = s_requested_rotation;
        int battery = s_battery_step;
        bool battery_retry = s_battery_retry;
#if CONFIG_HOMEHUB_VOICE
        char *voice_text = s_voice_buffers->snapshot;
        memcpy(voice_text, s_voice_text, VOICE_TEXT_BYTES);
        unsigned voice_generation = s_voice_generation;
        unsigned voice_page = s_voice_page;
#endif
        xSemaphoreGive(s_mutex);

        xSemaphoreTake(s_panel_lock, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool rotated = rotation != s_drawn_rotation;
        if (rotated) {
            s_rotation = rotation;
            s_shown_valid = false; // One full refresh, even for a rotation-symmetric image.
        }
        if (battery_retry && !s_image_mode) s_status_drawn = false;
        bool redraw = !s_image_mode && (!s_status_drawn || label != s_drawn_label
                                        || strcmp(title, s_drawn_title) != 0);
#if CONFIG_HOMEHUB_VOICE
        if (voice_generation != s_drawn_voice_generation) {
            // A completed voice turn replaces an old image; a clear returns to status.
            s_image_mode = false;
            redraw = true;
        }
        bool page_changed = !s_image_mode && s_status_drawn && s_drawn_caption_layout
                            && voice_page != s_drawn_voice_page;
        bool page_only = page_changed && !redraw && !rotated && s_shown_valid;
        if (page_changed) redraw = true;
#endif
        // A new screen after an image gets a full refresh too.
        bool full = !s_status_drawn || s_fast_refreshes >= FULL_REFRESH_EVERY;
        full = full || rotated;
#if CONFIG_HOMEHUB_VOICE
        if (page_only) full = false;
#endif
        if (redraw) {
            s_status_band_y = STATUS_Y;
#if CONFIG_HOMEHUB_VOICE
            // Include old caption descenders when returning to the normal label.
            s_status_band_y = STATUS_Y - 18; // Include the page marker.
            s_status_band_height = EPD_H - s_status_band_y;
            bool compact = voice_text[0] != 0;
            if (compact != s_drawn_caption_layout) {
                s_status_band_y = ANIM_Y;
                s_status_band_height = EPD_H - ANIM_Y;
            } else if (!compact) {
                s_status_band_y = 158;
                s_status_band_height = 36;
            }
            s_drawn_caption_layout = compact;
#else
            s_status_band_height = STATUS_LINES * STATUS_SCALE * PIXEL_FONT_HEIGHT
                                   + (STATUS_LINES - 1) * LINE_GAP * STATUS_SCALE;
#endif
            draw_status(title, state
#if CONFIG_HOMEHUB_VOICE
                        , voice_text, voice_page
#endif
                        );
#if CONFIG_HOMEHUB_VOICE
            s_drawn_voice_generation = voice_generation;
            s_drawn_voice_page = voice_page;
#endif
            draw_battery_strip(battery, NULL);
            epd_dither();
            // A battery change must not turn a simultaneous caption update into a full refresh.
            if (s_shown_valid && !full && battery != s_drawn_battery_step)
                for (int y = 0; y < BATTERY_STRIP_H; y++) {
                    for (int x = 0; x < EPD_W; x++) {
                        int px, py;
                        epaper_154g_rotation_xy(s_rotation, x, y, &px, &py);
                        epaper_154g_rotation_put(s_frame, px, py, epaper_154g_rotation_pixel(s_shown, px, py));
                    }
                }
            // An image drawn during the refresh clears this again.
            s_status_drawn = true;
            s_drawn_label = label;
            memcpy(s_drawn_title, title, sizeof(s_drawn_title));
        }
        if (rotated && !redraw) {
            if (!s_image_mode) draw_battery_strip(battery, NULL);
            epd_dither();
        }
        xSemaphoreGive(s_lock);
        redraw = redraw || rotated;
#if CONFIG_HOMEHUB_VOICE
        int fast_refreshes = s_fast_refreshes;
#endif
        if (redraw && epd_update(full) != ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status_drawn = false;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_battery_retry = true;
            xSemaphoreGive(s_mutex);
            xSemaphoreGive(s_lock);
        }
#if CONFIG_HOMEHUB_VOICE
        if (page_only) s_fast_refreshes = fast_refreshes;
#endif
        if (rotated && s_shown_valid) s_drawn_rotation = rotation;
        if (redraw && full && s_shown_valid) {
            s_drawn_battery_step = battery;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_battery_retry = false;
            xSemaphoreGive(s_mutex);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_image_mode && s_status_drawn && s_shown_valid && battery != s_drawn_battery_step) {
            draw_battery_strip(battery, s_frame);
            int band_y = s_status_band_y, band_h = s_status_band_height;
            int refreshes = s_fast_refreshes;
            s_status_band_y = 0;
            s_status_band_height = BATTERY_STRIP_H;
            xSemaphoreGive(s_lock);
            // Never use the periodic full-refresh counter for a battery-only update.
            esp_err_t battery_err = epd_update(false);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_battery_retry = battery_err != ESP_OK;
            xSemaphoreGive(s_mutex);
            if (battery_err == ESP_OK) s_drawn_battery_step = battery;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status_band_y = band_y;
            s_status_band_height = band_h;
            s_fast_refreshes = refreshes;
        }
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_panel_lock);
        stack_monitor_poll(&stack);
    }
}

// ---- led_status.h -----------------------------------------------------------

static esp_err_t epd_init(void) {
    // Stay on once PWR is let go, keep the codec and amplifier off at boot,
    // and power the panel. Levels first, so the pins come up at them.
    gpio_set_level(BOARD_PIN_LATCH, 1);
    gpio_set_level(BOARD_PIN_AUDIO_PWR, 1);
    gpio_set_level(BOARD_PIN_AMP, 0);
    gpio_set_level(EPD_PIN_PWR, 0);
    const gpio_config_t power = {
        .pin_bit_mask = 1ULL << BOARD_PIN_LATCH | 1ULL << BOARD_PIN_AUDIO_PWR
                        | 1ULL << BOARD_PIN_AMP | 1ULL << EPD_PIN_PWR,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t power_err = gpio_config(&power);
    // Held through restarts too: a reset releases the pin, and on battery the
    // board would turn off instead of restarting.
    if (power_err == ESP_OK) power_err = gpio_hold_en(BOARD_PIN_LATCH);
    if (power_err != ESP_OK) return power_err;
    vTaskDelay(pdMS_TO_TICKS(50));  // the panel supply settles
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
        // The panels only listen.
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

static unsigned rotation_load(void) {
    nvs_handle_t display;
    uint8_t saved_rotation = 0;
    if (nvs_open("ep154_display", NVS_READONLY, &display) == ESP_OK) {
        if (nvs_get_u8(display, "rotation", &saved_rotation) != ESP_OK || saved_rotation > 3)
            saved_rotation = 0;
        nvs_close(display);
    }
    return saved_rotation;
}

bool led_status_init(void) {
    s_chunk = heap_caps_malloc(EPD_CHUNK_BYTES, MALLOC_CAP_DMA);
    s_canvas = heap_caps_malloc(CANVAS_BYTES, MALLOC_CAP_SPIRAM);
    s_frame = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_shown = heap_caps_calloc(1, EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_logical_frame = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_panel_lock = xSemaphoreCreateMutex();
    s_lock = xSemaphoreCreateMutex();
    s_mutex = xSemaphoreCreateMutex();
    bool ok = s_chunk && s_canvas && s_frame && s_panel_lock && s_lock && s_mutex;
#if CONFIG_HOMEHUB_VOICE
    s_voice_buffers = heap_caps_malloc(sizeof(*s_voice_buffers), MALLOC_CAP_SPIRAM);
    ok = ok && s_voice_buffers;
#endif
    ok = ok && s_shown;
    ok = ok && s_logical_frame;
    if (!ok) {
        ESP_LOGE(TAG, "e-paper buffer alloc failed");
        return false;
    }
    s_rotation = s_drawn_rotation = s_requested_rotation = rotation_load();
    esp_err_t err = epd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, EPD_NAME " init failed: %s", esp_err_to_name(err));
        return false;
    }
#if CONFIG_HOMEHUB_EPD154G_BATTERY
    s_battery_step = battery_step(-1, epaper_154g_battery_cached_percent());
#endif
    if (xTaskCreate(epd_task, "epd", 3072, NULL, 2, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the e-paper task");
        return false;
    }
    s_ready = true;
    xTaskNotifyGive(s_task);
#if CONFIG_HOMEHUB_VOICE
    if (!voice_epaper_154g_led_init()) ESP_LOGE(TAG, "voice LED unavailable");
#endif
    ESP_LOGI(TAG, "LED status ready: " EPD_NAME " %dx%d e-paper, " EPD_DEPTH,
             EPD_W, EPD_H);
    return true;
}

void led_status_set_state(led_state_t state) {
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = s_state != state;
#if CONFIG_HOMEHUB_VOICE
    // Transitions among equivalent Connecting states keep the answer visible.
    if (changed && status_label(s_state) != status_label(state) && s_voice_text[0]) {
        s_voice_text[0] = '\0';
        s_voice_generation++;
    }
#endif
    s_state = state;
    xSemaphoreGive(s_mutex);
    if (changed) xTaskNotifyGive(s_task);
}

#if CONFIG_HOMEHUB_VOICE
void voice_epaper_154g_show_text(const char *reply) {
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    char *text = s_voice_buffers->formatted;
    epaper_154g_caption_normalize(reply, text, EPAPER_154G_CAPTION_BYTES);
    // A new PTT turn must leave an old image even when no old caption exists.
    // The full background refresh starts here; the final reply then changes only its band.
    if (!reply || strcmp(text, s_voice_text) != 0) {
        snprintf(s_voice_text, sizeof(s_voice_text), "%s", text);
        s_voice_page = 0;
        s_voice_generation++;
        xTaskNotifyGive(s_task);
    }
    xSemaphoreGive(s_mutex);
}

void epaper_154g_status_next_page(void) {
    if (!s_ready) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    unsigned pages = (epaper_154g_caption_wrap(s_voice_text, VOICE_TEXT_COLS, 0, NULL, NULL)
                      + VOICE_TEXT_LINES - 1) / VOICE_TEXT_LINES;
    bool changed = s_status_drawn && !s_image_mode && s_drawn_caption_layout && pages > 1;
    bool retry = !s_image_mode && s_voice_text[0] && pages > 1 && s_battery_retry;
    if (changed) s_voice_page = (s_voice_page + 1) % pages;
    xSemaphoreGive(s_mutex);
    xSemaphoreGive(s_lock);
    if (changed || retry) xTaskNotifyGive(s_task);
}
#endif

void epaper_154g_status_rotate(void) {
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    unsigned rotation = (s_requested_rotation + 1) & 3;
    nvs_handle_t display;
    esp_err_t err = nvs_open("ep154_display", NVS_READWRITE, &display);
    if (err == ESP_OK) {
        err = nvs_set_u8(display, "rotation", rotation);
        if (err == ESP_OK) err = nvs_commit(display);
        nvs_close(display);
    }
    if (err == ESP_OK) s_requested_rotation = rotation;
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "display rotation: %u degrees", rotation * 90);
        xTaskNotifyGive(s_task);
    } else ESP_LOGW(TAG, "display rotation not saved: %s", esp_err_to_name(err));
}

void epaper_154g_status_set_battery(int percent) {
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int step = battery_step(s_battery_step, percent);
    bool changed = step != s_battery_step;
    s_battery_step = step;
    bool retry = s_battery_retry;
    xSemaphoreGive(s_mutex);
    if (changed || retry) xTaskNotifyGive(s_task);
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
    return EPD_INK_BITS;
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
            line[i] = px;
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
    if (show) {
        s_status_band_y = 0;
        s_status_band_height = EPD_H;
        if (epd_update(s_fast_refreshes >= FULL_REFRESH_EVERY) != ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_image_mode) s_image_dirty = true;
            xSemaphoreGive(s_lock);
        }
    }
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
