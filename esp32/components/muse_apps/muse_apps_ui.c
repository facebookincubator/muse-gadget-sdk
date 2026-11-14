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

/*
 * The API's ui_* group: LVGL objects under the app view's root. The object
 * table, properties, events, styles, animation, grids, the widgets' extras,
 * images (PNG and JPEG, decoded once into PSRAM) and keypad navigation.
 *
 * Everything here is under LVGL's lock: the natives take it with
 * muse_apps_ui_lock(), LVGL's own callbacks (events, animations, the
 * navigation keypad) run with it, and the image loader and the hooks take it
 * themselves.
 */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "lvgl.h"
#include "muse_app_view.h"
#include "muse_apps_priv.h"
#include "muse_apps_ui_priv.h"
#include "muse_link.h"
#include "src/core/lv_obj_private.h"         /* an object's style list, for STYLES_PER_OBJECT */
#include "src/core/lv_obj_style_private.h"
#if LV_USE_LODEPNG
#include "src/libs/lodepng/lodepng.h"
#endif
#if LV_USE_TJPGD
#include "src/libs/tjpgd/tjpgd.h"
#endif
/* An image's cached decode and header, keyed by its descriptor: dropped
 * before the descriptor's memory goes (both caches are off on these boards). */
#if __has_include("src/misc/cache/instance/lv_image_cache.h")
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#define IMAGE_DROP(src) (lv_image_cache_drop(src), lv_image_header_cache_drop(src))
#else
#define IMAGE_DROP(src) ((void)(src))
#endif

static const char *TAG = "muse_apps_ui";

#define MAX_OBJECTS 256                /* limits.objects, the root included */
#define MAX_STYLES 32                  /* limits.styles */
#define MAX_ANIMS 32                   /* animations running at once */
#define MAX_TEXT 4096                  /* limits.text_bytes */
#define MAX_LINE_POINTS 64             /* limits.line_points */
#define MAX_SERIES 4                   /* limits.chart_series */
#define MAX_POINTS 1024                /* a chart's P_POINT_COUNT */
#define MAX_GRID 8                     /* tracks in a ui_grid */
#define MAX_TABLE_ROWS 256
#define MAX_TABLE_COLS 16
#define MAX_IMAGE_FILE (512 * 1024)    /* a PNG or JPEG ui_image_load reads */
#define MAX_LOADS 4                    /* image loads in flight (limits.ops) */
#define LOAD_TIMEOUT_MS 30000
#define LOADER_STACK 8192
#define JPEG_POOL 4096                 /* TJpgDec's work area */
#define ANIM_STEPS 1024                /* an animation's progress, 0 to this */
#define PCT_BASE 0x20000000            /* GADGET_PCT(0) */
#define PCT_SPAN 1000
#define COORD_LIMIT (1 << 28)          /* plain pixels stay under this */
#define STYLE_COST 96                  /* a shared style and its first properties */
#define EVENT_COST 16                  /* an event callback ui_on adds */

/* Enums the API takes as LVGL's own values. */
_Static_assert(GADGET_ALIGN_CENTER == LV_ALIGN_CENTER && GADGET_ALIGN_TOP_LEFT == LV_ALIGN_TOP_LEFT &&
                   GADGET_ALIGN_OUT_RIGHT_BOTTOM == LV_ALIGN_OUT_RIGHT_BOTTOM,
               "align is LVGL's");
_Static_assert(GADGET_DIR_LEFT == LV_DIR_LEFT && GADGET_DIR_RIGHT == LV_DIR_RIGHT && GADGET_DIR_TOP == LV_DIR_TOP &&
                   GADGET_DIR_BOTTOM == LV_DIR_BOTTOM && GADGET_DIR_ALL == LV_DIR_ALL,
               "dir is LVGL's");
_Static_assert(GADGET_SCROLLBAR_OFF == LV_SCROLLBAR_MODE_OFF && GADGET_SCROLLBAR_ON == LV_SCROLLBAR_MODE_ON &&
                   GADGET_SCROLLBAR_ACTIVE == LV_SCROLLBAR_MODE_ACTIVE && GADGET_SCROLLBAR_AUTO == LV_SCROLLBAR_MODE_AUTO,
               "scrollbar modes are LVGL's");
_Static_assert(GADGET_SNAP_NONE == LV_SCROLL_SNAP_NONE && GADGET_SNAP_START == LV_SCROLL_SNAP_START &&
                   GADGET_SNAP_END == LV_SCROLL_SNAP_END && GADGET_SNAP_CENTER == LV_SCROLL_SNAP_CENTER,
               "snap is LVGL's");
_Static_assert(GADGET_PLACE_START == LV_FLEX_ALIGN_START && GADGET_PLACE_END == LV_FLEX_ALIGN_END &&
                   GADGET_PLACE_CENTER == LV_FLEX_ALIGN_CENTER && GADGET_PLACE_SPACE_EVENLY == LV_FLEX_ALIGN_SPACE_EVENLY &&
                   GADGET_PLACE_SPACE_AROUND == LV_FLEX_ALIGN_SPACE_AROUND &&
                   GADGET_PLACE_SPACE_BETWEEN == LV_FLEX_ALIGN_SPACE_BETWEEN,
               "place is LVGL's");
_Static_assert(GADGET_TEXT_ALIGN_AUTO == LV_TEXT_ALIGN_AUTO && GADGET_TEXT_ALIGN_LEFT == LV_TEXT_ALIGN_LEFT &&
                   GADGET_TEXT_ALIGN_CENTER == LV_TEXT_ALIGN_CENTER && GADGET_TEXT_ALIGN_RIGHT == LV_TEXT_ALIGN_RIGHT,
               "text align is LVGL's");
_Static_assert(GADGET_BORDER_BOTTOM == LV_BORDER_SIDE_BOTTOM && GADGET_BORDER_TOP == LV_BORDER_SIDE_TOP &&
                   GADGET_BORDER_LEFT == LV_BORDER_SIDE_LEFT && GADGET_BORDER_RIGHT == LV_BORDER_SIDE_RIGHT &&
                   GADGET_BORDER_FULL == LV_BORDER_SIDE_FULL,
               "border sides are LVGL's");
_Static_assert(GADGET_DECOR_UNDERLINE == LV_TEXT_DECOR_UNDERLINE &&
                   GADGET_DECOR_STRIKETHROUGH == LV_TEXT_DECOR_STRIKETHROUGH,
               "text decor is LVGL's");
_Static_assert(GADGET_RADIUS_CIRCLE == LV_RADIUS_CIRCLE, "RADIUS_CIRCLE is LVGL's");

/* ---- The table ------------------------------------------------------------------------- */

typedef struct {
    lv_obj_t *obj;      /* NULL: free */
    uint32_t gen;       /* in the handle; moves on each time the slot frees */
    uint32_t events;    /* GADGET_UI_ bits from ui_on */
    uint32_t charged;   /* bytes charged to the budget for it */
    void *ext;          /* by type: image_t, a canvas, line points, chart_t, roller_t */
    int32_t *grid;      /* ui_grid's column and row tracks */
    int32_t needle_of;  /* a line: the scale it's the needle of (its handle), or 0 */
    uint8_t type;       /* GADGET_W_ */
} slot_t;

typedef struct {
    bool used;
    bool color;
    uint16_t slot, prop;
    int32_t from, to;
} anim_t;

typedef struct {
    slot_t slots[MAX_OBJECTS];  /* 0 is the root */
    lv_style_t *styles[MAX_STYLES];
    anim_t anims[MAX_ANIMS];
    int n_styles;
    int n_objects;              /* besides the root */
    int next;                   /* where the next free slot is looked for */
    uint32_t pixels, pixel_limit;
    lv_group_t *group;          /* navigation */
    lv_indev_t *indev;
} ui_t;

static ui_t *s_ui;                 /* while an app runs */
static uint32_t s_session;         /* moves on at each start and stop: the image loader's check */
static atomic_int s_loaders;       /* image loader threads alive */

/* Per widget type: a guess at what LVGL allocates for one (the object, its
 * class data, internal children, the theme's style list). */
static const uint16_t s_cost[] = {
    [GADGET_W_OBJ] = 160,      [GADGET_W_LABEL] = 192,    [GADGET_W_BUTTON] = 160,  [GADGET_W_IMAGE] = 192,
    [GADGET_W_LINE] = 176,     [GADGET_W_ARC] = 224,      [GADGET_W_BAR] = 256,     [GADGET_W_SLIDER] = 288,
    [GADGET_W_SWITCH] = 224,   [GADGET_W_CHECKBOX] = 224, [GADGET_W_ROLLER] = 512,  [GADGET_W_DROPDOWN] = 384,
    [GADGET_W_TEXTAREA] = 640, [GADGET_W_KEYBOARD] = 2048, [GADGET_W_SCALE] = 384,  [GADGET_W_SPINNER] = 288,
    [GADGET_W_CHART] = 384,    [GADGET_W_TABLE] = 384,    [GADGET_W_CANVAS] = 224,
};

static int32_t handle_of(int idx)
{
    return idx == 0 ? GADGET_UI_ROOT : (int32_t)((s_ui->slots[idx].gen << 8) | (uint32_t)idx);
}

static int index_of(const slot_t *s)
{
    return (int)(s - s_ui->slots);
}

/* A live object's slot, or NULL. */
static slot_t *find(int32_t h)
{
    if (!s_ui || h <= 0) {
        return NULL;
    }
    if (h == GADGET_UI_ROOT) {
        return s_ui->slots[0].obj ? &s_ui->slots[0] : NULL;
    }
    int idx = h & 0xFF;
    uint32_t gen = (uint32_t)h >> 8;
    if (idx == 0 || gen == 0) {
        return NULL;
    }
    slot_t *s = &s_ui->slots[idx];
    return s->obj && s->gen == gen ? s : NULL;
}

/* The slot of an LVGL object the app made (or the root), or NULL. */
static slot_t *slot_of(lv_obj_t *o)
{
    if (!s_ui || !o) {
        return NULL;
    }
    if (o == s_ui->slots[0].obj) {
        return &s_ui->slots[0];
    }
    uintptr_t idx = (uintptr_t)lv_obj_get_user_data(o);
    if (idx == 0 || idx >= MAX_OBJECTS || s_ui->slots[idx].obj != o) {
        return NULL;
    }
    return &s_ui->slots[idx];
}

/* Takes LVGL's lock, then finds the object. */
static slot_t *obj_arg(int32_t h)
{
    muse_apps_ui_lock();
    return find(h);
}

static bool is_root(const slot_t *s)
{
    return s == &s_ui->slots[0];
}

/* Moves a slot's charge from old_len bytes to new_len (a text, its event
 * callbacks, a chart's points); false, and nothing moved, past the budget. */
static bool charge_delta(slot_t *s, size_t old_len, size_t new_len)
{
    if (new_len > old_len) {
        if (!muse_apps_charge(new_len - old_len)) {
            return false;
        }
        s->charged += new_len - old_len;
    } else {
        size_t d = old_len - new_len;
        d = d > s->charged ? s->charged : d;
        muse_apps_uncharge(d);
        s->charged -= d;
    }
    return true;
}

/* ---- Pixels and fonts (also the canvas's) ------------------------------------------------- */

bool muse_apps_ui_pixels_take(uint32_t n)
{
    if (!s_ui || n > s_ui->pixel_limit - s_ui->pixels) {
        return false;
    }
    s_ui->pixels += n;
    return true;
}

void muse_apps_ui_pixels_give(uint32_t n)
{
    if (s_ui) {
        s_ui->pixels = n > s_ui->pixels ? 0 : s_ui->pixels - n;
    }
}

const lv_font_t *muse_apps_ui_font(int32_t id)
{
    switch (id) {
    case GADGET_FONT_DEFAULT:
    case GADGET_FONT_14: return &lv_font_montserrat_14;
#if LV_FONT_MONTSERRAT_16
    case GADGET_FONT_16: return &lv_font_montserrat_16;
#endif
#if LV_FONT_MONTSERRAT_20
    case GADGET_FONT_20: return &lv_font_montserrat_20;
#endif
#if LV_FONT_MONTSERRAT_28
    case GADGET_FONT_28: return &lv_font_montserrat_28;
#endif
#if LV_FONT_MONTSERRAT_48
    case GADGET_FONT_48: return &lv_font_montserrat_48;
#endif
    default: return NULL;
    }
}

static int32_t font_id(const void *font)
{
    for (int32_t id = GADGET_FONT_14; id <= GADGET_FONT_48; id++) {
        if (font && muse_apps_ui_font(id) == font) {
            return id;
        }
    }
    return GADGET_FONT_DEFAULT;
}

lv_obj_t *muse_apps_ui_canvas(int32_t handle, void **ext)
{
    slot_t *s = find(handle);
    if (!s || s->type != GADGET_W_CANVAS) {
        return NULL;
    }
    *ext = s->ext;
    return s->obj;
}

/* ---- Values ------------------------------------------------------------------------------ */

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* An API coordinate (pixels, PCT(p) or SIZE_CONTENT) as LVGL's. Pixels are
 * kept within PX_MAX either way, as a canvas's sides are. */
#define PX_MAX 4096
static bool coord_in(int32_t v, bool size, int32_t *out)
{
    if (v == GADGET_SIZE_CONTENT) {
        *out = LV_SIZE_CONTENT;
        return size;
    }
    if (v >= PCT_BASE - PCT_SPAN && v <= PCT_BASE + PCT_SPAN) {
        *out = lv_pct(v - PCT_BASE);
        return true;
    }
    *out = clamp(v, size ? 0 : -PX_MAX, PX_MAX);
    return v > -COORD_LIMIT && v < COORD_LIMIT;
}

static int32_t coord_out(int32_t v)
{
    if (v == LV_SIZE_CONTENT) {
        return GADGET_SIZE_CONTENT;
    }
    if (LV_COORD_IS_PCT(v)) {
        return PCT_BASE + LV_COORD_GET_PCT(v);
    }
    return v;
}

static bool coord_special(int32_t api)
{
    return api == GADGET_SIZE_CONTENT || (api >= PCT_BASE - PCT_SPAN && api <= PCT_BASE + PCT_SPAN);
}

static int32_t rgb_out(lv_color_t c)
{
    return (int32_t)(lv_color_to_u32(c) & 0xFFFFFF);
}

/* An API enum's LVGL value through a table; false if out of range. */
#define MAP_IN(table, v, out)                                                           \
    ((v) >= 0 && (size_t)(v) < sizeof(table) / sizeof((table)[0]) ? ((out) = (table)[(v)], true) : false)

static int32_t map_out(const uint8_t *table, size_t n, int32_t lv)
{
    for (size_t i = 0; i < n; i++) {
        if (table[i] == lv) {
            return (int32_t)i;
        }
    }
    return 0;
}
#define MAP_OUT(table, lv) map_out((table), sizeof(table) / sizeof((table)[0]), (lv))

static const uint8_t s_layouts[] = {LV_LAYOUT_NONE, LV_LAYOUT_FLEX, LV_LAYOUT_GRID};
static const uint8_t s_flows[] = {LV_FLEX_FLOW_ROW,         LV_FLEX_FLOW_COLUMN,     LV_FLEX_FLOW_ROW_WRAP,
                                  LV_FLEX_FLOW_COLUMN_WRAP, LV_FLEX_FLOW_ROW_REVERSE, LV_FLEX_FLOW_COLUMN_REVERSE};
static const uint8_t s_grads[] = {LV_GRAD_DIR_NONE, LV_GRAD_DIR_VER, LV_GRAD_DIR_HOR};
static const uint8_t s_long_modes[] = {LV_LABEL_LONG_MODE_WRAP, LV_LABEL_LONG_MODE_DOTS, LV_LABEL_LONG_MODE_SCROLL,
                                       LV_LABEL_LONG_MODE_SCROLL_CIRCULAR, LV_LABEL_LONG_MODE_CLIP};
static const uint8_t s_bar_modes[] = {LV_BAR_MODE_NORMAL, LV_BAR_MODE_SYMMETRICAL, LV_BAR_MODE_RANGE};
static const uint8_t s_orients[] = {LV_BAR_ORIENTATION_AUTO, LV_BAR_ORIENTATION_HORIZONTAL,
                                    LV_BAR_ORIENTATION_VERTICAL};
