/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0 */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include "sim_board.h"
#include "sim_platform.h"
#include "sim_services.h"
#include "../../components/muse/muse_ui.c"
static bool fail_display_alloc;
static void *display_alloc(size_t size, uint32_t caps)
{
    return fail_display_alloc ? NULL : heap_caps_malloc(size, caps);
}
#define heap_caps_malloc display_alloc
#include "../../components/muse/muse_latest_reply.c"
#undef heap_caps_malloc

static lv_point_t pointer;
static bool pointer_down;
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = pointer;
    data->state = pointer_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void swipe(int x1, int y1, int x2, int y2)
{
    pointer_down = true;
    for (int i = 0; i <= 12; i++) {
        pointer.x = x1 + (x2 - x1) * i / 12;
        pointer.y = y1 + (y2 - y1) * i / 12;
        sim_time_advance_us(20000);
        lv_indev_read(s_indev);
        lv_timer_handler();
    }
    pointer_down = false;
    lv_indev_read(s_indev);
    for (int i = 0; i < 30; i++) {
        sim_time_advance_us(20000);
        lv_timer_handler();
    }
}

static void advance_reply(unsigned ms)
{
    for (unsigned elapsed = 0; elapsed < ms; elapsed += 20) {
        sim_time_advance_us(20000);
        lv_timer_handler();
    }
}

static void open_latest(void)
{
    lv_tileview_set_tile(s_tv, muse_latest_reply_tile(), LV_ANIM_OFF);
    lv_obj_update_layout(lv_screen_active());
    assert(!muse_ui_face_active());
}

static void screenshot(const char *path)
{
    lv_obj_update_layout(lv_screen_active());
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    assert(buf);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n%u %u\n255\n", (unsigned)buf->header.w, (unsigned)buf->header.h);
    for (unsigned y = 0; y < buf->header.h; y++) {
        const uint16_t *row = (const uint16_t *)(buf->data + y * buf->header.stride);
        for (unsigned x = 0; x < buf->header.w; x++) {
            uint16_t pixel = row[x];
            unsigned char rgb[] = {(pixel >> 11) * 255 / 31, ((pixel >> 5) & 63) * 255 / 63, (pixel & 31) * 255 / 31};
            fwrite(rgb, 1, 3, file);
        }
    }
    fclose(file);
    lv_draw_buf_destroy(buf);
}

