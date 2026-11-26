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

/*
 * The Confirm disc through LVGL's real input path: a finger's samples go to the
 * touch indev's read callback, lv_indev_read() dispatches them as on the
 * gadget, and muse_ui.c's own callbacks take them. That covers on_pair_ok for
 * the setup and re-setup prompt (an object event on the disc), and
 * on_any_press for the update card (an indev event: the point comes from
 * lv_indev_active()). A tap on the disc (a press and its release on it)
 * confirms each one, at the release; a press that slides off the disc before
 * it lifts confirms nothing; a tap elsewhere confirms nothing, and on the
 * update card it cancels the update. On a screen
 * that went dark under the setup card, the waking tap lands on the sleep
 * cover and confirms nothing; the next tap, once lit, confirms. The asking
 * update card never goes dark. An injected touch (a bench press: muse_touch_real() false) confirms nothing, a
 * setup's, a re-setup's or the update's, in a release build (this one: no test or automation image). Built with
 * CONFIG_MUSE_GADGET_BLE_AUDIO, the BLE gadget's UI; its update and press
 * state are the fakes below.
 */
#undef NDEBUG
#define CONFIG_MUSE_GADGET_BLE_AUDIO 1
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "sim_board.h"
#include "sim_platform.h"
#include "sim_services.h"

static int confirms;
static bool test_confirm_key_press(void);
static bool touch_real = true;   /* the touch is a finger's, not a bench press */
static bool test_touch_real(void) { return touch_real; }
#define muse_link_confirm_key_press test_confirm_key_press   /* the setup prompt's confirm, counted */
#define muse_touch_real test_touch_real
#include "../../components/muse/muse_ui.c"
#undef muse_touch_real
#undef muse_link_confirm_key_press

static bool test_confirm_key_press(void)
{
    if (muse_link_state() != MUSE_LINK_CONFIRM) {
        return false;
    }
    confirms++;
    return true;
}

/* The update prompt: mg_ble.c's state for the card, muse_local_press.c's press. */
static uint8_t s_upd_state;
static bool s_prompting;
static int notes, cancels;
uint8_t mg_ble_update_ui(uint8_t *pct) { if (pct) *pct = 0; return s_upd_state; }
void mg_ble_update_ui_clear(void) { s_upd_state = 0; }
const char *mg_ble_update_stop_text(void) { return "Nothing was installed"; }
void mg_ble_update_ui_shown(uint8_t state) { (void)state; }
bool mg_ble_ptt_ready(void) { return false; }
bool muse_local_press_prompting(void) { return s_prompting; }
bool muse_local_press_note(const char *what) { (void)what; if (!s_prompting) return false; notes++; s_prompting = false; return true; }
void muse_local_press_cancel(const char *what) { (void)what; if (s_prompting) { cancels++; s_prompting = false; } }

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
    lv_obj_update_layout(lv_layer_top());
}

static void tap(int x, int y)
{
    touch(x, y, true);
    touch(x, y, true);
    touch(x, y, false);
}

/* A finger down on the disc's center, dragged slowly (no swipe) to just past the disc's hit area, lifted there. */
static void slide_off(void)
{
    lv_area_t a, hit;
    lv_obj_get_coords(s_pair_code, &a);
    lv_obj_get_click_area(s_pair_code, &hit);
    int x = (a.x1 + a.x2) / 2, y = (a.y1 + a.y2) / 2, to = hit.y2 + 4;
    assert(to < lv_display_get_vertical_resolution(NULL));
    touch(x, y, true);
    touch(x, y, true);
    for (; y < to; y += 2) {
        touch(x, y, true);
    }
    touch(x, to, true);
    touch(x, to, false);
}

/* A tap on a dark screen, with the UI's frames running between its samples as on the gadget. */
static void tap_ticking(int x, int y)
{
    touch(x, y, true);
    frame_tick(NULL);
    touch(x, y, true);
    frame_tick(NULL);
    touch(x, y, false);
    frame_tick(NULL);
}

/* The screen goes dark under the card (the sleep timer, the power key): the next frame darkens it. */
static void go_dark(void)
{
    muse_state_set_asleep(true);
    frame_tick(NULL);
    assert(s_dark && !lv_obj_has_flag(s_cover, LV_OBJ_FLAG_HIDDEN));
}

/* The chrome's next 4 Hz pass (the card, its Confirm and whether it's on show). */
static void chrome(void)
{
    static float now = 1.0f;
    now += 1.0f;
    update_chrome(now);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_update_layout(lv_layer_top());
}

static void disc_center(int *x, int *y)
{
    lv_area_t a;
    lv_obj_get_coords(s_pair_code, &a);
    *x = (a.x1 + a.x2) / 2;
    *y = (a.y1 + a.y2) / 2;
}

/* A point well outside the disc and on screen: a corner. */
static void elsewhere(int *x, int *y)
{
    *x = 2;
    *y = 2;
    lv_area_t a;
    lv_obj_get_coords(s_pair_code, &a);
    assert(!lv_area_is_point_on(&a, &(lv_point_t){*x, *y}, 0));
}

