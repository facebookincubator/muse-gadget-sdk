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

static lv_obj_t *s_tile, *s_scroll, *s_label, *s_new, *s_previous_label;
static char *s_display, *s_previous_display;
static bool s_transition;
static bool s_visible;
static uint32_t s_revision;
static muse_latest_reply_font_t s_font_for;
#if LV_USE_SNAPSHOT
static atomic_int s_bench_action;
bool muse_latest_reply_bench(const char *command)
{
    int action = !strcmp(command, "ui=reply") ? 1
        : !strcmp(command, "ui.reply=down") ? 2
        : !strcmp(command, "ui.reply=new") ? 3
        : !strcmp(command, "ui.reply=test-long") ? 4
        : !strcmp(command, "ui.reply=end") ? 5 : 0;
    if (action) atomic_store(&s_bench_action, action);
    return action != 0;
}
#endif

/* Mark truncation in the display copy without growing its fixed buffer. */
static void prepare_display(char *text, bool shortened)
{
    muse_text_to_caption(text, MUSE_REVIEW_CAP);
    if (!shortened) return;
    size_t end = strlen(text);
    if (end > MUSE_REVIEW_CAP - 4) {
        end = MUSE_REVIEW_CAP - 4; // three dots and the terminating NUL
        while (end && ((unsigned char)text[end] & 0xc0) == 0x80) end--;
    }
    memcpy(text + end, "...", 4);
}

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
        prepare_display(s_display, snapshot.shortened);
        lv_obj_set_style_text_font(s_label, s_font_for(s_display), 0);
        lv_label_set_text_static(s_label, s_display);
    } else {
        lv_label_set_text(s_label, snapshot.failed ? "Latest reply unavailable" : "No reply yet");
    }
    lv_obj_add_flag(s_new, LV_OBJ_FLAG_HIDDEN);
    layout_reply();
}

static void scroll_reply(void *object, int32_t value)
{
    lv_obj_scroll_to_y(object, value, LV_ANIM_OFF);
}

static void finish_transition(lv_anim_t *animation)
{
    (void)animation;
    if (s_previous_label) lv_obj_delete(s_previous_label);
    if (s_previous_display) heap_caps_free(s_previous_display);
    s_previous_label = NULL;
    s_previous_display = NULL;
    s_transition = false;
    lv_obj_update_layout(s_scroll);
    lv_obj_scroll_to_y(s_scroll, 0, LV_ANIM_OFF);
}

static void new_reply_cb(lv_event_t *event)
{
    (void)event;
    if (s_transition) return;
    char *incoming = heap_caps_malloc(MUSE_REVIEW_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!incoming) {
        ESP_LOGW("latest_reply", "new reply snapshot allocation failed");
        return; // keep the existing answer and offer a retry
    }
    muse_review_snapshot_t snapshot;
    muse_review_snapshot(incoming, MUSE_REVIEW_CAP, &snapshot);
    if (!snapshot.available) { heap_caps_free(incoming); return; }
    prepare_display(incoming, snapshot.shortened);

    int old_y = lv_obj_get_scroll_y(s_scroll);
    s_previous_label = s_label;
    s_previous_display = s_display;
    s_display = incoming;
    s_revision = snapshot.revision;
    /* Put the new answer before the old one. Offset the viewport by the same
     * amount, so tapping doesn't jump the text under the reader's finger.
     * Scrolling to zero then brings the new answer into view naturally. */
    s_label = lv_label_create(s_scroll);
    lv_obj_set_width(s_label, 308);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_label, s_font_for(s_display), 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0xd8d2ff), 0);
    lv_obj_set_style_text_line_space(s_label, 6, 0);
    lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_remove_flag(s_label, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text_static(s_label, s_display);
    lv_obj_update_layout(s_label);
    int shift = lv_obj_get_height(s_label) + 24;
    lv_obj_set_y(s_previous_label, shift);
    lv_obj_update_layout(s_scroll);
    lv_obj_scroll_to_y(s_scroll, old_y + shift, LV_ANIM_OFF);
    lv_obj_add_flag(s_new, LV_OBJ_FLAG_HIDDEN);
    s_transition = true;
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_scroll);
    lv_anim_set_exec_cb(&animation, scroll_reply);
    lv_anim_set_values(&animation, lv_obj_get_scroll_y(s_scroll), 0);
    lv_anim_set_duration(&animation, 350);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&animation, finish_transition);
    lv_anim_start(&animation);
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
    /* Returning is a reread: retain this answer and its scroll position.
     * A newer stored answer is offered by the button instead of replacing it. */
    if (!visible && s_transition) {
        lv_anim_delete(s_scroll, scroll_reply);
        finish_transition(NULL);
    }
    if (visible && !s_visible && (!s_display || !s_revision)) load_reply();
    s_visible = visible;
}

void muse_latest_reply_tick(void)
{
#if LV_USE_SNAPSHOT
    int action = atomic_exchange(&s_bench_action, 0);
    if (action && s_tile) {
        lv_tileview_set_tile(lv_obj_get_parent(s_tile), s_tile, LV_ANIM_OFF);
        if (action == 2) lv_obj_scroll_by(s_scroll, 0, -200, LV_ANIM_OFF);
        if (action == 4) {
            /* A synthetic oversized answer for repeatable hardware checks. */
            uint32_t turn = muse_review_turn();
            char part[512];
            memset(part, 'a', sizeof(part) - 1);
            for (size_t i = 31; i < sizeof(part) - 1; i += 32) part[i] = '\n';
            part[sizeof(part) - 1] = '\0';
            for (unsigned i = 0; i < 80; i++) muse_review_store(turn, part);
        }
        if (action == 3 || action == 4) new_reply_cb(NULL);
        if (action == 5) lv_obj_scroll_to_y(s_scroll, LV_COORD_MAX, LV_ANIM_OFF);
    }
#endif
    if (!s_visible || s_transition) return;
    muse_review_snapshot_t snapshot;
    muse_review_snapshot(NULL, 0, &snapshot);
    lv_obj_set_flag(s_new, LV_OBJ_FLAG_HIDDEN,
                    !snapshot.available || snapshot.revision == s_revision);
}
