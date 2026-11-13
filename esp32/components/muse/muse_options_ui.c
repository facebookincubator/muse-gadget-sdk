// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
#include "muse_options_ui.h"
#include "muse_options.h"
#include "muse_state.h"
#include <stdint.h>
#include <math.h>

static lv_obj_t *s_overlay, *s_panel, *s_open;
static const lv_font_t *s_font;
static muse_options_t *s_view;
static uint32_t s_seen_generation, s_seen_revision;
static bool s_expanded;

static void toggle(lv_event_t *e)
{
    (void)e;
    s_expanded = !s_expanded;
    muse_state_poke();
}

static void select_option(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (muse_options_request(s_view->generation, index)) s_expanded = false;
    muse_state_poke();
    muse_state_nudge();
}

static lv_obj_t *label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_width(obj, lv_pct(100));
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(obj, s_font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xf0edf8), 0);
    return obj;
}

void muse_options_ui_start(int width, int height, bool round, const lv_font_t *font)
{
    s_view = lv_malloc_zeroed(sizeof(*s_view));
    if (!s_view) return;
    s_font = font;
    s_open = lv_button_create(lv_layer_top());
    lv_obj_set_size(s_open, LV_SIZE_CONTENT, 36);
    lv_obj_align(s_open, LV_ALIGN_TOP_MID, 0, height > 300 ? 54 : 22);
    lv_obj_add_event_cb(s_open, toggle, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(s_open, lv_color_hex(0x49386c), 0);
    label(s_open, "Options");
    lv_obj_add_flag(s_open, LV_OBJ_FLAG_HIDDEN);

    /* A separate top-layer object owns touches; its gestures never bubble to
     * the face's volume, avatar, microphone or tile handlers. Sleep is above it. */
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    s_panel = lv_obj_create(s_overlay);
    int panel_w = width * 78 / 100, panel_h = height * 72 / 100;
    if (round) {
        /* Every corner, including the scrollbar, must fit within the circle.
         * Rounded card corners alone do not make a wide rectangle fit. Leave
         * ten pixels for the bezel, antialiasing and integer centering. */
        int diameter = (width < height ? width : height) - 20;
        float diagonal = hypotf((float)panel_w, (float)panel_h);
        if (diagonal > diameter) {
            panel_w = (int)(panel_w * diameter / diagonal);
            panel_h = (int)(panel_h * diameter / diagonal);
        }
    }
    lv_obj_set_size(s_panel, panel_w, panel_h);
    lv_obj_center(s_panel);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1a1530), 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x8c6ed4), 0);
    lv_obj_set_style_radius(s_panel, 18, 0);
    lv_obj_set_style_shadow_width(s_panel, 0, 0);
    lv_obj_set_style_outline_width(s_panel, 0, 0);
    lv_obj_set_style_clip_corner(s_panel, true, 0);
    lv_obj_set_style_pad_all(s_panel, 12, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLL_CHAIN_VER | LV_OBJ_FLAG_GESTURE_BUBBLE);
}

void muse_options_ui_tick(bool available)
{
    if (!s_overlay) return;
    bool have = muse_options_snapshot(s_view);
    if (have && s_view->generation != s_seen_generation) {
        s_seen_generation = s_view->generation;
        s_expanded = s_view->status == MUSE_OPTIONS_READY;
    }
    if (!have) s_expanded = false;
    if (s_view->revision != s_seen_revision) {
        s_seen_revision = s_view->revision;
        lv_obj_clean(s_panel);
        if (have) {
            lv_obj_t *close = lv_button_create(s_panel);
            lv_obj_set_width(close, lv_pct(100));
            lv_obj_add_event_cb(close, toggle, LV_EVENT_CLICKED, NULL);
            label(close, "Close options");
            label(s_panel, s_view->question[0] ? s_view->question : "Choose an option");
            for (int i = 0; i < s_view->count; ++i) {
                lv_obj_t *button = lv_button_create(s_panel);
                lv_obj_set_width(button, lv_pct(100));
                lv_obj_set_height(button, LV_SIZE_CONTENT);
                lv_obj_set_style_min_height(button, 44, 0);
                lv_obj_set_style_bg_color(button, lv_color_hex(i == s_view->selected ? 0x67508e : 0x302642), 0);
                label(button, s_view->labels[i]);
                lv_obj_add_event_cb(button, select_option, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                if (s_view->status != MUSE_OPTIONS_READY) lv_obj_add_state(button, LV_STATE_DISABLED);
            }
            const char *status = s_view->detail;
            if (!status[0]) {
                status = s_view->status == MUSE_OPTIONS_SELECTED ? "Answer selected"
                         : s_view->status == MUSE_OPTIONS_QUEUED || s_view->status == MUSE_OPTIONS_SENDING ? "Sending answer..."
                         : s_view->status == MUSE_OPTIONS_READY ? "Tap one answer" : "Continue in the Muse app";
            }
            label(s_panel, status);
            lv_obj_scroll_to_y(s_panel, 0, LV_ANIM_OFF);
        }
    }
    lv_obj_set_flag(s_open, LV_OBJ_FLAG_HIDDEN, !have || !available || s_expanded);
    lv_obj_set_flag(s_overlay, LV_OBJ_FLAG_HIDDEN, !have || !available || !s_expanded);
}
