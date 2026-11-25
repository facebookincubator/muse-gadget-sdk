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
    esp_err_t (*update_gray4)(const uint8_t *frame);
    // UC8179's two packed one-bit planes, each the same size as a mono frame.
    esp_err_t (*update_gray4_planes)(const uint8_t *old_plane, const uint8_t *new_plane);
    // One luma byte per pixel, in screen order.
    esp_err_t (*update_gray16)(const uint8_t *luma);
} epd_panel_t;

const epd_panel_t *epd_panel_get(void);

// reTerminal grayscale backends only. Start with a white canvas.
void led_status_draw_gray_begin(void);
// Finish a reTerminal grayscale image. False if there was no image to
// show, the panel does not support this depth, or the refresh failed.
bool led_status_draw_gray_done(int levels);