static const uint8_t s_switch_orients[] = {LV_SWITCH_ORIENTATION_AUTO, LV_SWITCH_ORIENTATION_HORIZONTAL,
                                           LV_SWITCH_ORIENTATION_VERTICAL};
static const uint8_t s_arc_modes[] = {LV_ARC_MODE_NORMAL, LV_ARC_MODE_SYMMETRICAL, LV_ARC_MODE_REVERSE};
static const uint8_t s_kb_modes[] = {LV_KEYBOARD_MODE_TEXT_LOWER, LV_KEYBOARD_MODE_TEXT_UPPER,
                                     LV_KEYBOARD_MODE_SPECIAL, LV_KEYBOARD_MODE_NUMBER};
static const uint8_t s_scale_modes[] = {LV_SCALE_MODE_HORIZONTAL_TOP, LV_SCALE_MODE_HORIZONTAL_BOTTOM,
                                        LV_SCALE_MODE_VERTICAL_LEFT,  LV_SCALE_MODE_VERTICAL_RIGHT,
                                        LV_SCALE_MODE_ROUND_INNER,    LV_SCALE_MODE_ROUND_OUTER};
static const uint8_t s_chart_types[] = {LV_CHART_TYPE_NONE, LV_CHART_TYPE_LINE, LV_CHART_TYPE_BAR};
static const uint8_t s_chart_updates[] = {LV_CHART_UPDATE_MODE_SHIFT, LV_CHART_UPDATE_MODE_CIRCULAR};
static const uint8_t s_image_aligns[] = {LV_IMAGE_ALIGN_CENTER,  LV_IMAGE_ALIGN_TOP_LEFT, LV_IMAGE_ALIGN_STRETCH,
                                         LV_IMAGE_ALIGN_TILE,    LV_IMAGE_ALIGN_CONTAIN,  LV_IMAGE_ALIGN_COVER};

/* ---- What each property is, from api.json ----------------------------------------------- */

enum {
    PT_coord = 1, PT_int, PT_opa, PT_angle, PT_percent, PT_rgb, PT_text, PT_bool, PT_handle, PT_font, PT_align,
    PT_dir, PT_scrollbar_mode, PT_snap, PT_layout, PT_flex_flow, PT_place, PT_grad_dir, PT_border_side,
    PT_text_align, PT_text_decor, PT_long_mode, PT_bar_mode, PT_orientation, PT_arc_mode, PT_keyboard_mode,
    PT_scale_mode, PT_chart_type, PT_chart_update, PT_image_align,
};
#define PA_r 0x100
#define PA_w 0x200
#define PA_rw 0x300

/* PT_ type | PA_ access, or 0 for no such property. */
static int prop_info(int32_t prop)
{
    switch (prop) {
#define GADGET_PROP(id, name, type, where, access) \
    case id:                                       \
        return PT_##type | PA_##access;
        MUSE_APPS_PROPS(GADGET_PROP)
#undef GADGET_PROP
    default: return 0;
    }
}

/* ---- Style properties (100 to 154) ------------------------------------------------------- */

enum { K_INT = 1, K_COORD, K_RGB, K_OPA, K_BOOL, K_FONT, K_ANGLE, K_SCALE, K_TEXT_ALIGN, K_GRAD, K_SIDE, K_DECOR, K_PADS,
       K_SHADOW, K_OFFSET, K_RADIUS, K_SPACE };

/* Shadows, radii and dashes within 64 px, as cv_rect_style's: LVGL sizes a
 * shadow's corner buffer from them, and steps a dash in 16 bits. */
#define SHADOW_MAX 64
#define STYLES_PER_OBJECT 32   /* limits.styles_per_object: LVGL aborts past 63 on one object */

typedef struct {
    lv_style_prop_t prop;
    uint8_t kind;
} style_map_t;

static const style_map_t s_style_map[] = {
    [GADGET_P_BG_COLOR - 100] = {LV_STYLE_BG_COLOR, K_RGB},
    [GADGET_P_BG_OPA - 100] = {LV_STYLE_BG_OPA, K_OPA},
    [GADGET_P_BG_GRAD_COLOR - 100] = {LV_STYLE_BG_GRAD_COLOR, K_RGB},
    [GADGET_P_BG_GRAD_DIR - 100] = {LV_STYLE_BG_GRAD_DIR, K_GRAD},
    [GADGET_P_BORDER_COLOR - 100] = {LV_STYLE_BORDER_COLOR, K_RGB},
    [GADGET_P_BORDER_WIDTH - 100] = {LV_STYLE_BORDER_WIDTH, K_INT},
    [GADGET_P_BORDER_OPA - 100] = {LV_STYLE_BORDER_OPA, K_OPA},
    [GADGET_P_BORDER_SIDE - 100] = {LV_STYLE_BORDER_SIDE, K_SIDE},
    [GADGET_P_OUTLINE_COLOR - 100] = {LV_STYLE_OUTLINE_COLOR, K_RGB},
    [GADGET_P_OUTLINE_WIDTH - 100] = {LV_STYLE_OUTLINE_WIDTH, K_INT},
    [GADGET_P_OUTLINE_PAD - 100] = {LV_STYLE_OUTLINE_PAD, K_INT},
    [GADGET_P_SHADOW_COLOR - 100] = {LV_STYLE_SHADOW_COLOR, K_RGB},
    [GADGET_P_SHADOW_WIDTH - 100] = {LV_STYLE_SHADOW_WIDTH, K_SHADOW},
    [GADGET_P_SHADOW_OPA - 100] = {LV_STYLE_SHADOW_OPA, K_OPA},
    [GADGET_P_SHADOW_OFFSET_X - 100] = {LV_STYLE_SHADOW_OFFSET_X, K_OFFSET},
    [GADGET_P_SHADOW_OFFSET_Y - 100] = {LV_STYLE_SHADOW_OFFSET_Y, K_OFFSET},
    [GADGET_P_SHADOW_SPREAD - 100] = {LV_STYLE_SHADOW_SPREAD, K_SHADOW},
    [GADGET_P_RADIUS - 100] = {LV_STYLE_RADIUS, K_RADIUS},
    [GADGET_P_PAD_ALL - 100] = {LV_STYLE_PAD_TOP, K_PADS},
    [GADGET_P_PAD_LEFT - 100] = {LV_STYLE_PAD_LEFT, K_INT},
    [GADGET_P_PAD_RIGHT - 100] = {LV_STYLE_PAD_RIGHT, K_INT},
    [GADGET_P_PAD_TOP - 100] = {LV_STYLE_PAD_TOP, K_INT},
    [GADGET_P_PAD_BOTTOM - 100] = {LV_STYLE_PAD_BOTTOM, K_INT},
    [GADGET_P_PAD_ROW - 100] = {LV_STYLE_PAD_ROW, K_INT},
    [GADGET_P_PAD_COLUMN - 100] = {LV_STYLE_PAD_COLUMN, K_INT},
    [GADGET_P_TEXT_COLOR - 100] = {LV_STYLE_TEXT_COLOR, K_RGB},
    [GADGET_P_TEXT_OPA - 100] = {LV_STYLE_TEXT_OPA, K_OPA},
    [GADGET_P_TEXT_FONT - 100] = {LV_STYLE_TEXT_FONT, K_FONT},
    [GADGET_P_TEXT_ALIGN - 100] = {LV_STYLE_TEXT_ALIGN, K_TEXT_ALIGN},
    [GADGET_P_TEXT_LETTER_SPACE - 100] = {LV_STYLE_TEXT_LETTER_SPACE, K_INT},
    [GADGET_P_TEXT_LINE_SPACE - 100] = {LV_STYLE_TEXT_LINE_SPACE, K_SPACE},
    [GADGET_P_TEXT_DECOR - 100] = {LV_STYLE_TEXT_DECOR, K_DECOR},
    [GADGET_P_LINE_COLOR - 100] = {LV_STYLE_LINE_COLOR, K_RGB},
    [GADGET_P_LINE_WIDTH - 100] = {LV_STYLE_LINE_WIDTH, K_INT},
    [GADGET_P_LINE_OPA - 100] = {LV_STYLE_LINE_OPA, K_OPA},
    [GADGET_P_LINE_ROUNDED - 100] = {LV_STYLE_LINE_ROUNDED, K_BOOL},
    [GADGET_P_LINE_DASH_WIDTH - 100] = {LV_STYLE_LINE_DASH_WIDTH, K_SHADOW},
    [GADGET_P_LINE_DASH_GAP - 100] = {LV_STYLE_LINE_DASH_GAP, K_SHADOW},
    [GADGET_P_ARC_COLOR - 100] = {LV_STYLE_ARC_COLOR, K_RGB},
    [GADGET_P_ARC_WIDTH - 100] = {LV_STYLE_ARC_WIDTH, K_INT},
    [GADGET_P_ARC_OPA - 100] = {LV_STYLE_ARC_OPA, K_OPA},
    [GADGET_P_ARC_ROUNDED - 100] = {LV_STYLE_ARC_ROUNDED, K_BOOL},
    [GADGET_P_IMAGE_OPA - 100] = {LV_STYLE_IMAGE_OPA, K_OPA},
    [GADGET_P_IMAGE_RECOLOR - 100] = {LV_STYLE_IMAGE_RECOLOR, K_RGB},
    [GADGET_P_IMAGE_RECOLOR_OPA - 100] = {LV_STYLE_IMAGE_RECOLOR_OPA, K_OPA},
    [GADGET_P_OPA - 100] = {LV_STYLE_OPA, K_OPA},
    [GADGET_P_TRANSFORM_ROTATION - 100] = {LV_STYLE_TRANSFORM_ROTATION, K_ANGLE},
    [GADGET_P_TRANSFORM_SCALE - 100] = {LV_STYLE_TRANSFORM_SCALE_X, K_SCALE},
    [GADGET_P_TRANSFORM_PIVOT_X - 100] = {LV_STYLE_TRANSFORM_PIVOT_X, K_COORD},
    [GADGET_P_TRANSFORM_PIVOT_Y - 100] = {LV_STYLE_TRANSFORM_PIVOT_Y, K_COORD},
    [GADGET_P_TRANSLATE_X - 100] = {LV_STYLE_TRANSLATE_X, K_COORD},
    [GADGET_P_TRANSLATE_Y - 100] = {LV_STYLE_TRANSLATE_Y, K_COORD},
    [GADGET_P_LENGTH - 100] = {LV_STYLE_LENGTH, K_INT},
    [GADGET_P_CLIP_CORNER - 100] = {LV_STYLE_CLIP_CORNER, K_BOOL},
    [GADGET_P_ANIM_DURATION - 100] = {LV_STYLE_ANIM_DURATION, K_INT},
};

static const style_map_t *style_entry(int32_t prop)
{
    if (prop < 100 || prop >= 100 + (int32_t)(sizeof(s_style_map) / sizeof(s_style_map[0]))) {
        return NULL;
    }
    const style_map_t *m = &s_style_map[prop - 100];
    return m->kind ? m : NULL;
}

/* The API's value as LVGL's: 0 or GADGET_ERR_ARG. */
static int32_t style_value(int kind, int32_t v, lv_style_value_t *out)
{
    int32_t c;
    switch (kind) {
    case K_INT: out->num = clamp(v, -32767, 32767); return 0;
    case K_SHADOW: out->num = clamp(v, 0, SHADOW_MAX); return 0;   /* a negative spread overruns LVGL's buffer */
    case K_OFFSET: out->num = clamp(v, -SHADOW_MAX, SHADOW_MAX); return 0;
    case K_SPACE: out->num = v < 0 ? 0 : v; return 0;   /* a roller divides by the line plus this */
    case K_RADIUS: out->num = v == GADGET_RADIUS_CIRCLE ? v : clamp(v, 0, SHADOW_MAX); return 0;
    case K_COORD:
        if (!coord_in(v, false, &c)) {
            return GADGET_ERR_ARG;
        }
        out->num = c;
        return 0;
    case K_RGB: out->color = lv_color_hex((uint32_t)v & 0xFFFFFF); return 0;
    case K_OPA: out->num = clamp(v, 0, 255); return 0;
    case K_BOOL: out->num = v != 0; return 0;
    case K_FONT: out->ptr = muse_apps_ui_font(v); return out->ptr ? 0 : GADGET_ERR_ARG;
    case K_ANGLE: out->num = clamp(v, -3600, 3600) * 10; return 0;   /* LVGL's are 0.1 degree */
    case K_SCALE:                                                    /* 100 %: LVGL's 256 */
        if (v < 0 || v > 10000) {
            return GADGET_ERR_ARG;
        }
        out->num = v * 256 / 100;
        return 0;
    case K_TEXT_ALIGN:
        out->num = v;
        return v >= GADGET_TEXT_ALIGN_AUTO && v <= GADGET_TEXT_ALIGN_RIGHT ? 0 : GADGET_ERR_ARG;
    case K_GRAD: {
        uint8_t g = 0;
        if (!MAP_IN(s_grads, v, g)) {
            return GADGET_ERR_ARG;
        }
        out->num = g;
        return 0;
    }
    case K_SIDE: out->num = v; return v >= 0 && v <= GADGET_BORDER_FULL ? 0 : GADGET_ERR_ARG;
    case K_DECOR:
        out->num = v;
        return v >= 0 && v <= (GADGET_DECOR_UNDERLINE | GADGET_DECOR_STRIKETHROUGH) ? 0 : GADGET_ERR_ARG;
    default: return GADGET_ERR_ARG;
    }
}

static int32_t style_api(int kind, lv_style_value_t v)
{
    switch (kind) {
    case K_COORD: return coord_out(v.num);
    case K_RGB: return rgb_out(v.color);
    case K_FONT: return font_id(v.ptr);
    case K_ANGLE: return v.num / 10;
    case K_SCALE: return (v.num * 100 + 128) / 256;
    case K_GRAD: return MAP_OUT(s_grads, v.num);
    default: return v.num;
    }
}

static void style_put(lv_obj_t *o, lv_style_t *st, lv_style_selector_t sel, lv_style_prop_t prop,
                      lv_style_value_t v)
{
    if (o) {
        lv_obj_set_local_style_prop(o, prop, v, sel);
    } else {
        lv_style_set_prop(st, prop, v);
    }
}

/* A style property on an object (o, for sel) or in a shared style (st). */
static int32_t style_set(lv_obj_t *o, lv_style_t *st, lv_style_selector_t sel, int32_t prop, int32_t v)
{
    const style_map_t *m = style_entry(prop);
    if (!m) {
        return GADGET_ERR_ARG;
    }
    lv_style_value_t val = {0};
    if (m->kind == K_PADS) {
        static const lv_style_prop_t pads[] = {LV_STYLE_PAD_TOP, LV_STYLE_PAD_BOTTOM, LV_STYLE_PAD_LEFT,
                                               LV_STYLE_PAD_RIGHT};
        val.num = clamp(v, -32767, 32767);
        for (size_t i = 0; i < sizeof(pads) / sizeof(pads[0]); i++) {
            style_put(o, st, sel, pads[i], val);
        }
        return 0;
    }
    int32_t err = style_value(m->kind, v, &val);
    if (err) {
        return err;
    }
    style_put(o, st, sel, m->prop, val);
    if (m->kind == K_SCALE) {
        style_put(o, st, sel, LV_STYLE_TRANSFORM_SCALE_Y, val);
    }
    return 0;
}

static int32_t style_get(lv_obj_t *o, int32_t prop, int32_t *out)
{
    const style_map_t *m = style_entry(prop);
    if (!m || m->kind == K_PADS) {
        return GADGET_ERR_ARG;
    }
    *out = style_api(m->kind, lv_obj_get_style_prop(o, LV_PART_MAIN, m->prop));
    return 0;
}

