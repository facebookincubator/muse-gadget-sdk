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

#pragma once

#include <stddef.h>

#include "esp_lv_adapter.h"

/*
 * LVGL on a QSPI panel, drawn in a few tall bands in PSRAM.
 *
 * LVGL walks every widget once per band it draws, so a full screen in 4 tall
 * bands rather than 40-odd short ones is what lets a swipe keep up with the
 * finger. The SPI DMA can't read PSRAM fast enough while both cores draw, so a
 * task copies each band out a few rows at a time, into one of two internal
 * buffers while the other is on the wire. LVGL draws the next band meanwhile,
 * in the panel's byte order so nothing has to swap each one.
 */

/*
 * One task sends to the panel, on MUSE_UI_CORE, and the panel's SPI interrupt
 * has to be there too. ESP-IDF's SPI bus lock can lose a task waiting for the
 * bus if the interrupt that finishes a transfer is on the other core: the
 * interrupt decides nobody is waiting, the task starts to, and it waits
 * forever with the screen half drawn. An interrupt goes to the core of the
 * task that set it up, so set up the panel's SPI bus from a task pinned to
 * MUSE_UI_CORE.
 */

/*
 * esp_lv_adapter_register_display() with bands of `lines` rows, sent in
 * pieces of up to chunk_bytes, which the panel's SPI bus must take in one
 * transfer. Pieces are whole even numbers of rows. Takes over the panel IO's
 * color-done callback. Call it from the task that set up the bus. NULL if
 * something couldn't be set up.
 */
lv_display_t *muse_lcd_bands_register(esp_lv_adapter_display_config_t cfg, int lines, size_t chunk_bytes);

/*
 * Runs fn(arg) on the task that sends, between bands, and returns once it
 * has. Any other use of the panel or its IO once registered goes through
 * this, whichever core it's called from.
 */
void muse_lcd_bands_run(void (*fn)(void *arg), void *arg);

/*
 * The panel's tearing-effect output on `gpio` (high during vertical
 * blanking; the panel sends it once told to, DCS 35h). Each band LVGL sends
 * then goes out at a moment its write won't meet the panel's scan (usually
 * at once): no tear line where the scan would have passed the write. Logs
 * the refresh period, and how many bands it held or couldn't keep clear,
 * every 1000 bands. Call it after muse_lcd_bands_register.
 */
void muse_lcd_bands_set_te(int gpio);
