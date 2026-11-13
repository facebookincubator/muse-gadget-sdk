/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0. */
/* Drive real LVGL pointer dispatch through the complete production UI. */
#undef NDEBUG
#include <assert.h>
#include "sim_board.h"
#include "sim_platform.h"
#include "sim_services.h"
#include "muse_settings.h"
#include "muse_audio.h"

static int saved_volume = 50, preview_volume = 50, saves;
static int test_volume(void) { return saved_volume; }
static void test_save(int volume) { saved_volume = volume; saves++; }
static void test_preview(int volume) { preview_volume = volume; }
#define muse_settings_volume test_volume
#define muse_settings_set_volume test_save
#define muse_audio_set_volume test_preview
#include "../../components/muse/muse_ui.c"

static lv_point_t finger;
static bool down;
static void read_touch(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = finger;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void touch(int x, int y, bool pressed)
{
    finger = (lv_point_t){x, y};
    down = pressed;
    sim_time_advance_us(20000);
    lv_indev_read(s_indev);
    lv_obj_update_layout(lv_screen_active());
}

int main(void)
{
    sim_time_reset();
    sim_services_reset();
    lv_init();
    muse_state_init();
    muse_board = sim_board_get();
    assert(muse_ui_start() == ESP_OK);
    muse_state_set_asleep(false);
    lv_indev_set_read_cb(s_indev, read_touch);
    lv_obj_update_layout(lv_screen_active());
    int x = s_w / 2, y = s_h / 2;

    touch(x, y, true);
    touch(x, y - 80, true);
    assert(muse_ui_volume_swiping());
    assert(preview_volume > 50 && saved_volume == 50 && saves == 0);
    assert(!lv_obj_has_flag(s_volume_hint, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_scroll_x(s_tv) == 0);
    touch(x, y - 80, false);
    assert(!muse_ui_volume_swiping());
    assert(saved_volume == preview_volume && saves == 1);
    assert(lv_obj_has_flag(s_volume_hint, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE));

    touch(x, y, true);
    touch(x, y + 80, true);
    assert(preview_volume == 50);
    touch(x, y + 80, false);
    assert(saved_volume == 50 && saves == 2);

    /* A tap and small finger jitter never save or preview a new volume. */
    touch(x, y, true);
    touch(x + 2, y - 2, true);
    touch(x + 2, y - 2, false);
    assert(saved_volume == 50 && preview_volume == 50 && saves == 2);

    /* A horizontal swipe is still owned by the tileview. */
    touch(x, y, true);
    touch(x - 90, y, true);
    assert(!muse_ui_volume_swiping());
    assert(lv_indev_get_scroll_obj(s_indev) == s_tv);
    touch(x - 90, y, false);
    assert(saves == 2);
    lv_tileview_set_tile(s_tv, s_face, LV_ANIM_OFF);
    s_face_active = true;
    lv_obj_update_layout(lv_screen_active());

    /* A gesture interrupted by pairing stops and commits exactly once. */
    touch(x, y, true);
    touch(x, y - 80, true);
    assert(muse_ui_volume_swiping());
    lv_obj_remove_flag(s_pair, LV_OBJ_FLAG_HIDDEN);
    touch(x, y - 90, true);
    assert(!muse_ui_volume_swiping() && saves == 3);
    touch(x, y - 90, false);
    assert(saves == 3);
    lv_obj_add_flag(s_pair, LV_OBJ_FLAG_HIDDEN);

    /* The waking touch remains owned by the sleep cover. */
    lv_obj_remove_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
    touch(x, y, true);
    touch(x, y - 80, true);
    touch(x, y - 80, false);
    assert(saves == 3 && !muse_ui_volume_swiping());
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);

    /* Settings retains vertical scrolling, with no volume side effects. */
    lv_tileview_set_tile(s_tv, s_settings, LV_ANIM_OFF);
    lv_obj_update_layout(lv_screen_active());
    touch(x, y, true);
    touch(x, y - 80, true);
    touch(x, y - 80, false);
    assert(saves == 3 && !muse_ui_volume_swiping());
    lv_tileview_set_tile(s_tv, s_face, LV_ANIM_OFF);
    s_face_active = true;
    lv_obj_update_layout(lv_screen_active());

    /* A stationary long hold cannot turn into a volume drag after PTT starts. */
    touch(x, y, true);
    sim_time_advance_us(600000);
    touch(x, y, true);
    touch(x, y - 80, true);
    touch(x, y - 80, false);
    assert(saves == 3);

    /* Lost touches commit the preview once, and preserve the mute setting. */
    sim_services_set_speaker(false);
    assert(volume_swipe_allowed());
    touch(x, y, true);
    assert(s_volume_swipe.tracking);
    touch(x, y + 80, true);
    assert(muse_ui_volume_swiping());
    lv_obj_send_event(s_canvas, LV_EVENT_PRESS_LOST, NULL);
    assert(saves == 4 && saved_volume == preview_volume);
    touch(x, y + 80, false);
    assert(saves == 4 && !muse_settings_speaker_on());
    return 0;
}