/* A PART_ plus STATE_ bits as LVGL's selector. */
static bool selector_in(int32_t sel, lv_style_selector_t *out)
{
    static const lv_part_t parts[] = {LV_PART_MAIN,     LV_PART_SCROLLBAR, LV_PART_INDICATOR, LV_PART_KNOB,
                                      LV_PART_SELECTED, LV_PART_ITEMS,     LV_PART_CURSOR};
    uint32_t part = ((uint32_t)sel >> 8) & 0xFF, states = (uint32_t)sel & 0xFF;
    if ((uint32_t)sel >> 16 || part >= sizeof(parts) / sizeof(parts[0]) || (states & ~0x1Fu)) {
        return false;
    }
    lv_state_t st = LV_STATE_DEFAULT;
    st |= (states & GADGET_STATE_CHECKED) ? LV_STATE_CHECKED : 0;
    st |= (states & GADGET_STATE_FOCUSED) ? LV_STATE_FOCUSED : 0;
    st |= (states & GADGET_STATE_PRESSED) ? LV_STATE_PRESSED : 0;
    st |= (states & GADGET_STATE_DISABLED) ? LV_STATE_DISABLED : 0;
    st |= (states & GADGET_STATE_EDITED) ? LV_STATE_EDITED : 0;
    *out = parts[part] | st;
    return true;
}

/* ---- Object properties (1 to 31) ----------------------------------------------------------- */

static int32_t set_flag(lv_obj_t *o, lv_obj_flag_t f, int32_t v)
{
    if (v) {
        lv_obj_add_flag(o, f);
    } else {
        lv_obj_remove_flag(o, f);
    }
    return 0;
}

static int32_t set_state(lv_obj_t *o, lv_state_t st, int32_t v)
{
    if (v) {
        lv_obj_add_state(o, st);
    } else {
        lv_obj_remove_state(o, st);
    }
    return 0;
}

/* Gives back what ui_table_cell charged for the cells outside rows x cols,
 * which LVGL frees when the table shrinks. */
static void table_refund(slot_t *s, uint32_t rows, uint32_t cols)
{
    uint32_t n_rows = lv_table_get_row_count(s->obj), n_cols = lv_table_get_column_count(s->obj);
    for (uint32_t r = 0; r < n_rows; r++) {
        for (uint32_t c = r < rows ? cols : 0; c < n_cols; c++) {
            const char *v = lv_table_get_cell_value(s->obj, r, c);
            if (v && *v) {
                charge_delta(s, strlen(v) + 8, 0);
            }
        }
    }
}

static int32_t obj_set(slot_t *s, int32_t prop, int32_t v)
{
    lv_obj_t *o = s->obj;
    int32_t c;
    uint8_t m = 0;
    /* The root stays where the view put it, whole and shown. */
    if (is_root(s) && ((prop >= GADGET_P_X && prop <= GADGET_P_HIDDEN) || prop == GADGET_P_FLOATING ||
                       prop == GADGET_P_INDEX)) {
        return GADGET_ERR_UNSUPPORTED;
    }
    switch (prop) {
    case GADGET_P_X:
    case GADGET_P_Y:
        if (!coord_in(v, false, &c)) {
            return GADGET_ERR_ARG;
        }
        if (prop == GADGET_P_X) {
            lv_obj_set_x(o, c);
        } else {
            lv_obj_set_y(o, c);
        }
        return 0;
    case GADGET_P_WIDTH:
    case GADGET_P_HEIGHT:
        if (s->type == GADGET_W_CANVAS) {   /* its buffer's size; the object follows it */
            return muse_apps_canvas_size(o, &s->ext, prop == GADGET_P_WIDTH, v);
        }
        if (!coord_in(v, true, &c)) {
            return GADGET_ERR_ARG;
        }
        if (prop == GADGET_P_WIDTH) {
            lv_obj_set_width(o, c);
        } else {
            lv_obj_set_height(o, c);
        }
        return 0;
    case GADGET_P_MIN_WIDTH:
    case GADGET_P_MAX_WIDTH:
    case GADGET_P_MIN_HEIGHT:
    case GADGET_P_MAX_HEIGHT:
        if (!coord_in(v, true, &c)) {
            return GADGET_ERR_ARG;
        }
        if (prop == GADGET_P_MIN_WIDTH) {
            lv_obj_set_style_min_width(o, c, 0);
        } else if (prop == GADGET_P_MAX_WIDTH) {
            lv_obj_set_style_max_width(o, c, 0);
        } else if (prop == GADGET_P_MIN_HEIGHT) {
            lv_obj_set_style_min_height(o, c, 0);
        } else {
            lv_obj_set_style_max_height(o, c, 0);
        }
        return 0;
    case GADGET_P_ALIGN:
        if (v < GADGET_ALIGN_DEFAULT || v > GADGET_ALIGN_OUT_RIGHT_BOTTOM) {
            return GADGET_ERR_ARG;
        }
        lv_obj_set_align(o, (lv_align_t)v);
        return 0;
    case GADGET_P_HIDDEN: return set_flag(o, LV_OBJ_FLAG_HIDDEN, v);
    case GADGET_P_CLICKABLE: return set_flag(o, LV_OBJ_FLAG_CLICKABLE, v);
    case GADGET_P_SCROLLABLE: return set_flag(o, LV_OBJ_FLAG_SCROLLABLE, v);
    case GADGET_P_CHECKABLE: return set_flag(o, LV_OBJ_FLAG_CHECKABLE, v);
    case GADGET_P_EVENT_BUBBLE: return set_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE, v);
    case GADGET_P_FLOATING: return set_flag(o, LV_OBJ_FLAG_FLOATING, v);
    case GADGET_P_CHECKED: return set_state(o, LV_STATE_CHECKED, v);
    case GADGET_P_DISABLED: return set_state(o, LV_STATE_DISABLED, v);
    case GADGET_P_SCROLL_DIR:
        if (v < 0 || v > GADGET_DIR_ALL) {
            return GADGET_ERR_ARG;
        }
        lv_obj_set_scroll_dir(o, (lv_dir_t)v);
        return 0;
    case GADGET_P_SCROLLBAR_MODE:
        if (v < GADGET_SCROLLBAR_OFF || v > GADGET_SCROLLBAR_AUTO) {
            return GADGET_ERR_ARG;
        }
        lv_obj_set_scrollbar_mode(o, (lv_scrollbar_mode_t)v);
        return 0;
    case GADGET_P_SCROLL_SNAP_X:
    case GADGET_P_SCROLL_SNAP_Y:
        if (v < GADGET_SNAP_NONE || v > GADGET_SNAP_CENTER) {
            return GADGET_ERR_ARG;
        }
        if (prop == GADGET_P_SCROLL_SNAP_X) {
            lv_obj_set_scroll_snap_x(o, (lv_scroll_snap_t)v);
        } else {
            lv_obj_set_scroll_snap_y(o, (lv_scroll_snap_t)v);
        }
        return 0;
    case GADGET_P_INDEX: lv_obj_move_to_index(o, v); return 0;
    case GADGET_P_LAYOUT:
        if (!MAP_IN(s_layouts, v, m)) {
            return GADGET_ERR_ARG;
        }
        if (m == LV_LAYOUT_GRID && !s->grid) {
            return GADGET_ERR_ARG;   /* ui_grid sets it, with the tracks: LVGL's subgrid indexes the parent's unchecked */
        }
        lv_obj_set_layout(o, m);
        return 0;
    case GADGET_P_FLEX_FLOW:
        if (!MAP_IN(s_flows, v, m)) {
            return GADGET_ERR_ARG;
        }
        lv_obj_set_flex_flow(o, (lv_flex_flow_t)m);
        return 0;
    case GADGET_P_FLEX_MAIN_PLACE:
    case GADGET_P_FLEX_CROSS_PLACE:
    case GADGET_P_FLEX_TRACK_PLACE:
        if (v < GADGET_PLACE_START || v > GADGET_PLACE_SPACE_BETWEEN) {
            return GADGET_ERR_ARG;
        }
        if (prop == GADGET_P_FLEX_MAIN_PLACE) {
            lv_obj_set_style_flex_main_place(o, (lv_flex_align_t)v, 0);
        } else if (prop == GADGET_P_FLEX_CROSS_PLACE) {
            lv_obj_set_style_flex_cross_place(o, (lv_flex_align_t)v, 0);
        } else {
            lv_obj_set_style_flex_track_place(o, (lv_flex_align_t)v, 0);
        }
        return 0;
    case GADGET_P_FLEX_GROW: lv_obj_set_flex_grow(o, (uint8_t)clamp(v, 0, 255)); return 0;
    default: return GADGET_ERR_ARG;
    }
}

static int32_t obj_get(slot_t *s, int32_t prop, int32_t *out)
{
    lv_obj_t *o = s->obj;
    switch (prop) {
    case GADGET_P_X: *out = coord_out(lv_obj_get_style_x(o, LV_PART_MAIN)); return 0;
    case GADGET_P_Y: *out = coord_out(lv_obj_get_style_y(o, LV_PART_MAIN)); return 0;
    case GADGET_P_WIDTH:   /* in pixels, laid out: what PCT or SIZE_CONTENT came to */
    case GADGET_P_HEIGHT:
        if (s->type == GADGET_W_CANVAS) {   /* as set, even before the other side is */
            *out = muse_apps_canvas_get_size(s->ext, prop == GADGET_P_WIDTH);
            return 0;
        }
        lv_obj_update_layout(o);
        *out = prop == GADGET_P_WIDTH ? lv_obj_get_width(o) : lv_obj_get_height(o);
        return 0;
    case GADGET_P_MIN_WIDTH: *out = coord_out(lv_obj_get_style_min_width(o, LV_PART_MAIN)); return 0;
    case GADGET_P_MAX_WIDTH: *out = coord_out(lv_obj_get_style_max_width(o, LV_PART_MAIN)); return 0;
    case GADGET_P_MIN_HEIGHT: *out = coord_out(lv_obj_get_style_min_height(o, LV_PART_MAIN)); return 0;
    case GADGET_P_MAX_HEIGHT: *out = coord_out(lv_obj_get_style_max_height(o, LV_PART_MAIN)); return 0;
    case GADGET_P_ALIGN: *out = lv_obj_get_style_align(o, LV_PART_MAIN); return 0;
    case GADGET_P_HIDDEN: *out = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN); return 0;
    case GADGET_P_CLICKABLE: *out = lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE); return 0;
    case GADGET_P_SCROLLABLE: *out = lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE); return 0;
    case GADGET_P_CHECKABLE: *out = lv_obj_has_flag(o, LV_OBJ_FLAG_CHECKABLE); return 0;
    case GADGET_P_EVENT_BUBBLE: *out = lv_obj_has_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE); return 0;
    case GADGET_P_FLOATING: *out = lv_obj_has_flag(o, LV_OBJ_FLAG_FLOATING); return 0;
    case GADGET_P_CHECKED: *out = lv_obj_has_state(o, LV_STATE_CHECKED); return 0;
    case GADGET_P_DISABLED: *out = lv_obj_has_state(o, LV_STATE_DISABLED); return 0;
    case GADGET_P_SCROLL_DIR: *out = lv_obj_get_scroll_dir(o); return 0;
    case GADGET_P_SCROLLBAR_MODE: *out = lv_obj_get_scrollbar_mode(o); return 0;
    case GADGET_P_SCROLL_SNAP_X: *out = lv_obj_get_scroll_snap_x(o); return 0;
    case GADGET_P_SCROLL_SNAP_Y: *out = lv_obj_get_scroll_snap_y(o); return 0;
    case GADGET_P_SCROLL_X: *out = lv_obj_get_scroll_x(o); return 0;
    case GADGET_P_SCROLL_Y: *out = lv_obj_get_scroll_y(o); return 0;
    case GADGET_P_INDEX: *out = lv_obj_get_index(o); return 0;
    case GADGET_P_CHILD_COUNT: *out = (int32_t)lv_obj_get_child_count(o); return 0;
    case GADGET_P_LAYOUT: *out = MAP_OUT(s_layouts, lv_obj_get_style_layout(o, LV_PART_MAIN)); return 0;
    case GADGET_P_FLEX_FLOW: *out = MAP_OUT(s_flows, lv_obj_get_style_flex_flow(o, LV_PART_MAIN)); return 0;
    case GADGET_P_FLEX_MAIN_PLACE: *out = lv_obj_get_style_flex_main_place(o, LV_PART_MAIN); return 0;
    case GADGET_P_FLEX_CROSS_PLACE: *out = lv_obj_get_style_flex_cross_place(o, LV_PART_MAIN); return 0;
    case GADGET_P_FLEX_TRACK_PLACE: *out = lv_obj_get_style_flex_track_place(o, LV_PART_MAIN); return 0;
    case GADGET_P_FLEX_GROW: *out = lv_obj_get_style_flex_grow(o, LV_PART_MAIN); return 0;
    default: return GADGET_ERR_ARG;
    }
}

/* ---- Widget properties (200 and up) ------------------------------------------------------- */

typedef struct {
    lv_chart_series_t *ser[MAX_SERIES];
    int n;
    int32_t min, max;   /* the Y axis, which LVGL doesn't give back */
} chart_t;

typedef struct {
    bool infinite;
    char options[];     /* as the app gave them: an infinite roller's own text repeats them */
} roller_t;

static chart_t *chart_ext(slot_t *s)
{
    if (!s->ext) {
        chart_t *c = muse_apps_calloc(1, sizeof(chart_t));
        if (c) {
            c->max = 100;   /* LVGL's default range */
        }
        s->ext = c;
    }
    return s->ext;
}

/* A roller's options and mode, together as LVGL takes them. */
static int32_t roller_set(slot_t *s, const char *options, bool infinite)
{
    /* LVGL divides by an option's height (the font's line plus the letter space) to lay out infinite pages */
    if (infinite && lv_font_get_line_height(lv_obj_get_style_text_font(s->obj, LV_PART_MAIN)) +
                            lv_obj_get_style_text_letter_space(s->obj, LV_PART_MAIN) <= 0) {
        return GADGET_ERR_ARG;
    }
    size_t n = strlen(options);
    roller_t *r = muse_apps_alloc(sizeof(roller_t) + n + 1);
    if (!r) {
        return GADGET_ERR_NO_MEMORY;
    }
    r->infinite = infinite;
    memcpy(r->options, options, n + 1);
    lv_roller_set_options(s->obj, r->options, infinite ? LV_ROLLER_MODE_INFINITE : LV_ROLLER_MODE_NORMAL);
    muse_apps_free(s->ext);
    s->ext = r;
    return 0;
}

/* A scale's or chart's range: LVGL divides by max - min, so it must be
 * positive and fit an int32_t. */
static bool range_ok(int32_t lo, int32_t hi)
{
    return lo < hi && (int64_t)hi - lo <= INT32_MAX;
}

/* Whether a scale has a line as its needle. */
static bool has_needle(const slot_t *scale)
{
    int32_t h = handle_of(index_of(scale));
    for (int i = 1; i < MAX_OBJECTS; i++) {
        if (s_ui->slots[i].obj && s_ui->slots[i].needle_of == h) {
            return true;
        }
    }
    return false;
}

#define ONLY(t)                             \
    do {                                    \
        if (s->type != (t)) {               \
            return GADGET_ERR_UNSUPPORTED;  \
        }                                   \
    } while (0)
#define ENUM_IN(table, v, m)               \
    do {                                   \
        if (!MAP_IN(table, v, m)) {        \
            return GADGET_ERR_ARG;         \
        }                                  \
    } while (0)

