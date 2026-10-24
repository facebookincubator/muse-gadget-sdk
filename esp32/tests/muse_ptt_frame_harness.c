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
#include <assert.h>
#include <math.h>
#include "muse_voice.h"
#include "muse_pixel.h"

typedef void lv_timer_t;
static bool s_snapshot;
static float s_level;
static muse_mode_t s_last_mode=MUSE_MODE_COUNT, mode;
static muse_mode_t s_last_epaper_pose=MUSE_MODE_COUNT;
static const struct { bool epaper; } board = { false }, *muse_board = &board;
static muse_voice_view_t view;
static float source_level;
static muse_pose_t rendered;
static struct { void *data; } s_image_dsc;
static void *s_tv;
static void send_snapshot(void) {}
static void image_sync(void) {}
static void image_hide_locked(void) {}
static void muse_ui_show_face(void) {}
static bool update_sleep(void) { return false; }
static void update_chrome(float now) { (void)now; }
static bool muse_menu_tick(float now) { (void)now; return false; }
static int lv_obj_get_scroll_x(void *obj) { (void)obj; return 0; }
static void invalidate_muse(void) {}
static void update_status(muse_mode_t m,float now) { (void)now; assert(m==mode); }
static void update_control_hints(muse_mode_t m) { assert(m==mode); }   /* StopWatch rim icons */
int64_t esp_timer_get_time(void) { return 1000000; }
muse_mode_t muse_state_mode(float *t) { if (t) *t=0; return mode; }
float muse_state_level(void) { return source_level; }
float muse_state_happiness(void) { return 0; }
bool muse_voice_in_call(void) { return true; }
muse_voice_view_t muse_voice_call_view(void) { return view; }
void muse_pixel_render(const muse_pose_t *pose) { rendered=*pose; }

/* The test runner copies the complete production frame_tick function here. */
#include "muse_ptt_frame.inc"

int main(int argc,char **argv)
{
    (void)argv;
    assert(argc==2);
    mode=MUSE_MODE_SPEAKING; view=MUSE_VOICE_VIEW_SPEAKING; source_level=0.9f;
    frame_tick(NULL);
    assert(rendered.mode==MUSE_MODE_SPEAKING && fabsf(rendered.level-0.54f)<0.0001f);
    mode=MUSE_MODE_LISTENING; view=MUSE_VOICE_VIEW_LISTENING; source_level=0.1f;
    frame_tick(NULL);
    assert(rendered.mode==MUSE_MODE_LISTENING && fabsf(rendered.level-0.06f)<0.0001f);
    /* Mode was read just before the voice task switched to speaker output. */
    view=MUSE_VOICE_VIEW_SPEAKING; source_level=0.9f;
    frame_tick(NULL);
    assert(fabsf(rendered.level-0.048f)<0.0001f); /* only the previous mic level decays */
    return 0;
}
