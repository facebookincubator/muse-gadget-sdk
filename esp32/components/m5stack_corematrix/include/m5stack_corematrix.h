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

// M5Stack CoreMatrix (ESP32-C61) board support: the I2C bus, BtnA-C on the
// M5PM1 power chip, and the 16x16 white LED matrix (TM1680, powered through
// the M5IOE1 expander) as an esp_lcd panel.
// https://docs.m5stack.com/en/core/CoreMatrix

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

typedef enum {
    COREMATRIX_BTN_A,   // KEY1
    COREMATRIX_BTN_B,   // KEY2
    COREMATRIX_BTN_C,   // KEY3
} corematrix_button_t;

// Starts the I2C bus and the power chip. The buttons can be read from here on.
esp_err_t corematrix_init(void);

// Stores whether `button` is down and returns true, or returns false, leaving
// *pressed alone, if it couldn't be read.
bool corematrix_button_read(corematrix_button_t button, bool *pressed);

// Powers the matrix and makes it a panel, cleared and on. One bit per pixel:
// esp_lcd_panel_draw_bitmap() takes rows of (x_end - x_start + 7) / 8 bytes,
// a lit pixel a set bit, the leftmost pixel in each byte's top bit. Call after
// corematrix_init().
esp_err_t corematrix_new_matrix(esp_lcd_panel_handle_t *panel);