static int32_t widget_set(slot_t *s, int32_t prop, int32_t v)
{
    lv_obj_t *o = s->obj;
    int t = s->type;
    uint8_t m = 0;
    switch (prop) {
    case GADGET_P_LONG_MODE:
        ONLY(GADGET_W_LABEL);
        ENUM_IN(s_long_modes, v, m);
        lv_label_set_long_mode(o, (lv_label_long_mode_t)m);
        return 0;
    case GADGET_P_VALUE:
        if (t == GADGET_W_ARC) {
            lv_arc_set_value(o, v);
        } else if (t == GADGET_W_BAR) {
            lv_bar_set_value(o, v, LV_ANIM_OFF);
        } else if (t == GADGET_W_SLIDER) {
            lv_slider_set_value(o, v, LV_ANIM_OFF);
        } else {
            return GADGET_ERR_UNSUPPORTED;
        }
        return 0;
    case GADGET_P_MIN_VALUE:
    case GADGET_P_MAX_VALUE: {
        bool min = prop == GADGET_P_MIN_VALUE;
        if (t == GADGET_W_ARC) {
            /* LVGL divides by the range when the arc is dragged */
            if (v == (min ? lv_arc_get_max_value(o) : lv_arc_get_min_value(o))) {
                return GADGET_ERR_ARG;
            }
            min ? lv_arc_set_min_value(o, v) : lv_arc_set_max_value(o, v);
        } else if (t == GADGET_W_BAR) {
            min ? lv_bar_set_min_value(o, v) : lv_bar_set_max_value(o, v);
        } else if (t == GADGET_W_SLIDER) {
            min ? lv_slider_set_min_value(o, v) : lv_slider_set_max_value(o, v);
        } else if (t == GADGET_W_SCALE) {
            /* LVGL places a needle (on each style change too) dividing by the range */
            int32_t lo = min ? v : lv_scale_get_range_min_value(o), hi = min ? lv_scale_get_range_max_value(o) : v;
            if (!range_ok(lo, hi) && has_needle(s)) {
                return GADGET_ERR_ARG;
            }
            min ? lv_scale_set_min_value(o, v) : lv_scale_set_max_value(o, v);
        } else if (t == GADGET_W_CHART) {
            chart_t *c = chart_ext(s);
            if (!c) {
                return GADGET_ERR_NO_MEMORY;
            }
            *(min ? &c->min : &c->max) = v;
            if (range_ok(c->min, c->max)) {   /* LVGL divides by it drawing bars: until then, the last good one */
                lv_chart_set_axis_range(o, LV_CHART_AXIS_PRIMARY_Y, c->min, c->max);
            }
        } else {
            return GADGET_ERR_UNSUPPORTED;
        }
        return 0;
    }
    case GADGET_P_START_VALUE:
        if (t == GADGET_W_BAR) {
            lv_bar_set_start_value(o, v, LV_ANIM_OFF);
        } else if (t == GADGET_W_SLIDER) {
            lv_slider_set_start_value(o, v, LV_ANIM_OFF);
        } else {
            return GADGET_ERR_UNSUPPORTED;
        }
        return 0;
    case GADGET_P_BAR_MODE:
        if (t != GADGET_W_BAR && t != GADGET_W_SLIDER) {
            return GADGET_ERR_UNSUPPORTED;
        }
        ENUM_IN(s_bar_modes, v, m);
        if (t == GADGET_W_BAR) {
            lv_bar_set_mode(o, (lv_bar_mode_t)m);
        } else {
            lv_slider_set_mode(o, (lv_slider_mode_t)m);
        }
        return 0;
    case GADGET_P_ORIENTATION:
        if (t == GADGET_W_SWITCH) {
            ENUM_IN(s_switch_orients, v, m);
            lv_switch_set_orientation(o, (lv_switch_orientation_t)m);
            return 0;
        }
        if (t != GADGET_W_BAR && t != GADGET_W_SLIDER) {
            return GADGET_ERR_UNSUPPORTED;
        }
        ENUM_IN(s_orients, v, m);
        if (t == GADGET_W_BAR) {
            lv_bar_set_orientation(o, (lv_bar_orientation_t)m);
        } else {
            lv_slider_set_orientation(o, (lv_slider_orientation_t)m);
        }
        return 0;
    case GADGET_P_ARC_MODE:
        ONLY(GADGET_W_ARC);
        ENUM_IN(s_arc_modes, v, m);
        lv_arc_set_mode(o, (lv_arc_mode_t)m);
        return 0;
    case GADGET_P_START_ANGLE: ONLY(GADGET_W_ARC); lv_arc_set_start_angle(o, clamp(v, -720, 720)); return 0;
    case GADGET_P_END_ANGLE: ONLY(GADGET_W_ARC); lv_arc_set_end_angle(o, clamp(v, -720, 720)); return 0;
    case GADGET_P_BG_START_ANGLE: ONLY(GADGET_W_ARC); lv_arc_set_bg_start_angle(o, clamp(v, -720, 720)); return 0;
    case GADGET_P_BG_END_ANGLE: ONLY(GADGET_W_ARC); lv_arc_set_bg_end_angle(o, clamp(v, -720, 720)); return 0;
    case GADGET_P_ROTATION:
        v = clamp(v, -3600, 3600);
        if (t == GADGET_W_ARC) {
            lv_arc_set_rotation(o, v);
        } else if (t == GADGET_W_SCALE) {
            lv_scale_set_rotation(o, v);
        } else if (t == GADGET_W_IMAGE) {
            lv_image_set_rotation(o, v * 10);   /* LVGL's are 0.1 degree */
        } else {
            return GADGET_ERR_UNSUPPORTED;
        }
        return 0;
    case GADGET_P_SELECTED:
        if (t == GADGET_W_ROLLER) {
            if (v < 0 || (uint32_t)v >= lv_roller_get_option_count(o)) {
                return GADGET_ERR_ARG;
            }
            lv_roller_set_selected(o, (uint32_t)v, LV_ANIM_OFF);
        } else if (t == GADGET_W_DROPDOWN) {
            if (v < 0 || (uint32_t)v >= lv_dropdown_get_option_count(o)) {
                return GADGET_ERR_ARG;
            }
            lv_dropdown_set_selected(o, (uint32_t)v);
        } else {
            return GADGET_ERR_UNSUPPORTED;
        }
        return 0;
    case GADGET_P_VISIBLE_ROWS:
        ONLY(GADGET_W_ROLLER);
        if (v < 1 || v > 32) {
            return GADGET_ERR_ARG;
        }
        lv_roller_set_visible_row_count(o, (uint32_t)v);
        return 0;
    case GADGET_P_INFINITE: {
        ONLY(GADGET_W_ROLLER);
        roller_t *r = s->ext;
        if (r && r->infinite == (v != 0)) {
            return 0;
        }
        uint32_t sel = lv_roller_get_selected(o);
        int32_t err = roller_set(s, r ? r->options : lv_roller_get_options(o), v != 0);
        if (!err) {
            lv_roller_set_selected(o, sel, LV_ANIM_OFF);
        }
        return err;
    }
    case GADGET_P_LIST_DIR:
        ONLY(GADGET_W_DROPDOWN);
        if (v != GADGET_DIR_LEFT && v != GADGET_DIR_RIGHT && v != GADGET_DIR_TOP && v != GADGET_DIR_BOTTOM) {
            return GADGET_ERR_ARG;
        }
        lv_dropdown_set_dir(o, (lv_dir_t)v);
        return 0;
    case GADGET_P_ONE_LINE: ONLY(GADGET_W_TEXTAREA); lv_textarea_set_one_line(o, v != 0); return 0;
    case GADGET_P_PASSWORD: ONLY(GADGET_W_TEXTAREA); lv_textarea_set_password_mode(o, v != 0); return 0;
    case GADGET_P_MAX_LENGTH:
        ONLY(GADGET_W_TEXTAREA);
        lv_textarea_set_max_length(o, (uint32_t)clamp(v, 0, MAX_TEXT));
        return 0;
    case GADGET_P_CURSOR_POS: ONLY(GADGET_W_TEXTAREA); lv_textarea_set_cursor_pos(o, v); return 0;
    case GADGET_P_TEXTAREA: {
        ONLY(GADGET_W_KEYBOARD);
        slot_t *ta = v ? find(v) : NULL;
        if (v && (!ta || ta->type != GADGET_W_TEXTAREA)) {
            return GADGET_ERR_ARG;
        }
        lv_keyboard_set_textarea(o, ta ? ta->obj : NULL);
        return 0;
    }
    case GADGET_P_KEYBOARD_MODE:
        ONLY(GADGET_W_KEYBOARD);
        ENUM_IN(s_kb_modes, v, m);
        lv_keyboard_set_mode(o, (lv_keyboard_mode_t)m);
        return 0;
    case GADGET_P_SCALE_MODE:
        ONLY(GADGET_W_SCALE);
        ENUM_IN(s_scale_modes, v, m);
        lv_scale_set_mode(o, (lv_scale_mode_t)m);
        return 0;
    case GADGET_P_TOTAL_TICKS:
        ONLY(GADGET_W_SCALE);
        lv_scale_set_total_tick_count(o, (uint32_t)clamp(v, 0, 1000));
        return 0;
    case GADGET_P_MAJOR_TICK_EVERY:
        ONLY(GADGET_W_SCALE);
        lv_scale_set_major_tick_every(o, (uint32_t)clamp(v, 0, 1000));
        return 0;
    case GADGET_P_LABEL_SHOW: ONLY(GADGET_W_SCALE); lv_scale_set_label_show(o, v != 0); return 0;
    case GADGET_P_ANGLE_RANGE: ONLY(GADGET_W_SCALE); lv_scale_set_angle_range(o, (uint32_t)clamp(v, 0, 360)); return 0;
    case GADGET_P_SPIN_TIME:
        ONLY(GADGET_W_SPINNER);
        lv_spinner_set_anim_duration(o, (uint32_t)clamp(v, 1, 600000));
        return 0;
    case GADGET_P_ARC_SWEEP: ONLY(GADGET_W_SPINNER); lv_spinner_set_arc_sweep(o, clamp(v, 0, 360)); return 0;
    case GADGET_P_CHART_TYPE:
        ONLY(GADGET_W_CHART);
        ENUM_IN(s_chart_types, v, m);
        lv_chart_set_type(o, (lv_chart_type_t)m);
        return 0;
    case GADGET_P_POINT_COUNT: {
        ONLY(GADGET_W_CHART);
        if (v < 1 || v > MAX_POINTS) {
            return GADGET_ERR_ARG;
        }
        chart_t *c = chart_ext(s);
        if (!c) {
            return GADGET_ERR_NO_MEMORY;
        }
        size_t was = lv_chart_get_point_count(o) * sizeof(int32_t) * c->n, now = (size_t)v * sizeof(int32_t) * c->n;
        if (!charge_delta(s, was, now)) {
            return GADGET_ERR_NO_MEMORY;
        }
        lv_chart_set_point_count(o, (uint32_t)v);
        return 0;
    }
    case GADGET_P_UPDATE_MODE:
        ONLY(GADGET_W_CHART);
        ENUM_IN(s_chart_updates, v, m);
        lv_chart_set_update_mode(o, (lv_chart_update_mode_t)m);
        return 0;
    case GADGET_P_HOR_DIV_LINES:
        ONLY(GADGET_W_CHART);
        lv_chart_set_hor_div_line_count(o, (uint32_t)clamp(v, 0, 100));
        return 0;
    case GADGET_P_VER_DIV_LINES:
        ONLY(GADGET_W_CHART);
        lv_chart_set_ver_div_line_count(o, (uint32_t)clamp(v, 0, 100));
        return 0;
    case GADGET_P_ROW_COUNT:
        ONLY(GADGET_W_TABLE);
        if (v < 0 || v > MAX_TABLE_ROWS) {
            return GADGET_ERR_ARG;
        }
        table_refund(s, (uint32_t)v, lv_table_get_column_count(o));
        lv_table_set_row_count(o, (uint32_t)v);
        return 0;
    case GADGET_P_COLUMN_COUNT:
        ONLY(GADGET_W_TABLE);
        if (v < 1 || v > MAX_TABLE_COLS) {
            return GADGET_ERR_ARG;
        }
        table_refund(s, lv_table_get_row_count(o), (uint32_t)v);
        lv_table_set_column_count(o, (uint32_t)v);
        return 0;
    case GADGET_P_IMAGE_SCALE:
        ONLY(GADGET_W_IMAGE);
        if (v < 1 || v > 10000) {
            return GADGET_ERR_ARG;
        }
        lv_image_set_scale(o, (uint32_t)(v * 256 / 100 > 0 ? v * 256 / 100 : 1));
        return 0;
    case GADGET_P_OFFSET_X: ONLY(GADGET_W_IMAGE); lv_image_set_offset_x(o, clamp(v, -32767, 32767)); return 0;
    case GADGET_P_OFFSET_Y: ONLY(GADGET_W_IMAGE); lv_image_set_offset_y(o, clamp(v, -32767, 32767)); return 0;
    case GADGET_P_INNER_ALIGN:
        ONLY(GADGET_W_IMAGE);
        ENUM_IN(s_image_aligns, v, m);
        lv_image_set_inner_align(o, (lv_image_align_t)m);
        return 0;
    case GADGET_P_Y_INVERT: ONLY(GADGET_W_LINE); lv_line_set_y_invert(o, v != 0); return 0;
    default: return GADGET_ERR_ARG;
    }
}

