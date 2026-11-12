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

#include "muse_latest_reply.h"

#include <string.h>
#if LV_USE_SNAPSHOT
#include <stdatomic.h>
#endif

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "muse_review.h"
#include "muse_text.h"
#include "muse_ui.h"

static lv_obj_t *s_tile, *s_scroll, *s_label, *s_notice, *s_new;
static char *s_display;
static bool s_visible;
static uint32_t s_revision;
static muse_latest_reply_font_t s_font_for;
#if LV_USE_SNAPSHOT
static atomic_int s_bench_action;
bool muse_latest_reply_bench(const char *command)
{
    int action = !strcmp(command, "ui=reply") ? 1
        : !strcmp(command, "ui.reply=down") ? 2
        : !strcmp(command, "ui.reply=new") ? 3 : 0;
    if (action) atomic_store(&s_bench_action, action);
    return action != 0;
}
#endif

/* Keep the main screen's caption font and let LVGL measure the full label. */
static void layout_reply(void)
{
    lv_obj_update_layout(s_scroll);
    lv_obj_scroll_to_y(s_scroll, 0, LV_ANIM_OFF);
}

static void load_reply(void)
{
    if (!s_display) {
        s_display = heap_caps_malloc(MUSE_REVIEW_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_display) {
            ESP_LOGW("latest_reply", "display snapshot allocation failed");
            /* Leave an existing view intact; retry on the next visit. */
            lv_label_set_text(s_label, "Latest reply unavailable");
            layout_reply();
            return;
        }
    }
    muse_review_snapshot_t snapshot;
    muse_review_snapshot(s_display, MUSE_REVIEW_CAP, &snapshot);
    s_revision = snapshot.revision;
    if (snapshot.available) {
        muse_text_to_caption(s_display, MUSE_REVIEW_CAP);
        lv_obj_set_style_text_font(s_label, s_font_for(s_display), 0);
        lv_label_set_text_static(s_label, s_display);
    } else {
        lv_label_set_text(s_label, snapshot.failed ? "Latest reply unavailable" : "No reply yet");
    }
    lv_label_set_text(s_notice, snapshot.shortened ? "REPLY SHORTENED" : "");
    lv_obj_add_flag(s_new, LV_OBJ_FLAG_HIDDEN);
    layout_reply();
}

static void new_reply_cb(lv_event_t *event)
{
    (void)event;
    load_reply();
}

void muse_latest_reply_build(lv_obj_t *tileview, muse_latest_reply_font_t font_for)
{
    s_font_for = font_for;
    s_tile = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_RIGHT);
    lv_obj_set_style_bg_color(s_tile, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tile, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *title = lv_label_create(s_tile);
    lv_label_set_text(title, "LATEST REPLY");
    lv_obj_set_style_text_font(title, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8b84a8), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 55);

    s_scroll = lv_obj_create(s_tile);
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_size(s_scroll, 320, 280);
    lv_obj_align(s_scroll, LV_ALIGN_TOP_MID, 0, 106);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_remove_flag(s_scroll, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_all(s_scroll, 4, 0);
    lv_obj_set_style_bg_color(s_scroll, lv_color_hex(0x8b84a8), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_scroll, 2, LV_PART_SCROLLBAR);

    s_label = lv_label_create(s_scroll);
    lv_obj_set_width(s_label, 308);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_label, s_font_for(""), 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0xd8d2ff), 0);
    lv_obj_set_style_text_line_space(s_label, 6, 0);
    lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(s_label, "No reply yet");
    lv_obj_remove_flag(s_label, LV_OBJ_FLAG_CLICKABLE);

    s_notice = lv_label_create(s_tile);
    lv_label_set_text(s_notice, "");
    lv_obj_set_style_text_font(s_notice, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_notice, lv_color_hex(0x8b84a8), 0);
    lv_obj_align(s_notice, LV_ALIGN_TOP_MID, 0, 399);

    s_new = lv_button_create(s_tile);
    lv_obj_set_size(s_new, 190, 42);
    lv_obj_align(s_new, LV_ALIGN_TOP_MID, 0, 372);
    lv_obj_set_style_bg_color(s_new, lv_color_hex(0xa77dff), 0);
    lv_obj_set_style_radius(s_new, 21, 0);
    lv_obj_set_style_shadow_width(s_new, 0, 0);
    lv_obj_t *new_label = lv_label_create(s_new);
    lv_label_set_text(new_label, "New reply");
    lv_obj_set_style_text_font(new_label, &lv_font_unscii_16, 0);
    lv_obj_align(new_label, LV_ALIGN_CENTER, -16, 0);
    lv_obj_t *arrow = lv_label_create(s_new);
    lv_label_set_text(arrow, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_20, 0);
    lv_obj_align(arrow, LV_ALIGN_CENTER, 76, 0);
    lv_obj_add_event_cb(s_new, new_reply_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_new, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *muse_latest_reply_tile(void) { return s_tile; }

void muse_latest_reply_set_visible(bool visible)
{
    if (visible && !s_visible) load_reply();
    s_visible = visible;
}

void muse_latest_reply_tick(void)
{
#if LV_USE_SNAPSHOT
    int action = atomic_exchange(&s_bench_action, 0);
    if (action && s_tile) {
        lv_tileview_set_tile(lv_obj_get_parent(s_tile), s_tile, LV_ANIM_OFF);
        if (action == 2) lv_obj_scroll_by(s_scroll, 0, -200, LV_ANIM_OFF);
        if (action == 3) load_reply();
    }
#endif
    if (!s_visible) return;
    muse_review_snapshot_t snapshot;
    muse_review_snapshot(NULL, 0, &snapshot);
    lv_obj_set_flag(s_new, LV_OBJ_FLAG_HIDDEN,
                    !snapshot.available || snapshot.revision == s_revision);
}
