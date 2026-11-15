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
/* Host fake for tests/video_fill_harness.c. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct fake_lv_obj { int unused; } lv_obj_t;
typedef struct { int unused; } lv_font_t;
typedef struct { uint32_t v; } lv_color_t;
typedef uint32_t lv_obj_flag_t;
typedef struct _lv_display_t lv_display_t;
typedef struct _lv_indev_t lv_indev_t;
typedef uint8_t lv_align_t;

enum { LV_OBJ_FLAG_HIDDEN = 1, LV_OBJ_FLAG_CLICKABLE = 2, LV_OBJ_FLAG_SCROLLABLE = 4 };
enum { LV_ALIGN_CENTER = 9, LV_TEXT_ALIGN_CENTER = 2, LV_OPA_COVER = 255, LV_LABEL_LONG_MODE_WRAP = 0 };

static lv_font_t lv_font_montserrat_20;
static lv_obj_t fake_lv_objs[4];
static inline lv_obj_t *lv_screen_active(void) { return &fake_lv_objs[0]; }
static inline lv_obj_t *lv_obj_create(lv_obj_t *p) { (void)p; return &fake_lv_objs[1]; }
static inline lv_obj_t *lv_label_create(lv_obj_t *p) { (void)p; return &fake_lv_objs[2]; }
static inline lv_color_t lv_color_black(void) { return (lv_color_t){ 0 }; }
static inline lv_color_t lv_color_hex(uint32_t c) { return (lv_color_t){ c }; }
static inline void lv_obj_remove_style_all(lv_obj_t *o) { (void)o; }
static inline void lv_obj_set_size(lv_obj_t *o, int w, int h) { (void)o; (void)w; (void)h; }
static inline void lv_obj_set_width(lv_obj_t *o, int w) { (void)o; (void)w; }
static inline void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, int s) { (void)o; (void)c; (void)s; }
static inline void lv_obj_set_style_bg_opa(lv_obj_t *o, int v, int s) { (void)o; (void)v; (void)s; }
static inline void lv_obj_set_style_text_font(lv_obj_t *o, const lv_font_t *f, int s) { (void)o; (void)f; (void)s; }
static inline void lv_obj_set_style_text_color(lv_obj_t *o, lv_color_t c, int s) { (void)o; (void)c; (void)s; }
static inline void lv_obj_set_style_text_align(lv_obj_t *o, int a, int s) { (void)o; (void)a; (void)s; }
static inline void lv_obj_remove_flag(lv_obj_t *o, lv_obj_flag_t f) { (void)o; (void)f; }
static inline void lv_obj_add_flag(lv_obj_t *o, lv_obj_flag_t f) { (void)o; (void)f; }
static inline void lv_obj_set_flag(lv_obj_t *o, lv_obj_flag_t f, bool v) { (void)o; (void)f; (void)v; }
static inline bool lv_obj_has_flag(const lv_obj_t *o, lv_obj_flag_t f) { (void)o; (void)f; return true; }
static inline void lv_obj_move_foreground(lv_obj_t *o) { (void)o; }
static inline void lv_obj_invalidate(lv_obj_t *o) { (void)o; }
static inline void lv_obj_align(lv_obj_t *o, int a, int x, int y) { (void)o; (void)a; (void)x; (void)y; }
static inline void lv_label_set_text(lv_obj_t *o, const char *t) { (void)o; (void)t; }
static inline const char *lv_label_get_text(const lv_obj_t *o) { (void)o; return ""; }
static inline void lv_label_set_long_mode(lv_obj_t *o, int m) { (void)o; (void)m; }