static int32_t widget_get(slot_t *s, int32_t prop, int32_t *out)
{
    lv_obj_t *o = s->obj;
    int t = s->type;
    switch (prop) {
    case GADGET_P_LONG_MODE: ONLY(GADGET_W_LABEL); *out = MAP_OUT(s_long_modes, lv_label_get_long_mode(o)); return 0;
    case GADGET_P_VALUE:
        if (t == GADGET_W_ARC) {
            *out = lv_arc_get_value(o);
        } else if (t == GADGET_W_BAR) {
            *out = lv_bar_get_value(o);
        } else if (t == GADGET_W_SLIDER) {
            *out = lv_slider_get_value(o);
        } else {
            return GADGET_ERR_UNSUPPORTED;
        }
        return 0;
    case GADGET_P_MIN_VALUE:
    case GADGET_P_MAX_VALUE: {
        bool min = prop == GADGET_P_MIN_VALUE;
        if (t == GADGET_W_ARC) {
            *out = min ? lv_arc_get_min_value(o) : lv_arc_get_max_value(o);
        } else if (t == GADGET_W_BAR) {
            *out = min ? lv_bar_get_min_value(o) : lv_bar_get_max_value(o);
        } else if (t == GADGET_W_SLIDER) {
            *out = min ? lv_slider_get_min_value(o) : lv_slider_get_max_value(o);
        } else if (t == GADGET_W_SCALE) {
            *out = min ? lv_scale_get_range_min_value(o) : lv_scale_get_range_max_value(o);
        } else if (t == GADGET_W_CHART) {
            chart_t *c = s->ext;
            *out = c ? (min ? c->min : c->max) : (min ? 0 : 100);
        } else {
            return GADGET_ERR_UNSUPPORTED;
        }
        return 0;
    }
    case GADGET_P_START_VALUE:
        if (t == GADGET_W_BAR) {
            *out = lv_bar_get_start_value(o);
        } else if (t == GADGET_W_SLIDER) {
            *out = lv_slider_get_left_value(o);
        } else {
            return GADGET_ERR_UNSUPPORTED;
        }
        return 0;
    case GADGET_P_BAR_MODE:
        if (t == GADGET_W_BAR) {
            *out = MAP_OUT(s_bar_modes, lv_bar_get_mode(o));
        } else if (t == GADGET_W_SLIDER) {
            *out = MAP_OUT(s_bar_modes, lv_slider_get_mode(o));
        } else {
            return GADGET_ERR_UNSUPPORTED;
        }
        return 0;
    case GADGET_P_ORIENTATION:
        if (t == GADGET_W_BAR) {
            *out = MAP_OUT(s_orients, lv_bar_get_orientation(o));
        } else if (t == GADGET_W_SLIDER) {
            *out = MAP_OUT(s_orients, lv_slider_get_orientation(o));
        } else if (t == GADGET_W_SWITCH) {
            *out = MAP_OUT(s_switch_orients, lv_switch_get_orientation(o));
        } else {
            return GADGET_ERR_UNSUPPORTED;
        }
        return 0;
    case GADGET_P_ARC_MODE: ONLY(GADGET_W_ARC); *out = MAP_OUT(s_arc_modes, lv_arc_get_mode(o)); return 0;
    case GADGET_P_START_ANGLE: ONLY(GADGET_W_ARC); *out = (int32_t)lv_arc_get_angle_start(o); return 0;
    case GADGET_P_END_ANGLE: ONLY(GADGET_W_ARC); *out = (int32_t)lv_arc_get_angle_end(o); return 0;
    case GADGET_P_BG_START_ANGLE: ONLY(GADGET_W_ARC); *out = (int32_t)lv_arc_get_bg_angle_start(o); return 0;
    case GADGET_P_BG_END_ANGLE: ONLY(GADGET_W_ARC); *out = (int32_t)lv_arc_get_bg_angle_end(o); return 0;
    case GADGET_P_ROTATION:
        if (t == GADGET_W_ARC) {
            *out = lv_arc_get_rotation(o);
        } else if (t == GADGET_W_SCALE) {
            *out = lv_scale_get_rotation(o);
        } else if (t == GADGET_W_IMAGE) {
            *out = lv_image_get_rotation(o) / 10;
        } else {
            return GADGET_ERR_UNSUPPORTED;
        }
        return 0;
    case GADGET_P_SELECTED:
        if (t == GADGET_W_ROLLER) {
            *out = (int32_t)lv_roller_get_selected(o);
        } else if (t == GADGET_W_DROPDOWN) {
            *out = (int32_t)lv_dropdown_get_selected(o);
        } else {
            return GADGET_ERR_UNSUPPORTED;
        }
        return 0;
    case GADGET_P_VISIBLE_ROWS: {
        ONLY(GADGET_W_ROLLER);   /* as lv_roller_set_visible_row_count sized it */
        const lv_font_t *f = lv_obj_get_style_text_font(o, LV_PART_MAIN);
        int32_t row = lv_font_get_line_height(f) + lv_obj_get_style_text_line_space(o, LV_PART_MAIN);
        lv_obj_update_layout(o);
        int32_t h = lv_obj_get_height(o) - 2 * lv_obj_get_style_border_width(o, LV_PART_MAIN);
        *out = row > 0 ? (h + row / 2) / row : 0;
        return 0;
    }
    case GADGET_P_INFINITE: {
        ONLY(GADGET_W_ROLLER);
        roller_t *r = s->ext;
        *out = r && r->infinite;
        return 0;
    }
    case GADGET_P_LIST_DIR: ONLY(GADGET_W_DROPDOWN); *out = lv_dropdown_get_dir(o); return 0;
    case GADGET_P_ONE_LINE: ONLY(GADGET_W_TEXTAREA); *out = lv_textarea_get_one_line(o); return 0;
    case GADGET_P_PASSWORD: ONLY(GADGET_W_TEXTAREA); *out = lv_textarea_get_password_mode(o); return 0;
    case GADGET_P_MAX_LENGTH: ONLY(GADGET_W_TEXTAREA); *out = (int32_t)lv_textarea_get_max_length(o); return 0;
    case GADGET_P_CURSOR_POS: ONLY(GADGET_W_TEXTAREA); *out = (int32_t)lv_textarea_get_cursor_pos(o); return 0;
    case GADGET_P_TEXTAREA: {
        ONLY(GADGET_W_KEYBOARD);
        slot_t *ta = slot_of(lv_keyboard_get_textarea(o));
        *out = ta ? handle_of(index_of(ta)) : 0;
        return 0;
    }
    case GADGET_P_KEYBOARD_MODE: ONLY(GADGET_W_KEYBOARD); *out = MAP_OUT(s_kb_modes, lv_keyboard_get_mode(o)); return 0;
    case GADGET_P_SCALE_MODE: ONLY(GADGET_W_SCALE); *out = MAP_OUT(s_scale_modes, lv_scale_get_mode(o)); return 0;
    case GADGET_P_TOTAL_TICKS: ONLY(GADGET_W_SCALE); *out = (int32_t)lv_scale_get_total_tick_count(o); return 0;
    case GADGET_P_MAJOR_TICK_EVERY: ONLY(GADGET_W_SCALE); *out = (int32_t)lv_scale_get_major_tick_every(o); return 0;
    case GADGET_P_LABEL_SHOW: ONLY(GADGET_W_SCALE); *out = lv_scale_get_label_show(o); return 0;
    case GADGET_P_ANGLE_RANGE: ONLY(GADGET_W_SCALE); *out = (int32_t)lv_scale_get_angle_range(o); return 0;
    case GADGET_P_SPIN_TIME: ONLY(GADGET_W_SPINNER); *out = (int32_t)lv_spinner_get_anim_duration(o); return 0;
    case GADGET_P_ARC_SWEEP: ONLY(GADGET_W_SPINNER); *out = (int32_t)lv_spinner_get_arc_sweep(o); return 0;
    case GADGET_P_CHART_TYPE: ONLY(GADGET_W_CHART); *out = MAP_OUT(s_chart_types, lv_chart_get_type(o)); return 0;
    case GADGET_P_POINT_COUNT: ONLY(GADGET_W_CHART); *out = (int32_t)lv_chart_get_point_count(o); return 0;
    case GADGET_P_UPDATE_MODE:
        ONLY(GADGET_W_CHART);
        *out = MAP_OUT(s_chart_updates, lv_chart_get_update_mode(o));
        return 0;
    case GADGET_P_HOR_DIV_LINES: ONLY(GADGET_W_CHART); *out = (int32_t)lv_chart_get_hor_div_line_count(o); return 0;
    case GADGET_P_VER_DIV_LINES: ONLY(GADGET_W_CHART); *out = (int32_t)lv_chart_get_ver_div_line_count(o); return 0;
    case GADGET_P_ROW_COUNT: ONLY(GADGET_W_TABLE); *out = (int32_t)lv_table_get_row_count(o); return 0;
    case GADGET_P_COLUMN_COUNT: ONLY(GADGET_W_TABLE); *out = (int32_t)lv_table_get_column_count(o); return 0;
    case GADGET_P_IMAGE_SCALE: ONLY(GADGET_W_IMAGE); *out = (lv_image_get_scale(o) * 100 + 128) / 256; return 0;
    case GADGET_P_OFFSET_X: ONLY(GADGET_W_IMAGE); *out = lv_image_get_offset_x(o); return 0;
    case GADGET_P_OFFSET_Y: ONLY(GADGET_W_IMAGE); *out = lv_image_get_offset_y(o); return 0;
    case GADGET_P_INNER_ALIGN:
        ONLY(GADGET_W_IMAGE);
        *out = MAP_OUT(s_image_aligns, lv_image_get_inner_align(o));
        return 0;
    case GADGET_P_Y_INVERT: ONLY(GADGET_W_LINE); *out = lv_line_get_y_invert(o); return 0;
    default: return GADGET_ERR_ARG;
    }
}

/* ---- Any property --------------------------------------------------------------------------- */

static int32_t set_prop(slot_t *s, int32_t prop, int32_t v)
{
    int info = prop_info(prop);
    if (!info || (info & 0xFF) == PT_text || !(info & PA_w)) {
        return GADGET_ERR_ARG;
    }
    if (prop >= 100 && prop < 200) {
        return style_set(s->obj, NULL, LV_PART_MAIN | LV_STATE_DEFAULT, prop, v);
    }
    return prop < 100 ? obj_set(s, prop, v) : widget_set(s, prop, v);
}

static int32_t get_prop(slot_t *s, int32_t prop, int32_t *out)
{
    int info = prop_info(prop);
    if (!info || (info & 0xFF) == PT_text || !(info & PA_r)) {
        return GADGET_ERR_ARG;
    }
    if (prop >= 100 && prop < 200) {
        return style_get(s->obj, prop, out);
    }
    return prop < 100 ? obj_get(s, prop, out) : widget_get(s, prop, out);
}

/* A text property's text now (NULL: none), or *err. */
static const char *text_get(slot_t *s, int32_t prop, int32_t *err)
{
    lv_obj_t *o = s->obj;
    int t = s->type;
    *err = 0;
    switch (prop) {
    case GADGET_P_TEXT:
        if (t == GADGET_W_LABEL) return lv_label_get_text(o);
        if (t == GADGET_W_CHECKBOX) return lv_checkbox_get_text(o);
        if (t == GADGET_W_TEXTAREA) return lv_textarea_get_text(o);
        if (t == GADGET_W_DROPDOWN) return lv_dropdown_get_text(o);
        break;
    case GADGET_P_OPTIONS:
        if (t == GADGET_W_ROLLER) return s->ext ? ((roller_t *)s->ext)->options : lv_roller_get_options(o);
        if (t == GADGET_W_DROPDOWN) return lv_dropdown_get_options(o);
        break;
    case GADGET_P_PLACEHOLDER:
        if (t == GADGET_W_TEXTAREA) return lv_textarea_get_placeholder_text(o);
        break;
    case GADGET_P_ACCEPTED_CHARS:
        if (t == GADGET_W_TEXTAREA) return lv_textarea_get_accepted_chars(o);
        break;
    default: *err = GADGET_ERR_ARG; return NULL;
    }
    *err = GADGET_ERR_UNSUPPORTED;
    return NULL;
}

static int32_t text_set(slot_t *s, int32_t prop, const char *text)
{
    lv_obj_t *o = s->obj;
    int t = s->type;
    int32_t err;
    const char *old = text_get(s, prop, &err);
    if (err) {
        return err;
    }
    size_t old_len = old ? strlen(old) : 0, len = strlen(text);
    if (len > MAX_TEXT) {
        return GADGET_ERR_ARG;
    }
    if (!charge_delta(s, old_len, len)) {
        return GADGET_ERR_NO_MEMORY;
    }
    switch (prop) {
    case GADGET_P_TEXT:
        if (t == GADGET_W_LABEL) {
            lv_label_set_text(o, text);
        } else if (t == GADGET_W_CHECKBOX) {
            lv_checkbox_set_text(o, text);
        } else if (t == GADGET_W_TEXTAREA) {
            lv_textarea_set_text(o, text);
            /* It keeps what its max length and accepted characters allow:
             * charge that, not what was offered. */
            const char *kept = lv_textarea_get_text(o);
            charge_delta(s, len, kept ? strlen(kept) : 0);
        } else {
            lv_dropdown_set_text(o, *text ? text : NULL);   /* "": the selected option again */
        }
        return 0;
    case GADGET_P_OPTIONS:
        if (t == GADGET_W_ROLLER) {
            roller_t *r = s->ext;
            err = roller_set(s, text, r && r->infinite);
            if (err) {
                charge_delta(s, len, old_len);
            }
            return err;
        }
        lv_dropdown_set_options(o, text);
        return 0;
    case GADGET_P_PLACEHOLDER: lv_textarea_set_placeholder_text(o, text); return 0;
    default: lv_textarea_set_accepted_chars(o, *text ? text : NULL); return 0;
    }
}

/* ---- Images ---------------------------------------------------------------------------------- */

/* A decoded image: RGB565, or RGB565A8 when it has transparency, after this
 * header in one PSRAM block, charged to the budget while an object shows it. */
typedef struct {
    lv_image_dsc_t dsc;
    uint32_t pixels;
    size_t bytes;
} image_t;
#define IMAGE_HEAD ((sizeof(image_t) + 7) & ~(size_t)7)

static image_t *image_new(uint32_t w, uint32_t h, bool alpha)
{
    size_t data = (size_t)w * h * (alpha ? 3 : 2);
    image_t *img = heap_caps_aligned_alloc(8, IMAGE_HEAD + data, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!img) {
        return NULL;
    }
    memset(img, 0, sizeof(*img));
    img->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    img->dsc.header.cf = alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    img->dsc.header.w = w;
    img->dsc.header.h = h;
    img->dsc.header.stride = w * 2;   /* RGB565A8: the colour plane's; the alpha plane follows */
    img->dsc.data_size = data;
    img->dsc.data = (const uint8_t *)img + IMAGE_HEAD;
    img->pixels = w * h;
    img->bytes = IMAGE_HEAD + data;
    return img;
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#if LV_USE_LODEPNG
static int32_t decode_png(const uint8_t *data, size_t len, uint32_t max_pixels, image_t **out)
{
    unsigned w = 0, h = 0;
    LodePNGState st;
    lodepng_state_init(&st);
    unsigned err = lodepng_inspect(&w, &h, &st, data, len);
    /* LVGL's lodepng unfilters into a buffer of 4 bytes a pixel before it
     * converts: 16-bit channels (up to 8 bytes a pixel) would overrun it. */
    unsigned depth = st.info_png.color.bitdepth;
    lodepng_state_cleanup(&st);
    if (err || !w || !h || w > 0xFFFF || h > 0xFFFF || depth > 8) {
        return GADGET_ERR_ARG;
    }
    if ((uint64_t)w * h > max_pixels) {
        return GADGET_ERR_NO_MEMORY;
    }
    /* LVGL's lodepng hands back a draw buffer (RGBA bytes, rows of its
     * stride), not the bare pixels upstream's does. */
    lv_draw_buf_t *decoded = NULL;
    err = lodepng_decode32((unsigned char **)&decoded, &w, &h, data, len);
    if (err || !decoded) {
        if (decoded) {
            lv_draw_buf_destroy(decoded);
        }
        return err == 83 ? GADGET_ERR_NO_MEMORY : GADGET_ERR_ARG;   /* 83: lodepng's out of memory */
    }
    uint32_t stride = decoded->header.stride;
    if (!w || !h || (uint64_t)w * h > max_pixels || decoded->header.w != w || decoded->header.h != h ||
        stride < 4 * w || (uint64_t)stride * h > decoded->data_size) {
        lv_draw_buf_destroy(decoded);
        return GADGET_ERR_ARG;
    }
    size_t n = (size_t)w * h;
    bool alpha = false;
    for (unsigned y = 0; y < h && !alpha; y++) {
        const uint8_t *row = decoded->data + (size_t)y * stride;
        for (unsigned x = 0; x < w && !alpha; x++) {
            alpha = row[x * 4 + 3] != 0xFF;
        }
    }
    image_t *img = image_new(w, h, alpha);
    if (img) {
        uint16_t *px = (uint16_t *)img->dsc.data;
        uint8_t *a = (uint8_t *)(px + n);
        for (unsigned y = 0; y < h; y++) {
            const uint8_t *row = decoded->data + (size_t)y * stride;
            for (unsigned x = 0; x < w; x++) {
                const uint8_t *p = row + x * 4;
                size_t i = (size_t)y * w + x;
                px[i] = rgb565(p[0], p[1], p[2]);
                if (alpha) {
                    a[i] = p[3];
                }
            }
        }
    }
    lv_draw_buf_destroy(decoded);
    *out = img;
    return img ? 0 : GADGET_ERR_NO_MEMORY;
}
#endif

#if LV_USE_TJPGD
typedef struct {
    const uint8_t *data;
    size_t len, at;
    uint16_t *px;
    uint32_t w;
} jpeg_io_t;

static size_t jpeg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jpeg_io_t *io = jd->device;
    size_t left = io->len - io->at;
    n = n < left ? n : left;
    if (buf) {
        memcpy(buf, io->data + io->at, n);
    }
    io->at += n;
    return n;
}

static int jpeg_out(JDEC *jd, void *bitmap, JRECT *r)
{
    jpeg_io_t *io = jd->device;
    const uint8_t *p = bitmap;   /* B, G, R: LVGL's TJpgDec */
    for (unsigned y = r->top; y <= r->bottom; y++) {
        uint16_t *row = io->px + (size_t)y * io->w;
        for (unsigned x = r->left; x <= r->right; x++, p += 3) {
            row[x] = rgb565(p[2], p[1], p[0]);
        }
    }
    return 1;
}

static int32_t decode_jpeg(const uint8_t *data, size_t len, uint32_t max_pixels, image_t **out)
{
    void *pool = heap_caps_malloc(JPEG_POOL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pool) {
        return GADGET_ERR_NO_MEMORY;
    }
    jpeg_io_t io = {.data = data, .len = len};
    JDEC jd;
    int32_t result = GADGET_ERR_ARG;
    image_t *img = NULL;
    if (jd_prepare(&jd, jpeg_in, pool, JPEG_POOL, &io) == JDR_OK && jd.width && jd.height) {
        if ((uint64_t)jd.width * jd.height > max_pixels) {
            result = GADGET_ERR_NO_MEMORY;
        } else if (!(img = image_new(jd.width, jd.height, false))) {
            result = GADGET_ERR_NO_MEMORY;
        } else {
            io.px = (uint16_t *)img->dsc.data;
            io.w = jd.width;
            if (jd_decomp(&jd, jpeg_out, 0) == JDR_OK) {
                result = 0;
            } else {
                heap_caps_free(img);
                img = NULL;
            }
        }
    }
    heap_caps_free(pool);
    *out = img;
    return result;
}
#endif

/* A PNG or JPEG file into a new image of at most max_pixels. Any task; no lock. */
static int32_t decode(const uint8_t *data, size_t len, uint32_t max_pixels, image_t **out)
{
    *out = NULL;
#if LV_USE_LODEPNG
    if (len >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) {
        return decode_png(data, len, max_pixels, out);
    }
#endif
#if LV_USE_TJPGD
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return decode_jpeg(data, len, max_pixels, out);
    }
#endif
    return GADGET_ERR_ARG;
}

