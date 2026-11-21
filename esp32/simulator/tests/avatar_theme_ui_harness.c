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
static uint16_t pixels[16] = {0xffff,0xffff,0xffff,0xffff,0xffff,0,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff};
static muse_avatar_image_t personal = {.data={pixels,4,4},.revision=1};
static unsigned refs;
static uint32_t content_epoch=1;
static bool loading;
uint32_t muse_avatar_content_epoch(void) {return content_epoch;}
static bool motion_missing;
static unsigned motion_ticks;
void muse_avatar_motion_seen(uint32_t revision) {(void)revision;}
static muse_avatar_image_t moving={.data={pixels,4,4},.revision=0x80000001,.release=muse_avatar_release};
void muse_avatar_motion_tick(const muse_pose_t *pose) {
    (void)pose;motion_ticks++;
}
const muse_avatar_image_t *muse_avatar_motion_acquire(void) {
    if(selected!=MUSE_AVATAR_PERSONAL || motion_missing) return NULL;
    refs++;return &moving;
}
void muse_avatar_start(void) {}
muse_avatar_source_t muse_avatar_source(void) { return selected; }
void muse_avatar_select(muse_avatar_source_t source) { selected = source; }
const muse_avatar_image_t *muse_avatar_acquire(void) {
    if (selected != MUSE_AVATAR_PERSONAL || loading) return NULL;
    refs++; return &personal;
}
void muse_avatar_release(const muse_avatar_image_t *image) { if(image) { assert((image == &personal || image == &moving) && refs); refs--; } }

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
    frame_tick(NULL); assert(s_avatar_visible && s_avatar_revision==0x80000001 && !refs);
    assert(s_avatar_transform.filter==MUSE_AVATAR_FAST_SMOOTH);
    motion_missing=true;frame_tick(NULL);
    assert(s_avatar_visible && s_avatar_revision==personal.revision && s_avatar_transform.filter==MUSE_AVATAR_BILINEAR);
    motion_missing=false;frame_tick(NULL);

    assert(muse_theme_background()==0xffffff);
    lv_image_decoder_dsc_t decoder={.header=s_muse_src.header};
    assert(muse_dec_open(NULL,&decoder)==LV_RESULT_OK && refs==1);
    /* Source changes do not release a frame held by an open strip decoder. */
    muse_avatar_select(MUSE_AVATAR_BUILTIN);
    lv_area_t full={0,0,s_canvas_px-1,s_canvas_px-1},area={.y1=LV_COORD_MIN};
    assert(muse_dec_get_area(NULL,&decoder,&full,&area)==LV_RESULT_OK);
    muse_dec_close(NULL,&decoder);assert(!refs);
    frame_tick(NULL);assert(!s_avatar_visible);
    muse_avatar_select(MUSE_AVATAR_PERSONAL);frame_tick(NULL);
    unsigned before=motion_ticks;
    muse_ui_bench_page(true);frame_tick(NULL);
    assert(motion_ticks==before); /* Settings never requests a home frame. */
    muse_ui_bench_page(false);frame_tick(NULL);assert(motion_ticks>before);
    int x=s_w/2,y=s_h/2;
    touch(x,y,true);touch(x,y,false);assert(muse_state_happiness()>0);
    frame_tick(NULL);sim_time_advance_us(180000);frame_tick(NULL);
    assert(s_avatar_visible && s_avatar_revision==moving.revision && !refs);
    assert(s_avatar_transform.zoom_y<1000); /* the dog bounces, with no character swap */

    touch(x,y,true);touch(x,y-80,true);assert(muse_ui_volume_swiping());
    touch(x,y-80,false);assert(!muse_ui_volume_swiping());
    muse_state_set_mode(MUSE_MODE_LISTENING);frame_tick(NULL);assert(s_avatar_visible && s_avatar_revision==moving.revision);assert(muse_theme_background()==0xffffff);
    muse_state_set_mode(MUSE_MODE_SPEAKING);muse_state_set_level(1);frame_tick(NULL);assert(s_avatar_visible && s_avatar_revision==moving.revision);assert(muse_theme_background()==0xffffff);
    muse_state_set_asleep(true);frame_tick(NULL);
    muse_state_set_asleep(false);muse_state_set_mode(MUSE_MODE_IDLE);sim_time_advance_us(10000000);frame_tick(NULL);assert(s_avatar_visible);
    for(int i=0;i<200;i++) {
        muse_avatar_select(i%2?MUSE_AVATAR_PERSONAL:MUSE_AVATAR_BUILTIN);
        frame_tick(NULL);
        assert(s_avatar_visible==(i%2!=0) && !refs);
        assert(muse_theme_mode()==MUSE_THEME_AUTO);
        assert(muse_theme_background()==(i%2?0xffffffu:0));
    }
    muse_avatar_select(MUSE_AVATAR_BUILTIN);frame_tick(NULL);
    muse_avatar_select(MUSE_AVATAR_PERSONAL);loading=true;frame_tick(NULL);
    assert(muse_theme_background()==0xffffff); /* remembered personal hint during reload */
    content_epoch=0;frame_tick(NULL);assert(muse_theme_background()==0);
    loading=false;frame_tick(NULL);assert(muse_theme_background()==0); /* stale revision */
    personal.revision++;content_epoch=2;frame_tick(NULL);assert(muse_theme_background()==0xffffff);
    for(int mode=MUSE_THEME_DARK;mode<=MUSE_THEME_LIGHT;mode++) {
        muse_theme_request_mode((muse_theme_mode_t)mode);
        for(unsigned i=0;i<100;i++) {
            muse_avatar_select(i%2?MUSE_AVATAR_PERSONAL:MUSE_AVATAR_BUILTIN);frame_tick(NULL);
            assert(muse_theme_mode()==(muse_theme_mode_t)mode && !refs);
            assert(muse_theme_background()==(mode==MUSE_THEME_LIGHT?0xffffffu:0));
        }
    }
    lv_deinit();assert(!refs);
    puts("avatar UI: default, runtime switch, pinned strip lifetime, touch/pet, volume, voice states, sleep/wake passed");
}
