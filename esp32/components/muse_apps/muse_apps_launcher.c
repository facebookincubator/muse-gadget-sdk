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
 * The launcher: the installed apps as a list, like the menu, on the app view
 * (muse_app_view.h). It's LVGL objects sized from the screen (a round one
 * keeps the list in the square inside the circle), so it works on every board
 * with the full UI. Apps in Settings (touch boards) or in the menu (button
 * boards) opens it. Each row is an app's glyph on its colour
 * (muse_apps_icons.h) and its name; the last row closes the launcher.
 *
 * Touch: scroll, tap a row to open it, hold one to remove it (after asking).
 * Buttons: the second button moves down, the talk button opens on release,
 * and on boards with a second button holding the talk button asks to remove;
 * on a touch screen the first press only shows the highlight.
 * Keyboards: the arrows, Enter and Esc. The power key, or the exit button
 * held 2 s (muse_app_view.c), closes it as it closes an app, and an app
 * opened here comes back here.
 *
 * LVGL's callbacks only post to the app task (muse_apps_post); the rest runs
 * there, under LVGL's lock.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "muse_app_view.h"
#include "muse_apps_icons.h"
#include "muse_apps_priv.h"
#include "muse_board.h"
#include "muse_text.h"

#define NOTE_MS 3500
#define GLYPH 0.56f   /* of the disc */

/* What LVGL's callbacks post, beside the view's own events (muse_app_view.h). */
#define EV_ROW_TAP 100   /* a: the row */
#define EV_ROW_HOLD 101  /* a: the row */
#define EV_ANSWER 102    /* a: 1 remove, 0 keep */

#define COLOR_TEXT 0xF2F2F7
#define COLOR_DIM 0x8E8E93
#define COLOR_CHIP 0x2C2C2E    /* the selected row, Cancel, the Close row's disc */
#define COLOR_DANGER 0xE5484D

#if LV_FONT_MONTSERRAT_12
#define FONT_SMALL (&lv_font_montserrat_12)
#else
#define FONT_SMALL (&lv_font_montserrat_14)
#endif
#if LV_FONT_MONTSERRAT_28
#define FONT_LARGE (&lv_font_montserrat_28)
#else
#define FONT_LARGE (&lv_font_montserrat_20)
#endif

static const muse_apps_origin_t s_origin = {.from = MUSE_APPS_FROM_LAUNCHER};

typedef enum { VIEW_LIST, VIEW_ASK } view_t;

typedef struct {
    const int16_t *glyph;  /* NULL: the initial, a label in the disc */
    uint32_t disc, ink;
} look_t;

static EXT_RAM_BSS_ATTR muse_apps_entry_t s_apps[MUSE_APPS_MAX];
static EXT_RAM_BSS_ATTR look_t s_looks[MUSE_APPS_MAX + 1];      /* and the Close row's */
static EXT_RAM_BSS_ATTR lv_obj_t *s_rows[MUSE_APPS_MAX + 1];
static int s_count;          /* apps listed; the Close row is row s_count */
static int s_sel;            /* the selected row */
static bool s_show_sel;      /* highlighted: always without touch, else after a button */
static view_t s_view;
static int s_down_view = -1; /* where the talk button went down: its release acts only there */
static bool s_held;          /* its long press asked to remove: the release does nothing */
static char s_ask_id[MUSE_APPS_ID_MAX];
static char s_last_id[MUSE_APPS_ID_MAX];   /* the app opened last: selected when the launcher comes back */
static uint32_t s_note_until;   /* ms; 0: no note */

/* Sizes, from the screen (layout()). */
static struct {
    int w, h, inset, row_h, disc, pad;
    bool small, round, touch;
    const lv_font_t *font;
} L;

static lv_obj_t *s_screen, *s_title, *s_list, *s_note, *s_ask, *s_ask_name, *s_hint_down, *s_hint_select;

