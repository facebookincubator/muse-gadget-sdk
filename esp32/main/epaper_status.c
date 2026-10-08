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

// led_status.h on e-paper boards: a still status screen (the agent's name,
// the character and a line of status text), redrawn only when the text
// changes. The panel driver is behind epd_panel.h; this file never touches
// a GPIO or sends a command.
//
// Everything is drawn into a PSRAM canvas (8-bit gray on mono panels,
// RGB565 on colour panels), then dithered to the panel's inks just before
// the refresh, so photos keep their tones and the inks themselves stay exact.

#include "led_status.h"
#include "epd_panel.h"
#include "sdkconfig.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "happy_anim.h"
#include "pixel_font.h"
#include "stack_monitor.h"

static const char *TAG = "link.led";

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

// ---- Panel state ------------------------------------------------------------

static const epd_panel_t *s_panel;

#define EPD_W       (s_panel->width)
#define EPD_H       (s_panel->height)
#define EPD_COLOR   (s_panel->bits_per_pixel > 1)
#define EPD_ROW_BYTES (EPD_COLOR ? (EPD_W / 2) : (EPD_W / 8))
#define EPD_FRAME_BYTES (EPD_ROW_BYTES * EPD_H)

// A status change waits this long for the next, so that the burst while
// connecting costs one refresh.
#define STATUS_SETTLE_MS 1500
#define FULL_REFRESH_EVERY 10

static uint8_t *s_canvas;   // what is being drawn (gray or RGB565)
static size_t s_canvas_bytes;
static uint8_t *s_frame;    // s_canvas dithered, about to be shown
static uint8_t *s_shown;    // what the panel shows (mono panels only)
static int16_t *s_err;      // dithering error rows
static bool s_ready;

static SemaphoreHandle_t s_panel_lock;
static int s_fast_refreshes = FULL_REFRESH_EVERY;

static SemaphoreHandle_t s_lock;
static bool s_image_mode;
static bool s_image_dirty;
static bool s_status_drawn;
static const char *s_drawn_label;
static char s_drawn_title[48];

static SemaphoreHandle_t s_mutex;
static led_state_t s_state = LED_STATE_BOOT;
static char s_title[48];
static TaskHandle_t s_task;

static void epd_dither(void) {
    if (EPD_COLOR) {
        dither_color((const uint16_t *)s_canvas, s_frame, EPD_W, EPD_H, s_err);
    } else {
        dither_frame(s_canvas, s_frame, EPD_W, EPD_H, s_err);
    }
}

// ---- Status screen ----------------------------------------------------------

static const char *status_label(led_state_t state) {
    switch (state) {
        case LED_STATE_BOOT:                     return "Starting";
        case LED_STATE_SETUP_IDLE:
            return "Press " CONFIG_HOMEHUB_BUTTON_LABEL " to set up";
        case LED_STATE_BLE_ADVERTISING:          return "Ready to pair over Bluetooth";
        case LED_STATE_BLE_CONNECTED:            return "Pairing";
        case LED_STATE_PAIRING_CONFIRM_REQUIRED:
            return "Press " CONFIG_HOMEHUB_BUTTON_LABEL " to confirm";
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

static void draw_text(const char *text, int y, int max_scale) {
    const int adv = PIXEL_FONT_WIDTH + 1;
    const int min_scale = EPD_W >= 400 ? 2 : 1;
    int n = (int)strlen(text);
    int scale = max_scale;
    while (scale > min_scale && n * adv * scale - scale > EPD_W) scale--;
    if (n > (EPD_W + scale) / (adv * scale)) n = (EPD_W + scale) / (adv * scale);
    int x0 = (EPD_W - (n * adv * scale - scale)) / 2;
    y += (max_scale - scale) * PIXEL_FONT_HEIGHT / 2;
    int pixel_bytes = EPD_COLOR ? 2 : 1;
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < PIXEL_FONT_FIRST || ch > PIXEL_FONT_LAST) ch = '?';
        const uint8_t *glyph = pixel_font[ch - PIXEL_FONT_FIRST];
        for (int gx = 0; gx < PIXEL_FONT_WIDTH; gx++) {
            for (int gy = 0; gy < PIXEL_FONT_HEIGHT; gy++) {
                if (!(glyph[gx] >> gy & 1)) continue;
                int px = x0 + (i * adv + gx) * scale, py = y + gy * scale;
                for (int r = 0; r < scale; r++) {
                    memset(s_canvas + ((size_t)(py + r) * EPD_W + px) * pixel_bytes,
                           0, scale * pixel_bytes);
                }
            }
        }
    }
}

