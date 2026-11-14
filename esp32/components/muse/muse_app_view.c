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

#include "muse_app_view.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "muse_board.h"
#include "muse_link.h"
#include "muse_state.h"
#include "muse_voice.h"

static const char *TAG = "muse_app_view";

#define LONG_PRESS_US (600 * 1000)
#define EXIT_HOLD_US (2000 * 1000)   /* the exit button held this long closes the app */

static atomic_bool s_open;          /* the root exists and covers the face */
static atomic_uint s_app_down;      /* held buttons (bit per muse_app_view_buttons() row) whose press the app got */
static atomic_bool s_talk_to_muse;  /* the talk key records for Muse, not the app */
static muse_app_view_cbs_t s_cbs;
static lv_obj_t *s_root;            /* under LVGL's lock */
static bool s_touch_down;           /* LVGL's task */
static esp_timer_handle_t s_long_timer;
static atomic_int s_long_button;    /* the button whose long press is pending, 0 for none */
static esp_timer_handle_t s_exit_timer;
static atomic_bool s_exit_swallow;  /* the hold closed the app: its release goes to nobody */
static _Atomic(muse_app_view_nav_fn) s_nav;  /* the app's keypad navigation, if on */

void muse_app_view_lock(void)
{
    muse_board->display_lock(-1);
}

void muse_app_view_unlock(void)
{
    muse_board->display_unlock();
}

lv_obj_t *muse_app_view_root(void)
{
    return s_root;
}

static void emit(int type, int a, int b)
{
    if (s_cbs.input && atomic_load(&s_open)) {
        s_cbs.input(type, a, b);
    }
}

static void on_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (indev) {
        lv_indev_get_point(indev, &p);
    }
    muse_state_poke();   /* touch keeps the screen awake, as on the face */
    if (code == LV_EVENT_PRESSED) {
        s_touch_down = true;
        emit(MUSE_APP_EV_TOUCH_DOWN, p.x, p.y);
    } else if (code == LV_EVENT_PRESSING && s_touch_down) {
        emit(MUSE_APP_EV_TOUCH_MOVE, p.x, p.y);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && s_touch_down) {
        s_touch_down = false;
        emit(MUSE_APP_EV_TOUCH_UP, p.x, p.y);
    }
}

static void on_gesture(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    switch (lv_indev_get_gesture_dir(indev)) {
    case LV_DIR_LEFT: emit(MUSE_APP_EV_GESTURE, MUSE_APP_GESTURE_LEFT, 0); break;
    case LV_DIR_RIGHT: emit(MUSE_APP_EV_GESTURE, MUSE_APP_GESTURE_RIGHT, 0); break;
    case LV_DIR_TOP: emit(MUSE_APP_EV_GESTURE, MUSE_APP_GESTURE_UP, 0); break;
    case LV_DIR_BOTTOM: emit(MUSE_APP_EV_GESTURE, MUSE_APP_GESTURE_DOWN, 0); break;
    default: break;
    }
}

static void on_long(void *arg)
{
    (void)arg;
    int button = atomic_exchange(&s_long_button, 0);
    if (button) {
        emit(MUSE_APP_EV_BUTTON, button, MUSE_APP_BTN_LONG);
    }
}

/* The exit button held for EXIT_HOLD_US: as the power key, on every board. */
static void on_exit_hold(void *arg)
{
    (void)arg;
    if (atomic_load(&s_open) && s_cbs.exit) {
        atomic_store(&s_exit_swallow, true);
        atomic_store(&s_long_button, 0);
        s_cbs.exit(MUSE_APP_STOP_USER);
    }
}

int muse_app_view_exit_button(void)
{
    return MUSE_APP_BUTTON_PRIMARY;
}