/* ---- Glyphs, drawn into a row's disc ---- */

static void stroke_rect(lv_layer_t *layer, int x, int y, int w, int h, int radius, uint32_t rgb)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(rgb);
    d.bg_opa = LV_OPA_COVER;
    d.radius = radius;
    lv_area_t a = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(layer, &d, &a);
}

/* An arc clockwise from a0 to a1, degrees from 12 o'clock, as the glyphs give them. */
static void stroke_arc(lv_layer_t *layer, int cx, int cy, int r, int width, int a0, int a1, uint32_t rgb,
                       bool rounded)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    int start = ((a0 - 90) % 360 + 360) % 360;
    int span = a1 - a0 >= 360 ? 360 : ((a1 - a0) % 360 + 360) % 360;
    d.color = lv_color_hex(rgb);
    d.center.x = cx;
    d.center.y = cy;
    d.radius = r;
    d.width = width;
    d.start_angle = start;
    d.end_angle = start + span;
    d.rounded = rounded;
    lv_draw_arc(layer, &d);
}

static void stroke_line(lv_layer_t *layer, int x0, int y0, int x1, int y1, int width, uint32_t rgb)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(rgb);
    d.p1.x = x0;
    d.p1.y = y0;
    d.p2.x = x1;
    d.p2.y = y1;
    d.width = width;
    d.round_start = 1;
    d.round_end = 1;
    lv_draw_line(layer, &d);
}

static int iround(float v)
{
    return (int)(v < 0 ? v - 0.5f : v + 0.5f);
}

static void draw_glyph(lv_layer_t *layer, const int16_t *g, float cx, float cy, float d, uint32_t ink, uint32_t disc)
{
    float box = d * GLYPH, s = box / 240, ox = cx - box / 2, oy = cy - box / 2;
    int sw = iround(20 * s) > 2 ? iround(20 * s) : 2;
#define X(v) iround(ox + (v) * s)
#define Y(v) iround(oy + (v) * s)
#define LEN(v) iround((v) * s)
    for (;;) {
        switch (*g) {
        case G_LINE: {
            int n = g[1];
            for (int k = 0; k + 1 < n; k++) {
                const int16_t *p = g + 2 + 2 * k;
                stroke_line(layer, X(p[0]), Y(p[1]), X(p[2]), Y(p[3]), sw, ink);
            }
            g += 2 + 2 * n;
            break;
        }
        case G_RING:
            stroke_arc(layer, X(g[1]), Y(g[2]), LEN(g[3]) + sw / 2, sw, 0, 360, ink, false);
            g += 4;
            break;
        case G_ARC:
            stroke_arc(layer, X(g[1]), Y(g[2]), LEN(g[3]) + sw / 2, sw, g[4], g[5], ink, true);
            g += 6;
            break;
        case G_DOT: {
            int r = LEN(g[3]) > 1 ? LEN(g[3]) : 1;
            stroke_rect(layer, X(g[1]) - r, Y(g[2]) - r, 2 * r, 2 * r, r, ink);
            g += 4;
            break;
        }
        case G_BOX: {  /* ink, then the disc inside it */
            int x0 = X(g[1] - 10), y0 = Y(g[2] - 10), x1 = X(g[1] + g[3] + 10), y1 = Y(g[2] + g[4] + 10);
            stroke_rect(layer, x0, y0, x1 - x0, y1 - y0, LEN(g[5] + 10), ink);
            stroke_rect(layer, x0 + sw, y0 + sw, x1 - x0 - 2 * sw, y1 - y0 - 2 * sw, LEN(g[5] - 10), disc);
            g += 6;
            break;
        }
        case G_BLOCK:
            stroke_rect(layer, X(g[1]), Y(g[2]), LEN(g[3]), LEN(g[4]), LEN(g[5]), ink);
            g += 6;
            break;
        default:
            return;
        }
    }
#undef X
#undef Y
#undef LEN
}