static void draw_character(void) {
    int anim_scale = s_panel->anim_scale;
    int anim_w = HAPPY_ANIM_WIDTH * anim_scale;
    int anim_x = (EPD_W - anim_w) / 2;
    int anim_y = s_panel->anim_y;
    int pixel_bytes = EPD_COLOR ? 2 : 1;
    const uint8_t *cells = happy_anim_frames[0];
    for (int cy = 0; cy < HAPPY_ANIM_HEIGHT; cy++) {
        uint8_t *line = s_canvas + (size_t)(anim_y + cy * anim_scale) * EPD_W * pixel_bytes
                        + anim_x * pixel_bytes;
        for (int cx = 0; cx < HAPPY_ANIM_WIDTH; cx++) {
            uint8_t c = cells[cy * HAPPY_ANIM_WIDTH + cx];
            uint16_t be = happy_anim_palette[c];
            uint16_t px = (uint16_t)(be >> 8 | be << 8);
            if (EPD_COLOR) {
                uint16_t v = c == 0 ? 0xFFFF : px;
                for (int k = 0; k < anim_scale; k++) {
                    uint16_t *p = (uint16_t *)line + cx * anim_scale + k;
                    *p = v;
                }
            } else {
                uint8_t v = c == 0 ? 255 : luma565(px);
                memset(line + cx * anim_scale, v, anim_scale);
            }
        }
        for (int k = 1; k < anim_scale; k++) {
            memcpy(line + k * EPD_W * pixel_bytes, line, anim_w * pixel_bytes);
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
        bool full = !s_status_drawn || s_fast_refreshes >= FULL_REFRESH_EVERY;
        if (redraw) {
            memset(s_canvas, 255, s_canvas_bytes);
            draw_text(title, s_panel->title_y, s_panel->title_max_scale);
            draw_character();
            draw_text(label, s_panel->status_y, s_panel->status_scale);
            epd_dither();
            s_status_drawn = true;
            s_drawn_label = label;
            memcpy(s_drawn_title, title, sizeof(s_drawn_title));
        }
        xSemaphoreGive(s_lock);
        if (redraw) {
            esp_err_t err = s_panel->update(s_frame, s_shown, full);
            if (err == ESP_OK) {
                if (s_shown) memcpy(s_shown, s_frame, EPD_FRAME_BYTES);
                s_fast_refreshes = full ? 0 : s_fast_refreshes + 1;
            } else {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status_drawn = false;
                xSemaphoreGive(s_lock);
            }
        }
        xSemaphoreGive(s_panel_lock);
        stack_monitor_poll(&stack);
    }
}

// ---- led_status.h -----------------------------------------------------------

bool led_status_init(void) {
    s_panel = epd_panel_get();
    int pixel_bytes = EPD_COLOR ? 2 : 1;
    s_canvas_bytes = (size_t)EPD_W * EPD_H * pixel_bytes;
    s_canvas = heap_caps_malloc(s_canvas_bytes, MALLOC_CAP_SPIRAM);
    s_frame = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    size_t err_values;
    if (EPD_COLOR) {
        s_shown = NULL;
        err_values = 6 * (EPD_W + 2);
    } else {
        s_shown = heap_caps_calloc(1, EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
        err_values = 2 * (EPD_W + 2);
    }
    s_err = heap_caps_malloc(err_values * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    s_panel_lock = xSemaphoreCreateMutex();
    s_lock = xSemaphoreCreateMutex();
    s_mutex = xSemaphoreCreateMutex();
    bool ok = s_canvas && s_frame && s_err && s_panel_lock && s_lock && s_mutex;
    if (!EPD_COLOR) ok = ok && s_shown;
    if (!ok) {
        ESP_LOGE(TAG, "e-paper buffer alloc failed");
        return false;
    }
    esp_err_t err = s_panel->init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s init failed: %s", s_panel->name, esp_err_to_name(err));
        return false;
    }
    if (xTaskCreate(epd_task, "epd", 3072, NULL, 2, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the e-paper task");
        return false;
    }
    s_ready = true;
    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "LED status ready: %s %dx%d e-paper, %s",
             s_panel->name, EPD_W, EPD_H, s_panel->depth);
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
    return s_panel->bits_per_pixel;
}

bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels) {
    if (!s_ready || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > EPD_W || y + h > EPD_H) {
        return false;
    }
    const uint8_t *src = (const uint8_t *)pixels;
    int pixel_bytes = EPD_COLOR ? 2 : 1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_image_mode) {
        s_image_mode = true;
        s_status_drawn = false;
        memset(s_canvas, 255, s_canvas_bytes);
    }
    for (int r = 0; r < h; r++) {
        uint8_t *line = s_canvas + ((size_t)(y + r) * EPD_W + x) * pixel_bytes;
        if (EPD_COLOR) {
            memcpy(line, src, w * 2);
            src += w * 2;
        } else {
            for (int i = 0; i < w; i++, src += 2) {
                line[i] = luma565((uint16_t)(src[0] << 8 | src[1]));
            }
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
        if (s_panel->update(s_frame, s_shown, true) == ESP_OK && s_shown) {
            memcpy(s_shown, s_frame, EPD_FRAME_BYTES);
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
