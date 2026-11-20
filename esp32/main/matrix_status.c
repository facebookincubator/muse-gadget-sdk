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

// Link's status on a 16x16 one-bit LED matrix, in place of led_status.c: a
// picture for each state, and Muse's images thresholded to lit or dark. The
// matrix is an esp_lcd panel taking one bit per pixel, leftmost in each
// byte's top bit; the board supplies it in matrix_start().

#include "led_status.h"

#include <stdatomic.h>
#include <string.h>

#include "button.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if CONFIG_HOMEHUB_LED_BACKEND_M5_COREMATRIX
#include "m5stack_corematrix.h"
#endif

static const char *TAG = "link.matrix";

#define SIZE     16
#define FRAME_MS 200

typedef uint8_t frame_t[SIZE][SIZE / 8];

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lock;    // s_frame and s_image
static frame_t s_frame;
static bool s_image;                // showing an image, not the status
static atomic_int s_state = LED_STATE_BOOT;
static unsigned s_draw_failures;

// ---- The board ---------------------------------------------------------------

#if CONFIG_HOMEHUB_LED_BACKEND_M5_COREMATRIX
static bool read_setup_button(bool *pressed) {
    return corematrix_button_read(COREMATRIX_BTN_A, pressed);
}

static esp_err_t matrix_start(esp_lcd_panel_handle_t *panel) {
    esp_err_t err = corematrix_init();
    if (err != ESP_OK) return err;
    // BtnA, on the power chip, is the setup button, matrix or no matrix.
    button_set_reader(read_setup_button);
    return corematrix_new_matrix(panel);
}
#endif

// ---- Pictures ----------------------------------------------------------------

static void pixel(frame_t f, int x, int y) {
    if ((unsigned)x < SIZE && (unsigned)y < SIZE) f[y][x / 8] |= (uint8_t)(0x80 >> (x % 8));
}

static void block(frame_t f, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) pixel(f, x + i, y + j);
    }
}

// Two eyes and a smile; X eyes and a flat mouth for an error.
static void draw_face(frame_t f, bool error) {
    if (error) {
        for (int i = 0; i < 5; i++) {
            pixel(f, 2 + i, 3 + i);
            pixel(f, 6 - i, 3 + i);
            pixel(f, 9 + i, 3 + i);
            pixel(f, 13 - i, 3 + i);
        }
    } else {
        block(f, 3, 4, 2, 2);
        block(f, 10, 4, 2, 2);
        pixel(f, 3, 10);
        pixel(f, 12, 10);
        pixel(f, 4, 11);
        pixel(f, 11, 11);
    }
    for (int x = 5; x <= 10; x++) pixel(f, x, 12);
}

static void draw_check(frame_t f) {
    for (int i = 0; i < 5; i++) block(f, 2 + i, 8 + i, 1, 2);
    for (int i = 0; i < 9; i++) block(f, 6 + i, 12 - i, 1, 2);
}

// Three dots chasing round a ring.
static void draw_spinner(frame_t f, unsigned phase) {
    static const int8_t ring[8][2] = {
        {7, 2}, {11, 3}, {13, 7}, {11, 11}, {7, 13}, {3, 11}, {2, 7}, {3, 3},
    };
    for (unsigned i = 0; i < 3; i++) {
        const int8_t *p = ring[(phase + i) % 8];
        block(f, p[0], p[1], 2, 2);
    }
}

static void draw_status(frame_t f, led_state_t state, unsigned phase) {
    memset(f, 0, sizeof(frame_t));
    switch (state) {
    case LED_STATE_BOOT:
    case LED_STATE_SETUP_IDLE:
    case LED_STATE_BLE_ADVERTISING:
        draw_face(f, false);
        break;
    case LED_STATE_PAIRING_CONFIRM_REQUIRED:
        // A flashing "!": press the button.
        if (phase & 1) {
            block(f, 7, 3, 2, 7);
            block(f, 7, 12, 2, 2);
        }
        break;
    case LED_STATE_WS_CONNECTED:
    case LED_STATE_VM_OK:
        draw_check(f);
        break;
    case LED_STATE_UNPAIRED:
    case LED_STATE_ERROR:
        draw_face(f, true);
        break;
    default:
        draw_spinner(f, phase);
        break;
    }
}

static void show(void) {
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, SIZE, SIZE, s_frame);
    if (err == ESP_OK) {
        s_draw_failures = 0;
    } else if (s_draw_failures++ % 50 == 0) {
        ESP_LOGW(TAG, "matrix draw failed: %s", esp_err_to_name(err));
    }
}

static void matrix_task(void *arg) {
    (void)arg;
    for (unsigned phase = 0;; phase++) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_image) {
            draw_status(s_frame, (led_state_t)atomic_load(&s_state), phase);
            show();
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}

// ---- led_status.h --------------------------------------------------------------

bool led_status_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;
    esp_err_t err = matrix_start(&s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED matrix unavailable: %s", esp_err_to_name(err));
        s_panel = NULL;
        return false;
    }
    draw_status(s_frame, LED_STATE_BOOT, 0);
    show();
    if (xTaskCreate(matrix_task, "matrix", 3072, NULL, 1, NULL) != pdPASS) {
        s_panel = NULL;
        return false;
    }
    ESP_LOGI(TAG, "LED status ready: 16x16 LED matrix");
    return true;
}

void led_status_set_state(led_state_t state) {
    atomic_store(&s_state, (int)state);
}

// 16x16 pixels can't show the agent's name.
void led_status_set_title(const char *title) {
    (void)title;
}

// ---- Images (display.draw_url, display.draw_file) ------------------------------

bool led_status_display_info(int *width, int *height) {
    if (!s_panel) return false;
    *width = SIZE;
    *height = SIZE;
    return true;
}

int led_status_display_bits(void) {
    return s_panel ? 1 : 0;
}

// RGB565, high byte first. A pixel lights if it's brighter than mid-gray:
// no dithering, which at 16x16 only breaks up icons.
bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels) {
    if (!s_panel || !pixels || x < 0 || y < 0 || w <= 0 || h <= 0 || w > SIZE - x || h > SIZE - y) {
        return false;
    }
    const uint8_t *src = (const uint8_t *)pixels;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_image) {
        s_image = true;
        memset(s_frame, 0, sizeof(s_frame));
    }
    for (int r = 0; r < h; r++) {
        for (int i = 0; i < w; i++, src += 2) {
            unsigned px = (unsigned)src[0] << 8 | src[1];
            unsigned red = (px >> 11) * 255 / 31, green = (px >> 5 & 63) * 255 / 63,
                     blue = (px & 31) * 255 / 31;
            uint8_t *byte = &s_frame[y + r][(x + i) / 8], bit = (uint8_t)(0x80 >> ((x + i) % 8));
            if ((77 * red + 150 * green + 29 * blue) >> 8 >= 128) {
                *byte |= bit;
            } else {
                *byte &= (uint8_t)~bit;
            }
        }
    }
    show();
    xSemaphoreGive(s_lock);
    return true;
}

// Each draw is shown at once.
void led_status_draw_done(void) {
}

void led_status_show_animation(void) {
    if (!s_panel) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_image = false;    // the matrix task draws the status again
    xSemaphoreGive(s_lock);
}
