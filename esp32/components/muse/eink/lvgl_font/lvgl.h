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
/*
 * The LVGL v9.5 font types an lv_font_conv font declares, so the e-ink UI,
 * which builds without LVGL, compiles fonts/muse_font_cjk_16.c as is and reads
 * its glyphs (eink/muse_eink_fonts.cpp). Only those two files see it.
 */
#include <stddef.h>
#include <stdint.h>

#define LVGL_VERSION_MAJOR 9
#define LVGL_VERSION_MINOR 5
#define LV_VERSION_CHECK(x, y, z) \
    (x == LVGL_VERSION_MAJOR && (y < LVGL_VERSION_MINOR || (y == LVGL_VERSION_MINOR && z <= 0)))
#define LV_ATTRIBUTE_LARGE_CONST

typedef struct {
    uint32_t bitmap_index : 20;
    uint32_t adv_w : 12;        /* 1/16 px */
    uint8_t box_w;
    uint8_t box_h;
    int8_t ofs_x;
    int8_t ofs_y;               /* the box's bottom, up from the baseline */
} lv_font_fmt_txt_glyph_dsc_t;

typedef enum {
    LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL,
    LV_FONT_FMT_TXT_CMAP_SPARSE_FULL,
    LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY,
    LV_FONT_FMT_TXT_CMAP_SPARSE_TINY,
} lv_font_fmt_txt_cmap_type_t;

typedef struct {
    uint32_t range_start;
    uint16_t range_length;
    uint16_t glyph_id_start;
    const uint16_t *unicode_list;
    const void *glyph_id_ofs_list;
    uint16_t list_length;
    lv_font_fmt_txt_cmap_type_t type;
} lv_font_fmt_txt_cmap_t;

typedef struct {
    const uint8_t *glyph_bitmap;
    const lv_font_fmt_txt_glyph_dsc_t *glyph_dsc;
    const lv_font_fmt_txt_cmap_t *cmaps;
    const void *kern_dsc;
    uint16_t kern_scale;
    uint16_t cmap_num : 9;
    uint16_t bpp : 4;
    uint16_t kern_classes : 1;
    uint16_t bitmap_format : 2;
} lv_font_fmt_txt_dsc_t;

enum { LV_FONT_SUBPX_NONE };

typedef struct _lv_font_t {
    const void *get_glyph_dsc;
    const void *get_glyph_bitmap;
    int32_t line_height;
    int32_t base_line;
    uint8_t subpx;
    int8_t underline_position;
    int8_t underline_thickness;
    const void *dsc;
    const struct _lv_font_t *fallback;
    void *user_data;
} lv_font_t;

#define lv_font_get_glyph_dsc_fmt_txt NULL
#define lv_font_get_bitmap_fmt_txt NULL
