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
#include "muse_mirror.h"
#include "muse_mirror_session.h"
#include "muse_board.h"
#include "muse_input.h"
#include "muse_keypad.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "esp_log.h"
#include <string.h>

#define KEYS 11
static int s_w, s_h;
static const unsigned s_press[KEYS] = {
    0, MUSE_BTN_TALK_PRESS, MUSE_BTN_AUX_PRESS, MUSE_BTN_POWER_PRESS, MUSE_BTN_SPEAKER_PRESS,
    MUSE_BTN_UP, MUSE_BTN_DOWN, MUSE_BTN_LEFT, MUSE_BTN_RIGHT, MUSE_BTN_ENTER, MUSE_BTN_ESCAPE,
};
static const unsigned s_release[KEYS] = {
    0, MUSE_BTN_TALK_RELEASE, MUSE_BTN_AUX_RELEASE, MUSE_BTN_POWER_RELEASE, MUSE_BTN_SPEAKER_RELEASE,
};

static void flush_start(lv_event_t *e) {
    lv_display_t *disp = lv_event_get_target(e);
    const lv_area_t *area = lv_event_get_param(e);
    lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
    if (!buf || !area) return;
    bool swapped = buf->header.cf == LV_COLOR_FORMAT_RGB565_SWAPPED;
    if (buf->header.cf != LV_COLOR_FORMAT_RGB565 && !swapped) return;
    uint16_t *fb = muse_mirror_frame_begin();
    if (!fb) return;
    int w = lv_area_get_width(area), h = lv_area_get_height(area);
    bool whole = lv_display_get_render_mode(disp) != LV_DISPLAY_RENDER_MODE_PARTIAL;   /* a screen-sized buffer */
    int x0 = area->x1 < 0 ? 0 : area->x1, x1 = area->x2 >= s_w ? s_w - 1 : area->x2;
    for (int y = 0; y < h && x1 >= x0; y++) {
        int ty = area->y1 + y;
        if (ty < 0 || ty >= s_h) continue;
        const uint8_t *row = buf->data + (whole ? ty : y) * buf->header.stride + (whole ? x0 : x0 - area->x1) * 2;
        uint16_t *to = fb + ty * s_w + x0;
        if (!swapped) {
            memcpy(to, row, (x1 - x0 + 1) * 2);
        } else {
            for (int i = 0; i <= x1 - x0; i++) to[i] = (uint16_t)(row[2 * i] << 8 | row[2 * i + 1]);
        }
    }
    muse_mirror_frame_end(area->x1, area->y1, w, h, lv_display_flush_is_last(disp));
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    int x, y; bool down;
    data->continue_reading = muse_mirror_pointer_read(&x, &y, &down);
    data->point.x = x; data->point.y = y;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void hint_point(const muse_button_hint_t *hint, int *x, int *y)
{
    int ax, ay;
    switch (hint->align) {
    case LV_ALIGN_TOP_LEFT: ax = 0; ay = 0; break;
    case LV_ALIGN_TOP_MID: ax = s_w / 2; ay = 0; break;
    case LV_ALIGN_TOP_RIGHT: ax = s_w; ay = 0; break;
    case LV_ALIGN_BOTTOM_LEFT: ax = 0; ay = s_h; break;
    case LV_ALIGN_BOTTOM_MID: ax = s_w / 2; ay = s_h; break;
    case LV_ALIGN_BOTTOM_RIGHT: ax = s_w; ay = s_h; break;
    case LV_ALIGN_LEFT_MID: ax = 0; ay = s_h / 2; break;
    case LV_ALIGN_RIGHT_MID: ax = s_w; ay = s_h / 2; break;
    default: ax = s_w / 2; ay = s_h / 2; break;
    }
    *x = ax + hint->x;
    *y = ay + hint->y;
}


static void key(int role, bool down)
{
    if (role < 1 || role >= KEYS) return;
    unsigned ev = down ? s_press[role] : s_release[role];
    if (ev) muse_input_inject(ev);
    if (down) muse_state_poke();
}

/* The focused, visible textarea under obj; with ta, the visible keyboard typing into ta. */
static lv_obj_t *text_find(lv_obj_t *obj, lv_obj_t *ta) {
    if (!obj || !lv_obj_is_visible(obj)) return NULL;
    if (!ta && lv_obj_check_type(obj, &lv_textarea_class) && lv_obj_has_state(obj, LV_STATE_FOCUSED)) return obj;
    if (ta && lv_obj_check_type(obj, &lv_keyboard_class) && lv_keyboard_get_textarea(obj) == ta) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = text_find(lv_obj_get_child(obj, i), ta);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *text_on_display(lv_display_t *disp, lv_obj_t *ta) {
    lv_obj_t *found = text_find(lv_display_get_layer_top(disp), ta);
    return found ? found : text_find(lv_display_get_screen_active(disp), ta);
}

/* Enter as the field's own keyboard would take it: its OK key, or a new line
 * unless the field is one line, which then reports it is done. */
static void text_enter(lv_display_t *disp, lv_obj_t *ta, lv_obj_t *kp) {
    if (kp) {
        lv_obj_send_event(kp, LV_EVENT_READY, NULL);
        return;
    }
    lv_obj_t *kb = text_on_display(disp, ta);
    if (kb) {
        if (lv_obj_send_event(kb, LV_EVENT_READY, NULL) == LV_RESULT_OK && (ta = lv_keyboard_get_textarea(kb))) {
            lv_obj_send_event(ta, LV_EVENT_READY, NULL);
        }
        return;
    }
    if (lv_textarea_get_one_line(ta)) lv_obj_send_event(ta, LV_EVENT_READY, NULL);
    else lv_textarea_add_char(ta, '\n');
}

/* In LVGL's task, like every LVGL call: whether a field is open, and the
 * viewer's typing into it. No tree walks while nobody watches. */
static void text_update(lv_display_t *disp) {
    for (int i = 0; i < 8; i++) {
        lv_obj_t *ta = text_on_display(disp, NULL);
        muse_mirror_text_open(ta != NULL);
        muse_mirror_text_t m;
        if (!muse_mirror_text_read(&m)) break;
        if (ta) {
            lv_obj_t *kp = muse_keypad_for_textarea(ta);
            switch (m.edit) {
            case MUSE_MIRROR_TEXT_INSERT: lv_textarea_add_text(ta, m.text); break;
            case MUSE_MIRROR_TEXT_BACKSPACE: lv_textarea_delete_char(ta); break;
            case MUSE_MIRROR_TEXT_DELETE: lv_textarea_delete_char_forward(ta); break;
            case MUSE_MIRROR_TEXT_LEFT: lv_textarea_cursor_left(ta); break;
            case MUSE_MIRROR_TEXT_RIGHT: lv_textarea_cursor_right(ta); break;
            case MUSE_MIRROR_TEXT_ENTER: text_enter(disp, ta, kp); break;
            }
        }
        memset(&m, 0, sizeof(m));
    }
}

static void capture_tick(lv_timer_t *timer)
{
    lv_display_t *disp = lv_timer_get_user_data(timer);
    bool watched = muse_mirror_connected();
    lv_timer_set_period(timer, watched ? 20 : 250);
    if (!watched) {
        muse_mirror_text_open(false);
        return;
    }
    muse_mirror_set_screen_state(muse_state_asleep() ? 2 : 0);
    unsigned generation;
    if (muse_mirror_capture_begin(&generation)) {
        lv_obj_invalidate(lv_display_get_screen_active(disp));
        lv_refr_now(disp);
        muse_mirror_capture_ready(generation);
    }
    text_update(disp);
}

lv_indev_t *muse_mirror_start(lv_display_t *disp)
{
    s_w = lv_display_get_horizontal_resolution(disp);
    s_h = lv_display_get_vertical_resolution(disp);
    muse_mirror_button_t buttons[4];
    size_t count = 0;
    struct { const char *name; const muse_button_hint_t *hint; } keys[] = {
        { muse_board->touch_talk ? NULL : muse_board->talk_button, &muse_board->talk_hint },
        { muse_board->aux_button, &muse_board->aux_hint },
        { muse_board->power_button, &muse_board->power_hint },
        { muse_board->speaker_button, &muse_board->speaker_hint },
    };
    for (size_t i = 0; i < 4; i++) {
        if (!keys[i].name) continue;
        muse_mirror_button_t *b = &buttons[count++];
        *b = (muse_mirror_button_t){ .role = i + 1, .name = keys[i].name, .x = -1, .y = -1 };
        if (keys[i].hint->align != LV_ALIGN_DEFAULT) hint_point(keys[i].hint, &b->x, &b->y);
    }
    const muse_mirror_config_t config = {
        .name = muse_board->name, .width = s_w, .height = s_h,
        .round = muse_board->round, .diagonal = muse_board->diagonal_in,
        .touch = muse_board->touch, .keyboard = muse_board->keyboard,
        .audio = muse_board->audio_init || muse_board->mic_init,
#if CONFIG_MUSE_CONSOLE_UART
        .uart = true,
#endif
        .buttons = buttons, .button_count = count, .on_key = key,
    };
    lv_timer_t *timer = lv_timer_create(capture_tick, 250, disp);
    lv_indev_t *pointer = muse_board->touch ? lv_indev_create() : NULL;
    if (!timer || (muse_board->touch && !pointer) || muse_mirror_session_start(&config) != ESP_OK) {
        if (timer) lv_timer_delete(timer);
        if (pointer) lv_indev_delete(pointer);
        ESP_LOGE("muse_mirror", "mirror unavailable");
        return NULL;
    }
    lv_display_add_event_cb(disp, flush_start, LV_EVENT_FLUSH_START, NULL);
    if (pointer) {
        lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(pointer, disp);
        lv_indev_set_read_cb(pointer, pointer_read);
    }
    return pointer;
}