static void on_disc_draw(lv_event_t *e)
{
    const look_t *look = lv_event_get_user_data(e);
    if (!look || !look->glyph) {
        return;
    }
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    float d = (float)lv_area_get_width(&a);
    draw_glyph(lv_event_get_layer(e), look->glyph, a.x1 + d / 2, a.y1 + d / 2, d, look->ink, look->disc);
}

/* ---- Building it ---- */

static void on_row(lv_event_t *e)
{
    int row = (int)(intptr_t)lv_event_get_user_data(e);
    muse_apps_post(lv_event_get_code(e) == LV_EVENT_LONG_PRESSED ? EV_ROW_HOLD : EV_ROW_TAP, row, 0);
}

static void on_answer(lv_event_t *e)
{
    muse_apps_post(EV_ANSWER, (int)(intptr_t)lv_event_get_user_data(e), 0);
}

static void on_deleted(lv_event_t *e)
{
    (void)e;   /* with the view's root: everything below it went too */
    s_screen = s_title = s_list = s_note = s_ask = s_ask_name = s_hint_down = s_hint_select = NULL;
    memset(s_rows, 0, sizeof(s_rows));
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t rgb, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_label_set_text(l, text);
    return l;
}

/* A name as the fonts can show it (muse_text.h). */
static void shown_name(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s", name);
    muse_text_to_ascii(out, cap);
}

static void layout(void)
{
    muse_app_view_size(&L.w, &L.h, &L.round);
    int side = L.w < L.h ? L.w : L.h;
    L.small = L.w < 200 || L.h < 200;
    L.touch = muse_board && muse_board->touch;
    L.inset = muse_apps_safe_inset();
    L.font = L.small ? FONT_SMALL : side >= 400 ? FONT_LARGE : &lv_font_montserrat_20;
    L.row_h = L.small ? 20 : side >= 400 ? 64 : 44;
    L.disc = L.row_h - (L.small ? 4 : 14);
    L.pad = L.small ? 2 : side >= 400 ? 12 : 8;
}

static void look_of(const muse_apps_entry_t *app, look_t *look)
{
    int c = muse_apps_color_find(app->color), g = muse_apps_glyph_find(app->icon);
    if (c < 0) {
        c = muse_apps_color_for_id(app->id);
    }
    look->disc = muse_apps_colors[c].disc;
    look->ink = muse_apps_colors[c].ink;
    look->glyph = g >= 0 ? muse_apps_glyphs[g].strokes : NULL;
}

