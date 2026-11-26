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

#include "muse_type.h"

#include "sdkconfig.h"

/* MUSE_TYPE_ALL: the host simulator, which renders every board. Firmware
 * builds a board's own set with CONFIG_MUSE_UI_PANEL_TYPE, else the
 * built-in sizes below. */
#if MUSE_TYPE_ALL || (CONFIG_MUSE_UI_PANEL_TYPE && CONFIG_MUSE_BOARD_M5STACK_STOPWATCH)
#define TYPE_466 1
#else
#define TYPE_466 0
#endif
#if MUSE_TYPE_ALL || (CONFIG_MUSE_UI_PANEL_TYPE && CONFIG_MUSE_BOARD_ESP_MOSAICO)
#define TYPE_480 1
#else
#define TYPE_480 0
#endif
#if MUSE_TYPE_ALL || (CONFIG_MUSE_UI_PANEL_TYPE && CONFIG_MUSE_BOARD_SENSECAP_WATCHER)
#define TYPE_412 1
#else
#define TYPE_412 0
#endif
#if MUSE_TYPE_ALL || (CONFIG_MUSE_UI_PANEL_TYPE && CONFIG_MUSE_BOARD_WAVESHARE_C6_18)
#define TYPE_368 1
#else
#define TYPE_368 0
#endif

/* A role converted at the same size as another role of its set (gen_ui_assets.py) uses that one's font:
 * one copy in flash. */
#define DECLARE(S, VALUE, SMALL)                                                                     \
    extern const lv_font_t muse_font_time_##S, muse_font_date_##S, muse_font_title_##S,              \
        muse_font_conn_##S, muse_font_row_##S, muse_font_label_##S, muse_font_value_##S,           \
        muse_font_small_##S, muse_font_digit_##S;                                                    \
    static const muse_type_t TYPE_##S##_SET = { &muse_font_time_##S, &muse_font_date_##S,            \
                                               &muse_font_title_##S, &muse_font_conn_##S,           \
                                               &muse_font_row_##S, &muse_font_label_##S,            \
                                               &muse_font_##VALUE##_##S, &muse_font_##SMALL##_##S,  \
                                               &muse_font_digit_##S };

#if TYPE_466
DECLARE(466, value, small)
#endif
#if TYPE_480
DECLARE(480, row, label)   /* value at row's 22 px, small at label's 28 px */
#endif
#if TYPE_412
DECLARE(412, value, small)
#endif
#if TYPE_368
DECLARE(368, value, value)   /* small at value's 22 px */
#endif

#if MUSE_TYPE_ALL || (TYPE_466 + TYPE_480 + TYPE_412 + TYPE_368) != 1
/* The time: Montserrat 48 where a board's overlay builds it in, else 28. */
#if LV_FONT_MONTSERRAT_48
#define BUILTIN_TIME &lv_font_montserrat_48
#else
#define BUILTIN_TIME &lv_font_montserrat_28
#endif

/* Other boards: LVGL's built-in sizes nearest the reference (capitals about
 * 0.7 of the size). */
static const muse_type_t BUILTIN_LARGE = { BUILTIN_TIME, &lv_font_montserrat_28, &lv_font_montserrat_20,
                                           &lv_font_montserrat_28, &lv_font_montserrat_20,
                                           &lv_font_montserrat_20, &lv_font_montserrat_16,
                                           &lv_font_montserrat_28, BUILTIN_TIME };
static const muse_type_t BUILTIN_SMALL = { BUILTIN_TIME, &lv_font_montserrat_20, &lv_font_montserrat_16,
                                           &lv_font_montserrat_20, &lv_font_montserrat_16,
                                           &lv_font_montserrat_16, &lv_font_montserrat_14,
                                           &lv_font_montserrat_20, &lv_font_montserrat_28 };
#endif

#if !MUSE_TYPE_ALL && (TYPE_466 + TYPE_480 + TYPE_412 + TYPE_368) == 1
/* A catalog board compiles only its own set and always draws at its own
 * size: no built-in fallback. */
const muse_type_t *muse_type(int s)
{
    (void)s;
#if TYPE_466
    return &TYPE_466_SET;
#elif TYPE_480
    return &TYPE_480_SET;
#elif TYPE_412
    return &TYPE_412_SET;
#else
    return &TYPE_368_SET;
#endif
}
#else
const muse_type_t *muse_type(int s)
{
    (void)s;
#if TYPE_480
    if (s >= 473) return &TYPE_480_SET;
#endif
#if TYPE_466
    if (s >= 440) return &TYPE_466_SET;
#endif
#if TYPE_412
    if (s >= 390) return &TYPE_412_SET;
#endif
#if TYPE_368
    if (s >= 340 && s < 390) return &TYPE_368_SET;
#endif
    return s >= 400 ? &BUILTIN_LARGE : &BUILTIN_SMALL;
}
#endif

int muse_type_ink_top(const lv_font_t *font, uint32_t letter)
{
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(font, &g, letter, 0)) {
        return 0;
    }
    /* LVGL sets a glyph's bottom ofs_y above the baseline, which sits
     * base_line above the line's bottom. */
    return lv_font_get_line_height(font) - font->base_line - g.box_h - g.ofs_y;
}
