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

/* Host fake for tests/video_slots/harness.c (tests/test_video_untrusted.py). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "muse_state.h"
#include "muse_board_win.h"
typedef struct {
    const char *name;
    int width, height;
    bool round;
    void (*draw_frame)(int x1, int y1, int x2, int y2, int rows,
                       void (*fill)(uint8_t *dst, int stride, int row, int rows, void *arg), void *arg);
    uint8_t frame_x_align;
    void (*draw_windows)(int x0, int y0, const muse_board_win_t *win, int n, int rows,
                         void (*fill)(uint8_t *dst, int stride, int x, int y, int w, int rows, void *arg), void *arg);
    uint32_t (*panel_writes)(void);
    uint32_t (*panel_stack_free)(void);
    bool video_fill;   /* video fills the panel (the C6: muse_video_fit's fill) */
} muse_board_t;
extern const muse_board_t *muse_board;
const muse_board_t *muse_board_get(void);