static lv_obj_t *add_row(int i, const char *name, const look_t *look, const char *initial)
{
    lv_obj_t *r = lv_obj_create(s_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), L.row_h);
    lv_obj_set_style_pad_hor(r, L.pad, 0);
    lv_obj_set_style_pad_column(r, L.pad + 2, 0);
    lv_obj_set_style_radius(r, L.small ? 3 : L.row_h / 4, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(COLOR_CHIP), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (L.touch) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, on_row, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        if (i < s_count) {
            lv_obj_add_event_cb(r, on_row, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        }
    } else {
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *disc = lv_obj_create(r);
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, L.disc, L.disc);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(look->disc), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_remove_flag(disc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (look->glyph) {
        lv_obj_add_event_cb(disc, on_disc_draw, LV_EVENT_DRAW_MAIN_END, (void *)look);
    } else {
        lv_obj_center(label(disc, L.small ? FONT_SMALL : &lv_font_montserrat_20, look->ink, initial));
    }
    lv_obj_t *n = label(r, L.font, COLOR_TEXT, name);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(n, 1);
    return r;
}

static void build_rows(void)
{
    lv_obj_clean(s_list);
    memset(s_rows, 0, sizeof(s_rows));
    if (!s_count) {
        lv_obj_t *empty = label(s_list, L.font, COLOR_DIM, "No apps yet.\nAsk Muse to make you one.");
        lv_obj_set_width(empty, lv_pct(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_ver(empty, L.pad * 2, 0);
    }
    for (int i = 0; i < s_count; i++) {
        char name[MUSE_APPS_NAME_MAX + 8], initial[2] = {0};
        shown_name(s_apps[i].name, name, sizeof(name));
        look_of(&s_apps[i], &s_looks[i]);
        initial[0] = name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : name[0];
        s_rows[i] = add_row(i, name, &s_looks[i], initial);
    }
    s_looks[s_count] = (look_t){.glyph = NULL, .disc = COLOR_CHIP, .ink = COLOR_DIM};
    s_rows[s_count] = add_row(s_count, "Close", &s_looks[s_count], LV_SYMBOL_CLOSE);
}

static void show_selection(void)
{
    for (int i = 0; i <= s_count; i++) {
        if (s_rows[i]) {
            lv_obj_set_style_bg_opa(s_rows[i], s_show_sel && i == s_sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }
    if (s_show_sel && s_rows[s_sel]) {
        lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_ON);
    }
}

/* Where on the bottom bar a button's hint goes, as the menu does (muse_menu.c):
 * -1 left, 0 middle, 1 right. */
static int bar_slot(const muse_button_hint_t *icon)
{
    switch (icon->align) {
    case LV_ALIGN_BOTTOM_LEFT:
    case LV_ALIGN_LEFT_MID:
        return -1;
    case LV_ALIGN_BOTTOM_MID:
    case LV_ALIGN_TOP_MID:
        return (icon->x > 0) - (icon->x < 0);
    default:
        return 1;
    }
}

static void align_on_bar(lv_obj_t *l, int slot)
{
    int pad = L.pad;
    lv_obj_align(l, slot < 0 ? LV_ALIGN_BOTTOM_LEFT : slot == 0 ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_BOTTOM_RIGHT,
                 slot < 0 ? 2 * pad : slot == 0 ? 0 : -2 * pad, -pad);
}

static void set_hints(void)
{
    if (!s_hint_down) {
        return;
    }
    bool kb = muse_board->keyboard;
    if (s_view == VIEW_ASK) {
        lv_label_set_text(s_hint_down, kb ? "Esc Cancel" : "Cancel");
        lv_label_set_text(s_hint_select, kb ? "Enter Remove" : "Remove");
        lv_obj_set_style_text_color(s_hint_select, lv_color_hex(COLOR_DANGER), 0);
    } else {
        lv_label_set_text(s_hint_down, kb ? "Esc Close" : LV_SYMBOL_DOWN " Next");
        lv_label_set_text(s_hint_select, kb ? "Enter Open" : s_sel == s_count ? "Close" : "Open");
        lv_obj_set_style_text_color(s_hint_select, lv_color_hex(COLOR_TEXT), 0);
    }
}

static void build_ask(void)
{
    s_ask = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_ask);
    lv_obj_set_size(s_ask, L.w, L.h);
    lv_obj_set_style_bg_color(s_ask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ask, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_ask, L.inset + L.pad, 0);
    lv_obj_set_style_pad_row(s_ask, L.pad * 2, 0);
    lv_obj_set_flex_flow(s_ask, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ask, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_ask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ask, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);   /* takes the taps meant for the list */
    s_ask_name = label(s_ask, L.font, COLOR_TEXT, "");
    lv_obj_set_width(s_ask_name, lv_pct(100));
    lv_obj_set_style_text_align(s_ask_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_ask_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *q = label(s_ask, L.small ? FONT_SMALL : &lv_font_montserrat_14, COLOR_DIM, "Remove from this gadget?");
    lv_obj_set_width(q, lv_pct(100));
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(q, LV_LABEL_LONG_MODE_WRAP);
    if (!L.touch) {
        return;   /* the hints say which button does which */
    }
    lv_obj_t *buttons = lv_obj_create(s_ask);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_size(buttons, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);
    static const char *const names[2] = {"Cancel", "Remove"};
    for (int which = 0; which < 2; which++) {
        lv_obj_t *b = lv_obj_create(buttons);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, L.small ? 52 : L.row_h * 2, L.small ? 20 : L.row_h);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(which ? COLOR_DANGER : COLOR_CHIP), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, on_answer, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)which);
        lv_obj_center(label(b, L.font == FONT_LARGE ? &lv_font_montserrat_20 : L.font, COLOR_TEXT, names[which]));
    }
}

bool muse_apps_launcher_show(void)
{
    layout();
    s_count = muse_apps_store_list(s_apps, MUSE_APPS_MAX);
    s_sel = 0;
    for (int i = 0; i < s_count; i++) {
        if (!strcmp(s_apps[i].id, s_last_id)) {
            s_sel = i;   /* back from it: where it was, highlighted if it was */
        }
    }
    s_show_sel = s_show_sel || !L.touch;
    s_view = VIEW_LIST;
    s_down_view = -1;
    s_held = false;
    s_note_until = 0;
    s_ask_id[0] = '\0';
    muse_app_view_lock();
    lv_obj_t *root = muse_app_view_root();
    if (root) {
        /* Its own black screen over the root, whatever the last app left on it. */
        s_screen = lv_obj_create(root);
        lv_obj_remove_style_all(s_screen);
        lv_obj_set_size(s_screen, L.w, L.h);
        lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_screen, on_deleted, LV_EVENT_DELETE, NULL);

        int hint_h = L.touch ? 0 : (L.small ? 15 : 40);
        int title_h = L.small ? 14 : L.row_h * 3 / 4;
        int top = L.round ? L.inset : title_h, bottom = L.round ? L.inset : hint_h;
        s_title = label(s_screen, L.small ? FONT_SMALL : &lv_font_montserrat_20, COLOR_DIM, "Apps");
        lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, L.round ? (L.inset - lv_font_get_line_height(L.small ? FONT_SMALL : &lv_font_montserrat_20)) / 2 + L.pad : L.pad / 2);

        s_list = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_list);
        lv_obj_set_size(s_list, L.w - 2 * (L.round ? L.inset : 0), L.h - top - bottom);
        lv_obj_set_pos(s_list, L.round ? L.inset : 0, top);
        lv_obj_set_style_pad_hor(s_list, L.round ? 0 : L.pad, 0);
        lv_obj_set_style_pad_row(s_list, L.small ? 0 : 2, 0);
        lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
        if (!L.touch) {
            lv_obj_remove_flag(s_list, LV_OBJ_FLAG_CLICKABLE);
        }
        build_rows();

        /* A note at the bottom: "Removed Timer", "Timer stopped". */
        s_note = label(s_screen, L.small ? FONT_SMALL : &lv_font_montserrat_14, COLOR_TEXT, "");
        lv_obj_set_style_bg_color(s_note, lv_color_hex(COLOR_CHIP), 0);
        lv_obj_set_style_bg_opa(s_note, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_note, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(s_note, L.pad * 2, 0);
        lv_obj_set_style_pad_ver(s_note, L.small ? 1 : L.pad / 2, 0);
        lv_obj_set_style_max_width(s_note, L.w - 2 * (L.inset + L.pad), 0);
        lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(s_note, LV_ALIGN_BOTTOM_MID, 0, -(L.round ? L.inset / 3 : hint_h + L.pad));
        lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);

        build_ask();
        if (!L.touch) {
            const muse_button_hint_t *talk = &muse_board->talk_hint, *aux = &muse_board->aux_hint;
            int select = bar_slot(talk);
            int down = aux->align == LV_ALIGN_RIGHT_MID ? -1 : bar_slot(aux);
            if (down == select) {
                down = select > -1 ? select - 1 : 0;
            }
            s_hint_down = label(s_screen, L.small ? FONT_SMALL : &lv_font_montserrat_20, COLOR_TEXT, "");
            align_on_bar(s_hint_down, down);
            s_hint_select = label(s_screen, L.small ? FONT_SMALL : &lv_font_montserrat_20, COLOR_TEXT, "");
            align_on_bar(s_hint_select, select);
            set_hints();
        }
        show_selection();
    }
    muse_app_view_unlock();
    return s_screen != NULL;
}