/* Pixels an image object could show: what's free, plus what its image holds now. */
static uint32_t image_room(const slot_t *s)
{
    const image_t *old = s->ext;
    return s_ui->pixel_limit - s_ui->pixels + (old ? old->pixels : 0);
}

/* Shows img on an image object, in place of its old one; takes img on success. */
static int32_t image_attach(slot_t *s, image_t *img)
{
    image_t *old = s->ext;
    uint32_t old_px = old ? old->pixels : 0;
    muse_apps_ui_pixels_give(old_px);
    if (!muse_apps_ui_pixels_take(img->pixels)) {
        muse_apps_ui_pixels_take(old_px);
        return GADGET_ERR_NO_MEMORY;
    }
    if (!muse_apps_charge(img->bytes)) {
        muse_apps_ui_pixels_give(img->pixels);
        muse_apps_ui_pixels_take(old_px);
        return GADGET_ERR_NO_MEMORY;
    }
    lv_image_set_src(s->obj, &img->dsc);
    s->ext = img;
    if (old) {
        IMAGE_DROP(&old->dsc);
        muse_apps_uncharge(old->bytes);
        heap_caps_free(old);
    }
    return 0;
}

static void image_free(slot_t *s)
{
    image_t *img = s->ext;
    if (img) {
        IMAGE_DROP(&img->dsc);
        muse_apps_ui_pixels_give(img->pixels);
        muse_apps_uncharge(img->bytes);
        heap_caps_free(img);
    }
    s->ext = NULL;
}

/* ---- Animation ------------------------------------------------------------------------------- */

static const lv_anim_path_cb_t s_paths[] = {
    [GADGET_EASE_LINEAR] = lv_anim_path_linear,     [GADGET_EASE_IN] = lv_anim_path_ease_in,
    [GADGET_EASE_OUT] = lv_anim_path_ease_out,      [GADGET_EASE_IN_OUT] = lv_anim_path_ease_in_out,
    [GADGET_EASE_OVERSHOOT] = lv_anim_path_overshoot, [GADGET_EASE_BOUNCE] = lv_anim_path_bounce,
    [GADGET_EASE_STEP] = lv_anim_path_step,
};

/* LVGL's task: one step. Each animation runs from 0 to ANIM_STEPS; this maps
 * that onto the property's own values (colours mix). */
static void anim_exec(lv_anim_t *a, int32_t v)
{
    anim_t *r = lv_anim_get_user_data(a);
    if (!s_ui || !r || !r->used || !s_ui->slots[r->slot].obj) {
        return;
    }
    int32_t value;
    if (r->color) {
        uint8_t mix = (uint8_t)clamp(v * 255 / ANIM_STEPS, 0, 255);
        value = rgb_out(lv_color_mix(lv_color_hex((uint32_t)r->to), lv_color_hex((uint32_t)r->from), mix));
    } else {
        value = r->from + (int32_t)(((int64_t)r->to - r->from) * v / ANIM_STEPS);
    }
    set_prop(&s_ui->slots[r->slot], r->prop, value);
}

static void anim_completed(lv_anim_t *a)
{
    anim_t *r = lv_anim_get_user_data(a);
    if (s_ui && r && r->used) {
        slot_t *s = &s_ui->slots[r->slot];
        if (s->obj && (s->events & GADGET_UI_ANIM_DONE)) {
            muse_apps_post(GADGET_EV_UI, handle_of(r->slot), GADGET_UI_ANIM_DONE);
        }
    }
}

static void anim_deleted(lv_anim_t *a)
{
    anim_t *r = lv_anim_get_user_data(a);
    if (r) {
        r->used = false;
    }
}

/* Stops a slot's animations of prop (0: all of them) where they are. */
static void anim_stop(int idx, int32_t prop)
{
    for (int i = 0; i < MAX_ANIMS; i++) {
        anim_t *r = &s_ui->anims[i];
        if (r->used && r->slot == idx && (!prop || r->prop == prop)) {
            lv_anim_delete(r, NULL);
            r->used = false;
        }
    }
}

/* ---- Events ----------------------------------------------------------------------------------- */

static const struct {
    uint32_t bit;
    lv_event_code_t code;
} s_events[] = {
    {GADGET_UI_CLICKED, LV_EVENT_CLICKED},       {GADGET_UI_PRESSED, LV_EVENT_PRESSED},
    {GADGET_UI_RELEASED, LV_EVENT_RELEASED},     {GADGET_UI_LONG_PRESSED, LV_EVENT_LONG_PRESSED},
    {GADGET_UI_VALUE_CHANGED, LV_EVENT_VALUE_CHANGED}, {GADGET_UI_FOCUSED, LV_EVENT_FOCUSED},
    {GADGET_UI_DEFOCUSED, LV_EVENT_DEFOCUSED},   {GADGET_UI_READY, LV_EVENT_READY},
    {GADGET_UI_CANCEL, LV_EVENT_CANCEL},         {GADGET_UI_SCROLL_END, LV_EVENT_SCROLL_END},
};
#define N_EVENTS (sizeof(s_events) / sizeof(s_events[0]))

/* LVGL's task: an event the app asked for with ui_on. A bubbled one names
 * the object it happened to, unless that object asked for it itself. */
static void on_ui_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    uint32_t bit = 0;
    for (size_t i = 0; i < N_EVENTS && !bit; i++) {
        bit = s_events[i].code == code ? s_events[i].bit : 0;
    }
    slot_t *cur = slot_of(lv_event_get_current_target_obj(e));
    if (!bit || !cur || !(cur->events & bit)) {
        return;
    }
    slot_t *from = cur;
    lv_obj_t *target = lv_event_get_target_obj(e);
    if (target != cur->obj) {
        slot_t *t = slot_of(target);
        if (t && (t->events & bit)) {
            return;   /* it told the app itself */
        }
        from = t ? t : cur;
    }
    muse_apps_post(GADGET_EV_UI, handle_of(index_of(from)), (int)bit);
}

/* Any task holding the lock: an object the app made is going (with its
 * parent, ui_delete, ui_clean, or the app's stop). Frees what it held. */
static void on_delete(lv_event_t *e)
{
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_target_obj(e);
    if (!s_ui || idx == 0 || idx >= MAX_OBJECTS || s_ui->slots[idx].obj != o) {
        return;
    }
    slot_t *s = &s_ui->slots[idx];
    anim_stop((int)idx, 0);
    switch (s->type) {
    case GADGET_W_IMAGE: image_free(s); break;
    case GADGET_W_CANVAS: muse_apps_canvas_free(s->ext); break;
    case GADGET_W_TEXTAREA:   /* keyboards typing into it let go */
        for (int i = 1; i < MAX_OBJECTS; i++) {
            slot_t *k = &s_ui->slots[i];
            if (k->obj && k->type == GADGET_W_KEYBOARD && lv_keyboard_get_textarea(k->obj) == o) {
                lv_keyboard_set_textarea(k->obj, NULL);
            }
        }
        break;
    default: muse_apps_free(s->ext); break;   /* line points, chart_t, roller_t, or NULL */
    }
    muse_apps_free(s->grid);
    muse_apps_uncharge(s->charged);
    uint32_t gen = s->gen >= 0x7FFFFF ? 1 : s->gen + 1;
    memset(s, 0, sizeof(*s));
    s->gen = gen;
    s_ui->n_objects--;
}

/* ---- Objects ------------------------------------------------------------------------------------ */

/* Tracks in one half of a ui_grid's array: its columns, or its rows. */
static uint32_t grid_tracks(const int32_t *grid, bool rows)
{
    const int32_t *t = grid + (rows ? MAX_GRID + 1 : 0);
    uint32_t n = 0;
    while (n < MAX_GRID && t[n] != LV_GRID_TEMPLATE_LAST) {
        n++;
    }
    return n;
}

static bool track_fits(int32_t pos, int32_t span, uint32_t tracks)
{
    return pos >= 0 && span >= 1 && (uint32_t)pos < tracks && (uint32_t)span <= tracks - (uint32_t)pos;
}

/* Whether an object's cell lies within its parent's tracks: LVGL's grid
 * indexes the tracks with the cell's position and span unchecked. */
static bool cell_fits(lv_obj_t *o, const int32_t *grid)
{
    return track_fits(lv_obj_get_style_grid_cell_column_pos(o, LV_PART_MAIN),
                      lv_obj_get_style_grid_cell_column_span(o, LV_PART_MAIN), grid_tracks(grid, false)) &&
           track_fits(lv_obj_get_style_grid_cell_row_pos(o, LV_PART_MAIN),
                      lv_obj_get_style_grid_cell_row_span(o, LV_PART_MAIN), grid_tracks(grid, true));
}

static void first_cell(lv_obj_t *o)
{
    lv_obj_set_grid_cell(o, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
}

/* A crowded line chart (as many points as pixels) has LVGL allocate
 * (w + 2 * extra) * 3 points for up to w + 1 columns: with no content width,
 * or no extra (no bullet and no line width), it writes past them. Such a
 * chart isn't drawn. */
static void chart_guard(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    uint32_t n = lv_chart_get_point_count(o);
    int32_t w = lv_obj_get_content_width(o);
    if (lv_chart_get_type(o) != LV_CHART_TYPE_LINE || n < 2 || (int64_t)n < w) {
        return;
    }
    int32_t extra = w / (int32_t)(n - 1) + lv_obj_get_style_width(o, LV_PART_INDICATOR) / 2 +
                    lv_obj_get_style_line_width(o, LV_PART_ITEMS);
    if (w < 1 || extra < 1) {
        lv_event_stop_processing(e);
    }
}

static lv_obj_t *create(int type, lv_obj_t *parent)
{
    switch (type) {
    case GADGET_W_OBJ: return lv_obj_create(parent);
    case GADGET_W_LABEL: return lv_label_create(parent);
    case GADGET_W_BUTTON: return lv_button_create(parent);
    case GADGET_W_IMAGE: return lv_image_create(parent);
    case GADGET_W_LINE: return lv_line_create(parent);
    case GADGET_W_ARC: return lv_arc_create(parent);
    case GADGET_W_BAR: return lv_bar_create(parent);
    case GADGET_W_SLIDER: return lv_slider_create(parent);
    case GADGET_W_SWITCH: return lv_switch_create(parent);
    case GADGET_W_CHECKBOX: return lv_checkbox_create(parent);
    case GADGET_W_ROLLER: return lv_roller_create(parent);
    case GADGET_W_DROPDOWN: return lv_dropdown_create(parent);
    case GADGET_W_TEXTAREA: return lv_textarea_create(parent);
    case GADGET_W_KEYBOARD: return lv_keyboard_create(parent);
    case GADGET_W_SCALE: return lv_scale_create(parent);
    case GADGET_W_SPINNER: return lv_spinner_create(parent);
    case GADGET_W_CHART: return lv_chart_create(parent);
    case GADGET_W_TABLE: return lv_table_create(parent);
    case GADGET_W_CANVAS: return lv_canvas_create(parent);
    default: return NULL;
    }
}

/* Types keypad navigation moves between. */
static bool focusable(int type)
{
    switch (type) {
    case GADGET_W_BUTTON:
    case GADGET_W_ARC:
    case GADGET_W_SLIDER:
    case GADGET_W_SWITCH:
    case GADGET_W_CHECKBOX:
    case GADGET_W_ROLLER:
    case GADGET_W_DROPDOWN:
    case GADGET_W_TEXTAREA:
    case GADGET_W_KEYBOARD:
    case GADGET_W_TABLE: return true;
    default: return false;
    }
}

/* LVGL takes a roller's first child as its label: an app's object there
 * (created there, or moved ahead of the label) would be read as one. */
static bool takes_children(const slot_t *p)
{
    return p->type != GADGET_W_ROLLER;
}

int32_t gadget_ui_create(wasm_exec_env_t env, int32_t type, int32_t parent)
{
    (void)env;
    slot_t *p = obj_arg(parent);
    if (type < GADGET_W_OBJ || type > GADGET_W_CANVAS) {
        return GADGET_ERR_ARG;
    }
    if (!p) {
        return GADGET_ERR_HANDLE;
    }
    if (!takes_children(p)) {
        return GADGET_ERR_ARG;
    }
    if (s_ui->n_objects >= MAX_OBJECTS - 1) {
        return GADGET_ERR_LIMIT;
    }
    int idx = 0;
    for (int i = 0; i < MAX_OBJECTS - 1 && !idx; i++) {
        int k = 1 + (s_ui->next + i) % (MAX_OBJECTS - 1);
        idx = s_ui->slots[k].obj ? 0 : k;
    }
    if (!idx || !muse_apps_charge(s_cost[type])) {
        return idx ? GADGET_ERR_NO_MEMORY : GADGET_ERR_LIMIT;
    }
    lv_obj_t *o = create(type, p->obj);
    if (!o) {
        muse_apps_uncharge(s_cost[type]);
        return GADGET_ERR_NO_MEMORY;
    }
    if (type == GADGET_W_CHART) {
        lv_obj_add_event_cb(o, chart_guard, LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS, NULL);
    }
    slot_t *s = &s_ui->slots[idx];
    uint32_t gen = s->gen ? s->gen : 1;
    *s = (slot_t){.obj = o, .gen = gen, .charged = s_cost[type], .type = (uint8_t)type};
    s_ui->next = idx;
    s_ui->n_objects++;
    lv_obj_set_user_data(o, (void *)(uintptr_t)idx);
    lv_obj_add_event_cb(o, on_delete, LV_EVENT_DELETE, (void *)(uintptr_t)idx);
    if (s_ui->group && focusable(type)) {
        lv_group_add_obj(s_ui->group, o);
    }
    return handle_of(idx);
}

int32_t gadget_ui_delete(wasm_exec_env_t env, int32_t obj)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s || is_root(s)) {
        return GADGET_ERR_HANDLE;
    }
    lv_obj_delete(s->obj);
    return 0;
}

int32_t gadget_ui_clean(wasm_exec_env_t env, int32_t obj)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    /* Only the app's children: a text area's or roller's own label stays. */
    for (int32_t i = (int32_t)lv_obj_get_child_count(s->obj) - 1; i >= 0; i--) {
        lv_obj_t *child = lv_obj_get_child(s->obj, i);
        if (slot_of(child)) {
            lv_obj_delete(child);
        }
    }
    return 0;
}

int32_t gadget_ui_set_parent(wasm_exec_env_t env, int32_t obj, int32_t parent)
{
    (void)env;
    slot_t *s = obj_arg(obj), *p = find(parent);
    if (!s || !p || is_root(s)) {
        return GADGET_ERR_HANDLE;
    }
    if (!takes_children(p)) {
        return GADGET_ERR_ARG;
    }
    for (lv_obj_t *up = p->obj; up; up = lv_obj_get_parent(up)) {
        if (up == s->obj) {
            return GADGET_ERR_ARG;   /* into itself */
        }
    }
    lv_obj_set_parent(s->obj, p->obj);
    if (p->grid && !cell_fits(s->obj, p->grid)) {
        first_cell(s->obj);   /* placed for another grid */
    }
    return 0;
}

int32_t gadget_ui_align(wasm_exec_env_t env, int32_t obj, int32_t align, int32_t dx, int32_t dy)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    if (is_root(s)) {
        return GADGET_ERR_UNSUPPORTED;
    }
    if (align < GADGET_ALIGN_DEFAULT || align > GADGET_ALIGN_OUT_RIGHT_BOTTOM) {
        return GADGET_ERR_ARG;
    }
    lv_obj_align(s->obj, (lv_align_t)align, clamp(dx, -32767, 32767), clamp(dy, -32767, 32767));
    return 0;
}

int32_t gadget_ui_align_to(wasm_exec_env_t env, int32_t obj, int32_t base, int32_t align, int32_t dx, int32_t dy)
{
    (void)env;
    slot_t *s = obj_arg(obj), *b = find(base);
    if (!s || !b) {
        return GADGET_ERR_HANDLE;
    }
    if (is_root(s)) {
        return GADGET_ERR_UNSUPPORTED;
    }
    if (align < GADGET_ALIGN_DEFAULT || align > GADGET_ALIGN_OUT_RIGHT_BOTTOM) {
        return GADGET_ERR_ARG;
    }
    lv_obj_align_to(s->obj, b->obj, (lv_align_t)align, clamp(dx, -32767, 32767), clamp(dy, -32767, 32767));
    return 0;
}