bool muse_app_view_open(const muse_app_view_cbs_t *cbs)
{
    if (!muse_board || !muse_board->display_lock) {
        return false;
    }
    if (!s_long_timer) {
        const esp_timer_create_args_t args = { .callback = on_long, .name = "app_long" };
        if (esp_timer_create(&args, &s_long_timer) != ESP_OK) {
            return false;
        }
    }
    if (!s_exit_timer) {
        const esp_timer_create_args_t args = { .callback = on_exit_hold, .name = "app_exit" };
        if (esp_timer_create(&args, &s_exit_timer) != ESP_OK) {
            return false;
        }
    }
    muse_app_view_lock();
    if (s_root) {
        lv_obj_delete(s_root);
    }
    s_root = lv_obj_create(lv_screen_active());
    if (s_root) {
        lv_obj_remove_style_all(s_root);
        lv_obj_set_pos(s_root, 0, 0);
        lv_obj_set_size(s_root, muse_board->width, muse_board->height);
        /* Opaque, so LVGL starts drawing an area from here instead of the face. */
        lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_root, lv_color_white(), 0);
        lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_root, on_touch, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_root, on_touch, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(s_root, on_touch, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_root, on_touch, LV_EVENT_PRESS_LOST, NULL);
        lv_obj_add_event_cb(s_root, on_gesture, LV_EVENT_GESTURE, NULL);
        lv_obj_move_foreground(s_root);
        s_touch_down = false;
        s_cbs = *cbs;
        atomic_store(&s_talk_to_muse, false);
        /* A press held from before (another app's) is no longer this view's:
         * neither its release nor its hold timers. */
        atomic_store(&s_app_down, 0);
        atomic_store(&s_long_button, 0);
        esp_timer_stop(s_long_timer);
        esp_timer_stop(s_exit_timer);
        atomic_store(&s_open, true);
        /* A call that started meanwhile found no view to stop (it checks after
         * it's on, we after we are): its hang-up buttons stay its own. */
        if (muse_voice_in_call()) {
            atomic_store(&s_open, false);
            lv_obj_delete(s_root);
            s_root = NULL;
        }
    }
    muse_app_view_unlock();
    if (!s_root && !muse_voice_in_call()) {
        ESP_LOGW(TAG, "no memory for the app's screen");
    }
    return s_root != NULL;
}

void muse_app_view_close(void)
{
    atomic_store(&s_open, false);
    atomic_store(&s_nav, NULL);
    atomic_store(&s_long_button, 0);
    if (s_long_timer) {
        esp_timer_stop(s_long_timer);
    }
    if (s_exit_timer) {
        esp_timer_stop(s_exit_timer);
    }
    if (!muse_board || !muse_board->display_lock) {
        return;
    }
    muse_app_view_lock();
    if (s_root) {
        lv_obj_delete(s_root);   /* and every object the app made */
        s_root = NULL;
        lv_obj_invalidate(lv_screen_active());
    }
    muse_app_view_unlock();
}

bool muse_app_view_is_open(void)
{
    return atomic_load(&s_open);
}

void muse_app_view_set_nav(muse_app_view_nav_fn fn)
{
    atomic_store(&s_nav, fn);
}

void muse_app_view_talk_to_muse(bool on)
{
    atomic_store(&s_talk_to_muse, on);
}

void muse_app_view_size(int *w, int *h, bool *round)
{
    *w = muse_board ? muse_board->width : 0;
    *h = muse_board ? muse_board->height : 0;
    *round = muse_board && muse_board->round;
}

unsigned muse_app_view_buttons_present(void)
{
    if (!muse_board) {
        return 0;
    }
    unsigned b = 1u << MUSE_APP_BUTTON_PRIMARY;
    /* Both arrive as SECONDARY. An aux label naming the talk button (one
     * button doing both, as on the JC3248W535) isn't a second button. */
    const char *aux = muse_board->aux_button, *talk = muse_board->talk_button;
    if ((aux && !(talk && !strcmp(aux, talk))) || muse_board->speaker_button) {
        b |= 1u << MUSE_APP_BUTTON_SECONDARY;
    }
    if (muse_board->keyboard) {
        b |= (1u << MUSE_APP_BUTTON_UP) | (1u << MUSE_APP_BUTTON_DOWN) | (1u << MUSE_APP_BUTTON_LEFT) |
             (1u << MUSE_APP_BUTTON_RIGHT) | (1u << MUSE_APP_BUTTON_ENTER) | (1u << MUSE_APP_BUTTON_BACK);
    }
    return b;
}

static void (*_Atomic s_home)(void);

void muse_app_view_set_home(void (*open)(void))
{
    atomic_store(&s_home, open);
}

bool muse_app_view_home(void)
{
    void (*open)(void) = atomic_load(&s_home);
    if (!open) {
        return false;
    }
    open();
    return true;
}

void muse_app_view_preempt(int reason)
{
    if (atomic_load(&s_open) && s_cbs.exit) {
        s_cbs.exit(reason);
    }
}

bool muse_app_view_tick(void)
{
    return atomic_load(&s_open);
}

/* ---- Buttons (the input task) ---- */