void muse_apps_launcher_hide(void)
{
    muse_app_view_lock();
    if (s_screen) {
        lv_obj_delete(s_screen);   /* on_deleted clears the pointers */
    }
    muse_app_view_unlock();
}

/* ---- Notes, under LVGL's lock ---- */

static void note_locked(const char *text)
{
    if (!s_note) {
        return;
    }
    char shown[96];
    shown_name(text, shown, sizeof(shown));
    lv_label_set_text(s_note, shown);
    lv_obj_remove_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_note);
    s_note_until = (uint32_t)(esp_timer_get_time() / 1000) + NOTE_MS;
    if (!s_note_until) {
        s_note_until = 1;
    }
}

void muse_apps_launcher_note(const char *text)
{
    muse_app_view_lock();
    note_locked(text);
    muse_app_view_unlock();
}

/* ---- Acting on it, under LVGL's lock ---- */

static int find(const char *id)
{
    for (int i = 0; id[0] && i < s_count; i++) {
        if (!strcmp(s_apps[i].id, id)) {
            return i;
        }
    }
    return -1;
}

static void select_row(int row)
{
    s_sel = row < 0 ? s_count : row > s_count ? 0 : row;
    s_show_sel = true;
    show_selection();
    set_hints();
}

static void open_row(int row)
{
    if (row >= s_count) {
        muse_apps_launcher_close();
        return;
    }
    s_sel = row;
    snprintf(s_last_id, sizeof(s_last_id), "%s", s_apps[row].id);
    char note[MUSE_APPS_NAME_MAX + 16];
    if (muse_apps_store_open(&s_origin, s_apps[row].id)) {
        snprintf(note, sizeof(note), "Opening %s...", s_apps[row].name);
    } else {
        snprintf(note, sizeof(note), "Busy: try again");
    }
    note_locked(note);
}

