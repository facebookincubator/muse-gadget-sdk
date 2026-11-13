// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
/* Real LVGL pointer dispatch and production UI; no network simulation. */
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include "sim_board.h"
#include "sim_platform.h"
#include "sim_services.h"
#include "muse_options.h"
#include "../../components/muse/muse_ui.c"

static lv_point_t finger;
static bool down;
static muse_board_t test_board;
static lv_display_t *test_display_start(lv_indev_t **touch_input)
{
    lv_display_t *display = sim_board_get()->display_start(touch_input);
    lv_display_set_resolution(display, test_board.width, test_board.height);
    return display;
}

static void assert_panel_inside(lv_obj_t *panel)
{
    lv_area_t area;
    lv_obj_get_coords(panel, &area);
    assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < s_w && area.y2 < s_h);
    if (muse_board->round) {
        double radius = (s_w < s_h ? s_w : s_h) / 2.0 - 8;
        for (int i=0; i<4; ++i) {
            double dx = (i & 1 ? area.x2 : area.x1) - (s_w-1)/2.0;
            double dy = (i & 2 ? area.y2 : area.y1) - (s_h-1)/2.0;
            assert(dx*dx + dy*dy <= radius*radius);
        }
        assert(lv_obj_get_style_shadow_width(panel,0)==0);
        assert(lv_obj_get_style_outline_width(panel,0)==0);
    } else {
        assert(lv_obj_get_width(panel)==s_w*78/100 && lv_obj_get_height(panel)==s_h*72/100);
    }
}
static void read_touch(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev; data->point = finger;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void frame(void)
{
    sim_time_advance_us(40000);
    frame_tick(NULL);
    lv_obj_update_layout(lv_layer_top());
    lv_obj_update_layout(lv_screen_active());
}
static void touch(int x, int y, bool pressed)
{
    finger = (lv_point_t){x,y}; down = pressed;
    sim_time_advance_us(20000); lv_indev_read(s_indev);
    lv_obj_update_layout(lv_layer_top());
}
static lv_obj_t *find(lv_obj_t *obj, const char *text)
{
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return obj;
    for (uint32_t i=0; i<lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *result=find(lv_obj_get_child(obj,i),text);
        if (result) return result;
    }
    return NULL;
}
static void tap(lv_obj_t *obj)
{
    lv_obj_scroll_to_view(obj, LV_ANIM_OFF);
    lv_obj_update_layout(lv_layer_top());
    lv_area_t area; lv_obj_get_coords(obj,&area);
    int x=(area.x1+area.x2)/2, y=(area.y1+area.y2)/2;
    touch(x,y,true); touch(x,y,false); frame();
}
int main(int argc, char **argv)
{
    sim_time_reset(); sim_services_reset(); lv_init(); muse_state_init();
    test_board=*sim_board_get();
    if (argc==4) {
        test_board.width=atoi(argv[1]); test_board.height=atoi(argv[2]); test_board.round=atoi(argv[3])!=0;
    }
    test_board.display_start=test_display_start;
    muse_board=&test_board; assert(muse_ui_start()==ESP_OK);
    muse_state_set_mode(MUSE_MODE_IDLE); muse_state_set_asleep(false);
    lv_indev_set_read_cb(s_indev,read_touch); frame();
    lv_obj_t *reopen=lv_obj_get_parent(find(lv_layer_top(),"Options"));
    assert(lv_obj_has_flag(reopen,LV_OBJ_FLAG_HIDDEN));
    muse_options_t q={.count=3,.selected=-1};
    strcpy(q.widget_id,"widget-test"); strcpy(q.message_id,"message-test");
    strcpy(q.question,"Which lunch would you like?");
    strcpy(q.labels[0],"Pasta"); strcpy(q.labels[1],"Salad");
    strcpy(q.labels[2],"A longer answer that wraps over several lines and remains reachable by scrolling");
    assert(muse_options_publish(&q)); frame();
    lv_obj_t *answer=find(lv_layer_top(),"Pasta"); assert(answer);
    lv_obj_t *panel=lv_obj_get_parent(lv_obj_get_parent(answer));
    assert_panel_inside(panel);
    /* Only the scrim dismisses; the close control and list remain inside it. */
    touch(s_w/2,32,true); touch(s_w/2,32,false); frame();
    assert(lv_obj_has_flag(lv_obj_get_parent(panel),LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(reopen,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_width(reopen)>60);
    assert(lv_obj_get_style_bg_opa(reopen,0)==LV_OPA_TRANSP);
    assert(!muse_options_pending());
    tap(reopen);
    assert(!lv_obj_has_flag(lv_obj_get_parent(panel),LV_OBJ_FLAG_HIDDEN));
    /* The whole picker owns vertical scrolling, never the face's tile navigation. */
    touch(s_w/2,s_h/2,true); touch(s_w/2,s_h/2-60,true); touch(s_w/2,s_h/2-60,false);
    assert(!muse_options_pending() && lv_obj_get_scroll_x(s_tv) == 0);
    assert_panel_inside(panel);
    frame(); tap(lv_obj_get_parent(answer));
    assert(muse_options_snapshot(&q) && q.status==MUSE_OPTIONS_QUEUED && q.selected==0);
    assert(!muse_options_request(q.generation,1));
    assert(muse_options_take(&q));
    muse_options_result(q.generation,MUSE_OPTIONS_SELECTED,true,""); frame();
    assert(lv_obj_has_flag(reopen,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(lv_obj_get_parent(panel),LV_OBJ_FLAG_HIDDEN));
    answer=find(lv_layer_top(),"Salad"); assert(answer);
    assert(lv_obj_has_state(lv_obj_get_parent(answer),LV_STATE_DISABLED));
    /* A new question can replace the prior one; a wake touch cannot answer it. */
    strcpy(q.widget_id,"replacement"); q.selected=-1;
    assert(muse_options_publish(&q)); frame();
    muse_state_set_asleep(true); frame();
    touch(s_w/2,s_h/2,true); touch(s_w/2,s_h/2,false); frame();
    assert(!muse_options_pending());
    muse_options_clear(); frame();
    assert(!muse_options_snapshot(&q));
    assert(lv_obj_has_flag(reopen,LV_OBJ_FLAG_HIDDEN));
    return 0;
}