static char concurrent_text[2049];
static void *writer(void *unused)
{
    (void)unused;
    for (int i = 0; i < 400; i++) {
        memset(concurrent_text, i % 2 ? 'a' : 'b', sizeof(concurrent_text) - 1);
        muse_review_store(muse_review_turn(), concurrent_text);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    sim_time_reset();
    sim_services_reset();
    lv_init();
    muse_state_init();
    assert(muse_review_init() == ESP_OK);
    muse_board = sim_board_get();
    assert(muse_ui_start() == ESP_OK);
    assert(lv_tileview_get_tile_active(s_tv) == s_face);
    fail_display_alloc = true;
    open_latest();
    assert(!strcmp(lv_label_get_text(s_label), "Latest reply unavailable"));
    lv_tileview_set_tile(s_tv, s_face, LV_ANIM_OFF);
    fail_display_alloc = false;
    open_latest();
    assert(!strcmp(lv_label_get_text(s_label), "No reply yet"));
    assert(lv_obj_has_flag(s_new, LV_OBJ_FLAG_HIDDEN));
    muse_ui_show_face();
    lv_tileview_set_tile(s_tv, s_face, LV_ANIM_OFF);
    lv_obj_update_layout(lv_screen_active());
    static char answer[6000];
    for (int i = 0; i < 30; i++) strcat(answer, "A long answer stays available after its captions disappear. Scroll to read at your own pace.\n\n");
    uint32_t turn = muse_review_turn();
    muse_review_store(turn, answer);
    open_latest();
    assert(!strcmp(lv_label_get_text(s_label), answer));
    lv_area_t area;
    lv_obj_get_coords(s_scroll, &area);
    assert(area.x1 >= 70 && area.x2 <= 396 && area.y1 >= 100 && area.y2 <= 390);
    assert(lv_obj_get_scroll_bottom(s_scroll) > 0);
    assert(lv_obj_get_style_text_font(s_label, 0) == caption_font());
    assert(lv_obj_get_style_transform_scale_x(s_label, 0) == 256);
    assert(PAGE_DOTS == 3);
    s_next_settings_tick = 0;
    update_chrome(1.0f);
    assert(lv_obj_get_style_bg_color(s_dots[0], 0).red == lv_color_hex(COLOR_ACCENT).red);
    assert(!lv_obj_has_flag(s_dots[0], LV_OBJ_FLAG_HIDDEN));
    assert(!muse_latest_reply_bench("invalid"));
    assert(muse_latest_reply_bench("ui.reply=down"));
    muse_latest_reply_tick();
    assert(lv_obj_get_scroll_y(s_scroll) > 0);
    assert(muse_latest_reply_bench("ui.reply=new"));
    muse_latest_reply_tick();
    advance_reply(420);
    assert(lv_obj_get_scroll_y(s_scroll) == 0);
    lv_indev_set_read_cb(s_indev, read_pointer);
    swipe(230, 320, 230, 150);
    assert(lv_obj_get_scroll_y(s_scroll) > 0);
    assert(lv_tileview_get_tile_active(s_tv) == s_tile);
    int reread_y = lv_obj_get_scroll_y(s_scroll);
    swipe(350, 220, 100, 220);
    assert(lv_tileview_get_tile_active(s_tv) == s_face);
    swipe(100, 220, 350, 220);
    assert(lv_tileview_get_tile_active(s_tv) == s_tile);
    assert(lv_obj_get_scroll_y(s_scroll) == reread_y);

    lv_obj_scroll_to_y(s_scroll, 240, LV_ANIM_OFF);
    int scroll = lv_obj_get_scroll_y(s_scroll);
    assert(scroll > 0);
    muse_review_store(muse_review_turn(), "A new answer\n\nSecond paragraph.");
    muse_latest_reply_tick();
    assert(!lv_obj_has_flag(s_new, LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(lv_label_get_text(s_label), answer));
    assert(lv_obj_get_scroll_y(s_scroll) == scroll);
    lv_tileview_set_tile(s_tv, s_face, LV_ANIM_OFF);
    open_latest();
    muse_latest_reply_tick();
    assert(!strcmp(lv_label_get_text(s_label), answer));
    assert(lv_obj_get_scroll_y(s_scroll) == scroll);
    assert(!lv_obj_has_flag(s_new, LV_OBJ_FLAG_HIDDEN));
    if (argc > 1) screenshot(argv[1]);
    fail_display_alloc = true;
    lv_obj_send_event(s_new, LV_EVENT_CLICKED, NULL);
    assert(!s_transition && !strcmp(lv_label_get_text(s_label), answer));
    assert(lv_obj_get_scroll_y(s_scroll) == scroll);
    fail_display_alloc = false;
    lv_obj_send_event(s_new, LV_EVENT_CLICKED, NULL);
    assert(s_transition && s_previous_label && s_previous_display);
    assert(!strcmp(lv_label_get_text(s_previous_label), answer));
    int start_y = lv_obj_get_scroll_y(s_scroll);
    assert(start_y > scroll);
    advance_reply(160);
    assert(lv_obj_get_scroll_y(s_scroll) > 0 && lv_obj_get_scroll_y(s_scroll) < start_y);
    advance_reply(260);
    assert(!s_transition && !s_previous_label && !s_previous_display);
    assert(!strcmp(lv_label_get_text(s_label), "A new answer\n\nSecond paragraph."));
    assert(lv_obj_get_scroll_y(s_scroll) == 0);
    assert(lv_obj_has_flag(s_new, LV_OBJ_FLAG_HIDDEN));
    muse_review_store(muse_review_turn(), "“café” — a reply");
    lv_obj_send_event(s_new, LV_EVENT_CLICKED, NULL);
    advance_reply(420);
    assert(!strcmp(lv_label_get_text(s_label), "\"cafe\" -- a reply"));
    char original[64];
    muse_review_snapshot_t original_info;
    muse_review_snapshot(original, sizeof(original), &original_info);
    assert(!strcmp(original, "“café” — a reply"));
    turn = muse_review_turn();
    muse_review_store(turn, "First part");
    muse_review_store(turn, "Second part");
    muse_latest_reply_tick();
    lv_obj_send_event(s_new, LV_EVENT_CLICKED, NULL);
    advance_reply(420);
    assert(!strcmp(lv_label_get_text(s_label), "First part\n\nSecond part"));
    muse_review_store(muse_review_turn(), "");
    muse_latest_reply_tick();
    assert(lv_obj_has_flag(s_new, LV_OBJ_FLAG_HIDDEN));
    muse_review_store_complete(muse_review_turn(), "A shortened reply", true);
    lv_obj_send_event(s_new, LV_EVENT_CLICKED, NULL);
    advance_reply(420);
    assert(!strcmp(lv_label_get_text(s_label), "A shortened reply..."));
    muse_review_snapshot(original, sizeof(original), &original_info);
    assert(!strcmp(original, "A shortened reply"));
    /* Navigating away during the animation safely completes the selection. */
    muse_review_store(muse_review_turn(), "Selected before leaving");
    lv_obj_send_event(s_new, LV_EVENT_CLICKED, NULL);
    assert(s_transition);
    lv_tileview_set_tile(s_tv, s_face, LV_ANIM_OFF);
    assert(!s_transition && !s_previous_label && !s_previous_display);
    open_latest();
    assert(!strcmp(lv_label_get_text(s_label), "Selected before leaving"));
    assert(lv_obj_get_scroll_y(s_scroll) == 0);
    /* A full buffer must remain scrollable through its final line. */
    char *full = malloc(40000);
    for (int i = 0; i < 39999; i++) full[i] = i % 32 == 31 ? '\n' : 'a';
    full[39999] = 0;
    muse_review_store(muse_review_turn(), full);
    lv_obj_send_event(s_new, LV_EVENT_CLICKED, NULL);
    advance_reply(420);
    lv_obj_update_layout(s_scroll);
    assert(strlen(lv_label_get_text(s_label)) == MUSE_REVIEW_CAP - 1);
    assert(!strcmp(lv_label_get_text(s_label) + MUSE_REVIEW_CAP - 4, "..."));
    lv_obj_scroll_to_y(s_scroll, LV_COORD_MAX, LV_ANIM_OFF);
    assert(lv_obj_get_scroll_bottom(s_scroll) == 0);
    free(full);
    muse_state_set_asleep(true);
    assert(update_sleep());
    assert(lv_tileview_get_tile_active(s_tv) == s_face);
    muse_state_set_asleep(false);
    update_sleep();
    open_latest();
    assert(strlen(lv_label_get_text(s_label)) == MUSE_REVIEW_CAP - 1);
    assert(!strcmp(lv_label_get_text(s_label) + MUSE_REVIEW_CAP - 4, "..."));
    muse_ui_show_face();
    s_next_settings_tick = 0;
    update_chrome(2.0f);
    assert(lv_obj_get_style_bg_color(s_dots[1], 0).red == lv_color_hex(COLOR_ACCENT).red);
    lv_tileview_set_tile(s_tv, s_settings, LV_ANIM_OFF);
    s_next_settings_tick = 0;
    update_chrome(3.0f);
    assert(lv_obj_get_style_bg_color(s_dots[2], 0).red == lv_color_hex(COLOR_ACCENT).red);
    assert(!muse_ui_face_active());
    muse_ui_show_face();
    lv_obj_update_layout(lv_screen_active());
    assert(lv_tileview_get_tile_active(s_tv) == s_face);
    /* The hardware bench fixture must truncate and keep the end reachable. */
    assert(muse_latest_reply_bench("ui.reply=test-long"));
    muse_latest_reply_tick();
    advance_reply(420);
    assert(strlen(lv_label_get_text(s_label)) == MUSE_REVIEW_CAP - 1);
    assert(!strcmp(lv_label_get_text(s_label) + MUSE_REVIEW_CAP - 4, "..."));
    assert(muse_latest_reply_bench("ui.reply=end"));
    muse_latest_reply_tick();
    assert(lv_obj_get_scroll_bottom(s_scroll) == 0);
    int full_y = lv_obj_get_scroll_y(s_scroll);
    muse_ui_show_face();
    open_latest();
    assert(lv_obj_get_scroll_y(s_scroll) == full_y);
    /* Snapshot copies must never mix two answers during a concurrent store. */
    memset(concurrent_text, 'a', sizeof(concurrent_text) - 1);
    muse_review_store(muse_review_turn(), concurrent_text);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, writer, NULL));
    static char copy[MUSE_REVIEW_CAP];
    for (int i = 0; i < 400; i++) {
        muse_review_snapshot_t snapshot;
        muse_review_snapshot(copy, sizeof(copy), &snapshot);
        assert(snapshot.available && strlen(copy) == sizeof(concurrent_text) - 1);
        for (size_t j = 0; j < strlen(copy); j++) assert(copy[j] == copy[0]);
    }
    assert(!pthread_join(thread, NULL));
    return 0;
}
