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

/* Exercise the complete production UI with the simulator's services. */
#undef NDEBUG
#include <assert.h>
#include "sim_board.h"
#include "sim_platform.h"
#include "sim_services.h"
#include "../../components/muse/muse_ui.c"

static void check_read_caption(const char *text)
{
    muse_state_set_caption("%s", text);
    update_status(MUSE_MODE_SPEAKING, 1.0f);
    lv_obj_update_layout(lv_screen_active());
    const answer_layout_t *l = &s_answers[ANSWER_READ];
    assert(s_answer == ANSWER_READ);
    assert(lv_obj_get_style_text_font(s_reply_lbl, 0) == caption_font());
    assert(lv_obj_get_style_transform_scale_x(s_reply_lbl, 0) == 512);
    assert(lv_obj_get_style_transform_scale_y(s_reply_lbl, 0) == 512);
    assert(lv_obj_get_style_text_align(s_reply_lbl, 0) == LV_TEXT_ALIGN_CENTER);
    lv_area_t box;
    lv_obj_get_coords(s_reply_lbl, &box);
    assert(abs((box.x1 + box.x2 + 1) / 2 - s_w / 2) <= 1);
    assert(abs((box.y1 + box.y2 + 1) / 2 - (s_h / 2 + l->top + l->h / 2)) <= 1);
    lv_point_t size;
    lv_text_get_size(&size, lv_label_get_text(s_reply_lbl), caption_font(), 0,
                     CAPTION_LINE_SPACE, l->w / l->scale, LV_TEXT_FLAG_NONE);
    assert(size.y * l->scale <= l->h);
}

/* Rim hints fix a narrow reading column (StopWatch): keep its compact font. */
static void check_rim_column(void)
{
    muse_board_t rim = *sim_board_get();
    rim.rim_controls = true;
    muse_board = &rim;
    build_answer(lv_obj_create(lv_screen_active()), (s_w < s_h ? s_w : s_h) / 2 - 10);
    const answer_layout_t *l = &s_answers[ANSWER_READ];
    assert(l->scale == 1);
    assert(l->w <= 13 * lv_font_get_glyph_width(caption_font(), 'M', ' '));
    muse_board = sim_board_get();
}

int main(void)
{
    sim_time_reset();
    sim_services_reset();
    lv_init();
    muse_state_init();
    muse_board = sim_board_get();
    assert(muse_ui_start() == ESP_OK);
    sim_services_set_call(true);
    sim_services_set_speaker(false);
    check_read_caption("See you soon!");
    check_read_caption("The museum opens at ten tomorrow. You can take the riverside path and stop for coffee along the way.");
    assert(strstr(lv_label_get_text(s_reply_lbl), "along the way."));
    /* A mute toggle must restore the regular caption font and its scale. */
    sim_services_set_speaker(true);
    update_status(MUSE_MODE_SPEAKING, 2.0f);
    assert(s_answer == ANSWER_HEARD);
    assert(lv_obj_get_style_text_font(s_reply_lbl, 0) == caption_font());
    assert(lv_obj_get_style_transform_scale_x(s_reply_lbl, 0) == 256);
    sim_services_set_speaker(false);
    check_read_caption("Back in the centre.");
    check_rim_column();
    lv_deinit();
    return 0;
}