int32_t gadget_ui_set(wasm_exec_env_t env, int32_t obj, int32_t prop, int32_t value)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    return s ? set_prop(s, prop, value) : GADGET_ERR_HANDLE;
}

int32_t gadget_ui_get(wasm_exec_env_t env, int32_t obj, int32_t prop)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    int32_t v = 0;
    if (!s || get_prop(s, prop, &v)) {
        return GADGET_UI_INVALID;
    }
    return v;
}

int32_t gadget_ui_set_text(wasm_exec_env_t env, int32_t obj, int32_t prop, const char *text)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    return s ? text_set(s, prop, text) : GADGET_ERR_HANDLE;
}

int32_t gadget_ui_get_text(wasm_exec_env_t env, int32_t obj, int32_t prop, void *buf, uint32_t buf_size)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    int32_t err;
    const char *text = text_get(s, prop, &err);
    if (err) {
        return err;
    }
    size_t len = text ? strlen(text) : 0;
    if (buf && buf_size) {
        size_t n = len < buf_size - 1 ? len : buf_size - 1;
        memcpy(buf, text ? text : "", n);
        ((char *)buf)[n] = '\0';
    }
    return (int32_t)len;
}

int32_t gadget_ui_on(wasm_exec_env_t env, int32_t obj, int32_t events)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    uint32_t had = lv_obj_remove_event_cb(s->obj, on_ui_event), have = 0;
    s->events = (uint32_t)events;
    for (size_t i = 0; i < N_EVENTS; i++) {
        if (s->events & s_events[i].bit) {
            lv_obj_add_event_cb(s->obj, on_ui_event, s_events[i].code, NULL);
            have++;
        }
    }
    charge_delta(s, had * EVENT_COST, have * EVENT_COST);
    return 0;
}

/* ---- Layout extras ----------------------------------------------------------------------------- */

/* "1fr 40 auto" as LVGL's grid tracks; the count, or -1. */
static int parse_tracks(const char *spec, int32_t *out)
{
    int n = 0;
    const char *p = spec;
    for (;;) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        if (n >= MAX_GRID) {
            return -1;
        }
        if (!strncmp(p, "auto", 4)) {
            out[n++] = LV_GRID_CONTENT;
            p += 4;
        } else {
            if (*p < '0' || *p > '9') {
                return -1;
            }
            int32_t v = 0;
            for (; *p >= '0' && *p <= '9'; p++) {
                v = v * 10 + (*p - '0');
                if (v > 10000) {
                    return -1;
                }
            }
            if (p[0] == 'f' && p[1] == 'r') {
                if (v < 1 || v > 99) {   /* LV_GRID_FR(100) would be LV_GRID_TEMPLATE_LAST */
                    return -1;
                }
                out[n++] = LV_GRID_FR(v);
                p += 2;
            } else {
                out[n++] = v;
            }
        }
        if (*p && *p != ' ') {
            return -1;
        }
    }
    out[n] = LV_GRID_TEMPLATE_LAST;
    return n ? n : -1;
}

int32_t gadget_ui_grid(wasm_exec_env_t env, int32_t obj, const char *columns, const char *rows)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    int32_t cols[MAX_GRID + 1], rws[MAX_GRID + 1];
    if (parse_tracks(columns, cols) < 0 || parse_tracks(rows, rws) < 0) {
        return GADGET_ERR_ARG;
    }
    int32_t *grid = muse_apps_alloc(sizeof(cols) + sizeof(rws));   /* LVGL keeps pointers to them */
    if (!grid) {
        return GADGET_ERR_NO_MEMORY;
    }
    memcpy(grid, cols, sizeof(cols));
    memcpy(grid + MAX_GRID + 1, rws, sizeof(rws));
    /* Children placed for more tracks than these go to the first cell. */
    for (uint32_t i = 0; i < lv_obj_get_child_count(s->obj); i++) {
        lv_obj_t *child = lv_obj_get_child(s->obj, (int32_t)i);
        if (!cell_fits(child, grid)) {
            first_cell(child);
        }
    }
    lv_obj_set_grid_dsc_array(s->obj, grid, grid + MAX_GRID + 1);
    lv_obj_set_layout(s->obj, LV_LAYOUT_GRID);
    muse_apps_free(s->grid);
    s->grid = grid;
    return 0;
}

int32_t gadget_ui_grid_cell(wasm_exec_env_t env, int32_t obj, int32_t col, int32_t col_span, int32_t row,
                            int32_t row_span)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    if (is_root(s)) {
        return GADGET_ERR_ARG;
    }
    /* Within the parent's tracks if it has a grid; within any grid's
     * otherwise, and fitted to the parent's when it gets one (ui_grid). */
    const slot_t *p = slot_of(lv_obj_get_parent(s->obj));
    uint32_t cols = p && p->grid ? grid_tracks(p->grid, false) : MAX_GRID;
    uint32_t rws = p && p->grid ? grid_tracks(p->grid, true) : MAX_GRID;
    if (!track_fits(col, col_span, cols) || !track_fits(row, row_span, rws)) {
        return GADGET_ERR_ARG;
    }
    lv_obj_set_grid_cell(s->obj, LV_GRID_ALIGN_STRETCH, col, col_span, LV_GRID_ALIGN_STRETCH, row, row_span);
    return 0;
}

int32_t gadget_ui_scroll_to(wasm_exec_env_t env, int32_t obj, int32_t x, int32_t y, int32_t animate)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    lv_obj_scroll_to(s->obj, clamp(x, 0, COORD_LIMIT - 1), clamp(y, 0, COORD_LIMIT - 1),
                     animate ? LV_ANIM_ON : LV_ANIM_OFF);
    return 0;
}

int32_t gadget_ui_scroll_to_view(wasm_exec_env_t env, int32_t obj, int32_t animate)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    lv_obj_scroll_to_view_recursive(s->obj, animate ? LV_ANIM_ON : LV_ANIM_OFF);
    return 0;
}

int32_t gadget_ui_text_width(wasm_exec_env_t env, int32_t font, const char *text)
{
    (void)env;
    muse_apps_ui_lock();
    const lv_font_t *f = muse_apps_ui_font(font);
    if (!f || strlen(text) > MAX_TEXT) {
        return GADGET_ERR_ARG;
    }
    lv_point_t size;
    lv_text_get_size(&size, text, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

int32_t gadget_ui_text_height(wasm_exec_env_t env, int32_t font, int32_t width, const char *text)
{
    (void)env;
    muse_apps_ui_lock();
    const lv_font_t *f = muse_apps_ui_font(font);
    if (!f || width < 1 || strlen(text) > MAX_TEXT) {
        return GADGET_ERR_ARG;
    }
    lv_point_t size;
    lv_text_get_size(&size, text, f, 0, 0, clamp(width, 1, COORD_LIMIT - 1), LV_TEXT_FLAG_NONE);
    return size.y;
}

int32_t gadget_ui_span(wasm_exec_env_t env, int32_t y, int32_t margin)
{
    (void)env;
    int w, h;
    bool round;
    muse_app_view_size(&w, &h, &round);
    if (y < 0 || y >= h) {
        return 0;
    }
    float half = w / 2.0f;
    if (round) {
        float r = (w < h ? w : h) / 2.0f, dy = y + 0.5f - h / 2.0f;
        half = dy * dy < r * r ? sqrtf(r * r - dy * dy) : 0;
    }
    int32_t span = 2 * (int32_t)(half - clamp(margin, 0, COORD_LIMIT));
    return span > 0 ? span : 0;
}

int32_t gadget_ui_fit_text(wasm_exec_env_t env, int32_t obj, int32_t width, int32_t font)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    if (s->type != GADGET_W_LABEL) {
        return GADGET_ERR_UNSUPPORTED;
    }
    if (font == GADGET_FONT_DEFAULT) {
        font = GADGET_FONT_14;
    }
    if (!muse_apps_ui_font(font) || width < 1) {
        return GADGET_ERR_ARG;
    }
    width = clamp(width, 1, COORD_LIMIT - 1);
    /* The fonts get smaller with their numbers: the largest that fits. */
    const char *text = lv_label_get_text(s->obj);
    for (; font > GADGET_FONT_14; font--) {
        const lv_font_t *f = muse_apps_ui_font(font);
        if (!f) {
            continue;
        }
        lv_point_t size;
        lv_text_get_size(&size, text ? text : "", f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= width) {
            break;
        }
    }
    /* Dots need a fixed size: one line of the font, else LVGL wraps it. */
    const lv_font_t *f = muse_apps_ui_font(font);
    lv_obj_set_style_text_font(s->obj, f, 0);
    lv_label_set_long_mode(s->obj, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s->obj, width, lv_font_get_line_height(f));
    return font;
}

/* ---- Widget extras ------------------------------------------------------------------------------- */

static int32_t read_i32(const void *p, size_t i)
{
    int32_t v;
    memcpy(&v, (const uint8_t *)p + i * 4, 4);   /* the app's memory needn't be aligned */
    return v;
}

int32_t gadget_ui_line_points(wasm_exec_env_t env, int32_t obj, const void *xy, uint32_t xy_len)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s || s->type != GADGET_W_LINE) {
        return GADGET_ERR_HANDLE;
    }
    if (xy_len % 8) {
        return GADGET_ERR_ARG;
    }
    uint32_t n = xy_len / 8;
    if (n > MAX_LINE_POINTS) {
        return GADGET_ERR_TOO_LARGE;
    }
    lv_point_precise_t *pts = NULL;
    if (n) {
        pts = muse_apps_alloc(n * sizeof(lv_point_precise_t));   /* LVGL keeps the pointer */
        if (!pts) {
            return GADGET_ERR_NO_MEMORY;
        }
        for (uint32_t i = 0; i < n; i++) {
            pts[i].x = clamp(read_i32(xy, i * 2), -32767, 32767);
            pts[i].y = clamp(read_i32(xy, i * 2 + 1), -32767, 32767);
        }
    }
    lv_line_set_points(s->obj, pts, n);
    muse_apps_free(s->ext);
    s->ext = pts;
    return 0;
}

static slot_t *chart_arg(int32_t chart)
{
    slot_t *s = obj_arg(chart);
    return s && s->type == GADGET_W_CHART ? s : NULL;
}

int32_t gadget_ui_chart_series(wasm_exec_env_t env, int32_t chart, int32_t color)
{
    (void)env;
    slot_t *s = chart_arg(chart);
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    chart_t *c = chart_ext(s);
    if (!c) {
        return GADGET_ERR_NO_MEMORY;
    }
    if (c->n >= MAX_SERIES) {
        return GADGET_ERR_LIMIT;
    }
    size_t bytes = lv_chart_get_point_count(s->obj) * sizeof(int32_t) + 32;
    if (!muse_apps_charge(bytes)) {
        return GADGET_ERR_NO_MEMORY;
    }
    s->charged += bytes;
    lv_chart_series_t *ser = lv_chart_add_series(s->obj, lv_color_hex((uint32_t)color & 0xFFFFFF),
                                                 LV_CHART_AXIS_PRIMARY_Y);
    if (!ser) {
        return GADGET_ERR_NO_MEMORY;
    }
    c->ser[c->n++] = ser;
    return c->n;
}

static lv_chart_series_t *series_of(slot_t *s, int32_t series)
{
    chart_t *c = s->ext;
    return c && series >= 1 && series <= c->n ? c->ser[series - 1] : NULL;
}

int32_t gadget_ui_chart_set(wasm_exec_env_t env, int32_t chart, int32_t series, const void *values,
                            uint32_t values_len)
{
    (void)env;
    slot_t *s = chart_arg(chart);
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    lv_chart_series_t *ser = series_of(s, series);
    uint32_t count = lv_chart_get_point_count(s->obj), n = values_len / 4;
    if (!ser || values_len % 4 || n > count) {
        return GADGET_ERR_ARG;
    }
    int32_t *y = lv_chart_get_series_y_array(s->obj, ser);
    for (uint32_t i = 0; i < count; i++) {
        y[i] = i < n ? read_i32(values, i) : LV_CHART_POINT_NONE;
    }
    lv_chart_set_x_start_point(s->obj, ser, 0);
    lv_chart_refresh(s->obj);
    return 0;
}

int32_t gadget_ui_chart_push(wasm_exec_env_t env, int32_t chart, int32_t series, int32_t value)
{
    (void)env;
    slot_t *s = chart_arg(chart);
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    lv_chart_series_t *ser = series_of(s, series);
    if (!ser) {
        return GADGET_ERR_ARG;
    }
    lv_chart_set_next_value(s->obj, ser, value);
    return 0;
}

int32_t gadget_ui_table_cell(wasm_exec_env_t env, int32_t table, int32_t row, int32_t col, const char *text)
{
    (void)env;
    slot_t *s = obj_arg(table);
    if (!s || s->type != GADGET_W_TABLE) {
        return GADGET_ERR_HANDLE;
    }
    size_t len = strlen(text);
    if (row < 0 || row >= MAX_TABLE_ROWS || col < 0 || col >= MAX_TABLE_COLS || len > MAX_TEXT) {
        return GADGET_ERR_ARG;
    }
    const char *old = (uint32_t)row < lv_table_get_row_count(s->obj) && (uint32_t)col < lv_table_get_column_count(s->obj)
                          ? lv_table_get_cell_value(s->obj, (uint32_t)row, (uint32_t)col)
                          : NULL;
    if (!charge_delta(s, old ? strlen(old) + 8 : 0, len + 8)) {
        return GADGET_ERR_NO_MEMORY;
    }
    lv_table_set_cell_value(s->obj, (uint32_t)row, (uint32_t)col, text);
    return 0;
}

int32_t gadget_ui_table_column_width(wasm_exec_env_t env, int32_t table, int32_t col, int32_t width)
{
    (void)env;
    slot_t *s = obj_arg(table);
    if (!s || s->type != GADGET_W_TABLE) {
        return GADGET_ERR_HANDLE;
    }
    if (col < 0 || col >= MAX_TABLE_COLS || width < 0) {
        return GADGET_ERR_ARG;
    }
    if ((uint32_t)col >= lv_table_get_column_count(s->obj)) {
        lv_table_set_column_count(s->obj, (uint32_t)col + 1);
    }
    lv_table_set_column_width(s->obj, (uint32_t)col, clamp(width, 0, 32767));
    return 0;
}

int32_t gadget_ui_scale_needle(wasm_exec_env_t env, int32_t scale, int32_t line, int32_t length, int32_t value)
{
    (void)env;
    slot_t *s = obj_arg(scale), *l = find(line);
    if (!s || s->type != GADGET_W_SCALE || !l || l->type != GADGET_W_LINE) {
        return GADGET_ERR_HANDLE;
    }
    /* One scale's needle at a time: deleting a scale makes LVGL drop every
     * scale's deletion hook from the line, so another scale would keep it
     * after it's freed. And LVGL divides by the range to place it. */
    int32_t h = handle_of(index_of(s));
    if (l->needle_of && l->needle_of != h && find(l->needle_of)) {
        return GADGET_ERR_BUSY;
    }
    if (!range_ok(lv_scale_get_range_min_value(s->obj), lv_scale_get_range_max_value(s->obj))) {
        return GADGET_ERR_ARG;
    }
    l->needle_of = h;
    lv_scale_set_line_needle_value(s->obj, l->obj, clamp(length, -32767, 32767), value);
    return 0;
}

/* ---- Styles --------------------------------------------------------------------------------------- */

/* Room on the object for another of the app's styles: a shared one, or a
 * local selector it doesn't have yet (ui_style). The theme's don't count. */
static bool style_room(lv_obj_t *o, bool local, lv_style_selector_t sel)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < o->style_cnt; i++) {
        const lv_obj_style_t *st = &o->styles[i];
        if (local && st->is_local && st->selector == sel) {
            return true;
        }
        n += !st->is_theme && !st->is_trans;
    }
    return n < STYLES_PER_OBJECT;
}

int32_t gadget_ui_style(wasm_exec_env_t env, int32_t obj, int32_t selector, int32_t prop, int32_t value)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    lv_style_selector_t sel;
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    if (!selector_in(selector, &sel)) {
        return GADGET_ERR_ARG;
    }
    if (!style_room(s->obj, true, sel)) {
        return GADGET_ERR_LIMIT;
    }
    return style_set(s->obj, NULL, sel, prop, value);
}

