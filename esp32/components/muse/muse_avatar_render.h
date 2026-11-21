/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
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

enum { MUSE_AVATAR_MAX_EDGE = 466, MUSE_AVATAR_MAX_PIXELS = 466 * 466 };
typedef enum { MUSE_AVATAR_BUILTIN = 0, MUSE_AVATAR_PERSONAL = 1 } muse_avatar_source_t;
typedef enum { MUSE_AVATAR_NEAREST, MUSE_AVATAR_BILINEAR, MUSE_AVATAR_FAST_SMOOTH } muse_avatar_filter_t;
typedef struct {
    const uint16_t *pixels;
    int width, height;
} muse_avatar_pixels_t;
typedef struct {
    int zoom_x, zoom_y;  /* 900..1000; contain-fit, never crop */
    int offset_y;        /* -32..32, clamped to the available padding */
    muse_avatar_filter_t filter; /* fine still-image or fast video interpolation */
} muse_avatar_transform_t;

bool muse_avatar_blit_transform(const muse_avatar_pixels_t *image, int canvas_px,
                               const muse_avatar_transform_t *transform, uint16_t background,
                               uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);

/* Immutable RGB565 input; contain-fit into the current square canvas. zoom is
 * 950..1000, so pet/speech feedback never crops or moves outside the canvas.
 * Coordinates are inclusive, matching muse_pixel_scale. No allocation or I/O. */
bool muse_avatar_blit(const muse_avatar_pixels_t *image, int canvas_px, int zoom,
                      uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);

/* Same blit with explicit contain-fit padding. Asset pixels are unchanged. */
bool muse_avatar_blit_background(const muse_avatar_pixels_t *image, int canvas_px, int zoom,
                                 uint16_t background, uint16_t *dst, int stride_px,
                                 int x0, int x1, int y0, int y1);
