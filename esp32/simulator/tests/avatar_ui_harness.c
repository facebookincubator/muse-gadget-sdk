/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <assert.h>
#include "sim_board.h"
#include "sim_platform.h"
#include "sim_services.h"
#include "muse_avatar.h"
#include "../../components/muse/muse_ui.c"

static muse_avatar_source_t selected;
static uint16_t pixels[16] = {0xf800, 0x07e0, 0x001f, 0xffff};
static muse_avatar_image_t personal = {.data={pixels,4,4},.revision=1};
static unsigned refs;
void muse_avatar_start(void) {}
muse_avatar_source_t muse_avatar_source(void) { return selected; }
void muse_avatar_select(muse_avatar_source_t source) { selected = source; }
const muse_avatar_image_t *muse_avatar_acquire(void) {
    if (selected != MUSE_AVATAR_PERSONAL) return NULL;
    refs++; return &personal;
}
void muse_avatar_release(const muse_avatar_image_t *image) { if(image) { assert(image == &personal && refs); refs--; } }

static lv_point_t finger;
static bool down;
static void read_touch(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev; data->point=finger; data->state=down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}
static void touch(int x,int y,bool pressed) {
    finger=(lv_point_t){x,y};down=pressed;sim_time_advance_us(20000);
    lv_indev_read(s_indev);lv_obj_update_layout(lv_screen_active());
}
int main(void) {
    sim_time_reset(); sim_services_reset(); lv_init(); muse_state_init();
    muse_board=sim_board_get(); assert(muse_ui_start()==ESP_OK);
    muse_state_set_asleep(false); muse_state_set_mode(MUSE_MODE_IDLE);
    lv_indev_set_read_cb(s_indev,read_touch);lv_obj_update_layout(lv_screen_active());
    frame_tick(NULL); assert(!s_avatar_visible && !refs);
    muse_avatar_select(MUSE_AVATAR_PERSONAL);
    frame_tick(NULL); assert(s_avatar_visible && s_avatar_revision==1 && !refs);
    lv_image_decoder_dsc_t decoder={.header=s_muse_src.header};
    assert(muse_dec_open(NULL,&decoder)==LV_RESULT_OK && refs==1);
    /* Source changes do not release a frame held by an open strip decoder. */
    muse_avatar_select(MUSE_AVATAR_BUILTIN);
    lv_area_t full={0,0,s_canvas_px-1,s_canvas_px-1},area={.y1=LV_COORD_MIN};
    assert(muse_dec_get_area(NULL,&decoder,&full,&area)==LV_RESULT_OK);
    muse_dec_close(NULL,&decoder);assert(!refs);
    frame_tick(NULL);assert(!s_avatar_visible);
    muse_avatar_select(MUSE_AVATAR_PERSONAL);frame_tick(NULL);
    int x=s_w/2,y=s_h/2;
    touch(x,y,true);touch(x,y,false);assert(muse_state_happiness()>0);
    touch(x,y,true);touch(x,y-80,true);assert(muse_ui_volume_swiping());
    touch(x,y-80,false);assert(!muse_ui_volume_swiping());
    muse_state_set_mode(MUSE_MODE_LISTENING);frame_tick(NULL);assert(s_avatar_visible);
    muse_state_set_mode(MUSE_MODE_SPEAKING);muse_state_set_level(1);frame_tick(NULL);assert(s_avatar_visible);
    muse_state_set_asleep(true);frame_tick(NULL);
    muse_state_set_asleep(false);muse_state_set_mode(MUSE_MODE_IDLE);frame_tick(NULL);assert(s_avatar_visible);
    for(int i=0;i<200;i++) {
        muse_avatar_select(i%2?MUSE_AVATAR_PERSONAL:MUSE_AVATAR_BUILTIN);
        frame_tick(NULL);
        assert(s_avatar_visible==(i%2!=0) && !refs);
    }
    lv_deinit();assert(!refs);
    puts("avatar UI: default, runtime switch, pinned strip lifetime, touch/pet, volume, voice states, sleep/wake passed");
}
