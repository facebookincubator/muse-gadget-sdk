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

/* Canvas gradients, rounded borders and shadows, using the shared style API.
 * Build from the SDK folder: python3 build.py examples/canvas_effects.c */
#include "../gadget.h"

GADGET_MANIFEST("{\"api\":\"1.1\",\"name\":\"canvas-effects\",\"title\":\"Canvas effects\",\"icon\":\"chart\",\"color\":\"indigo\"}");

GADGET_EXPORT(on_start) void on_start(void)
{
    int32_t w = sys_info(INFO_SCREEN_WIDTH), h = sys_info(INFO_SCREEN_HEIGHT);
    int32_t side = w < h ? w : h;
    int32_t inset = sys_info(INFO_SAFE_INSET) + side / 12;
    int32_t width = w - inset * 2, height = (h - inset * 2) / 3;
    if (width < 1 || height < 1) return;

    int32_t canvas = ui_create(W_CANVAS, UI_ROOT);
    ui_set(canvas, P_WIDTH, w);
    ui_set(canvas, P_HEIGHT, h);
    cv_fill(canvas, 0x121214);

    int32_t style = ui_style_create();
    ui_style_set(style, P_RADIUS, side / 20);
    ui_style_set(style, P_BG_COLOR, 0x5E7FA6);
    ui_style_set(style, P_BG_GRAD_COLOR, 0x7E639F);
    ui_style_set(style, P_BG_GRAD_DIR, GRAD_HOR);
    ui_style_set(style, P_BORDER_COLOR, 0xD8D5CC);
    ui_style_set(style, P_BORDER_WIDTH, 1);
    ui_style_set(style, P_BORDER_OPA, 100);
    ui_style_set(style, P_SHADOW_COLOR, 0x000000);
    ui_style_set(style, P_SHADOW_WIDTH, 12);
    ui_style_set(style, P_SHADOW_SPREAD, 2);
    ui_style_set(style, P_SHADOW_OFFSET_Y, 6);
    ui_style_set(style, P_SHADOW_OPA, 180);
    cv_rect_style(canvas, inset, inset, width, height, style);

    /* Reuse the style: this paints the new values without changing the first card. */
    ui_style_set(style, P_BG_COLOR, 0x5FA87D);
    ui_style_set(style, P_BG_GRAD_COLOR, 0x5E7FA6);
    ui_style_set(style, P_BG_GRAD_DIR, GRAD_VER);
    cv_rect_style(canvas, inset, h - inset - height, width, height, style);
}