int main(int argc, char **argv)
{
    sim_time_reset();
    sim_services_reset();
    lv_init();
    muse_state_init();
    muse_board_t *b = (muse_board_t *)sim_board_get();
    assert(b->touch);
    if (argc == 4) {   /* a panel: width height round */
        b->width = atoi(argv[1]);
        b->height = atoi(argv[2]);
        b->round = atoi(argv[3]) != 0;
    }
    muse_board = b;
    assert(muse_ui_start() == ESP_OK);
    muse_state_set_asleep(false);
    lv_indev_set_read_cb(s_indev, read_touch);
    lv_obj_update_layout(lv_screen_active());
    int x, y;

    /* A first setup and a re-setup alike: the prompt (link confirm) puts the disc up; a tap on it confirms once, a
     * tap elsewhere doesn't. */
    for (int set_up = 0; set_up < 2; set_up++) {
        sim_services_set_paired(set_up);
        sim_services_set_link_state(MUSE_LINK_CONFIRM);
        chrome();
        assert(!lv_obj_has_flag(s_pair, LV_OBJ_FLAG_HIDDEN) && s_pair_ok == PAIR_OK_SETUP);
        confirms = 0;
        elsewhere(&x, &y);
        tap(x, y);
        assert(confirms == 0);
        /* A press that slides off the disc before it lifts confirms nothing. */
        slide_off();
        assert(confirms == 0);
        /* The tap confirms at its release, not its touch-down. */
        disc_center(&x, &y);
        touch(x, y, true);
        touch(x, y, true);
        assert(confirms == 0);
        touch(x, y, false);
        assert(confirms == 1);
        /* The screen went dark under the prompt: a tap on where the disc is wakes the screen and confirms nothing
         * (a prompt you can't see isn't shown); the next tap, lit, confirms. */
        go_dark();
        disc_center(&x, &y);
        tap_ticking(x, y);
        assert(confirms == 1 && !muse_state_asleep() && !s_dark);
        chrome();
        assert(!lv_obj_has_flag(s_pair, LV_OBJ_FLAG_HIDDEN) && s_pair_ok == PAIR_OK_SETUP);
        tap(x, y);
        assert(confirms == 2);
        sim_services_set_link_state(MUSE_LINK_ONLINE);
        chrome();
        assert(lv_obj_has_flag(s_pair, LV_OBJ_FLAG_HIDDEN) && s_pair_ok == PAIR_OK_NONE);
    }

    /* An injected touch on the disc (a bench press, over any link) confirms neither a first setup nor a re-setup;
     * the finger's next tap does. */
    for (int set_up = 0; set_up < 2; set_up++) {
        sim_services_set_paired(set_up);
        sim_services_set_link_state(MUSE_LINK_CONFIRM);
        chrome();
        assert(!lv_obj_has_flag(s_pair, LV_OBJ_FLAG_HIDDEN) && s_pair_ok == PAIR_OK_SETUP);
        confirms = 0;
        disc_center(&x, &y);
        touch_real = false;
        tap(x, y);
        assert(confirms == 0);
        touch_real = true;
        tap(x, y);
        assert(confirms == 1);
        sim_services_set_link_state(MUSE_LINK_ONLINE);
        chrome();
    }

    /* The update card: the disc's tap is the update's (on_any_press, the indev's point), never a setup confirm; a
     * tap elsewhere cancels it. */
    s_upd_state = MG_BLE_UPDATE_UI_ASK;
    s_prompting = true;
    chrome();
    assert(!lv_obj_has_flag(s_pair, LV_OBJ_FLAG_HIDDEN) && s_pair_ok == PAIR_OK_UPDATE && atomic_load(&s_ask_shown));
    confirms = 0;
    /* A press on the disc that slides off before it lifts neither confirms nor cancels it. */
    slide_off();
    assert(notes == 0 && cancels == 0 && confirms == 0 && s_prompting);
    /* A press on the disc that went down while the card wasn't on show (the video view over it, the screen dark)
     * confirms nothing, even released once it is. */
    disc_center(&x, &y);
    atomic_store(&s_ask_shown, false);
    touch(x, y, true);
    touch(x, y, true);
    chrome();
    assert(atomic_load(&s_ask_shown));
    touch(x, y, false);
    assert(notes == 0 && cancels == 0 && s_prompting);
    /* The tap confirms at its release, not its touch-down. */
    touch(x, y, true);
    touch(x, y, true);
    assert(notes == 0 && cancels == 0);
    touch(x, y, false);
    assert(notes == 1 && cancels == 0 && confirms == 0);
    s_prompting = true;   /* asked again */
    chrome();
    elsewhere(&x, &y);
    tap(x, y);
    assert(notes == 1 && cancels == 1 && confirms == 0);
    /* The asking card never goes dark: the screen's sleep (the timer, the power key) is undone by the next frame
     * (update_card_wake), so the card stays on show and its disc's tap confirms. */
    s_prompting = true;   /* asked again */
    chrome();
    assert(atomic_load(&s_ask_shown));
    muse_state_set_asleep(true);
    frame_tick(NULL);
    assert(!muse_state_asleep() && !s_dark && lv_obj_has_flag(s_cover, LV_OBJ_FLAG_HIDDEN));
    chrome();
    assert(atomic_load(&s_ask_shown));
    disc_center(&x, &y);
    tap(x, y);
    assert(notes == 2 && cancels == 1 && confirms == 0);
    /* An injected touch neither confirms nor cancels it. */
    s_prompting = true;   /* asked again */
    chrome();
    touch_real = false;
    disc_center(&x, &y);
    tap(x, y);
    elsewhere(&x, &y);
    tap(x, y);
    assert(notes == 2 && cancels == 1 && confirms == 0 && s_prompting);
    touch_real = true;
    puts("PASS confirm disc");
    return 0;
}