static void button_edge(int button, bool press, bool release)
{
    muse_state_poke();
    bool exit_button = button == muse_app_view_exit_button();
    if (press) {
        if (exit_button) {
            atomic_store(&s_exit_swallow, false);
        }
        emit(MUSE_APP_EV_BUTTON, button, MUSE_APP_BTN_DOWN);
        if (!release && s_long_timer) {
            atomic_store(&s_long_button, button);
            esp_timer_stop(s_long_timer);
            esp_timer_start_once(s_long_timer, LONG_PRESS_US);
        }
        if (!release && exit_button && s_exit_timer) {
            esp_timer_stop(s_exit_timer);
            esp_timer_start_once(s_exit_timer, EXIT_HOLD_US);
        }
    }
    if (release) {
        if (atomic_load(&s_long_button) == button) {
            atomic_store(&s_long_button, 0);
            esp_timer_stop(s_long_timer);
        }
        if (exit_button && s_exit_timer) {
            esp_timer_stop(s_exit_timer);
            if (atomic_exchange(&s_exit_swallow, false)) {
                return;   /* the hold closed the app: not a press for whatever is shown now */
            }
        }
        emit(MUSE_APP_EV_BUTTON, button, MUSE_APP_BTN_UP);
    }
}

unsigned muse_app_view_buttons(unsigned ev)
{
    /* A pairing prompt (drawn above the app) still takes its confirming press. */
    if (!atomic_load(&s_open) || muse_link_state() == MUSE_LINK_CONFIRM) {
        return ev;
    }
    /* Buttons with a press and a release edge. */
    static const struct {
        unsigned press, release;
        int button;
    } held[] = {
        { MUSE_BTN_TALK_PRESS, MUSE_BTN_TALK_RELEASE, MUSE_APP_BUTTON_PRIMARY },
        { MUSE_BTN_SPEAKER_PRESS, MUSE_BTN_SPEAKER_RELEASE, MUSE_APP_BUTTON_SECONDARY },
        { MUSE_BTN_AUX_PRESS, MUSE_BTN_AUX_RELEASE, MUSE_APP_BUTTON_SECONDARY },
    };
    /* Navigation keys report presses only. */
    static const struct {
        unsigned bit;
        int button;
    } keys[] = {
        { MUSE_BTN_UP, MUSE_APP_BUTTON_UP },       { MUSE_BTN_DOWN, MUSE_APP_BUTTON_DOWN },
        { MUSE_BTN_LEFT, MUSE_APP_BUTTON_LEFT },   { MUSE_BTN_RIGHT, MUSE_APP_BUTTON_RIGHT },
        { MUSE_BTN_ENTER, MUSE_APP_BUTTON_ENTER }, { MUSE_BTN_ESCAPE, MUSE_APP_BUTTON_BACK },
    };
    bool talk_to_muse = atomic_load(&s_talk_to_muse);
    for (size_t i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
        if (talk_to_muse && held[i].button == MUSE_APP_BUTTON_PRIMARY) {
            if (ev & (held[i].press | held[i].release)) {
                /* The firmware's gesture now: its release must reach the
                 * firmware even if the app takes the key back meanwhile, and
                 * the app's hold timers for it stop. */
                atomic_fetch_and(&s_app_down, ~(1u << i));
                if (atomic_load(&s_long_button) == held[i].button) {
                    atomic_store(&s_long_button, 0);
                    esp_timer_stop(s_long_timer);
                }
                if (held[i].button == muse_app_view_exit_button() && s_exit_timer) {
                    esp_timer_stop(s_exit_timer);
                }
            }
            continue;   /* left for Muse's push-to-talk */
        }
        unsigned bits = ev & (held[i].press | held[i].release);
        if (!bits) {
            continue;
        }
        unsigned row = 1u << i;
        if (!(bits & held[i].press) && !(atomic_load(&s_app_down) & row)) {
            /* Released after a press the app never got (one made before it
             * opened, holding push-to-talk or recording): the firmware's, so
             * the mic closes as it would without the app. */
            continue;
        }
        ev &= ~bits;
        if ((bits & held[i].press) && muse_state_asleep()) {
            muse_state_set_asleep(false); /* a press on a dark screen only wakes it */
            continue;
        }
        if (bits & held[i].press) {
            atomic_fetch_or(&s_app_down, row);
        }
        if (bits & held[i].release) {
            atomic_fetch_and(&s_app_down, ~row);
        }
        button_edge(held[i].button, bits & held[i].press, bits & held[i].release);
    }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (!(ev & keys[i].bit)) {
            continue;
        }
        ev &= ~keys[i].bit;
        if (muse_state_asleep()) {
            muse_state_set_asleep(false);
            continue;
        }
        muse_app_view_nav_fn nav = atomic_load(&s_nav);
        if (nav && nav(keys[i].button)) {
            muse_state_poke();   /* taken by the app's navigation */
            continue;
        }
        button_edge(keys[i].button, true, true);
    }
    return ev;
}

bool muse_app_view_power(void)
{
    if (!atomic_load(&s_open)) {
        return false;
    }
    if (s_cbs.exit) {
        s_cbs.exit(MUSE_APP_STOP_USER);
    }
    return true;
}
