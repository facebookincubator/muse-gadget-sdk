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
#include <string.h>
#include "muse_mirror_session.h"
#include "muse_keypad.c"

static muse_mirror_text_t edits[16];
static int queued, at, ready;
static bool field_open;
void muse_mirror_text_open(bool open) { field_open = open; }
bool muse_mirror_text_read(muse_mirror_text_t *out)
{
    if (at == queued) return false;
    *out = edits[at++];
    return true;
}
#include "text_ui.inc"

static void edit(lv_display_t *disp, int code, const char *text)
{
    edits[0] = (muse_mirror_text_t){.edit = code};
    if (text) strcpy(edits[0].text, text);
    at = 0; queued = 1;
    text_update(disp);
    assert(at == queued);
}

static void done(lv_event_t *e)
{
    (void)e;
    ready++;
}

static void delete_page(lv_event_t *e)
{
    ready++;
    lv_obj_delete(lv_event_get_user_data(e));
}

static void expect(lv_obj_t *ta, const char *text)
{
    assert(!strcmp(lv_textarea_get_text(ta), text));
}

int main(void)
{
    lv_init();
    lv_display_t *disp = lv_display_create(240, 320);
    lv_obj_t *screen = lv_display_get_screen_active(disp);
    lv_obj_t *page = lv_obj_create(screen);
    lv_obj_set_size(page, 240, 320);
    lv_obj_t *ta = lv_textarea_create(page);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_textarea_set_text(ta, "");
    lv_obj_t *kp = muse_keypad_create(page, ta, false);
    lv_obj_set_size(kp, 220, 160);
    lv_obj_align(kp, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_update_layout(screen);

    type_key(1); expect(ta, "a");
    assert(s_pending == 1);
    edit(disp, MUSE_MIRROR_TEXT_INSERT, "X");
    assert(field_open && s_pending == -1);
    expect(ta, "aX");
    type_key(1); expect(ta, "aXa");
    edit(disp, MUSE_MIRROR_TEXT_LEFT, NULL);
    type_key(1); expect(ta, "aXaa");
    edit(disp, MUSE_MIRROR_TEXT_BACKSPACE, NULL); expect(ta, "aXa");
    edit(disp, MUSE_MIRROR_TEXT_DELETE, NULL); expect(ta, "aX");
    edit(disp, MUSE_MIRROR_TEXT_RIGHT, NULL);
    edit(disp, MUSE_MIRROR_TEXT_INSERT, "\xc3\xa9\xf0\x9f\x99\x82");
    expect(ta, "aX\xc3\xa9\xf0\x9f\x99\x82");

    lv_textarea_set_text(ta, "");
    lv_textarea_set_accepted_chars(ta, "abc");
    lv_textarea_set_max_length(ta, 2);
    edit(disp, MUSE_MIRROR_TEXT_INSERT, "aXbc"); expect(ta, "ab");
    lv_textarea_set_accepted_chars(ta, NULL);
    lv_textarea_set_max_length(ta, 100);

    lv_obj_t *top = lv_textarea_create(lv_display_get_layer_top(disp));
    lv_obj_add_state(top, LV_STATE_FOCUSED);
    lv_textarea_set_text(top, "");
    lv_obj_update_layout(screen);
    edit(disp, MUSE_MIRROR_TEXT_INSERT, "top"); expect(top, "top"); expect(ta, "ab");
    lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);
    edit(disp, MUSE_MIRROR_TEXT_INSERT, "c"); expect(ta, "abc");
    lv_obj_delete(top);

    /* A compact keypad's Done handler may destroy both keypad and textarea. */
    lv_obj_add_event_cb(kp, delete_page, LV_EVENT_READY, page);
    edit(disp, MUSE_MIRROR_TEXT_ENTER, NULL);
    assert(ready == 1 && s_kp == NULL && !field_open);

    ta = lv_textarea_create(screen);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_textarea_set_text(ta, "line");
    lv_obj_update_layout(screen);
    edit(disp, MUSE_MIRROR_TEXT_ENTER, NULL); expect(ta, "line\n");
    lv_textarea_set_one_line(ta, true);
    lv_obj_add_event_cb(ta, done, LV_EVENT_READY, NULL);
    edit(disp, MUSE_MIRROR_TEXT_ENTER, NULL); assert(ready == 2);
    lv_obj_t *kb = lv_keyboard_create(screen);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, done, LV_EVENT_READY, NULL);
    lv_obj_update_layout(screen);
    edit(disp, MUSE_MIRROR_TEXT_ENTER, NULL); assert(ready == 4);
    lv_obj_add_flag(ta, LV_OBJ_FLAG_HIDDEN);
    edit(disp, MUSE_MIRROR_TEXT_INSERT, "ignored");
    assert(!field_open);
    lv_display_delete(disp);
    lv_deinit();
    return 0;
}