static void ask(int row)
{
    if (row < 0 || row >= s_count || !s_ask) {
        return;
    }
    snprintf(s_ask_id, sizeof(s_ask_id), "%s", s_apps[row].id);
    char name[MUSE_APPS_NAME_MAX + 8];
    shown_name(s_apps[row].name, name, sizeof(name));
    lv_label_set_text(s_ask_name, name);
    lv_obj_remove_flag(s_ask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ask);
    s_view = VIEW_ASK;
    s_down_view = -1;
    set_hints();
}

static void answer(bool remove)
{
    int i = find(s_ask_id);
    if (remove && i >= 0) {
        char note[MUSE_APPS_NAME_MAX + 16];
        snprintf(note, sizeof(note), muse_apps_store_remove(&s_origin, s_ask_id) ? "Removed %s" : "Busy: try again",
                 s_apps[i].name);
        note_locked(note);
    }
    s_ask_id[0] = '\0';
    if (s_ask) {
        lv_obj_add_flag(s_ask, LV_OBJ_FLAG_HIDDEN);
    }
    s_view = VIEW_LIST;
    s_down_view = -1;
    set_hints();
}

static void on_button(int button, int edge)
{
    bool talk = button == MUSE_APP_BUTTON_PRIMARY;
    /* On a touch screen the highlight starts hidden: a button's first press
     * only shows it, so nothing opens or asks about a row you can't see. */
    if (!s_show_sel && s_view == VIEW_LIST) {
        if (edge == MUSE_APP_BTN_DOWN) {
            s_down_view = -1;
            select_row(s_sel);
        }
        return;
    }
    if (talk) {
        /* Acts on release, so a hold can ask to remove instead. */
        if (edge == MUSE_APP_BTN_DOWN) {
            s_down_view = s_view;
            s_held = false;
        } else if (edge == MUSE_APP_BTN_LONG && s_down_view == (int)VIEW_LIST && s_sel < s_count &&
                   (muse_app_view_buttons_present() & (1u << MUSE_APP_BUTTON_SECONDARY))) {
            /* Only a press that went down in the list: not one that just showed the highlight. */
            s_held = true;
            ask(s_sel);
        } else if (edge == MUSE_APP_BTN_UP) {
            bool act = s_down_view == (int)s_view && !s_held;
            s_down_view = -1;
            s_held = false;
            if (act) {
                if (s_view == VIEW_ASK) {
                    answer(true);
                } else {
                    open_row(s_sel);
                }
            }
        }
        return;
    }
    if (edge != MUSE_APP_BTN_DOWN) {
        return;
    }
    if (s_view == VIEW_ASK) {
        if (button == MUSE_APP_BUTTON_ENTER) {
            answer(true);
        } else if (button == MUSE_APP_BUTTON_SECONDARY || button == MUSE_APP_BUTTON_BACK ||
                   button == MUSE_APP_BUTTON_LEFT) {
            answer(false);
        }
        return;
    }
    switch (button) {
    case MUSE_APP_BUTTON_SECONDARY:
    case MUSE_APP_BUTTON_DOWN:
        select_row(s_sel + 1);
        break;
    case MUSE_APP_BUTTON_UP:
        select_row(s_sel - 1);
        break;
    case MUSE_APP_BUTTON_ENTER:
    case MUSE_APP_BUTTON_RIGHT:
        open_row(s_sel);
        break;
    case MUSE_APP_BUTTON_BACK:
        muse_apps_launcher_close();
        break;
    default:
        break;
    }
}

