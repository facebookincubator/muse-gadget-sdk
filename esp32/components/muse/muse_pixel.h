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
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#include "muse_state.h"

/*
 * Muse avatar renderer. Most boards use a procedural 64x64 pixel-art grid;
 * the Waveshare 1.75C uses pre-rendered 160x160 RGB565 artwork instead.
 * Both implement the same frame and scaling API for the UI.
 */

#if defined(CONFIG_MUSE_BOARD_WAVESHARE_S3_175C) && CONFIG_MUSE_BOARD_WAVESHARE_S3_175C
#define MUSE_PX_W 160
#define MUSE_PX_H 160
#else
#define MUSE_PX_W 64
#define MUSE_PX_H 64
#endif

typedef struct {
    muse_mode_t mode;
    float t;         /* seconds since boot */
    float mode_t;    /* seconds in current mode */
    float level;     /* 0..1 live audio level */
    float happy;     /* 0..1 pet reaction */
    float working;   /* 0 or 1 in THINKING: a call's Muse is busy on work it was asked
                      * for, nothing playing. A renderer that ignores it shows THINKING. */
} muse_pose_t;

/* Accent colour of a mode (for the surrounding UI), as 0xRRGGBB. */
uint32_t muse_pixel_accent(muse_mode_t mode);

/* Optional: the accent of the working pose (pose->working), as 0xRRGGBB, so
 * the UI around the avatar matches it. 0, or a renderer without it (the UI
 * links a weak default), keeps THINKING's accent. */
uint32_t muse_pixel_working_accent(void);

/* Render one frame into Muse's own MUSE_PX_W x MUSE_PX_H grid. */
void muse_pixel_render(const muse_pose_t *pose);

/* Size (square, in screen pixels) muse_pixel_scale() blows the grid up to. */
void muse_pixel_set_size(int px);

/*
 * Write screen pixels [x0, x1] x [y0, y1] of the blown-up frame as RGB565,
 * stride_px apart. Cheap enough to call per display strip, so the full-size
 * image never has to exist in RAM.
 */
void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);

/* Optional theme hook. The bundled renderer recolors only its backdrop. */
void muse_pixel_set_background(uint32_t rgb);
