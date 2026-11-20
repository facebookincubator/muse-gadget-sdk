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

#undef NDEBUG
#include <assert.h>
#include "sim_board.h"
#include "sim_platform.h"
#include "sim_services.h"
#include "muse_theme_lvgl.h"
#include "src/core/lv_obj_private.h"
#include "../../components/muse/muse_ui.c"
static uint32_t color(lv_obj_t *obj) {return lv_color_to_u32(lv_obj_get_style_bg_color(obj,0))&0xffffff;}
int main(void) {
    sim_time_reset();sim_services_reset();lv_init();muse_state_init();muse_board=sim_board_get();
    assert(muse_ui_start()==ESP_OK);muse_state_set_asleep(false);muse_state_set_mode(MUSE_MODE_IDLE);
    frame_tick(NULL);assert(color(s_face)==0);
    muse_theme_request_mode(MUSE_THEME_LIGHT);frame_tick(NULL);
    assert(color(s_face)==0xffffff && color(lv_screen_active())==0xffffff);
    assert(color(s_cover)==0); /* sleep stays physically dark */
    uint16_t corner=0;muse_pixel_scale(&corner,1,0,0,0,0);assert(corner==0xffff);
    lv_obj_t *filled=lv_button_create(s_face);
    muse_theme_set_style_bg_color(filled,lv_color_hex(0xa77dff),0);
    lv_obj_t *ink=lv_label_create(filled);
    muse_theme_set_style_text_color(ink,lv_color_hex(0xf2efff),0);
    assert(muse_theme_contrast(lv_color_to_u32(lv_obj_get_style_text_color(ink,0)),color(filled))>=4.49f);
    lv_obj_t *test=lv_obj_create(s_face);
    for(int i=0;i<1000;i++)muse_theme_set_style_text_color(test,lv_color_hex(i%2?0x8b84a8:0xf2efff),0);
    assert(test->style_cnt<8); /* changing semantic role cannot grow style lists */
    for(int i=0;i<50;i++) {
        muse_theme_request_mode(i%2?MUSE_THEME_LIGHT:MUSE_THEME_DARK);frame_tick(NULL);
        assert(color(s_face)==(i%2?0xffffffu:0));
    }
    muse_theme_request_mode(MUSE_THEME_AUTO);muse_theme_set_hint(true,0xf7f3eb);frame_tick(NULL);
    assert(color(s_face)==0xf7f3eb);
    muse_theme_request_mode(MUSE_THEME_DARK);frame_tick(NULL);assert(color(s_face)==0);
    muse_theme_set_hint(true,0xffffff);frame_tick(NULL);assert(color(s_face)==0); /* override wins */
    muse_theme_request_mode(MUSE_THEME_AUTO);frame_tick(NULL);assert(color(s_face)==0xffffff);
    muse_theme_set_hint(false,0);frame_tick(NULL);assert(color(s_face)==0);
    muse_state_set_asleep(true);frame_tick(NULL);assert(color(s_cover)==0);
    muse_state_set_asleep(false);frame_tick(NULL);
    lv_deinit();puts("PASS theme: independent UI, live palette, override, backdrop, bounded styles, sleep");
}
