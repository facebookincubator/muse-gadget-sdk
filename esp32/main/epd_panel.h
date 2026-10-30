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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    const char *name;
    const char *depth;
    int width;
    int height;
    int bits_per_pixel;

    int title_y;
    int title_max_scale;
    int anim_scale;
    int anim_y;
    int status_y;
    int status_scale;

    esp_err_t (*init)(void);
    // Show `frame` on the panel. `prev` is what the panel currently shows
    // (NULL on colour panels). Both are `row_bytes * height` long.
    esp_err_t (*update)(const uint8_t *frame, const uint8_t *prev, bool full);
    // Optional four-level grayscale refresh from a packed 2-bit frame.
    esp_err_t (*update_gray4)(const uint8_t *frame);
    // Optional UC8179 four-gray refresh from two 1-bit data planes.
    esp_err_t (*update_gray4_planes)(const uint8_t *old_plane, const uint8_t *new_plane);
    // E1003: 8-bit luminance source, quantized to the IT8951's 16 levels.
    esp_err_t (*update_gray16)(const uint8_t *luma);
    // E1003: update only an aligned rectangle of a complete monochrome frame.
    esp_err_t (*update_partial)(const uint8_t *frame, int x, int y, int w, int h);
} epd_panel_t;

const epd_panel_t *epd_panel_get(void);
