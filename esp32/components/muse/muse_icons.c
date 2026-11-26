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

#include "muse_icons.h"

#include <stddef.h>

#include "sdkconfig.h"

#define DSC(bits_, w_, h_)                                                                  \
    {                                                                                       \
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = (w_),    \
                    .h = (h_), .stride = (w_) },                                            \
        .data_size = (w_) * (h_), .data = (bits_),                                          \
    }

typedef struct {
    muse_icon_t icon;
    int px;   /* the box (square masks), or the ink's height (the top row's) */
    lv_image_dsc_t dsc;
} entry_t;

/*
 * The sizes a board draws (the ESP-Mosaico's app slot is tight, so each of
 * the four catalog boards compiles only its own): Speaker / Call glyphs at
 * 52 % of the button (the largest mask that fits), the call controls' 28, the
 * StopWatch's rim hints 32, the charging bolt and the top row's ink at the
 * battery's height (MUSE_ICON_PX, MUSE_ICON_INK), and the Settings rows' icon
 * box (MUSE_ICON_MENU): 24 s on other boards, grown to the menu's
 * millimeter minimums on the four.
 * MUSE_TYPE_ALL (the simulator, every board): all of them. Any other board:
 * every button and hint size and the 20/21/24 menu masks.
 * Every icon a board draws must be in its set. The Live call's own icons
 * (call, call_end, add, remove, and with them the call controls' 28) only in
 * a build with Live call (MUSE_ICON_CALLS): one without carries none.
 */
#if MUSE_TYPE_ALL || CONFIG_MUSE_GADGET_BLE_LIVE_CALL
#define MUSE_ICON_CALLS 1
#else
#define MUSE_ICON_CALLS 0
#endif
#if MUSE_TYPE_ALL
#define MUSE_ICON_PX(px) 1
#define MUSE_ICON_MENU(px) 1
#define MUSE_ICON_INK(h) 1
#elif CONFIG_MUSE_BOARD_M5STACK_STOPWATCH   /* 466: buttons 92 -> 42, rim 32, bolt 23, ink 18; menu 29; passcode 29/51/27, phone 22 */
#define MUSE_ICON_PX(px) ((px) == 42 || ((px) == 28 && MUSE_ICON_CALLS) || (px) == 32 || (px) == 23 || (px) == 29 || (px) == 51 || (px) == 27 || (px) == 22)
#define MUSE_ICON_MENU(px) ((px) == 29)
#define MUSE_ICON_INK(h) ((h) == 18)
#elif CONFIG_MUSE_BOARD_ESP_MOSAICO         /* 480: buttons 95 -> 48, bolt 24, ink 18; menu 35; passcode 30/53/33, phone 38 */
#define MUSE_ICON_PX(px) ((px) == 48 || ((px) == 28 && MUSE_ICON_CALLS) || (px) == 24 || (px) == 30 || (px) == 53 || (px) == 33 || (px) == 38)
#define MUSE_ICON_MENU(px) ((px) == 35)
#define MUSE_ICON_INK(h) ((h) == 18)
#elif CONFIG_MUSE_BOARD_SENSECAP_WATCHER    /* 412: buttons 82 -> 42, bolt 21, ink 16; menu 28; passcode 26/45/26, phone 23 */
#define MUSE_ICON_PX(px) ((px) == 42 || ((px) == 28 && MUSE_ICON_CALLS) || (px) == 21 || (px) == 26 || (px) == 45 || (px) == 23)
#define MUSE_ICON_MENU(px) ((px) == 28)
#define MUSE_ICON_INK(h) ((h) == 16)
#elif CONFIG_MUSE_BOARD_WAVESHARE_C6_18     /* 368: buttons 73 -> 36, bolt 18, ink 14; menu 32; passcode 23/40/32, phone 29 */
#define MUSE_ICON_PX(px) ((px) == 36 || ((px) == 28 && MUSE_ICON_CALLS) || (px) == 18 || (px) == 23 || (px) == 40 || (px) == 32 || (px) == 29)
#define MUSE_ICON_MENU(px) ((px) == 32)
#define MUSE_ICON_INK(h) ((h) == 14)
#else
#define MUSE_ICON_PX(px) 1
#define MUSE_ICON_MENU(px) ((px) <= 24)
#define MUSE_ICON_INK(h) 1
#endif

#include "muse_icons_gen.h"

#if MUSE_TYPE_ALL
#include <stdio.h>
#include <stdlib.h>

/* The simulator: each lookup to MUSE_SIM_ICON_LOG, for the test above. */
static void note(char kind, muse_icon_t icon, int want, const entry_t *e)
{
    const char *path = getenv("MUSE_SIM_ICON_LOG");
    FILE *f = path ? fopen(path, "a") : NULL;
    if (f) {
        fprintf(f, "%c %d %d %d\n", kind, (int)icon, want, e ? e->px : -1);
        fclose(f);
    }
}
#else
#define note(kind, icon, want, e) ((void)0)
#endif

static const entry_t *lookup(const entry_t *e, size_t n, muse_icon_t icon, int px)
{
    const entry_t *best = NULL;
    for (size_t i = 0; i < n; i++) {
        if (e[i].icon == icon && (!best || e[i].px <= px)) {
            best = &e[i];
        }
    }
    return best;
}

const lv_image_dsc_t *muse_icon(muse_icon_t icon, int px)
{
    const entry_t *e = lookup(ENTRIES, sizeof(ENTRIES) / sizeof(ENTRIES[0]), icon, px);
    note('s', icon, px, e);
    return e ? &e->dsc : NULL;
}

const lv_image_dsc_t *muse_icon_ink(muse_icon_t icon, int ink_h)
{
    const entry_t *e = lookup(INK_ENTRIES, sizeof(INK_ENTRIES) / sizeof(INK_ENTRIES[0]), icon, ink_h);
    note('i', icon, ink_h, e);
    return e ? &e->dsc : NULL;
}

void muse_icon_set(lv_obj_t *img, muse_icon_t icon, int px, uint32_t color)
{
    const lv_image_dsc_t *d = muse_icon(icon, px);
    if (d && lv_image_get_src(img) != d) {
        lv_image_set_src(img, d);
    }
    lv_obj_set_style_image_recolor(img, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
}

lv_obj_t *muse_icon_create(lv_obj_t *parent, muse_icon_t icon, int px, uint32_t color)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    muse_icon_set(img, icon, px, color);
    lv_obj_center(img);
    return img;
}
