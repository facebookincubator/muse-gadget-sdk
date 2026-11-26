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
typedef struct fake_lv_obj lv_obj_t;
typedef struct { uint8_t r, g, b; } lv_color_t;
typedef struct { int unused; } lv_font_t;
extern const lv_font_t lv_font_montserrat_20;
enum { LV_ALIGN_CENTER, LV_LABEL_LONG_MODE_WRAP, LV_OPA_COVER = 255, LV_TEXT_ALIGN_CENTER };
enum { LV_OBJ_FLAG_CLICKABLE = 1, LV_OBJ_FLAG_HIDDEN = 2, LV_OBJ_FLAG_SCROLLABLE = 4 };
lv_obj_t *lv_screen_active(void);
lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
const char *lv_label_get_text(const lv_obj_t *o);
void lv_label_set_text(lv_obj_t *o, const char *t);
void lv_label_set_long_mode(lv_obj_t *o, int m);
void lv_obj_add_flag(lv_obj_t *o, int f);
void lv_obj_remove_flag(lv_obj_t *o, int f);
void lv_obj_set_flag(lv_obj_t *o, int f, bool on);
bool lv_obj_has_flag(const lv_obj_t *o, int f);
void lv_obj_align(lv_obj_t *o, int a, int x, int y);
void lv_obj_invalidate(lv_obj_t *o);
void lv_obj_move_foreground(lv_obj_t *o);
void lv_obj_remove_style_all(lv_obj_t *o);
void lv_obj_set_size(lv_obj_t *o, int w, int h);
void lv_obj_set_width(lv_obj_t *o, int w);
void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, int sel);
void lv_obj_set_style_bg_opa(lv_obj_t *o, int opa, int sel);
void lv_obj_set_style_text_align(lv_obj_t *o, int a, int sel);
void lv_obj_set_style_text_color(lv_obj_t *o, lv_color_t c, int sel);
void lv_obj_set_style_text_font(lv_obj_t *o, const lv_font_t *f, int sel);
lv_color_t lv_color_black(void);
lv_color_t lv_color_hex(uint32_t c);