int32_t gadget_ui_style_create(wasm_exec_env_t env)
{
    (void)env;
    muse_apps_ui_lock();
    if (!s_ui) {
        return GADGET_ERR_NO_MEMORY;
    }
    if (s_ui->n_styles >= MAX_STYLES) {
        return GADGET_ERR_LIMIT;
    }
    lv_style_t *st = muse_apps_alloc(sizeof(lv_style_t));
    if (!st || !muse_apps_charge(STYLE_COST)) {
        muse_apps_free(st);
        return GADGET_ERR_NO_MEMORY;
    }
    lv_style_init(st);
    s_ui->styles[s_ui->n_styles++] = st;
    return s_ui->n_styles;
}

static lv_style_t *style_arg(int32_t style)
{
    muse_apps_ui_lock();
    return s_ui && style >= 1 && style <= s_ui->n_styles ? s_ui->styles[style - 1] : NULL;
}

const lv_style_t *muse_apps_ui_style(int32_t handle)
{
    return style_arg(handle);
}

int32_t gadget_ui_style_set(wasm_exec_env_t env, int32_t style, int32_t prop, int32_t value)
{
    (void)env;
    lv_style_t *st = style_arg(style);
    if (!st) {
        return GADGET_ERR_HANDLE;
    }
    int32_t err = style_set(NULL, st, 0, prop, value);
    if (!err) {
        lv_obj_report_style_change(st);
    }
    return err;
}

int32_t gadget_ui_style_add(wasm_exec_env_t env, int32_t obj, int32_t style, int32_t selector)
{
    (void)env;
    lv_style_t *st = style_arg(style);
    slot_t *s = find(obj);
    lv_style_selector_t sel;
    if (!st || !s) {
        return GADGET_ERR_HANDLE;
    }
    if (!selector_in(selector, &sel)) {
        return GADGET_ERR_ARG;
    }
    if (!style_room(s->obj, false, sel)) {
        return GADGET_ERR_LIMIT;
    }
    lv_obj_add_style(s->obj, st, sel);
    return 0;
}

/* ---- Animation natives -------------------------------------------------------------------------------- */

int32_t gadget_ui_animate(wasm_exec_env_t env, int32_t obj, int32_t prop, int32_t to, int32_t ms, int32_t easing)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    int info = prop_info(prop), type = info & 0xFF;
    bool color = type == PT_rgb;
    bool number = type == PT_coord || type == PT_int || type == PT_opa || type == PT_angle || type == PT_percent;
    int ease = easing & 0xFF, flags = easing & ~0xFF;
    if ((info & PA_rw) != PA_rw || !(color || number) || ease > GADGET_EASE_STEP ||
        (flags & ~(GADGET_ANIM_REPEAT | GADGET_ANIM_REVERSE)) || ms < 0 || ms > 600000 ||
        (s->type == GADGET_W_CANVAS && (prop == GADGET_P_WIDTH || prop == GADGET_P_HEIGHT))) {
        return GADGET_ERR_ARG;
    }
    int32_t from;
    int32_t err = get_prop(s, prop, &from);
    if (err) {
        return err;
    }
    if (type == PT_coord && (coord_special(from) || coord_special(to))) {
        return GADGET_ERR_ARG;   /* between pixels only */
    }
    int idx = index_of(s);
    anim_stop(idx, prop);
    if (ms == 0) {
        err = set_prop(s, prop, to);
        if (!err && (s->events & GADGET_UI_ANIM_DONE)) {
            muse_apps_post(GADGET_EV_UI, handle_of(idx), GADGET_UI_ANIM_DONE);
        }
        return err;
    }
    anim_t *r = NULL;
    for (int i = 0; i < MAX_ANIMS && !r; i++) {
        r = s_ui->anims[i].used ? NULL : &s_ui->anims[i];
    }
    if (!r) {
        return GADGET_ERR_LIMIT;
    }
    *r = (anim_t){.used = true, .color = color, .slot = (uint16_t)idx, .prop = (uint16_t)prop, .from = from, .to = to};
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, r);   /* one var per animation, so each stops alone */
    lv_anim_set_user_data(&a, r);
    lv_anim_set_custom_exec_cb(&a, anim_exec);
    lv_anim_set_values(&a, 0, ANIM_STEPS);
    lv_anim_set_duration(&a, (uint32_t)ms);
    lv_anim_set_path_cb(&a, s_paths[ease]);
    if (flags & GADGET_ANIM_REVERSE) {
        lv_anim_set_reverse_duration(&a, (uint32_t)ms);
    }
    if (flags & GADGET_ANIM_REPEAT) {
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    }
    lv_anim_set_completed_cb(&a, anim_completed);
    lv_anim_set_deleted_cb(&a, anim_deleted);
    if (!lv_anim_start(&a)) {
        r->used = false;
        return GADGET_ERR_NO_MEMORY;
    }
    return 0;
}

int32_t gadget_ui_animate_stop(wasm_exec_env_t env, int32_t obj, int32_t prop)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s) {
        return GADGET_ERR_HANDLE;
    }
    anim_stop(index_of(s), prop);
    return 0;
}

/* ---- Image natives and the loader ----------------------------------------------------------------- */

int32_t gadget_ui_image_set(wasm_exec_env_t env, int32_t obj, const void *data, uint32_t data_len)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s || s->type != GADGET_W_IMAGE) {
        return GADGET_ERR_HANDLE;
    }
    image_t *img;
    int32_t err = decode(data, data_len, image_room(s), &img);
    if (!err) {
        err = image_attach(s, img);
        if (err) {
            heap_caps_free(img);
        }
    }
    return err;
}

typedef struct {
    int32_t handle;
    uint32_t session;
    bool gadget;   /* path is the gadget's own file's name, not the VM's */
    char path[192];
} load_t;

/* Its own short-lived thread: reads the file from the VM (which blocks),
 * decodes it, then shows it if the app and the object are still there. */
static void *loader(void *arg)
{
    load_t *job = arg;
    uint8_t *file = NULL;
    size_t len = 0;
    char err[96];
    image_t *img = NULL;
    int32_t result = GADGET_ERR_FAILED;
    bool read = false;
    if (job->gadget) {   /* the gadget's own file: from flash, offline too */
        int32_t e = muse_apps_store_file_load(job->path, MAX_IMAGE_FILE, &file, &len);
        read = e == 0;
        snprintf(err, sizeof(err), "%s", e == GADGET_ERR_NOT_FOUND ? "no such file on the gadget" : "couldn't read it");
    } else {
        read = muse_apps_vm_file_read(job->path, MAX_IMAGE_FILE, LOAD_TIMEOUT_MS, &file, &len, err, sizeof(err));
    }
    if (read) {
        uint32_t room = 0;
        muse_app_view_lock();
        slot_t *s = s_session == job->session ? find(job->handle) : NULL;
        room = s && s->type == GADGET_W_IMAGE ? image_room(s) : 0;
        muse_app_view_unlock();
        result = room ? decode(file, len, room, &img) : GADGET_ERR_HANDLE;
        free(file);
        if (result) {
            ESP_LOGW(TAG, "%s: not a PNG or JPEG this gadget can show (%d)", job->path, (int)result);
        }
    } else {
        ESP_LOGW(TAG, "%s: %s", job->path, err);
    }
    muse_app_view_lock();
    if (s_session == job->session) {
        slot_t *s = find(job->handle);
        if (img && s && s->type == GADGET_W_IMAGE && image_attach(s, img) == 0) {
            img = NULL;
            result = 0;
        } else if (!result) {
            result = GADGET_ERR_NO_MEMORY;
        }
        muse_apps_post(GADGET_EV_UI, job->handle, result ? GADGET_UI_LOAD_FAILED : GADGET_UI_LOADED);
    }
    muse_app_view_unlock();
    heap_caps_free(img);
    heap_caps_free(job);
    atomic_fetch_sub(&s_loaders, 1);
    return NULL;
}

int32_t gadget_ui_image_load(wasm_exec_env_t env, int32_t obj, const char *path)
{
    (void)env;
    slot_t *s = obj_arg(obj);
    if (!s || s->type != GADGET_W_IMAGE) {
        return GADGET_ERR_HANDLE;
    }
    load_t *job = heap_caps_calloc(1, sizeof(load_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job) {
        return GADGET_ERR_NO_MEMORY;
    }
    job->gadget = path && path[0] == '/';
    int32_t err = job->gadget ? muse_apps_gadget_path(path, job->path, sizeof(job->path))
                              : muse_apps_vm_path(path, job->path, sizeof(job->path));
    if (!err && !job->gadget && muse_link_state() != MUSE_LINK_ONLINE) {
        err = GADGET_ERR_OFFLINE;
    }
    if (!err && atomic_fetch_add(&s_loaders, 1) >= MAX_LOADS) {
        atomic_fetch_sub(&s_loaders, 1);
        err = GADGET_ERR_LIMIT;
    }
    if (err) {
        heap_caps_free(job);
        return err;
    }
    job->handle = obj;
    job->session = s_session;
    /* A pthread with its stack in PSRAM, as the app task's. The config is
     * this thread's own: put back whatever it was. */
    esp_pthread_cfg_t old, cfg = esp_pthread_get_default_config();
    bool had = esp_pthread_get_cfg(&old) == ESP_OK;
    cfg.stack_size = LOADER_STACK;
    cfg.prio = 2;
    cfg.thread_name = "app_image";
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_pthread_set_cfg(&cfg);
    pthread_t t;
    bool started = pthread_create(&t, NULL, loader, job) == 0;
    if (started) {
        pthread_detach(t);
    }
    if (!had) {
        old = esp_pthread_get_default_config();
    }
    esp_pthread_set_cfg(&old);
    if (!started) {
        atomic_fetch_sub(&s_loaders, 1);
        heap_caps_free(job);
        return GADGET_ERR_NO_MEMORY;
    }
    return 0;
}

/* ---- Navigation without touch ------------------------------------------------------------------ */

/* The view's buttons (input task) to the keypad (LVGL's task): a small ring. */
enum { NAV_PREV = 1, NAV_NEXT, NAV_ENTER, NAV_ESC };
#define NAV_RING 16
static uint8_t s_nav_keys[NAV_RING];
static atomic_uint s_nav_head, s_nav_tail;
static uint32_t s_nav_release;   /* LVGL's task: the key to let go of next */

static bool nav_button(int button)
{
    uint8_t k;
    switch (button) {
    case MUSE_APP_BUTTON_UP:
    case MUSE_APP_BUTTON_LEFT: k = NAV_PREV; break;
    case MUSE_APP_BUTTON_DOWN:
    case MUSE_APP_BUTTON_RIGHT: k = NAV_NEXT; break;
    case MUSE_APP_BUTTON_ENTER: k = NAV_ENTER; break;
    case MUSE_APP_BUTTON_BACK: k = NAV_ESC; break;
    default: return false;
    }
    unsigned h = atomic_load(&s_nav_head);
    if (h - atomic_load(&s_nav_tail) < NAV_RING) {   /* full: drop it */
        s_nav_keys[h % NAV_RING] = k;
        atomic_store(&s_nav_head, h + 1);
    }
    return true;
}

/* An encoder: UP and DOWN step focus (or the value being edited), ENTER
 * presses or starts editing, BACK is LV_KEY_ESC (UI_CANCEL). */
static void nav_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->enc_diff = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    if (s_nav_release) {
        data->key = s_nav_release;
        s_nav_release = 0;
    } else {
        unsigned t = atomic_load(&s_nav_tail);
        if (t != atomic_load(&s_nav_head)) {
            uint8_t k = s_nav_keys[t % NAV_RING];
            atomic_store(&s_nav_tail, t + 1);
            if (k == NAV_PREV || k == NAV_NEXT) {
                data->enc_diff = k == NAV_PREV ? -1 : 1;
            } else {
                data->key = s_nav_release = k == NAV_ENTER ? LV_KEY_ENTER : LV_KEY_ESC;
                data->state = LV_INDEV_STATE_PRESSED;
            }
        }
    }
    data->continue_reading = s_nav_release || atomic_load(&s_nav_tail) != atomic_load(&s_nav_head);
}

static void nav_off(void)
{
    muse_app_view_set_nav(NULL);
    if (s_ui->indev) {
        lv_indev_delete(s_ui->indev);
        s_ui->indev = NULL;
    }
    if (s_ui->group) {
        lv_group_delete(s_ui->group);
        s_ui->group = NULL;
    }
}

int32_t gadget_ui_navigation(wasm_exec_env_t env, int32_t on)
{
    (void)env;
    muse_apps_ui_lock();
    if (!s_ui) {
        return GADGET_ERR_NO_MEMORY;
    }
    if (!on) {
        nav_off();
        return 0;
    }
    if (!(muse_app_view_buttons_present() & (1u << MUSE_APP_BUTTON_ENTER))) {
        return GADGET_ERR_UNSUPPORTED;   /* no keys to navigate with */
    }
    if (s_ui->group) {
        return 0;
    }
    s_ui->group = lv_group_create();
    s_ui->indev = lv_indev_create();
    if (!s_ui->group || !s_ui->indev) {
        nav_off();
        return GADGET_ERR_NO_MEMORY;
    }
    lv_indev_set_type(s_ui->indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(s_ui->indev, nav_read);
    lv_indev_set_group(s_ui->indev, s_ui->group);
    for (int i = 1; i < MAX_OBJECTS; i++) {
        if (s_ui->slots[i].obj && focusable(s_ui->slots[i].type)) {
            lv_group_add_obj(s_ui->group, s_ui->slots[i].obj);
        }
    }
    s_nav_release = 0;
    atomic_store(&s_nav_tail, atomic_load(&s_nav_head));
    muse_app_view_set_nav(nav_button);
    return 0;
}

/* ---- The app starts and stops (the app task) -------------------------------------------------------- */

void muse_apps_ui_start(void)
{
    ui_t *ui = muse_apps_calloc(1, sizeof(ui_t));
    muse_app_view_lock();
    s_session++;
    s_ui = ui;
    lv_obj_t *root = muse_app_view_root();
    if (ui && root) {
        int w, h;
        bool round;
        muse_app_view_size(&w, &h, &round);
        ui->pixel_limit = (uint32_t)(w * h);   /* limits.image_pixels: the screen's own count */
        ui->slots[0].obj = root;
        ui->slots[0].type = GADGET_W_OBJ;
        /* FONT_DEFAULT is what text gets unless the app picks another. */
        lv_obj_set_style_text_font(root, muse_apps_ui_font(GADGET_FONT_DEFAULT), 0);
    } else {
        ESP_LOGW(TAG, "no memory for the app's objects");
    }
    muse_app_view_unlock();
}

void muse_apps_ui_stop(void)
{
    muse_app_view_lock();
    if (s_ui) {
        nav_off();
        for (int i = 0; i < MAX_ANIMS; i++) {
            if (s_ui->anims[i].used) {
                lv_anim_delete(&s_ui->anims[i], NULL);
                s_ui->anims[i].used = false;
            }
        }
        lv_obj_t *root = s_ui->slots[0].obj;
        if (root) {
            lv_obj_remove_event_cb(root, on_ui_event);
            lv_obj_clean(root);   /* every object the app made: on_delete frees what each held */
            for (int i = 0; i < s_ui->n_styles; i++) {
                lv_obj_remove_style(root, s_ui->styles[i], LV_PART_ANY | LV_STATE_ANY);
            }
            if (s_ui->slots[0].grid) {
                lv_obj_set_layout(root, LV_LAYOUT_NONE);
                lv_obj_set_grid_dsc_array(root, NULL, NULL);
            }
        }
        muse_apps_free(s_ui->slots[0].grid);
        muse_apps_uncharge(s_ui->slots[0].charged);
        for (int i = 0; i < s_ui->n_styles; i++) {
            lv_style_reset(s_ui->styles[i]);
            muse_apps_free(s_ui->styles[i]);
            muse_apps_uncharge(STYLE_COST);
        }
        if (s_ui->n_objects) {
            ESP_LOGW(TAG, "%d objects outlived the app's root", s_ui->n_objects);
        }
        muse_apps_free(s_ui);
        s_ui = NULL;
    }
    s_session++;   /* image loads still running find the app gone */
    muse_app_view_unlock();
}