void muse_apps_launcher_event(int type, int a, int b)
{
    muse_app_view_lock();
    if (!s_screen) {
        muse_app_view_unlock();
        return;
    }
    switch (type) {
    case EV_ROW_TAP:
        if (s_view == VIEW_LIST) {
            open_row(a);
        }
        break;
    case EV_ROW_HOLD:
        if (s_view == VIEW_LIST) {
            ask(a);
        }
        break;
    case EV_ANSWER:
        if (s_view == VIEW_ASK) {
            answer(a == 1);
        }
        break;
    case MUSE_APP_EV_BUTTON:
        on_button(a, b);
        break;
    default:
        break;   /* touches on the background, gestures */
    }
    muse_app_view_unlock();
}

/* A few times a second: the list as the store has it now, and the note's time. */
void muse_apps_launcher_frame(uint32_t now_ms)
{
    static EXT_RAM_BSS_ATTR muse_apps_entry_t now[MUSE_APPS_MAX];
    int n = muse_apps_store_list(now, MUSE_APPS_MAX);
    muse_app_view_lock();
    if (s_screen && (n != s_count || memcmp(now, s_apps, sizeof(now[0]) * (size_t)n))) {
        char sel_id[MUSE_APPS_ID_MAX] = "";
        if (s_sel < s_count) {
            snprintf(sel_id, sizeof(sel_id), "%s", s_apps[s_sel].id);
        }
        memcpy(s_apps, now, sizeof(now[0]) * (size_t)n);
        s_count = n;
        build_rows();
        int i = find(sel_id);
        s_sel = i >= 0 ? i : (s_sel > s_count ? s_count : s_sel);
        if (s_ask_id[0] && find(s_ask_id) < 0) {
            answer(false);   /* removed meanwhile */
        }
        show_selection();
        set_hints();
    }
    if (s_note && s_note_until && (int32_t)(s_note_until - now_ms) <= 0) {
        s_note_until = 0;
        lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    }
    muse_app_view_unlock();
}
