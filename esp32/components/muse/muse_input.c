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

#include "esp_timer.h"
#include "muse_f2.h"
#include "sdkconfig.h"
#if CONFIG_MUSE_HEAP_WATCH
#include "muse_heapwatch.h"
#endif
#include "muse_bootguard.h"
#include "muse_local_press.h"
#include "muse_restart.h"
#include "muse_wdt.h"
#include "muse_input.h"
#if !CONFIG_MUSE_UI_FREEINK
#include "muse_touch.h"
#else
#include "eink/muse_eink_compat.h"   /* no LVGL: no touch, home or passcode screens */
#endif

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
#include "esp_memory_utils.h"   /* the tasks word */
#include "freertos/freertos_debug.h"   /* the tasks word: each stack's size */
#endif
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#include "esp_sleep.h"
#endif

#include "muse_audio.h"
#include "muse_battery.h"
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_charged.h"
#include "muse_chat.h"
#include "muse_options.h"
#include "muse_console.h"
#include "muse_link.h"
#include "muse_mic_ring.h"
#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
#include "muse_subs.h"
#endif
#if CONFIG_MUSE_VIDEO
#include "muse_video.h"
#endif
#include "muse_mem.h"
#include "muse_motion.h"
#if !CONFIG_MUSE_UI_FREEINK
#include "muse_home.h"
#endif
#include "muse_menu.h"
#if CONFIG_MUSE_MIRROR
#include "muse_mirror.h"
#endif
#include "muse_passcode.h"
#if !CONFIG_MUSE_UI_FREEINK
#include "muse_passcode_ui.h"
#endif
#include "muse_perf.h"
#include "muse_settings.h"
#if CONFIG_MUSE_UI_THEME
#include "muse_theme.h"
#endif
#include "muse_mic_gain_word.h"
#include "muse_vol_word.h"   /* >vol= (the bench volume, RAM only) */
#include "muse_spk_dsp.h"   /* >spk.dsp */
#if !CONFIG_MUSE_UI_FREEINK
#include "muse_settings_ui.h"   /* LVGL only; e-paper builds use FreeInk */
#endif
#include "muse_state.h"
#include "muse_ui.h"
#if CONFIG_MUSE_LATEST_REPLY
#include "muse_latest_reply.h"
#endif
#include "muse_voice.h"
#include "muse_wakeword_glue.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_VIDEO
#include "muse_video.h"
#endif
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER
#include "muse_review.h"
#include "muse_wheel.h"
#endif
#if CONFIG_MUSE_APP_VIEW
#include "muse_app_view.h"
#endif
#if CONFIG_MUSE_WATCHER_CAMERA
#include "boards/watcher_camera.h"
#endif
#if CONFIG_MUSE_GADGET_BLE_AUDIO
#include "mg_haptic_test.h"
#include "mg_minrise.h"   /* >ptt.minrise (the bench guard) */
#include "mg_voice.h"
#endif
#if CONFIG_MUSE_GADGET_BLE_VIDEO || CONFIG_MUSE_GADGET_BLE_LIVE_CALL
#include "mg_ble.h"
#endif
#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
#include "muse_call.h"
#endif

static atomic_uint s_touch_talk_events;
/* The pairing prompt's showing (muse_link_prompt_seq) at the last touch-down, and at the down of the talk press being
 * handled (input task): a press that went down before the prompt up now showed never confirms it. */
static atomic_uint s_touch_down_seq;
static uint32_t s_down_seq;
/* The console's key=: a press of a board button, as poll_buttons reports it
 * (its release comes a poll later). Never a hold: a power key's long press
 * (power off) can't come from here. */
static atomic_uint s_bench_keys;
/* The console's key=talk,MS: a held press of the board's talk key, through
 * the key's own path (talk_key_filter, talk_button) from a dark panel or a lit
 * one, released MS later. Never a pairing's confirming press: the
 * press that confirms a Muse app pairing stays the user's own. */
static atomic_uint s_bench_talk_ms;      /* a press asked for, not yet made */
static atomic_bool s_bench_talk_busy;    /* made, not yet released */
/* key=talk,MS,AFTER_S: holds queued for later (a bench that arms them over
 * the Nordic UART, then lets a phone take the connection). Each starts at its
 * time, or once the hold before it is let go. */
#define BENCH_TALK_QUEUE 4
typedef struct {
    unsigned ms;
    TickType_t due;
} bench_hold_t;
static bench_hold_t s_bench_holds[BENCH_TALK_QUEUE];
static int s_bench_hold_n;
static portMUX_TYPE s_bench_mux = portMUX_INITIALIZER_UNLOCKED;
/* Console lines from another task (the Nordic UART's !IN, muse_input_console_post),
 * run here on the input task: never on the NimBLE host, which must not wait
 * on the UI. A few lines deep; static, so no heap. Only musegadgets BLE
 * (mg_glue.c) posts any. */
#define CONSOLE_POST_LINES 4
#define CONSOLE_POST_LEN 48
#if CONFIG_MUSE_GADGET_BLE_AUDIO
static StaticQueue_t s_post_q;
static uint8_t s_post_buf[CONSOLE_POST_LINES * CONSOLE_POST_LEN];
static QueueHandle_t s_post;
#endif

static void (*s_press_hook)(muse_press_t src);
static bool s_line_wireless;             /* the console line being run came over the Nordic UART */

void muse_input_set_press_hook(void (*hook)(muse_press_t src))
{
    s_press_hook = hook;
}

bool muse_input_line_wireless(void)
{
    return s_line_wireless;
}

/* No person in the loop (muse_input.h): an injected talk press opens the mic there; a release image takes only the
 * gadget's own (muse_input_talk_ok). */
#define MUSE_INJECTED_TALK_OK MUSE_NO_PERSON

bool muse_input_talk_ok(bool real)
{
    return MUSE_INJECTED_TALK_OK || real;   /* a release image: only the gadget's own press */
}

/* The console line being run may press talk (ptt=1, key=talk): the wired console's in every build, since the USB
 * console is fully open and an automated run never waits on a person; a wireless line (the Nordic UART's !IN) only
 * where an injected press may talk (muse_input_talk_ok: test, bench and automation builds). */
static bool line_talk_ok(void)
{
    return !s_line_wireless || muse_input_talk_ok(false);
}

void muse_input_press(muse_press_t src)
{
    if (s_press_hook) {
        s_press_hook(src);
    }
}

void muse_input_touch_down(void)
{
    atomic_store(&s_touch_down_seq, muse_link_prompt_seq());
}

void muse_input_touch_talk(bool pressed)
{
    if (pressed) {
        muse_input_touch_down();   /* called at the touch-down */
    }
    atomic_fetch_or(&s_touch_talk_events, pressed ? MUSE_BTN_TALK_PRESS : MUSE_BTN_TALK_RELEASE);
}

static atomic_bool s_touch_tap;

void muse_input_note_touch_tap(void)
{
    atomic_store(&s_touch_tap, true);
}

bool muse_input_take_touch_tap(void)
{
    return atomic_exchange(&s_touch_tap, false);
}

static TaskHandle_t s_input;

/*
 * Link modes (mgcommands.h "Link modes"): a user's wake or touch
 * on the gadget moves the phone link from sleep to ready (never active) and
 * restarts its 10 min in ready: any key press, a touch, the screen lit by the
 * user, and the bench's injected input alike (automation behaves like a
 * user). A talk press is the gadget's own activity besides (active at once,
 * at touch-down), so it never waits behind a ready request. On the input task.
 */
static void link_user(const char *why)
{
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    mg_ble_link_user(why);
#else
    (void)why;
#endif
}

static void link_talk_press(void)
{
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    mg_ble_link_activity();   /* a talk press: active, before the user's wake below */
#endif
    link_user("key");
}

#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER
#define USER_KEYS_WHEEL (MUSE_BTN_REVIEW_PREV | MUSE_BTN_REVIEW_NEXT)   /* the wheel's turns, a user's too */
#else
#define USER_KEYS_WHEEL 0
#endif
#define USER_KEYS (MUSE_BTN_TALK_PRESS | MUSE_BTN_AUX_PRESS | MUSE_BTN_POWER_PRESS | MUSE_BTN_SPEAKER_PRESS | \
                   MUSE_BTN_UP | MUSE_BTN_DOWN | MUSE_BTN_LEFT | MUSE_BTN_RIGHT | MUSE_BTN_ENTER | MUSE_BTN_ESCAPE | \
                   USER_KEYS_WHEEL)

static void link_user_keys(unsigned ev)
{
    if (ev & MUSE_BTN_TALK_PRESS) {
        link_talk_press();
    } else if (ev & USER_KEYS) {
        link_user("key");
    }
}

static atomic_bool s_user_touch;

void muse_input_user_touch(void)
{
    if (!atomic_exchange(&s_user_touch, true) && s_input) {
        xTaskNotifyGive(s_input);   /* out of wait_buttons(): the input task passes it on */
    }
}

static const char *TAG = "muse_input";

#define POLL_MS 10
#define REST_POLL_MS 50        /* screen off and paused: still quick to wake */
#define REST_WAIT_MS 1000      /* paused, buttons that interrupt: the rest waits this long */
#define NAP_WAIT_MS 10000      /* ... and Wi-Fi napping */
#define POWER_MS 2000          /* refresh battery */
#define REST_POWER_MS 10000    /* ... while paused */
#define WIFI_NAP_MS (2 * 60 * 1000)   /* low power this long: Wi-Fi off until it ends */
#define DOUBLE_TICKS 35        /* 350 ms: a second aux press within this toggles phone setup */

#define GOODBYE_MS 1500        /* let the goodbye animation play */
#define HINT_TICKS 60          /* 0.6 s: warn that holding powers off */
#define LONG_TICKS 150         /* 1.5 s: power off */
#define SLEEP_CHECK_MS 100

#define SERIAL_RX 1024         /* the driver drops what doesn't fit, so a console line must */
#define SERIAL_LINE 1024
#define CHAT_MAX (192 * 1024)  /* a typed message, assembled from "chat+=" lines */

static QueueHandle_t s_queue;
static bool (*s_console_hook)(const char *line);
#if CONFIG_MUSE_MIRROR
void muse_input_inject(unsigned events)
{
    if (events & MUSE_BTN_TALK_PRESS) {
        muse_input_touch_down();
    }
    atomic_fetch_or(&s_touch_talk_events, events);
    if (s_input) xTaskNotifyGive(s_input);
}
#endif
static bool s_talk_down;
static int64_t s_talk_released_us;   /* when the talk control was last let go */
#if CONFIG_MUSE_VOICE_NOTE_TOGGLE
static atomic_bool s_note_toggled;   /* a toggled voice note is recording, talk released */

void muse_input_note_toggled(bool on)
{
    atomic_store(&s_note_toggled, on);
}
#define NOTE_TOGGLED() atomic_load(&s_note_toggled)
#else
#define NOTE_TOGGLED() false
#endif
/* A push-to-talk press the voice task hasn't taken up yet: the speaker
 * button already ends the call it will start. Each such DOWN sets a mark of
 * its own and carries it; the voice task clears a mark only by that id, so
 * a press posted while it clears an older one keeps its mark. 0: none. */
static atomic_uint s_call_pending;
static atomic_uint s_mark_seq;
static atomic_uint s_call_epoch;
uint32_t muse_input_call_epoch(void) { return atomic_load(&s_call_epoch); }
void muse_input_clear_call_pending(uint32_t mark)
{
    if (mark) {
        atomic_compare_exchange_strong(&s_call_pending, &mark, 0);
    }
}
static bool s_cpu_low;      /* display stopped and the CPU allowed to sleep */
static volatile bool s_power_off_requested;
static volatile bool s_restart_requested;
static volatile bool s_power_off_armed;   /* the console's power.off=N: at s_power_off_at */
static volatile TickType_t s_power_off_at;
static bool s_cpu_pd;       /* muse_input_power_init() holds the CPU's retention memory */
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_awake;   /* with s_cpu_pd: held whenever !s_cpu_low */
static bool s_awake_held;
#endif
static volatile bool s_nap_now;   /* ">nap": asleep, as if on battery, nap without waiting WIFI_NAP_MS */

/* A press seen before its post (the Watcher's wheel decides a hold first,
 * muse_wheel.h): its own time, so the hold threshold counts from the press.
 * Input task only; 0: now. */
static int64_t s_post_at_us;

static void post(muse_ptt_t type, bool wake)
{
    muse_input_event_t ev = { .type = type, .wake = wake, .epoch = muse_input_call_epoch(),
                              .at_us = s_post_at_us ? s_post_at_us : esp_timer_get_time() };
    if (type == MUSE_PTT_DOWN) {
        /* The reply goes quiet at touch-down (its audio kept), so the press's
         * pre-roll doesn't record it: the barge-in echo rule. */
        muse_voice_talk_pressed();
    }
#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
    if (type == MUSE_PTT_DOWN) {
        muse_subs_talk_pause(true);   /* paused with the reply's audio: a tap brings them back, a turn drops them */
    }
#endif
    bool pending = type == MUSE_PTT_DOWN && muse_voice_ptt_enabled() && !muse_voice_in_call();
    if (pending) {
        uint32_t mark;
        do {
            mark = atomic_fetch_add(&s_mark_seq, 1) + 1;
        } while (!mark);
        ev.mark = mark;
        atomic_store(&s_call_pending, mark);   /* before the send, so the speaker button sees it */
    }
    ESP_LOGI(TAG, "PTT %s%s", type == MUSE_PTT_DOWN ? "down" : type == MUSE_PTT_UP ? "up" : "end call",
             wake ? " (waking)" : "");
    /* A connected musegadgets client hears a press, wherever the audio goes,
     * before the turn's audio (mgcommands.h, Push-to-talk: clients drop mic
     * Data that comes before the gesture), but only once it's held past the
     * hold threshold: the voice task sends button_down then,
     * and the release's button_up goes only after one. A tap sends nothing. */
    if (type == MUSE_PTT_UP) {
        muse_voice_talk_released();
    }
    if (xQueueSend(s_queue, &ev, pdMS_TO_TICKS(20)) != pdTRUE) {
        /* A lost edge cannot leave a live microphone open. Invalidate the
         * entire gesture, including queued older DOWNs, without another send. */
        atomic_fetch_add(&s_call_epoch, 1);
        atomic_store(&s_call_pending, 0);
        ESP_LOGW(TAG, "input queue full: call gesture invalidated");
    }

}

static bool update_power(void);

static void power_off(void)
{
    if (!muse_board->power_off) {
        return;   /* a physical switch powers it off */
    }
    muse_power_t now = muse_state_power();
    if (!muse_power_off_ok(muse_board->power_off_battery_only, &now)) {
        /* Every way here (Settings, the menu, the app's power_off) goes by the real power, never a bench
         * nap's as-if battery: on USB this board's latch stays held, and once USB goes it's off with only its
         * switch to bring it back (the ESP-Mosaico). */
        ESP_LOGW(TAG, "power-off refused: on USB power, which holds this board's latch on (unplug first)");
        muse_state_set_caption("Unplug first");
        return;
    }
    ESP_LOGI(TAG, "shutting down");
    muse_voice_shutdown(3000);   /* bounded best effort; never drop power before requesting end-call */
    muse_state_set_asleep(false);
    update_power();
    muse_state_set_progress(0);
    muse_state_set_level(0);
    muse_state_set_mode(MUSE_MODE_OFF);
    muse_state_set_caption("Goodbye!");
    vTaskDelay(pdMS_TO_TICKS(GOODBYE_MS));
#if CONFIG_PM_ENABLE
    /* Automatic light sleep leaves the timer armed, and a deep sleep would
     * wake on it at once (with s_cpu_pd light sleep stays configured). Only
     * the timer: a power-off that fails goes on light-sleeping and needs its
     * GPIO wakes. Armed first so the disable never logs "Incorrect wakeup
     * source" when light sleep had none set. */
    esp_sleep_enable_timer_wakeup(UINT64_C(24) * 3600 * 1000000);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
#endif
    esp_err_t err = muse_board->power_off();
    /* Only reached if the board couldn't power off. */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGE(TAG, "power-off failed (%s)", esp_err_to_name(err));
    muse_voice_cancel_shutdown();
    muse_state_set_mode(MUSE_MODE_IDLE);
    muse_state_set_caption("Couldn't power off");
}

static void restart(void)
{
    ESP_LOGI(TAG, "restarting");
    muse_voice_shutdown(3000);   /* bounded best effort, as before a power-off */
    muse_state_set_asleep(false);
    update_power();
    muse_state_set_progress(0);
    muse_state_set_level(0);
    muse_state_set_caption("RESTARTING...");
    vTaskDelay(pdMS_TO_TICKS(GOODBYE_MS));
    muse_restart(MUSE_RESTART_CONSOLE);   /* a planned restart, marked as one */
}

static void set_asleep(bool asleep, const char *why)
{
    if (asleep && NOTE_TOGGLED()) {
        return;   /* never hide a recording note */
    }
    if (asleep != muse_state_asleep()) {
        ESP_LOGI(TAG, "%s (%s)", asleep ? "sleeping" : "waking", why);
        muse_state_set_asleep(asleep);
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER
        if (asleep) {
            muse_review_close();   /* auto-sleep too: waking shows the live caption */
        }
#endif
        if (s_input) {
            xTaskNotifyGive(s_input);   /* out of wait_buttons() */
        }
        if (!asleep && !muse_wifi_connected()) {
            muse_wifi_apply();   /* the screen says reconnecting: don't sit out the backoff */
        }
    }
}

static void toggle_phone_setup(void)
{
    bool on = !muse_settings_ble_on();
    ESP_LOGI(TAG, "phone setup %s", on ? "on" : "off");
    muse_ble_phone_setup(on);
    muse_ble_status_t b;
    muse_ble_status(&b);
    if (on && b.name[0]) {
        /* The setup name stays up: it is the instruction for pairing. */
        muse_state_set_caption("Phone setup: %s", b.name);
    } else if (on) {
        muse_state_set_caption("Phone setup on");
    } else {
        /* Off is a confirmation, not a state worth keeping on screen. */
        muse_state_set_hint("Phone setup off");
    }
}

/*
 * Aux button: short press sleeps, a 1.5 s hold powers off, any press wakes.
 * Two quick presses toggle BLE phone setup, so the sleep waits a moment to
 * see whether a second press follows.
 */
static void aux_button(bool pressed, bool edge)
{
    static int held;
    static bool swallow;
    static bool hinted;
    static int sleep_in;    /* ticks until a pending single press sleeps */
    static char saved_caption[64];
    static bool saved_hint;

    if (sleep_in && --sleep_in == 0) {
        set_asleep(true, muse_board->aux_button);
    }
    if (edge && pressed) {
        held = 0;
        hinted = false;
        swallow = muse_state_asleep();
        if (swallow) {
            set_asleep(false, muse_board->aux_button);
        } else if (sleep_in) {
            sleep_in = 0;
            swallow = true;
            toggle_phone_setup();
        }
        return;
    }
    if (pressed && !swallow) {
        if (!muse_board->power_off) {
            return;   /* no hold to power off: a press only sleeps */
        }
        held++;
        if (held == HINT_TICKS) {
            uint32_t v = UINT32_MAX;
            muse_state_caption(saved_caption, sizeof(saved_caption), &v);
            saved_hint = muse_state_hint_pending();
            muse_state_set_caption("Hold to power off");
            hinted = true;
        } else if (held == LONG_TICKS) {
            swallow = true;
            power_off();
        }
        return;
    }
    if (edge && !pressed && !swallow) {
        if (hinted) {
            /* Let go early: cancel, and put back what was there, a hint as a hint. */
            if (saved_hint) {
                muse_state_set_hint(saved_caption);
            } else {
                muse_state_set_caption("%s", saved_caption);
            }
        } else {
            sleep_in = DOUBLE_TICKS;
        }
    }
}

/* No touch: the aux button opens the menu and steps down it; any press wakes. */
static void menu_button(bool pressed, bool edge)
{
    if (!edge || !pressed) {
        return;
    }
    if (muse_state_asleep()) {
        set_asleep(false, muse_board->aux_button);
    } else if (!s_talk_down && !NOTE_TOGGLED()) {
        muse_state_poke();
        muse_menu_key(MUSE_MENU_DOWN);
    }
}

static void aux_key(bool pressed, bool edge)
{
    static bool confirmed;   /* this press confirmed the pairing prompt: its hold and release are nothing else */
    if (edge && pressed && muse_link_state() == MUSE_LINK_CONFIRM && muse_link_confirm_key_press()) {
        /* The pairing prompt: the second key confirms it, a first setup or a re-setup (the talk key never confirms
         * a re-setup). */
        confirmed = true;
        muse_state_poke();
        return;
    }
    if (confirmed) {
        if (!pressed) {
            confirmed = false;
        }
        return;
    }
    if (muse_board->touch) {
        aux_button(pressed, edge);
    } else {
        menu_button(pressed, edge);
    }
}

/* Talk button: push-to-talk, or Select while the menu is open. Asleep, the
 * press wakes and is posted as a waking one: muse_voice records only if it's
 * still held once awake. A display-only board (no audio_init or mic_init)
 * runs no voice task to take presses off the queue, so there the press only
 * wakes, selects or confirms pairing. One press confirms a pairing prompt, a
 * first setup's or a re-setup's, whatever the press (the key's own, a held
 * one, the bench's), if it went down after the prompt showed (s_down_seq). */
static void talk_input(unsigned ev)
{
    bool talk_down = s_talk_down;
    bool voice = muse_board->audio_init || muse_board->mic_init;
    static bool swallow;
    /* A quick tap can latch press and release in the same poll, and a release
     * can land just before the next press; keep them ordered. */
    bool released = ev & MUSE_BTN_TALK_RELEASE;
    if ((talk_down || swallow) && released) {
        if (talk_down) {
            post(MUSE_PTT_UP, false);
        }
        talk_down = swallow = false;
        released = false;
    }
    if (!talk_down && !swallow && (ev & MUSE_BTN_TALK_PRESS)) {
        if (muse_link_confirm_press(s_talk_released_us, s_down_seq)) {
            /* Confirmed a Muse app pairing (Link's setup button). */
            muse_state_poke();
            swallow = true;
        } else if (muse_passcode_locked_at_press()) {
            /* The passcode lock: a talk press opens the keypad,
             * "Unlock to talk", and is never replayed (a turn needs a new
             * press). A talk from a dark panel is for unlocked only, "After
             * 5 min" applied at the press itself. */
            if (muse_state_asleep()) {
                set_asleep(false, muse_board->talk_button);
            }
            muse_state_poke();
            muse_passcode_request_keypad(MUSE_PASS_WHY_TALK);
            swallow = true;
        } else if (muse_state_asleep()) {
            set_asleep(false, muse_board->talk_button);
            if (voice) {
                post(MUSE_PTT_DOWN, true);
                talk_down = true;
            } else {
                swallow = true;
            }
        } else if (muse_menu_is_open()) {
            muse_state_poke();
            if (!muse_board->keyboard) muse_menu_key(MUSE_MENU_SELECT);
            swallow = true;
        } else if (voice) {
            post(MUSE_PTT_DOWN, false);
            talk_down = true;
        } else {
            muse_state_poke();
            swallow = true;
        }
    }
    if ((talk_down || swallow) && released) {
        if (talk_down) {
            post(MUSE_PTT_UP, false);
        }
        talk_down = swallow = false;
    }
    s_talk_down = talk_down;
    if (ev & MUSE_BTN_TALK_RELEASE) {
        s_talk_released_us = esp_timer_get_time();   /* after this event's press */
    }
}

static void talk_button(unsigned ev)
{
    talk_input(ev);
}

/* Keyboard menus don't repurpose Space/GO as Select. Navigation wakes the
 * screen without accidentally changing a setting; Enter can confirm pairing. */
static void keyboard_buttons(unsigned ev)
{
    const unsigned mask = MUSE_BTN_UP | MUSE_BTN_DOWN | MUSE_BTN_LEFT |
                          MUSE_BTN_RIGHT | MUSE_BTN_ENTER | MUSE_BTN_ESCAPE;
    if (!(ev & mask) || s_talk_down || NOTE_TOGGLED()) return;
    if ((ev & MUSE_BTN_ESCAPE) && muse_link_confirm_dismiss()) {
        muse_state_set_caption("Not now");   /* the pairing prompt: Escape is the back gesture */
        muse_state_poke();
        return;
    }
    if ((ev & MUSE_BTN_ENTER) && muse_link_confirm_key_press()) {   /* Enter: a confirm key, never the talk key */
        muse_state_poke();
        return;
    }
    if (muse_state_asleep()) {
        set_asleep(false, "keyboard");
        return;
    }
    muse_state_poke();
    if (ev & MUSE_BTN_ESCAPE) muse_menu_key(MUSE_MENU_BACK);
    else if (ev & MUSE_BTN_UP) muse_menu_key(MUSE_MENU_UP);
    else if (ev & MUSE_BTN_DOWN) muse_menu_key(MUSE_MENU_DOWN);
    else if (ev & MUSE_BTN_LEFT) muse_menu_key(MUSE_MENU_LEFT);
    else if (ev & MUSE_BTN_RIGHT) muse_menu_key(MUSE_MENU_RIGHT);
    else if (ev & MUSE_BTN_ENTER) muse_menu_key(MUSE_MENU_SELECT);
}

/* "@pair.pending" on the console when Muse Link setup starts waiting for the
 * confirmation press, once per wait: a test rig answers ">pair.confirm". */
#define PAIR_AUTO_DELAY_US (6 * 1000000LL)
static void announce_confirm(bool confirm)
{
    static bool s_announced;
    if (confirm && !s_announced) {
        printf("@pair.pending\n");
        fflush(stdout);
    }
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    /* The bench auto-confirm: armed from the console (!PAIR AUTO, >pair.auto) for
     * exactly one pairing within 10 min; the talk button's own press, made 6 s
     * into the wait as a person would: the iOS app listens for the
     * confirmation only once its own prompt is acknowledged, and an instant
     * one is lost (seen on the C6). */
    static int64_t s_wait_from_us;   /* when this wait began; 0 once it's answered or over */
    if (confirm && !s_announced) {
        s_wait_from_us = esp_timer_get_time();
    } else if (!confirm) {
        s_wait_from_us = 0;
    }
    if (confirm && s_wait_from_us && esp_timer_get_time() - s_wait_from_us >= PAIR_AUTO_DELAY_US) {
        s_wait_from_us = 0;
        if (mg_ble_pair_auto_take()) {
            (void)muse_link_bench_confirm();
        }
    }
#endif
    s_announced = confirm;
}

/* Sleep when charged, from every power reading (muse_charged.h). */
static muse_charged_t s_charged;

/* Each power reading; ok false when it failed (unknown: a % board isn't
 * charged, a done board keeps its latch, and nothing holds until a good
 * reading). Tracked whatever the setting. */
static void note_power(const muse_power_t *p, bool ok)
{
    if (!ok) {
        muse_charged_unknown(&s_charged, muse_board->charged);
        return;
    }
    muse_charged_reading_t r = { .usb = p->usb, .battery_pct = p->battery_pct, .charge_done = p->charge_done };
    if (muse_charged_read(&s_charged, muse_board->charged, &r)) {
        /* Plugging USB in is a wake; on a lit screen it restarts the
         * timer, as a press would. */
        if (muse_state_asleep()) {
            set_asleep(false, "USB plugged in");
        } else {
            muse_state_poke();
        }
    }
}

bool muse_input_charged_hold(void)
{
    return muse_charged_holds(muse_board->charged, !muse_settings_sleep_when_charged(), muse_state_on_battery(),
                              &s_charged);
}

#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER
static void watcher_buttons(unsigned ev)
{
    static muse_wheel_t wheel;
    static int64_t press_us;
    bool down = wheel.down;
    if (ev & MUSE_BTN_TALK_PRESS) down = true;
    else if (ev & MUSE_BTN_TALK_RELEASE) down = false;   /* let go before the hold timer runs */
    if (down && !wheel.down) {
        press_us = esp_timer_get_time();
    }
    bool confirmed = down && !wheel.down && muse_link_confirm_press(s_talk_released_us, muse_link_prompt_seq());   /* one press, any press */
    bool asleep = muse_state_asleep();
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
#if CONFIG_MUSE_WATCHER_CAMERA
    static bool sleep_pending;
    static uint32_t sleep_deadline;
    if (confirmed) sleep_pending = false;
    if (sleep_pending && (int32_t)(now - sleep_deadline) >= 0) {
        sleep_pending = false;
        set_asleep(true, "wheel click");
        asleep = true;
    }
    if (down && !wheel.down && sleep_pending && !muse_passcode_locked()) {   /* no camera view while locked */
        sleep_pending = false;
        confirmed = true;   /* consume the second click without starting a note */
        muse_review_close();
        ESP_LOGI(TAG, "wheel double-click: camera preview/shutter");
        watcher_camera_preview_toggle();
    }
#endif
    muse_wheel_action_t action = muse_wheel_update(&wheel, down, now, asleep, confirmed);
    if (confirmed) muse_state_poke();
    if (action == MUSE_WHEEL_WAKE) {
        set_asleep(false, "wheel click");
    } else if (action == MUSE_WHEEL_TALK_DOWN) {
        muse_review_close();
        s_post_at_us = press_us;   /* the hold threshold counts from the press, not the wheel's decision */
        talk_input(MUSE_BTN_TALK_PRESS);
        s_post_at_us = 0;
    }
    if ((ev & MUSE_BTN_TALK_RELEASE) && wheel.down) {   /* pressed and let go in one poll */
        action = muse_wheel_update(&wheel, false, now, muse_state_asleep(), false);
    }
    if (action == MUSE_WHEEL_TALK_UP) {
        talk_button(MUSE_BTN_TALK_RELEASE);
    } else if (action == MUSE_WHEEL_SLEEP) {
        muse_review_close();
#if CONFIG_MUSE_WATCHER_CAMERA
        sleep_pending = true;
        sleep_deadline = now + 350;
#else
        set_asleep(true, "wheel click");
#endif
    }
    if ((ev & (MUSE_BTN_REVIEW_PREV | MUSE_BTN_REVIEW_NEXT)) && !wheel.down) {
#if CONFIG_MUSE_WATCHER_CAMERA
        sleep_pending = false;
#endif
        set_asleep(false, "wheel review");
        muse_state_poke();
        if (muse_review_step(ev & MUSE_BTN_REVIEW_NEXT ? 1 : -1)) {
            ESP_LOGI(TAG, "wheel: reviewing reply");
        } else if (muse_state_mode(NULL) == MUSE_MODE_IDLE) {
            muse_state_set_caption("No reply yet");   /* not over an error or a turn's caption */
        }
    }
    if (ev & MUSE_BTN_TALK_RELEASE) {
        s_talk_released_us = esp_timer_get_time();   /* after this event's press */
    }
}
#endif

/* A pairing prompt wakes the screen and keeps it on; otherwise idle sleeps. */
static void check_sleep(void)
{
    muse_ble_status_t ble;
    muse_ble_status(&ble);
    bool confirm = muse_link_state() == MUSE_LINK_CONFIRM;
    announce_confirm(confirm);
    bool prompt = ble.passkey || confirm;
    if (prompt) {
        set_asleep(false, "pairing");
        return;
    }
    if (muse_voice_in_call()) return;
    /* On USB the screen times out as on battery (no
     * USB keep-alive); only Sleep when charged: Never holds a lit
     * one, below. The mic captures for someone: the ring stays in sight. */
    if (muse_mic_ring_on()) return;
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    /* A vibration pattern plays: it vetoes sleep until it ends. */
    if (mg_ble_haptic_busy()) return;
#endif
    int after = muse_settings_sleep_s();
    float mode_t;
    muse_mode_t mode = muse_state_mode(&mode_t);
    int64_t now = esp_timer_get_time();
    /* A turn the gadget started (its talk press) keeps the panel lit
     * through thinking and responding until it's done (idle, or error), and
     * the timer starts again from there. A phone's turn alone (its
     * assistant_state) neither wakes a dark panel nor holds a lit one: the
     * user may be chatting on the phone. */
    /* A reply on the gadget (a client's stream_audio reaching the speaker,
     * or subtitles' band on screen) holds a lit panel the same way until it
     * ends; neither wakes a dark one (subtitles' state 1 does, muse_subs.c). */
    /* Sleep when charged at Never holds a lit panel while on USB with
     * the battery charged (muse_charged.h: from the charger's first done of
     * the plug-in until unplug, tracked at Always too, so a switch to Never
     * holds at once), the same way: it never lights a dark one (this
     * function only ever darkens), and once it lets go the usual timeout
     * runs from there. */
    static int64_t held_seen_us;
    bool turn = mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_THINKING || mode == MUSE_MODE_SPEAKING;
    if ((turn && muse_state_own_turn()) || muse_voice_reply_holds_panel() || muse_input_charged_hold()) {
        held_seen_us = now;
        return;
    }
    bool rested = !held_seen_us || now - held_seen_us > (int64_t)after * 1000000;
    if (after && !muse_state_asleep() && (mode == MUSE_MODE_IDLE || turn) && rested
        && muse_state_idle_secs() > after) {
        set_asleep(true, "auto-sleep");
    }
}

static void set_cpu_low(bool low)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = low ? CONFIG_XTAL_FREQ : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = low,
    };
    if (s_cpu_pd) {
        /* Light sleep stays configured: turning it off would free the
         * retention memory. The lock keeps the chip out of it instead, taken
         * before the full clock and let go only after the low one. */
        pm.light_sleep_enable = true;
        if (!low && !s_awake_held) {
            esp_pm_lock_acquire(s_awake);
            s_awake_held = true;
        }
    }
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power management: %s", esp_err_to_name(err));
    }
    if (s_cpu_pd && low && s_awake_held && err == ESP_OK) {
        esp_pm_lock_release(s_awake);
        s_awake_held = false;
    }
#else
    (void)low;
#endif
}

void muse_input_power_init(void)
{
#if CONFIG_PM_ENABLE && CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP
    esp_err_t err = esp_sleep_cpu_retention_init();
    if (err == ESP_OK) {
        err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "muse_awake", &s_awake);
    }
    if (err == ESP_OK) {
        esp_pm_lock_acquire(s_awake);
        s_awake_held = true;
        const esp_pm_config_t pm = {
            .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .light_sleep_enable = true,
        };
        err = esp_pm_configure(&pm);
    }
    s_cpu_pd = err == ESP_OK;
    if (!s_cpu_pd) {
        if (s_awake) {
            if (s_awake_held) {
                esp_pm_lock_release(s_awake);
                s_awake_held = false;
            }
            esp_pm_lock_delete(s_awake);
            s_awake = NULL;
        }
        esp_sleep_cpu_retention_deinit();
        ESP_LOGW(TAG, "CPU stays powered in light sleep: no retention memory (%s)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "CPU powers down in light sleep: retention memory held");
    }
#endif
    muse_battery_note_cpu_pd(s_cpu_pd);
}

bool muse_input_cpu_low(void)
{
    return s_cpu_low;
}

bool muse_input_cpu_pd(void)
{
    return s_cpu_pd;
}

/*
 * On battery with the screen dark and voice resting: stop the display and let
 * the CPU drop to the crystal clock and light-sleep between polls, or until a
 * button interrupts (wait_buttons). Returns true while the display is stopped.
 * With a USB host attached (">nap" on the bench) the CPU stays at full speed:
 * at the crystal clock a long line of serial output can stall the USB console
 * until the host reopens the port.
 */
static bool update_power(void)
{
    static bool paused;
    bool pause = muse_board->display_pause && muse_state_on_battery() && muse_state_asleep()
                 && muse_ui_dark() && muse_voice_resting();
#if CONFIG_MUSE_MIRROR
    /* The viewer needs LVGL timers even while the physical screen is dark. */
    pause = pause && !muse_mirror_connected();
#endif
    bool want_low = pause && !muse_console_host();
    if (pause == paused && want_low == s_cpu_low) {
        return paused;
    }
    if (pause && !paused) {
        muse_board->display_pause(true);
    }
    if (want_low != s_cpu_low) {
        set_cpu_low(want_low);
    }
    if (!pause && paused) {
        muse_board->display_pause(false);
    }
    paused = pause;
    s_cpu_low = want_low;
    ESP_LOGI(TAG, "%s", s_cpu_low ? "low power: display paused"
                        : paused  ? "display paused (USB host: CPU at full speed)"
                                  : "full power");
    return paused;
}

/*
 * Low power for WIFI_NAP_MS: Wi-Fi off, since keeping it associated costs
 * more than the rest of the chip. It rejoins when the screen wakes or USB
 * power arrives; meanwhile nothing reaches the device over the network.
 * Not while a voice note recorded offline waits to go (for a while). Returns
 * true while napping.
 */
static bool update_wifi_nap(TickType_t now, bool paused)
{
    static TickType_t low_since;
    static bool napping;
    if (!s_cpu_low) {
        low_since = now;
    }
    if (s_nap_now && (!muse_state_asleep() || !muse_state_on_battery())) {   /* awake, or ">nap"'s 10 min ran out on USB */
        s_nap_now = false;
        muse_state_set_as_if_battery(false);
    }
    bool nap = paused && !muse_voice_notes_waiting()
               && (s_nap_now || (s_cpu_low && now - low_since >= pdMS_TO_TICKS(WIFI_NAP_MS)));
    if (nap != napping) {
        napping = nap;
        ESP_LOGI(TAG, "Wi-Fi %s", nap ? "napping" : "waking");
        muse_wifi_nap(nap);
    }
    return napping;
}

/* Independent POWER never goes through PTT, AUX's menu or its setup double
 * click. LONG is supplied by the board only after a validated hold. In a
 * live call a press is volume down instead of sleep (a hold still powers off). */
static bool s_power_down, s_power_swallow, s_power_call;

static void power_release(void)
{
    if (!s_power_down) return;
    s_power_down = false;
#if CONFIG_MUSE_APP_VIEW
    /* With an app open, the power key closes it instead of sleeping the screen. */
    if (!s_power_swallow && !s_power_call && muse_app_view_power()) {
        s_power_swallow = s_power_call = false;
        return;
    }
#endif
    if (!s_power_swallow && !s_power_call) set_asleep(true, muse_board->power_button);
    s_power_swallow = s_power_call = false;
}

static void dedicated_buttons(unsigned ev)
{
    bool release = (ev & MUSE_BTN_POWER_RELEASE) != 0;
    /* Preserve release-before-new-press ordering for latched boards. */
    if (s_power_down && release) {
        power_release();
        release = false;
    }
    if (!s_power_down && (ev & MUSE_BTN_POWER_PRESS)) {
        s_power_down = true;
        s_power_swallow = muse_state_asleep();
        muse_state_poke();
        if (s_power_swallow) set_asleep(false, muse_board->power_button);
#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
        else if (muse_call_owner_active()) {
            /* The owner's call only: an unproven central's call state shows the call screen but remaps no key,
             * so power always sleeps the screen and the speaker key toggles the speaker. */
            s_power_call = true;
            muse_call_volume_key(-1);
        }
#endif
    }
    if (s_power_down && !s_power_swallow && (ev & MUSE_BTN_POWER_LONG)) {
        s_power_swallow = true;   /* no sleep on release after shutdown/failure */
        if (s_talk_down) talk_button(MUSE_BTN_TALK_RELEASE);
        power_off();
    }
    if (release) power_release();
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH_FAMILY && CONFIG_MUSE_REALTIME_VOICE
    /* In a push-to-talk call, or with its press still on the way, the speaker
     * button (blue) hangs up, on the press, and wakes the screen. A quick
     * second press just after doesn't toggle the speaker. If the hang-up
     * can't be queued, post() moves the input epoch, which ends the call. */
    static int ignore_in;
    if (ignore_in) --ignore_in;
    if (ev & MUSE_BTN_SPEAKER_PRESS) {
        if (muse_voice_in_call() || atomic_load(&s_call_pending)) {
            muse_state_poke();
            set_asleep(false, muse_board->speaker_button);
            post(MUSE_PTT_END_CALL, false);
            ignore_in = DOUBLE_TICKS;
            return;
        }
        if (ignore_in) {
            return;
        }
    }
#endif
    if (ev & MUSE_BTN_SPEAKER_PRESS) {
        muse_state_poke();
        if (muse_state_asleep()) {
            set_asleep(false, muse_board->speaker_button);   /* wake-only press */
#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
        } else if (muse_call_owner_active()) {
            muse_call_volume_key(1);   /* in the owner's call: volume up (an unproven one remaps no key) */
#endif
        } else if (muse_state_mode(NULL) != MUSE_MODE_OFF) {
            bool on = !muse_settings_speaker_on();
            muse_settings_set_speaker_on(on);
            if (muse_state_mode(NULL) == MUSE_MODE_IDLE) {
                muse_state_set_hint(on ? "Speaker on" : "Speaker off");
            }
        }
    }
}

/* Where the voice task is, on mg.voice (the Nordic UART mirror carries it). */
/* A task's state (R ready, B blocked, X running) and its priority now: above
 * its own means it holds a lock a higher task waits on (priority
 * inheritance). Who can starve the voice task on its core shows here
 * (e.g. LVGL, at 5, spinning while the voice task at 6 sits ready). */
static void task_brief(char *out, size_t n, const char *name)
{
    TaskHandle_t t = xTaskGetHandle(name);
    if (!t) {
        snprintf(out, n, "%s -", name);
        return;
    }
    eTaskState st = eTaskGetState(t);
    char c = st == eRunning ? 'X' : st == eReady ? 'R' : st == eBlocked ? 'B' : st == eSuspended ? 'S' : '?';
    snprintf(out, n, "%s %c prio %u", name, c, (unsigned)uxTaskPriorityGet(t));
}

static void log_voice_where(const char *why)
{
    uint32_t ms = 0;
    const char *at = muse_voice_where(&ms);
    const char *audio = muse_audio_lock_holder();
    char voice[32], lvgl[32], worker[32], send[32];
    task_brief(voice, sizeof(voice), "muse_voice");
    task_brief(lvgl, sizeof(lvgl), "lvgl");
    task_brief(worker, sizeof(worker), "mg_ble");
    task_brief(send, sizeof(send), "lcd_send");
    ESP_LOGW("mg.voice", "%s: the voice task is at \"%s\" for %u ms, quiet %u ms (state %c); audio lock %s; %s, %s, "
             "%s, %s", why, at, (unsigned)ms, (unsigned)muse_voice_quiet_ms(), muse_voice_task_state(),
             audio ? audio : "free", voice, lvgl, worker, send);
}

/* The voice task's last sign of life (a place in its loop, a mic read, a
 * speaker write, a delay) is 5 s old (15 s resting, whose waits are 10 s
 * slices): stuck in a call, or starved of its core. Once per stall, on
 * mg.voice; the watchdog panics at 30 s with its backtrace (muse_wdt.h). */
#define VOICE_QUIET_MS 5000
#define VOICE_QUIET_RESTING_MS 15000
static void voice_stuck_watch(void)
{
    static bool told;
    uint32_t ms = 0;
    bool resting = !strcmp(muse_voice_where(&ms), "resting (waits for a wake or a request)");
    if (muse_voice_quiet_ms() >= (resting ? VOICE_QUIET_RESTING_MS : VOICE_QUIET_MS)) {
        if (!told) {
            told = true;
            log_voice_where("the voice task hasn't moved for 5 s");
        }
    } else {
        told = false;
    }
}

#if CONFIG_MUSE_GADGET_BLE_AUDIO
/* A client's start_mic the voice task hasn't taken up in 2 s (it's stuck
 * somewhere), or a client capture that has read no mic chunk for 2 s: says
 * where the task is, once per request. A capture that runs (any length) is
 * neither. */
static void voice_watch(void)
{
    static int64_t since;
    static bool told;
    bool waiting = mg_voice_capture_requested();
    int64_t now = esp_timer_get_time();
    if (!waiting) {
        since = 0;
        told = false;
        return;
    }
    if (told) {
        return;
    }
    if (muse_voice_in_client_capture()) {
        since = now;   /* taken up: from here only a stall counts */
        if (muse_voice_capture_stalled(2000)) {
            told = true;
            log_voice_where("a client's capture read no mic audio for 2 s");
        }
        return;
    }
    if (!since) {
        since = now;
    } else if (now - since >= 2000000) {
        told = true;
        log_voice_where("a client's start_mic waits 2 s");
    }
}
#endif

/* Settings > Talk applies where a board has both a talk
 * key and the home's hold region. */
bool muse_input_talk_choice(void)
{
    return muse_board->talk_button && !muse_board->touch_talk && muse_ui_has_hold_region();
}

/*
 * "Screen only": the talk key no longer talks. It still confirms a pairing
 * (the setup press) and wakes the screen, and says where talking went. A
 * release while a press is already down still goes through, so a change of
 * the setting mid-press never leaves the mic open. The screen's hold region
 * checks "Button only" itself (muse_ui.c).
 */
static unsigned talk_key_filter(unsigned ev)
{
    if (!muse_input_talk_choice() || muse_settings_talk_mode() != MUSE_TALK_SCREEN) {
        return ev;
    }
    if (s_talk_down) {
        return ev;
    }
    if (ev & MUSE_BTN_TALK_PRESS) {
        muse_state_poke();
        if (muse_link_confirm_press(s_talk_released_us, s_down_seq)) {
            /* confirmed a Muse app pairing: one press */
        } else if (muse_state_asleep()) {
            set_asleep(false, muse_board->talk_button);
        } else if (muse_menu_is_open()) {
            if (!muse_board->keyboard) muse_menu_key(MUSE_MENU_SELECT);
        } else if (muse_state_mode(NULL) == MUSE_MODE_IDLE) {
            muse_state_set_caption("Hold the screen to talk");
        }
    }
    if (ev & MUSE_BTN_TALK_RELEASE) {
        s_talk_released_us = esp_timer_get_time();   /* after this event's press */
    }
    return ev & ~(MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE);
}

/* The next queued hold's time, in ms from now (0: due); -1 with none queued. */
static int32_t bench_hold_due_ms(void)
{
    int32_t ms = -1;
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&s_bench_mux);
    for (int i = 0; i < s_bench_hold_n; i++) {
        int32_t d = (int32_t)(s_bench_holds[i].due - now);
        d = d < 0 ? 0 : (int32_t)(d * portTICK_PERIOD_MS);
        if (ms < 0 || d < ms) {
            ms = d;
        }
    }
    portEXIT_CRITICAL(&s_bench_mux);
    return ms;
}

/* The earliest queued hold that's due, taken off the queue: its length, or 0. */
static unsigned bench_hold_take_due(void)
{
    unsigned ms = 0;
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&s_bench_mux);
    int best = -1;
    for (int i = 0; i < s_bench_hold_n; i++) {
        if ((int32_t)(now - s_bench_holds[i].due) >= 0 &&
            (best < 0 || (int32_t)(s_bench_holds[i].due - s_bench_holds[best].due) < 0)) {
            best = i;
        }
    }
    if (best >= 0) {
        ms = s_bench_holds[best].ms;
        for (int i = best + 1; i < s_bench_hold_n; i++) {
            s_bench_holds[i - 1] = s_bench_holds[i];
        }
        s_bench_hold_n--;
    }
    portEXIT_CRITICAL(&s_bench_mux);
    return ms;
}

/*
 * key=talk[,MS] (400 ms by default): asks the input task for the held press.
 * key=talk,MS,AFTER_S queues one to start AFTER_S (1 to 3600) s from now, up
 * to BENCH_TALK_QUEUE; key=talk? lists them, key=talk,cancel drops them.
 */
static void bench_key_talk(const char *k)
{
    if (!muse_board->talk_button) {
        ESP_LOGW("mg.bench", "key %s: no such button on this board", k);
        return;
    }
    if (strcmp(k, "talk?") && strcmp(k, "talk,cancel") && !line_talk_ok()) {
        /* A wireless line's talk press never opens the mic on a release image (the wired console's does:
         * line_talk_ok). */
        ESP_LOGW("mg.bench", "key %s: %s: real press needed", k, s_line_wireless ? "the Nordic UART" : "the console");
        printf("@key real press needed\n");
        fflush(stdout);
        return;
    }
    if (!strcmp(k, "talk?")) {
        char list[BENCH_TALK_QUEUE * 24 + 1] = "";
        size_t used = 0;
        TickType_t now = xTaskGetTickCount();
        portENTER_CRITICAL(&s_bench_mux);
        int n = s_bench_hold_n;
        bench_hold_t holds[BENCH_TALK_QUEUE];
        memcpy(holds, s_bench_holds, sizeof(holds));
        portEXIT_CRITICAL(&s_bench_mux);
        for (int i = 0; i < n && used < sizeof(list); i++) {
            int32_t d = (int32_t)(holds[i].due - now);
            unsigned in_s = d <= 0 ? 0 : (unsigned)((d * portTICK_PERIOD_MS + 999) / 1000);
            used += (size_t)snprintf(list + used, sizeof(list) - used, "%s%u ms in %u s", i ? ", " : "", holds[i].ms, in_s);
        }
        ESP_LOGI("mg.bench", "key talk? %d queued%s%s%s", n, n ? ": " : "", list,
                 atomic_load(&s_bench_talk_busy) ? " (one held now)" : "");
        return;
    }
    if (!strcmp(k, "talk,cancel")) {
        portENTER_CRITICAL(&s_bench_mux);
        int n = s_bench_hold_n;
        s_bench_hold_n = 0;
        portEXIT_CRITICAL(&s_bench_mux);
        ESP_LOGI("mg.bench", "key talk,cancel: %d dropped", n);
        return;
    }
    const char *after = k[4] == ',' ? strchr(k + 5, ',') : NULL;
    int ms = k[4] == ',' ? atoi(k + 5) : 400;
    if (ms < 20 || ms > 10000) {
        ESP_LOGW("mg.bench", "key %s: hold 20 to 10000 ms", k);
        return;
    }
    if (after) {
        int after_s = atoi(after + 1);
        if (after_s < 1 || after_s > 3600) {
            ESP_LOGW("mg.bench", "key %s: start 1 to 3600 s from now", k);
            return;
        }
        bool queued = false;
        portENTER_CRITICAL(&s_bench_mux);
        if (s_bench_hold_n < BENCH_TALK_QUEUE) {
            s_bench_holds[s_bench_hold_n++] = (bench_hold_t){
                .ms = (unsigned)ms, .due = xTaskGetTickCount() + pdMS_TO_TICKS(after_s * 1000) };
            queued = true;
        }
        portEXIT_CRITICAL(&s_bench_mux);
        if (!queued) {
            ESP_LOGW("mg.bench", "key %s: %d already queued", k, BENCH_TALK_QUEUE);
            return;
        }
        ESP_LOGI("mg.bench", "key talk,%d queued at +%d s", ms, after_s);
        if (s_input) {
            xTaskNotifyGive(s_input);   /* out of wait_buttons, to wait for this one */
        }
        return;
    }
    if (atomic_load(&s_bench_talk_busy) || atomic_load(&s_bench_talk_ms)) {
        ESP_LOGW("mg.bench", "key %s: busy (a held press not yet released)", k);
        return;
    }
    atomic_store(&s_bench_talk_ms, (unsigned)ms);
    ESP_LOGI("mg.bench", "key %s pressed (held %d ms, %s)", k, ms,
             muse_state_asleep() || muse_ui_dark() ? "from a dark panel" : "on a lit screen");
    if (s_input) {
        xTaskNotifyGive(s_input);
    }
}

/* The bench's held talk press (key=talk,MS, or a queued one come due): its
 * press on the first poll, its release once MS have passed. A queued hold
 * that comes due while one is held waits for its release. */
static unsigned bench_talk(void)
{
    static TickType_t release_at;
    static unsigned held_ms;
    unsigned ms = 0;
    if (!atomic_load(&s_bench_talk_busy)) {
        ms = atomic_exchange(&s_bench_talk_ms, 0);
        if (!ms) {
            ms = bench_hold_take_due();
            if (ms) {
                ESP_LOGI("mg.bench", "key talk,%u pressed (held %u ms, %s; queued)", ms, ms,
                         muse_state_asleep() || muse_ui_dark() ? "from a dark panel" : "on a lit screen");
            }
        }
    }
    if (ms) {
        atomic_store(&s_bench_talk_busy, true);
        release_at = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
        held_ms = ms;
        return MUSE_BTN_TALK_PRESS;
    }
    if (atomic_load(&s_bench_talk_busy) && (int32_t)(xTaskGetTickCount() - release_at) >= 0) {
        atomic_store(&s_bench_talk_busy, false);
        ESP_LOGI("mg.bench", "key talk released after %u ms", held_ms);
        (void)held_ms;   /* logs compiled out */
        return MUSE_BTN_TALK_RELEASE;
    }
    return 0;
}

/* While the update's confirm prompt shows (muse_local_press_prompting: an update over Bluetooth asked for its
 * press), a press of the board's own talk, aux or Enter key confirms it, and that press (and its release) does
 * nothing else. Only the physical keys (poll_buttons): never a touch here (the card's Confirm is muse_ui.c's), the
 * bench's keys or the console's key=. A press at any other time never counts. */
static atomic_int s_update_wake;   /* 1: an update asks (wake to its card), 2: to the keypad first */

void muse_input_update_wake(bool locked)
{
    atomic_store(&s_update_wake, locked ? 2 : 1);
}

/* An update over Bluetooth started asking (muse_local_press): the screen up, on the keypad when locked. */
static void update_wake_take(void)
{
    int w = atomic_exchange(&s_update_wake, 0);
    if (!w) {
        return;
    }
    set_asleep(false, "an update asks");
    muse_state_poke();
    if (w == 2) {
        muse_passcode_request_keypad(MUSE_PASS_WHY_UPDATE);   /* "Unlock to update"; the confirm card follows */
    }
}

static unsigned update_prompt_press(unsigned phys)
{
    static unsigned held;   /* the confirming press's key: its release is the prompt's too */
    const unsigned keys[][2] = { { MUSE_BTN_TALK_PRESS, MUSE_BTN_TALK_RELEASE },
                                 { MUSE_BTN_AUX_PRESS, MUSE_BTN_AUX_RELEASE },
                                 { MUSE_BTN_ENTER, 0 } };
    for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        /* A key's own press, at its edge; never a finger held on the face (MUSE_BTN_TALK_HELD, reported long after it
         * went down). */
        bool face = k == 0 && (phys & MUSE_BTN_TALK_HELD);
        if ((phys & keys[k][0]) && !face && muse_local_press_prompting()
            && muse_local_press_note("a press of the gadget's own key on the update prompt")) {
            phys &= ~keys[k][0];
            held |= keys[k][1];
        }
        if (keys[k][1] && (phys & keys[k][1]) && (held & keys[k][1])) {
            phys &= ~keys[k][1];
            held &= ~keys[k][1];
        }
    }
    return phys;
}

static void input_task(void *arg)
{
    (void)arg;
    bool aux_down = false;
    bool paused = false;
    TickType_t checked = xTaskGetTickCount() - pdMS_TO_TICKS(SLEEP_CHECK_MS);
    TickType_t powered = xTaskGetTickCount() - pdMS_TO_TICKS(POWER_MS);

    for (;;) {
#if CONFIG_MUSE_MOTION_GESTURES
        if (muse_motion_tick((uint32_t)(esp_timer_get_time() / 1000))) {
            set_asleep(false, "lift");
        }
#endif
        unsigned own = muse_board->poll_buttons();
        update_wake_take();               /* an update asks: the screen up (the keypad first if locked) */
        own = update_prompt_press(own);   /* a press on the update's prompt confirms the update, only that */
        if (own & MUSE_BTN_TALK_PRESS) {
            muse_input_press(MUSE_PRESS_KEY);   /* the talk key's own press (never the bench's below) */
        }
        unsigned keys = own | bench_talk();
        /* Where each talk press went down (s_down_seq): a key's here, at its edge; a hold on the face, and a
         * touch's, at its touch-down. */
        s_down_seq = (keys & MUSE_BTN_TALK_HELD) ? atomic_load(&s_touch_down_seq) : muse_link_prompt_seq();
        unsigned ev = talk_key_filter(keys);
        unsigned touch = atomic_exchange(&s_touch_talk_events, 0);
        if (touch & MUSE_BTN_TALK_PRESS) {
            s_down_seq = atomic_load(&s_touch_down_seq);
        }
        ev |= touch;
        {
            /* The console's key=: the press now, its release on the next poll. */
            static unsigned bench_release;
            unsigned bench = atomic_exchange(&s_bench_keys, 0);
            ev |= bench_release;
            bench_release = (bench & MUSE_BTN_SPEAKER_PRESS ? MUSE_BTN_SPEAKER_RELEASE : 0) |
                            (bench & MUSE_BTN_POWER_PRESS ? MUSE_BTN_POWER_RELEASE : 0) |
                            (bench & MUSE_BTN_AUX_PRESS ? MUSE_BTN_AUX_RELEASE : 0);
            ev |= bench;
        }
        /* A user's key press or touch (the bench's too): link modes sleep ->
         * ready, a talk press active, before any filter below
         * takes the press for itself (a vibration test, the video view). */
        link_user_keys(ev);
        if (atomic_exchange(&s_user_touch, false)) {
            link_user("touch");
        }
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        if (s_post) {
            char line[CONSOLE_POST_LEN];
            while (xQueueReceive(s_post, line, 0) == pdTRUE) {
                s_line_wireless = true;   /* a talk press from it needs the gadget's own press lately (release) */
                muse_input_console_line(line);   /* the Nordic UART's !IN words (muse_input_console_post) */
                s_line_wireless = false;
            }
        }
#endif
        muse_touch_poll();   /* the panel's touches for a phone while video holds LVGL paused */
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        /* A vibration test playing (Developer > Tests > Vibration, the
         * console's vib words): any key stops it, and
         * that's all the press does (its release then does nothing). */
        {
            unsigned keys = MUSE_BTN_TALK_PRESS | MUSE_BTN_AUX_PRESS | MUSE_BTN_POWER_PRESS | MUSE_BTN_SPEAKER_PRESS |
                            MUSE_BTN_UP | MUSE_BTN_DOWN | MUSE_BTN_LEFT | MUSE_BTN_RIGHT | MUSE_BTN_ENTER |
                            MUSE_BTN_ESCAPE;
            if ((ev & keys) && mg_ble_haptic_test_key()) {
                muse_state_poke();
                ev &= ~(keys | MUSE_BTN_POWER_LONG);
            }
        }
#endif
#if CONFIG_MUSE_VIDEO
        /* Any press leaves the video view (its release then does nothing),
         * except during a live call: the call's video stays until it ends,
         * talk is push-to-talk and the other buttons are its volume. */
        unsigned presses = MUSE_BTN_TALK_PRESS | MUSE_BTN_AUX_PRESS | MUSE_BTN_POWER_PRESS |
                           MUSE_BTN_SPEAKER_PRESS | MUSE_BTN_UP | MUSE_BTN_DOWN | MUSE_BTN_LEFT |
                           MUSE_BTN_RIGHT | MUSE_BTN_ENTER | MUSE_BTN_ESCAPE;
#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
        if (muse_call_owner_active()) {
            presses = 0;   /* the owner's call only: an unproven call state remaps no key */
        }
#endif
#if CONFIG_MUSE_GADGET_BLE_VIDEO
        /* A UI stream (the phone's screen or a web page): the speaker button
         * (blue on the StopWatch with live calls, else the board's aux)
         * opens and closes the stream's menu, drawn by the phone; the others
         * still leave the view. */
        unsigned menu_btn = MUSE_BTN_SPEAKER_PRESS | MUSE_BTN_AUX_PRESS;
        if ((ev & menu_btn) && muse_video_ui_active() && mg_ble_ui_controls()) {
            mg_ble_ui_menu_toggle();
            ev &= ~(menu_btn | MUSE_BTN_SPEAKER_RELEASE | MUSE_BTN_AUX_RELEASE);
        }
#endif
        if ((ev & presses) && muse_video_is_open()) {
            muse_video_close();
            ev &= ~(presses | MUSE_BTN_POWER_LONG);
        }
#endif
#if CONFIG_MUSE_APP_VIEW
        /* An open app takes the talk, speaker and aux buttons; the power key still closes it.
         * A toggled voice note that's recording keeps the talk key: its next press stops it. */
        unsigned keep = NOTE_TOGGLED() ? ev & (MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE) : 0;
        ev = muse_app_view_buttons(ev & ~keep) | keep;
#endif
        if (ev & (MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE)) {
            ESP_LOGI(TAG, "talk key:%s%s", ev & MUSE_BTN_TALK_PRESS ? " press" : "",
                     ev & MUSE_BTN_TALK_RELEASE ? " release" : "");
            if (ev & MUSE_BTN_TALK_PRESS) {
                muse_perf_talk_press();
            }
#if !CONFIG_MUSE_BOARD_SENSECAP_WATCHER
            talk_button(ev);
#endif
        }
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER
        watcher_buttons(ev);
#endif
        keyboard_buttons(ev);
        dedicated_buttons(ev);
        /* A latched key (the 1.75's PMU) can report press and release in the
         * same poll, and a release can land just before the next press; keep
         * them ordered, as talk_button does. */
        bool aux_press = ev & MUSE_BTN_AUX_PRESS;
        bool aux_release = ev & MUSE_BTN_AUX_RELEASE;
        bool aux_edge = false;
        if (aux_down && aux_release) {
            aux_down = false;
            aux_release = false;
            aux_edge = true;
            aux_key(false, true);
        }
        if (!aux_down && aux_press) {
            aux_down = aux_edge = true;
            aux_key(true, true);
            if (aux_release) {
                aux_down = false;
                aux_key(false, true);
            }
        }
        if (!aux_edge) {
            aux_key(aux_down, false);
        }

        muse_wdt_feed(MUSE_WDT_MUSE_INPUT);   /* the gadget watchdog (muse_wdt.h) */
        if (s_power_off_armed && (int32_t)(xTaskGetTickCount() - s_power_off_at) >= 0) {
            s_power_off_armed = false;
            s_power_off_requested = true;
        }
        if (s_power_off_requested) {
            s_power_off_requested = false;
            power_off();
        }
        if (s_restart_requested) {
            s_restart_requested = false;
            restart();
        }
#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
        muse_call_poll();   /* a call that ended takes its video with it */
#endif

        TickType_t now = xTaskGetTickCount();
        if (now - checked >= pdMS_TO_TICKS(SLEEP_CHECK_MS)) {
            checked = now;
            check_sleep();
            voice_stuck_watch();
#if CONFIG_MUSE_GADGET_BLE_AUDIO
            voice_watch();
#endif
        }

        if (now - powered >= pdMS_TO_TICKS(paused ? REST_POWER_MS : POWER_MS)) {
            powered = now;
            muse_power_t p = { .battery_pct = -1 };
            if (muse_board->read_power) {
                bool ok = muse_board->read_power(&p) == ESP_OK;
                if (ok) {
                    muse_state_set_power(&p);
                    muse_battery_note_power(&p, muse_state_on_battery());
                }
                note_power(&p, ok);
            }
        }

        paused = update_power();
        bool napping = update_wifi_nap(now, paused);
        muse_battery_note_state(muse_state_asleep(), s_cpu_low);
        /* Paused, anything but a button (a pairing prompt, an image) is
         * noticed within REST_WAIT_MS. Napping, nothing comes over the
         * network; USB power arriving is noticed within NAP_WAIT_MS. */
        if (paused && muse_board->wait_buttons && !atomic_load(&s_bench_talk_busy)) {   /* a bench hold's release on time */
            int32_t wait_ms = napping ? NAP_WAIT_MS : REST_WAIT_MS;
#if CONFIG_MUSE_MOTION_GESTURES
            if (muse_motion_active()) wait_ms = (int32_t)muse_motion_poll_ms();
#endif
            int32_t due_ms = bench_hold_due_ms();   /* a queued bench hold starts on time too */
            if (due_ms >= 0 && due_ms < wait_ms) {
                wait_ms = due_ms > 0 ? due_ms : 1;
            }
            muse_board->wait_buttons(wait_ms);
        } else {
            vTaskDelay(pdMS_TO_TICKS(paused ? REST_POLL_MS : POLL_MS));
        }
    }
}

/* Reads the rest of a line into buf; false if it was too long (the rest is dropped). */
static bool read_line(char *buf, size_t cap)
{
    size_t len = 0;
    bool whole = true;
    uint8_t c;
    while (muse_console_getc(&c) && c != '\n') {
        if (c == '\r') {
            continue;
        }
        if (len < cap - 1) {
            buf[len++] = (char)c;
        } else {
            whole = false;
        }
    }
    buf[len] = '\0';
    return whole;
}

#if CONFIG_MUSE_HATCH
#define CHAT_OVER_SERIAL "true"

/*
 * "chat+=TEXT" adds a piece of a message and "chat=TEXT" adds the last piece
 * and sends it (TEXT escaped: \n \r \t \\). Every line is acknowledged, and the
 * host waits for that before the next: the driver drops what it has no room for.
 */
static char *s_chat;        /* the message so far */
static size_t s_chat_len;

static void chat_line(char *piece, bool last, bool whole)
{
    size_t n = muse_hatch_unescape(piece);
    if (!s_chat) {
        s_chat = heap_caps_malloc(CHAT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_chat_len = 0;
    }
    const char *err = !whole ? "Line too long" : !s_chat ? "Out of memory" : s_chat_len + n >= CHAT_MAX ? "Too long" : NULL;
    if (err) {
        free(s_chat);
        s_chat = NULL;
        muse_hatch_console("error", err, NULL);
        return;
    }
    memcpy(s_chat + s_chat_len, piece, n);
    s_chat_len += n;
    s_chat[s_chat_len] = '\0';
    muse_hatch_console("ack", NULL, "\"bytes\":%u", (unsigned)s_chat_len);
    if (last) {
        muse_hatch_text_turn(s_chat);   /* frees it */
        s_chat = NULL;
    }
}

/* Drops a half-sent message and ends a typed turn. */
static void chat_cancel(void)
{
    free(s_chat);
    s_chat = NULL;
    muse_hatch_text_cancel();
}
#else
#define CHAT_OVER_SERIAL "false"   /* no PSRAM: replies come over Link, and only short ones */
#endif

/*
 * Bench: puts the face in a mode ("face=thinking") until the voice path or
 * another "face=" moves it on. "face=happy" goes back to idle with the happy
 * hop, as a finished turn does.
 */
static void set_face(const char *name)
{
    static const char *const modes[MUSE_MODE_COUNT] = {
        [MUSE_MODE_BOOT] = "boot",
        [MUSE_MODE_IDLE] = "idle",
        [MUSE_MODE_LISTENING] = "listening",
        [MUSE_MODE_THINKING] = "thinking",
        [MUSE_MODE_SPEAKING] = "speaking",
        [MUSE_MODE_ERROR] = "error",
        [MUSE_MODE_OFF] = "off",
    };
    muse_state_poke();
    bool happy = !strcmp(name, "happy");
    if (happy) {
        name = "idle";   /* where a finished turn hops to */
    }
    for (int m = 0; m < MUSE_MODE_COUNT; m++) {
        if (!strcmp(name, modes[m])) {
            muse_state_set_mode(MUSE_MODE_IDLE);   /* only idle leaves "off" */
            muse_state_set_mode((muse_mode_t)m);
            if (happy) {
                muse_state_make_happy();
            }
            return;
        }
    }
    printf("@face.error unknown face \"%s\": boot idle listening thinking speaking error off happy\n", name);
    fflush(stdout);
}

/* mg.bench is on the Nordic UART log mirror, which any central can read with
 * no pairing, on release gadgets too: it never carries touch coordinates, a
 * passcode key or the passcode's state. Those go to BENCH_XY_TAG and
 * muse_pass, which only the cable console shows. */
#define BENCH_XY_TAG "muse_touch"

static const char *bench_what(const char *what)
{
    return strncmp(what, "pass.", 5) ? what : "a passcode key";
}

/* A bench press on Muse's own screen (press=, tap=): the outcome on mg.bench,
 * where it went on BENCH_XY_TAG. */
static void bench_press(const char *what, const muse_touch_gesture_t *g)
{
    if (!muse_touch_press(g->x1, g->y1, g->x2, g->y2, g->ms)) {
        ESP_LOGW("mg.bench", "tap %s: no touch screen", bench_what(what));
    } else {
        ESP_LOGI("mg.bench", "tap %s: pressed, %d ms", bench_what(what), g->ms);
        ESP_LOGI(BENCH_XY_TAG, "tap %s at %d,%d -> %d,%d %d ms", what, g->x1, g->y1, g->x2, g->y2, g->ms);
    }
}

/* tap=pass.swipe.: a finger's pace from key to key (the cells are ~100 px
 * apart: about 1 px a ms, read every 30 ms by LVGL, each read interpolated
 * by the keypad). */
#define PASS_SWIPE_MS_PER_KEY 120

/* tap=home.* / set.*, on the LVGL task (muse_ui_run_in_lvgl): the controls'
 * places come from LVGL's objects, which only that task may read. */
static void bench_tap_lvgl(const char *what)
{
    muse_touch_gesture_t g;
    bool found = false;
    if (!strncmp(what, "home.", 5)) {
        found = muse_home_bench_point(what + 5, &g);
    } else if (!strncmp(what, "set.", 4)) {
        found = muse_settings_ui_bench_point(what + 4, &g);
    } else if (!strncmp(what, "pass.swipe.", 11)) {
        /* One stroke through the keys (the keypad's three-key swipe on
         * hardware): down on the first, through the others at a finger's
         * pace, lifted on the last, read by the same keypad path as a finger. */
        int xs[6], ys[6];
        int n = muse_passcode_ui_bench_path(what + 11, xs, ys, 6);
        if (n && muse_touch_press_path(xs, ys, n, PASS_SWIPE_MS_PER_KEY * (n - 1))) {
            ESP_LOGI("mg.bench", "tap %s: a stroke through %d keys", bench_what(what), n);
        } else {
            ESP_LOGW("mg.bench", "tap %s: %s", bench_what(what), n ? "no touch screen" : "not on the screen now");
        }
        return;
    } else if (!strncmp(what, "pass.", 5)) {
        found = muse_passcode_ui_bench_point(what + 5, &g);   /* the keypad's keys, as a finger */
    }
    if (!found) {
        ESP_LOGW("mg.bench", "tap %s: not on the screen now", bench_what(what));
        return;
    }
    bench_press(what, &g);
}

/* page=, on the LVGL task. */
static void bench_page_lvgl(const char *page)
{
    muse_ui_goto(page);
    ESP_LOGI("mg.bench", "page %s", page);
}

/* ui.redraw=N, on the LVGL task: a render needs its stack (the console's overflows). */
static void bench_redraw_lvgl(const char *arg)
{
    int n = atoi(arg);
    n = n < 1 ? 1 : n > 200 ? 200 : n;
    uint32_t lo, avg, hi;
    muse_ui_redraw_cost(n, &lo, &avg, &hi);
    ESP_LOGI("mg.perf", "full redraw x%d: min %u us, avg %u us, max %u us", n, (unsigned)lo, (unsigned)avg,
             (unsigned)hi);
    printf("@ui.redraw min %u avg %u max %u\n", (unsigned)lo, (unsigned)avg, (unsigned)hi);
    fflush(stdout);
}

#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
/* The subtitle words' work, on the musegadgets worker (mg_ble_run_on_worker),
 * where a phone's subtitles run: the words, their layout and the band take
 * about 3 KB of stack, and the console task's 3.5 KB overflowed on sub.now
 * (the Watcher). The console only copies the text and queues it. */
#define BENCH_SUB_CLEAR -1
typedef struct {
    int ms_per_word;   /* BENCH_SUB_CLEAR: sub.clear */
    char text[];
} bench_sub_t;

static void bench_sub_run(void *arg)
{
    bench_sub_t *b = arg;
    if (b->ms_per_word == BENCH_SUB_CLEAR) {
        muse_subs_call_end();
    } else {
        muse_subs_bench(b->text, b->ms_per_word);
    }
    free(b);
}

static bool bench_sub_post(const char *text, int ms_per_word)
{
    size_t n = strlen(text);
    bench_sub_t *b = heap_caps_malloc(sizeof(*b) + n + 1, MUSE_BIG_CAPS);
    if (!b) {
        return false;
    }
    b->ms_per_word = ms_per_word;
    memcpy(b->text, text, n + 1);
    if (!mg_ble_run_on_worker(bench_sub_run, b)) {
        free(b);
        return false;
    }
    return true;
}
#endif

/*
 * Console-only commands; false for setup commands. Their buffers are taken
 * per command: without PSRAM, static ones would hold internal RAM for good.
 */
void noise_core_x25519_selftest(void) __attribute__((weak));

static atomic_bool s_x25519_running;

static void x25519_selftest_task(void *arg)
{
    (void)arg;
    noise_core_x25519_selftest();
    atomic_store(&s_x25519_running, false);
    vTaskDelete(NULL);
}

static bool console_command(char *line, bool whole)
{
#if CONFIG_MUSE_HATCH
    if (whole && muse_options_console(line)) {
        muse_state_poke();
        muse_state_set_asleep(false);
        muse_state_nudge();
        return true;
    }
#endif
    {
        /* safe=0 / safe.off (in SAFE: Muse on and a planned restart) and guard? */
        char safe[64];
        if (muse_bootguard_console_word(line, safe, sizeof(safe))) {
            if (safe[0]) {
                printf("@%s\n", safe);
                fflush(stdout);
            }
            return true;
        }
    }
    if (whole && s_console_hook && s_console_hook(line)) {
        return true;
    }
#if CONFIG_MUSE_MIRROR
    if (whole && (!strcmp(line, "mirror") || !strncmp(line, "mirror=", 7))) {
        muse_mirror_serial_session(line[6] ? atoi(line + 7) : 0);
        return true;
    }
#endif
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER
    if (!strcmp(line, "review.next") || !strcmp(line, "review.prev")) {
        set_asleep(false, "reply review");
        muse_state_poke();
        muse_review_step(!strcmp(line, "review.next") ? 1 : -1);
        char page[MUSE_CAPTION_MAX];
        int at = 0, pages = 0;
        muse_review_page(page, sizeof(page), &at, &pages);
        printf("@review {\"page\":%d,\"pages\":%d}\n", at, pages);
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "review.close")) {
        muse_review_close();
        return true;
    }
#endif
    /* noise.x25519: Noise's X25519 checked against RFC 7748 and timed
     * (components/noise_core, X25519Selftest.cpp), on its own task, never
     * the console's: a diagnostic, it sends nothing. */
    if (!strcmp(line, "noise.x25519")) {
        if (!noise_core_x25519_selftest) {
            printf("@noise.x25519 not in this build\n");
        } else if (atomic_exchange(&s_x25519_running, true)) {
            printf("@noise.x25519 already running\n");
        } else if (xTaskCreate(x25519_selftest_task, "x25519", 8192, NULL, 1, NULL) != pdPASS) {
            atomic_store(&s_x25519_running, false);
            printf("@noise.x25519 no memory\n");
        }
        fflush(stdout);
        return true;
    }
    /* Push-to-talk for a bench that has only console lines (the Mosaico's
     * console over ESP-Iris, a Mac client's !IN over the Nordic UART): the same as the d / u keys. */
    if (!strcmp(line, "ptt=1") || !strcmp(line, "ptt=0")) {
        if (line[4] == '1' && !line_talk_ok()) {
            /* A wireless line's talk press never opens the mic on a release image (the wired console's does:
             * line_talk_ok). */
            ESP_LOGW(TAG, "ptt=1: %s: real press needed", s_line_wireless ? "the Nordic UART" : "the console");
            printf("@ptt real press needed\n");
            return true;
        }
        muse_state_poke();
        if (line[4] == '1') {
            link_talk_press();   /* the bench's talk press, as the talk key's (link modes) */
        }
        /* Asleep, a press wakes the screen and goes as a waking one, as the
         * talk button's does (talk_button()). */
        bool wake = line[4] == '1' && muse_state_asleep();
        if (wake) {
            set_asleep(false, "console");
        }
        post(line[4] == '1' ? MUSE_PTT_DOWN : MUSE_PTT_UP, wake);
        return true;
    }
#if CONFIG_MUSE_GADGET_BLE_VIDEO
    if (!strncmp(line, "video.itvl=", 11)) {
        char *end;
        unsigned long itvl = strtoul(line + 11, &end, 10);
        unsigned long ce = *end == ',' ? strtoul(end + 1, NULL, 10) : 0;
        mg_ble_video_tune((uint16_t)itvl, (uint16_t)ce);   /* bench: the link a BLE stream asks for */
        return true;
    }
    if (!strncmp(line, "ui.md5=", 7)) {
        muse_video_set_md5(line[7] == '1');
        return true;
    }
    if (!strcmp(line, "ui.dump")) {
        muse_video_dump();   /* bench: the last UI picture on the console (tools/muse/uidump.py) */
        return true;
    }
    if (!strcmp(line, "ui.dump=buf")) {
        muse_video_dump_to_buffer();   /* ...into a buffer read over ESP-Iris (tools/muse/iris_bench.py) */
        return true;
    }
    if (!strncmp(line, "ui.hq=", 6)) {
        muse_video_set_hq(line[6] == '1');
        return true;
    }
    if (!strcmp(line, "hid.off")) {
        mg_ble_hid_off();   /* bench: no HID service, so a phone's OS stops attaching as its host */
        return true;
    }
    if (!strncmp(line, "hid.key=", 8)) {
        mg_ble_hid_key((uint16_t)strtoul(line + 8, NULL, 0));   /* bench: hid.key=0x224 (Back), 0x223 (Home) */
        return true;
    }
    if (!strcmp(line, "ui.menu")) {
        mg_ble_ui_menu_toggle();   /* bench: the menu button during a UI stream */
        return true;
    }
    if (!strcmp(line, "ble.forget")) {
        mg_ble_forget_bonds();   /* bench: a phone that bonded for the HID stops taking the connection */
        return true;
    }
    if (!strncmp(line, "ui.lat=", 7)) {
        mg_ble_ui_tune((uint16_t)strtoul(line + 7, NULL, 10));   /* bench: a still UI stream's link latency */
        return true;
    }
#endif
    /* Bench touches without a finger, for the phone's side (touch events,
     * HID): "touch=X,Y[,X2,Y2,MS]" presses at X,Y, drags to X2,Y2 over MS
     * and lifts (a tap without the drag). */
    if (!strcmp(line, "restart")) {
        printf("@restart\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
        muse_restart(MUSE_RESTART_CONSOLE);   /* bench: a clean restart without the USB reset lines */
    }
    if (!strcmp(line, "restart=0") || !strcmp(line, "restart=1")) {
        /* The same from a task on that core, 300 ms later (after an RPC's answer). */
        bool ok = muse_restart_later(MUSE_RESTART_CONSOLE, line[8] - '0', 300);
        printf("@restart %s\n", ok ? "soon" : "failed");
        return ok;
    }
    if (!strcmp(line, "restart=panic") || !strcmp(line, "restart=fail")) {
        /* A failure on purpose, 300 ms later: a panic, or a plain unmarked restart, to
         * check that the next boot names it (muse_restart.h). */
        bool ok = muse_restart_test_failure(line[8] == 'p', 300);
        printf("@restart %s\n", ok ? "soon" : "failed");
        return ok;
    }
    if (!strncmp(line, "touch=", 6)) {
        int v[5] = { 0, 0, -1, -1, 0 };
        int n = sscanf(line + 6, "%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4]);
        if (n == 2 || n == 5) {
            muse_touch_inject(v[0], v[1], n == 5 ? v[2] : v[0], n == 5 ? v[3] : v[1], n == 5 ? v[4] : 80);
            printf("@touch ok\n");
        } else {
            printf("@touch usage: touch=X,Y[,X2,Y2,MS]\n");
        }
        return true;
    }
    /* Bench presses on Muse's own screen and buttons, for a bench without
     * fingers (a Mac client's mute check presses every mute path; also over
     * the Nordic UART as !IN <word>: open in bench builds, in release builds
     * only after this connection's token proof). Each logs its
     * outcome on mg.bench (the mirror carries it):
     *   press=X,Y[,X2,Y2,MS]  a finger down at X,Y, moved to X2,Y2 over MS, lifted
     *   tap=<control>         the same at a named control, worked out by its screen:
     *                         home.speaker, home.speaker.hold, home.slider<N>,
     *                         home.status (Status's connection row: the app's version),
     *                         set.speaker, set.volume<N> (Settings > Sound),
     *                         pass.0 .. pass.9, pass.del, pass.forgot, pass.reset
     *                         (the passcode's keypad, wait and Forgot),
     *                         pass.swipe.<k1>,<k2>[,...] (one stroke through those keys)
     *                         call.minus, call.plus, call.panel, call.swipe.down, call.swipe.up
     *   key=speaker|power|aux a press and release of that button (never a hold)
     *   page=face|settings|settings/<page>   (not during a call: it keeps its face)
     *   wake / wake?          the screen on / whether it is ("screen awake|asleep|dark")
     * A press on a sleeping screen is refused ("screen asleep"): wake it first.
     * What reads LVGL's objects (home.*, set.*, page=) runs on the LVGL task
     * (muse_ui_run_in_lvgl); nothing here waits for the display lock. */
    if (!strncmp(line, "press=", 6)) {
        int v[5] = { 0, 0, -1, -1, MUSE_TOUCH_TAP_MS };
        int n = sscanf(line + 6, "%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4]);
        if (n != 2 && n != 5) {
            ESP_LOGW("mg.bench", "press usage: press=X,Y[,X2,Y2,MS]");
            return true;
        }
        if (n == 2) {
            v[2] = v[0];
            v[3] = v[1];
        }
        muse_state_poke();
        if (muse_state_asleep() || muse_ui_dark()) {
            ESP_LOGW("mg.bench", "press: screen asleep");
            return true;
        }
        bool ok = muse_touch_press(v[0], v[1], v[2], v[3], v[4]);
        ESP_LOGI("mg.bench", "press %d ms%s", v[4], ok ? ": pressed" : ": no touch screen");
        ESP_LOGI(BENCH_XY_TAG, "press %d,%d -> %d,%d %d ms%s", v[0], v[1], v[2], v[3], v[4], ok ? "" : ": not pressed");
        return true;
    }
    if (!strncmp(line, "tap=", 4)) {
        const char *what = line + 4;
        muse_state_poke();
        if (muse_state_asleep() || muse_ui_dark()) {
            ESP_LOGW("mg.bench", "tap %s: screen asleep", bench_what(what));   /* a tap would only wake it: wake first */
            return true;
        }
#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
        if (!strncmp(what, "call.", 5)) {
            /* The call's controls are plain arithmetic (no LVGL), so this works
             * over the call's video too, while LVGL is held. */
            muse_touch_gesture_t g;
            if (!muse_call_bench_point(what + 5, &g)) {
                ESP_LOGW("mg.bench", "tap %s: not on the screen now", bench_what(what));
            } else {
                bench_press(what, &g);
            }
            return true;
        }
#endif
        if (!muse_ui_run_in_lvgl(bench_tap_lvgl, what)) {
            ESP_LOGW("mg.bench", "tap %s: busy (another bench press on its way)", bench_what(what));
        }
        return true;
    }
    if (!strncmp(line, "key=", 4)) {
        const char *k = line + 4;
        if (!strncmp(k, "talk", 4) && (k[4] == 0 || k[4] == ',' || !strcmp(k + 4, "?"))) {
            bench_key_talk(k);
            return true;
        }
        unsigned bit = !strcmp(k, "speaker") && muse_board->speaker_button ? MUSE_BTN_SPEAKER_PRESS
                     : !strcmp(k, "power") && muse_board->power_button   ? MUSE_BTN_POWER_PRESS
                     : !strcmp(k, "aux") && muse_board->aux_button       ? MUSE_BTN_AUX_PRESS
                                                                         : 0;
        if (!bit) {
            ESP_LOGW("mg.bench", "key %s: no such button on this board", k);
            return true;
        }
        atomic_fetch_or(&s_bench_keys, bit);
        ESP_LOGI("mg.bench", "key %s pressed", k);
        return true;
    }
    if (!strncmp(line, "page=", 5)) {
        const char *page = line + 5;
#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
        if (muse_call_active()) {
            ESP_LOGW("mg.bench", "page %s: refused during a call", page);
            return true;
        }
#endif
        muse_state_poke();
        if (muse_state_asleep() || muse_ui_dark()) {
            ESP_LOGW("mg.bench", "page %s: screen asleep", page);
            return true;
        }
        if (!muse_ui_run_in_lvgl(bench_page_lvgl, page)) {
            ESP_LOGW("mg.bench", "page %s: busy (another bench press on its way)", page);
        }
        return true;
    }
    if (!strcmp(line, "voice?")) {
        log_voice_where("voice?");
        return true;
    }
    /* The user's Settings mic gain: mic.gain? says it,
     * the connection's set_mic_gain override and the gain captures use;
     * mic.gain=N (automation) sets it as the Settings slider does, saved, on
     * its 3 dB steps from 0 to MUSE_MIC_GAIN_MAX (muse_mic_gain_word.h). */
    {
        int db = 0;
        muse_mic_word_t w = muse_mic_gain_word(line, &db);
        if (w == MUSE_MIC_WORD_BAD) {
            ESP_LOGW("mg.bench", "mic.gain usage: mic.gain=N (0-%d dB) or mic.gain?", MUSE_MIC_GAIN_MAX);
            return true;
        }
        if (w != MUSE_MIC_WORD_NONE) {
            if (w == MUSE_MIC_WORD_SET) {
                muse_settings_set_mic_gain(db);   /* saved; its listener applies it (muse_app.c) */
            }
            char out[96];
            muse_mic_gain_status(out, sizeof(out), muse_settings_mic_gain(), muse_audio_mic_gain_override_db(),
                                 muse_audio_mic_gain());
            ESP_LOGI("mg.bench", "%s%s", w == MUSE_MIC_WORD_SET ? "set: " : "", out);
            return true;
        }
    }
    /* The bench volume (bench
     * and test mutes are RAM-only; muse_vol_word.h): vol=N (automation, 0-100)
     * sets the speaker in RAM only, never NVS, so a reboot comes back at the
     * saved volume; vol=saved (automation) restores the saved one; vol? (a
     * diagnostic) says both. Answered "@vol N (ram; saved M)" / "@vol N saved
     * M" on the console and on mg.bench (the Nordic UART mirror, !IN vol...). */
    {
        int pct = 0;
        muse_vol_word_t w = muse_vol_word(line, &pct);
        if (w == MUSE_VOL_WORD_BAD) {
            ESP_LOGW("mg.bench", "vol usage: " MUSE_VOL_USAGE);
            printf("@vol error: " MUSE_VOL_USAGE "\n");
            fflush(stdout);
            return true;
        }
        if (w != MUSE_VOL_WORD_NONE) {
            if (w == MUSE_VOL_WORD_SET) {
                muse_settings_set_volume_ram(pct);
            } else if (w == MUSE_VOL_WORD_SAVED) {
                muse_settings_volume_restore();
            }
            char out[48];
            muse_vol_text(out, sizeof(out), w == MUSE_VOL_WORD_SET,
                          muse_settings_speaker_on() ? muse_settings_volume() : 0,
                          muse_settings_saved_speaker_volume());
            printf("@%s\n", out);
            fflush(stdout);
            ESP_LOGI("mg.bench", "%s%s", w == MUSE_VOL_WORD_SET ? "set: " : w == MUSE_VOL_WORD_SAVED ? "restored: " : "",
                     out);
            return true;
        }
    }
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    /* The bench guard (mg_minrise.h): ptt.minrise=N
     * (automation, 1-40 dB; 0 off) holds each push-to-talk turn on the gadget
     * until its capture rises N dB over its own floor, and sends none of one
     * that never does; ptt.minrise? (a diagnostic) says it. RAM only: off at
     * every boot, never stored. Answered "@ptt.minrise N|off" on the console
     * and on mg.bench (the Nordic UART mirror, !IN ptt.minrise...). */
    {
        int db = 0;
        mg_minrise_word_t w = mg_minrise_word(line, &db);
        if (w == MG_MINRISE_WORD_BAD) {
            ESP_LOGW("mg.bench", "ptt.minrise usage: " MG_MINRISE_USAGE);
            printf("@ptt.minrise error: " MG_MINRISE_USAGE "\n");
            fflush(stdout);
            return true;
        }
        if (w != MG_MINRISE_WORD_NONE) {
            if (w == MG_MINRISE_WORD_SET) {
                mg_voice_set_minrise((uint8_t)db);
            }
            char out[64];
            mg_minrise_text(out, sizeof(out), mg_voice_minrise());
            printf("@%s\n", out);
            fflush(stdout);
            ESP_LOGI("mg.bench", "%s%s (ptt_nothing=%u)", w == MG_MINRISE_WORD_SET ? "set: " : "", out,
                     (unsigned)mg_voice_nothing_heard());
            return true;
        }
    }
#endif
    /* The speaker's speech chain (muse_spk_dsp.h;
     * the ESP-Mosaico, muse_board_t.speaker_speech_chain): spk.dsp / spk.dsp?
     * show it (a diagnostic), spk.dsp=on|off|HPF,THR,RATIO,MAKEUP (automation)
     * set it for every chain at once, live (a change retunes mid-reply, no
     * click), kept in NVS. Answered "@spk.dsp ..." on the console and on
     * mg.bench (the Nordic UART mirror, !IN spk.dsp...). */
    if (!strncmp(line, "spk.dsp", 7)) {
        float v[4];
        muse_spk_dsp_cfg_t c;
        bool on;
        muse_spk_dsp_config(&c, &on, NULL);
        switch (muse_spk_dsp_word(line + 7, v)) {
        case MUSE_SPK_DSP_WORD_BAD:
            ESP_LOGW("mg.bench", "spk.dsp usage: spk.dsp[?] or spk.dsp=on|off|HPF,THR,RATIO,MAKEUP (HPF 0 or "
                                 "50-4000 Hz, THR -60-0 dBFS, RATIO 1-20, MAKEUP 0-30 dB)");
            printf("@spk.dsp error: spk.dsp=on|off|HPF,THR,RATIO,MAKEUP: HPF 0 (none) or 50..4000 Hz, THR -60..0 "
                   "dBFS, RATIO 1..20, MAKEUP 0..30 dB\n");
            fflush(stdout);
            return true;
        case MUSE_SPK_DSP_WORD_ON:
        case MUSE_SPK_DSP_WORD_OFF:
            on = line[9] == 'n';
            muse_spk_dsp_enable(on);
            break;
        case MUSE_SPK_DSP_WORD_SET:
            c.hpf_hz = v[0];
            c.thr_db = v[1];
            c.ratio = v[2];
            c.makeup_db = v[3];
            muse_spk_dsp_set(&c);
            muse_spk_dsp_enable(true);
            on = true;
            break;
        case MUSE_SPK_DSP_WORD_SHOW:
            break;
        }
        char out[256];
        int len = muse_spk_dsp_describe(out, sizeof(out), &c, on);
        if (!muse_board->speaker_speech_chain && len > 0 && (size_t)len < sizeof(out)) {
            snprintf(out + len, sizeof(out) - (size_t)len, "; this board has no speech chain");
        }
        printf("%s\n", out);
        fflush(stdout);
        ESP_LOGI("mg.bench", "%s", out + 1);
        return true;
    }
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    /* The vibration tests, as Developer > Tests >
     * Vibration plays them, through the same engine and rules:
     * vib=pulse,MS[,PCT] (MS up to the board's 500 ms cap; a pulse under the
     * actuator's shortest is lengthened to it), vib=preset,NAME[,PCT]
     * (tap, double, long, ring, success, error; ring runs 10 s or until
     * vib=preset,ring again or vib=stop), vib=stop, and vib? (the page's
     * status line). They skip haptics_enabled and nothing else. */
    if (!strcmp(line, "vib?") || !strncmp(line, "vib=", 4)) {
        char out[192];
        mg_ble_haptic_console(line, out, sizeof(out));
        ESP_LOGI("mg.bench", "%s", out);
        return true;
    }
#endif
    /* The passcode lock (the shared bench words ?PASS / PASS lock /
     * PASS clear): "pass" says its
     * state, "@pass on locked tries=0 wait=0" or "@pass off" (a diagnostic:
     * never the code, its salt or its hash); "pass lock" locks now and "pass
     * clear" erases only the passcode, the pairing kept (bench hooks, automation). No word unlocks or reads the code;
     * every other word and injected input works locked or not. */
    if (!strcmp(line, "pass clear")) {
        muse_passcode_clear();   /* only the passcode; the pairing stays */
    }
    if (!strcmp(line, "pass") || !strcmp(line, "pass lock") || !strcmp(line, "pass clear")) {
        if (!strcmp(line, "pass lock")) {
            muse_passcode_lock_now();
        }
        char out[128];   /* "on locked tries=0 wait=0" */
        muse_passcode_status(out, sizeof(out));
        printf("@pass %s\n", out);
        fflush(stdout);
        ESP_LOGI("muse_pass", "pass %s", out);   /* never mg.*: its state is the owner's */
        return true;
    }
    /* wake: the screen on (as a press would); wake?: only says whether it is. */
    if (!strncmp(line, "ui.redraw=", 10)) {
        /* Bench: N timed full redraws of the screen on show, on mg.perf;
         * run on the LVGL task (bench_redraw_lvgl). */
        muse_state_poke();
        if (!muse_ui_run_in_lvgl(bench_redraw_lvgl, line + 10)) {
            ESP_LOGW("mg.bench", "ui.redraw: busy (another bench press on its way)");
        }
        return true;
    }
    if (!strcmp(line, "wake?")) {
        ESP_LOGI("mg.bench", "screen %s", muse_state_asleep() ? "asleep" : muse_ui_dark() ? "dark" : "awake");
        return true;
    }
    /* Developer mode (five taps on About's FW): dev? says whether
     * it's on (a diagnostic), dev=1 / dev=0 sets it as the switch does
     * (automation), so a bench or a person finds Developer > Tests. */
    if (!strcmp(line, "dev?") || !strcmp(line, "dev=1") || !strcmp(line, "dev=0")) {
        if (line[3] == '=') {
            muse_settings_set_developer(line[4] == '1');
        }
        ESP_LOGI("mg.bench", "developer mode %s", muse_settings_developer() ? "on" : "off");
        printf("@dev %s\n", muse_settings_developer() ? "on" : "off");
        fflush(stdout);
        return true;
    }
#if CONFIG_MUSE_VIDEO
    /* The Phone camera item, for a bench without fingers: "video" opens the
     * view (asking the phone for a stream), "video=0" closes it. */
    if (!strcmp(line, "video") || !strcmp(line, "video=0")) {
        muse_state_poke();
        if (line[5]) {
            muse_video_close();
        } else {
            muse_video_open();
        }
        printf("@video %s\n", muse_video_is_open() ? "open" : "closed");
        fflush(stdout);
        return true;
    }
    if (!strncmp(line, "vdirty", 6)) {
        /* Changed-only redraw of streamed video (muse_video.h), RAM only: vdirty=0|1 (A/B), vdirty.shadow=N
         * (compare the next N windowed pictures with the whole one), vdirty.tune=COST,PCT,MAX, vdirty? */
        const char *w = line + 6;
        if (w[0] == '=') {
            muse_video_set_dirty(w[1] == '1');
        } else if (!strncmp(w, ".shadow=", 8)) {
            muse_video_dirty_shadow(atoi(w + 8));
        } else if (!strncmp(w, ".tune=", 6)) {
            int cost = 0, pct = 0, max = 0;
            sscanf(w + 6, "%d,%d,%d", &cost, &pct, &max);
            muse_video_dirty_tune(cost, pct, max);
        } else if (strcmp(w, "?")) {
            printf("@vdirty: vdirty=0|1, vdirty.shadow=N, vdirty.tune=COST,PCT,MAX, vdirty?\n");
            fflush(stdout);
            return true;
        }
        char st[400];
        muse_video_dirty_status(st, sizeof(st));
        printf("@vdirty %s\n", st);
        fflush(stdout);
        ESP_LOGI("mg.bench", "vdirty %s", st);
        return true;
    }
    if (!strncmp(line, "lipsync", 7)) {
        /* Lip sync in replies on a board that can't show every picture, RAM only:
         * lipsync=0|1 (0: measured only, the old rules: the A/B), lipsync.tune=LATE,RESYNC,GAP,MAXSKIP, lipsync? */
        const char *w = line + 7;
        if (w[0] == '=') {
            muse_video_avsync(w[1] == '1');
        } else if (!strncmp(w, ".tune=", 6)) {
            int late = 9999, resync = 0, gap = 0, maxskip = -1;
            sscanf(w + 6, "%d,%d,%d,%d", &late, &resync, &gap, &maxskip);
            muse_video_avsync_tune(late, resync, gap, maxskip);
        } else if (strcmp(w, "?")) {
            printf("@lipsync: lipsync=0|1, lipsync.tune=LATE,RESYNC,GAP,MAXSKIP, lipsync?\n");
            fflush(stdout);
            return true;
        }
        char st[512];
        muse_video_avsync_status(st, sizeof(st));
        printf("@lipsync %s\n", st);
        fflush(stdout);
        ESP_LOGI("mg.bench", "lipsync %s", st);
        return true;
    }
#endif
#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
    /* Live call, for a bench without fingers: "call" starts one (as the
     * Settings item does), "call=0" ends it, "call?" only reports. */
    if (!strcmp(line, "framed?")) {
        mg_ble_framed_log();   /* framed Data's state and counters (a diagnostic) */
        printf("@framed?\n");
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "credit.coalesce=0") || !strcmp(line, "credit.coalesce=1")) {
        mg_ble_credit_coalesce(line[16] == '1');   /* bench A/B: request answers coalesced in a live-depth reply */
        printf("@credit.coalesce %c\n", line[16]);
        fflush(stdout);
        return true;
    }
    if (muse_call_steps_word(line)) {
        return true;   /* call.steps: the calling screen's step line on a shipping image */
    }
    if (!strcmp(line, "call.fastlink=0") || !strcmp(line, "call.fastlink=1")) {
        mg_ble_call_fastlink(line[14] == '1');   /* bench A/B: full-speed link from the call's start */
        printf("@call.fastlink %c\n", line[14]);
        fflush(stdout);
        return true;
    }
#if CONFIG_MUSE_VIDEO
    if (muse_call_stats_word(line)) {
        return true;   /* vstats: the call video's stats overlay */
    }
#endif
#if CONFIG_MUSE_VIDEO
    if (!strcmp(line, "call.last?")) {
        /* The last call video's numbers and its last reply's playback, on mg.call (the Nordic UART mirrors it):
         * a bench reads them after the phone that held the link lets go. A diagnostic. */
        char out[400];
        muse_video_last_summary(out, sizeof(out));
        for (char *p = out, *nl; p && *p; p = nl ? nl + 1 : NULL) {
            nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            ESP_LOGI("mg.call", "last: %s", p);
            printf("@call.last %s\n", p);
        }
        mg_ble_call_stats_t cs;
        mg_ble_call_stats(&cs);
        ESP_LOGI("mg.call", "last: reply audio: %u underruns, %u ms silence (stream %u)", (unsigned)cs.underruns,
                 (unsigned)cs.silence_ms, (unsigned)cs.stream);
        printf("@call.last reply audio: %u underruns, %u ms silence\n", (unsigned)cs.underruns, (unsigned)cs.silence_ms);
        fflush(stdout);
        return true;
    }
#endif
    if (!strncmp(line, "call.at", 7) && (line[7] == '=' || line[7] == '?')) {
        /* The bench's timed call (muse_call_at.h): call.at=DELAY_S,LEN_S arms it for the next client that
         * becomes ready, call.at=0 disarms, call.at? reports. RAM only. */
        if (line[7] == '=') {
            unsigned d = 0, l = 0;
            int n = sscanf(line + 8, "%u,%u", &d, &l);
            muse_call_at_set(n == 2 ? d : 0, n == 2 ? l : 0);
        }
        char out[96];
        muse_call_at_describe(out, sizeof(out));
        printf("@call.at %s\n", out);
        fflush(stdout);
        ESP_LOGI("mg.bench", "call.at %s", out);
        return true;
    }
    if (!strcmp(line, "call") || !strcmp(line, "call=0") || !strcmp(line, "call?")) {
        /* call? is an open read: it never counts as activity (an idle gadget still locks and sleeps). */
        if (line[4] == '=') {
            muse_state_poke();
            muse_call_end_from("console");
        } else if (!line[4]) {
            muse_state_poke();
            muse_call_start_from("console");
        }
        int secs;
        muse_call_phase_t phase = muse_call_phase(&secs);
        printf("@call %s %d volume %d\n", muse_call_phase_name(phase), secs, muse_call_volume());
        fflush(stdout);
        return true;
    }
#endif
    if (!strcmp(line, "status")) {
        size_t cap = 1024;   /* long SSID, host and VM names escaped: past 512 */
        char *json = heap_caps_malloc(cap, MUSE_BIG_CAPS);
        if (json) {
            muse_ble_status_json(json, cap);
#if CONFIG_MUSE_GADGET_BLE_AUDIO
            /* Control's queue counts (every reply drop counted; counts only). */
            char replies[256];
            mg_ble_reply_stats(true, replies, sizeof(replies));
            printf("@status {\"board\":\"%s\",\"chat\":%s,\"device\":%s,\"replies\":%s}\n", muse_board->name,
                   CHAT_OVER_SERIAL, json, replies);
#else
            printf("@status {\"board\":\"%s\",\"chat\":%s,\"device\":%s}\n", muse_board->name,
                   CHAT_OVER_SERIAL, json);
#endif
            fflush(stdout);
            free(json);
        }
        return true;
    }
#if CONFIG_MUSE_MOTION_GESTURES
    if (!strcmp(line, "motion")) {
        muse_motion_status();
        return true;
    }
#endif
    bool reset = !strcmp(line, "power.reset");
    if (reset || !strcmp(line, "power")) {
        if (reset) {
            muse_battery_reset();
        }
        const char *saved = muse_battery_saved_json();
        if (saved) {
            printf("@power.saved %s\n", saved);
        }
        size_t cap = 2048;   /* two dozen power locks */
        char *json = heap_caps_malloc(cap, MUSE_BIG_CAPS);
        if (json) {
            muse_battery_json(json, cap);
            printf("@power %s\n", json);
            free(json);
        }
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "nap")) {
        muse_state_set_as_if_battery(true);
        set_asleep(true, "serial");
        s_nap_now = true;
        return true;
    }
    /* Bench: the mic ring on or off without a capture (a test pattern for a
     * camera), and a rectangular panel's corner radius it follows. */
    if (!strcmp(line, "ring=1") || !strcmp(line, "ring=0")) {
        if (line[5] == '1') {
            set_asleep(false, "console");   /* a pattern to look at: the screen on */
            muse_state_poke();
        }
        muse_mic_ring_set(line[5] == '1');
        printf("@ring %c\n", line[5]);
        fflush(stdout);
        return true;
    }
    if (!strncmp(line, "ring.corner=", 12)) {
        muse_mic_ring_set_corner(atoi(line + 12));
        printf("@ring.corner %d\n", muse_mic_ring_corner());
        fflush(stdout);
        return true;
    }
#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
    /* Bench: Live call subtitles as if from the phone (muse_subs.h):
     * "sub=TEXT" reveals it a word every 300 ms, "sub.now=TEXT" at once,
     * "sub.clear" clears. Queued to the musegadgets worker, where the
     * phone's subtitles run (bench_sub_post): never done here. */
    if (!strncmp(line, "sub=", 4) || !strncmp(line, "sub.now=", 8)) {
        bool now = line[3] == '.';
        bool queued = bench_sub_post(line + (now ? 8 : 4), now ? 0 : 300);
        printf("@sub %s\n", queued ? "ok" : "busy");
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "sub.clear")) {
        printf("@sub %s\n", bench_sub_post("", BENCH_SUB_CLEAR) ? "cleared" : "busy");
        fflush(stdout);
        return true;
    }
#endif
#if CONFIG_MUSE_VIDEO
    /* Bench: the next video frame as the panel gets it (subtitles, controls
     * and the mic ring on it), RGB565 big-endian, base64 lines:
     * "@shot W H" then "@s ..." lines then "@shot end"; tools/muse/shot.py. */
#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
    if (!strncmp(line, "shot.auto=", 10)) {
        muse_subs_auto_shot(line[10] == '1');
        printf("@shot.auto %c\n", line[10] == '1' ? '1' : '0');
        return true;
    }
#endif
    /* ">shot.last" prints the frame captured last again (a lossy console:
     * ESP-Iris drops records under load), ">shot.rows=A,B" only its rows A..B. */
    bool shot_last = !strcmp(line, "shot.last") || !strncmp(line, "shot.rows=", 10);
    if (!strcmp(line, "shot") || shot_last) {
        const uint8_t *px;
        int w, h;
        if (shot_last ? !muse_video_shot_last(&px, &w, &h) : !muse_video_shot(&px, &w, &h, 2000)) {
            printf("@shot none (%s)\n", shot_last ? "nothing captured" : "no video frame drawn within 2 s");
            fflush(stdout);
            return true;
        }
        int r0 = 0, r1 = h - 1;
        if (!strncmp(line, "shot.rows=", 10) && sscanf(line + 10, "%d,%d", &r0, &r1) == 2) {
            r0 = r0 < 0 ? 0 : r0 > h - 1 ? h - 1 : r0;
            r1 = r1 < r0 ? r0 : r1 > h - 1 ? h - 1 : r1;
        }
        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        printf("@shot %d %d\n", w, h);
        size_t n = (size_t)(r1 + 1) * w * 2;
        char out[4 * 64 / 3 + 8];
        esp_log_level_set("*", ESP_LOG_NONE);   /* nothing between the lines */
        /* Below the video and the radio while it prints: a stream's frames keep
         * coming (the burst starved the show task and the watchdog restarted
         * the board mid-stream). */
        UBaseType_t prio = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, 1);
#if !CONFIG_IDF_TARGET_ESP32S31
        int lines = 0;
#endif
        for (size_t i = (size_t)r0 * w * 2 / 48 * 48; i < n;) {
            size_t at = i;
            int o = 0;
            for (int k = 0; k < 16 && i < n; k++) {   /* 48 bytes a line */
                uint32_t v = (uint32_t)px[i] << 16 | (i + 1 < n ? (uint32_t)px[i + 1] << 8 : 0) |
                             (i + 2 < n ? px[i + 2] : 0);
                out[o++] = b64[v >> 18 & 63];
                out[o++] = b64[v >> 12 & 63];
                out[o++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
                out[o++] = i + 2 < n ? b64[v & 63] : '=';
                i += 3;
            }
            out[o] = 0;
            printf("@s %x %s\n", (unsigned)at, out);   /* each line says where it goes */
#if CONFIG_IDF_TARGET_ESP32S31
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(6));   /* ESP-Iris carries the console as log records: at its pace */
#else
            if (++lines % 8 == 0) {
                fflush(stdout);
                vTaskDelay(1);   /* let the console drain and the others run */
            }
#endif
        }
        esp_log_level_set("*", ESP_LOG_INFO);
        vTaskPrioritySet(NULL, prio);
        printf("@shot end\n");
        fflush(stdout);
        return true;
    }
#endif
    /* Bench: the screen on (and its idle timer restarted), as a key would.
     * With the wake word built in, "wake" is its status instead (below). */
#if !CONFIG_MUSE_WAKEWORD_ENABLE
    if (!strcmp(line, "wake")) {
        ESP_LOGI("mg.bench", "wake: %s", muse_state_asleep() ? "waking the screen" : "already awake");
        set_asleep(false, "console");
        muse_state_poke();
        link_user("wake");   /* the screen lit by the bench, as by a user: link modes sleep -> ready */
        printf("@wake\n");
        fflush(stdout);
        return true;
    }
#endif
#if CONFIG_IDF_TARGET_ESP32S31
    if (!strcmp(line, "flashops")) {
        muse_faultlog_dump("muse_input");   /* the S31 fault log's flash operations (muse_restart.h) */
        return true;
    }
#endif
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    if (!strcmp(line, "tasks")) {
        /* Bench: each task's stack low-water mark (bytes never used) and where
         * the stack is, to size stacks from measurements (internal RAM). */
        enum { MAX_T = 40 };
        TaskStatus_t *st = heap_caps_malloc(MAX_T * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!st) st = malloc(MAX_T * sizeof(TaskStatus_t));
        UBaseType_t n = st ? uxTaskGetSystemState(st, MAX_T, NULL) : 0;
        static const char STATE[] = "XRBSD?";   /* running, ready, blocked, suspended, deleted, invalid */
        for (UBaseType_t i = 0; i < n; i++) {
            bool ext = esp_ptr_external_ram(st[i].pxStackBase);
            unsigned state = (unsigned)st[i].eCurrentState < sizeof(STATE) - 1 ? (unsigned)st[i].eCurrentState : 5;
            /* The stack's size, to 16 bytes (its top is aligned down): free + used. */
            TaskSnapshot_t snap;
            unsigned size = vTaskGetSnapshot(st[i].xHandle, &snap) == pdTRUE && snap.pxEndOfStack > st[i].pxStackBase
                                ? ((unsigned)((uint8_t *)snap.pxEndOfStack - (uint8_t *)st[i].pxStackBase) + 16u) & ~15u
                                : 0;
            if (size > 65536) size = 0;   /* a task deleted since the list: no size */
            printf("@task %-16s prio %2u free %5u of %5u %s %c\n", st[i].pcTaskName,
                   (unsigned)st[i].uxCurrentPriority, (unsigned)st[i].usStackHighWaterMark, size,
                   ext ? "psram" : "internal", STATE[state]);
        }
        printf("@tasks %u; internal %u free, largest %u, lowest %u\n", (unsigned)n,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        fflush(stdout);
        free(st);
        return true;
    }
#endif
#if CONFIG_MUSE_HEAP_WATCH
    if (!strcmp(line, "heap.allocs")) {
        muse_heapwatch_print_allocs();   /* bench: large internal allocations */
        return true;
    }
    if (!strcmp(line, "heap.tasks")) {
        muse_heapwatch_print_tasks();   /* bench: internal free, lowest, per-task peaks */
        return true;
    }
#endif
    if (!strcmp(line, "crash")) {
        /* Bench (the crash-recovery boot path's timing): a deliberate
         * panic. On a board with the boot guard it counts as a crash reset
         * (two within 10 minutes: SAFE). */
        printf("@crash\n");
        fflush(stdout);
        ESP_LOGW(TAG, "bench: deliberate crash");
        vTaskDelay(pdMS_TO_TICKS(100));
        abort();
    }
    if (!strncmp(line, "power.off=", 10)) {
        /* Bench (power-on timing): power off in N s (1-120), time for
         * the bench to cut USB first, since a board whose latch USB holds
         * (the ESP-Mosaico) keeps running on USB. The input task acts on it. */
        int secs = atoi(line + 10);
        if (secs >= 1 && secs <= 120) {
            s_power_off_at = xTaskGetTickCount() + pdMS_TO_TICKS(secs * 1000);
            s_power_off_armed = true;
            printf("@power.off in %d s\n", secs);
            ESP_LOGW(TAG, "bench: powering off in %d s", secs);
        } else {
            printf("@power.off usage: power.off=1..120\n");
        }
        fflush(stdout);
        return true;
    }
    if (!strncmp(line, "power.wake=", 11)) {
        /* Bench (charge-dark power-off tests): the power chip turns the board on
         * in N s (10-3600), the way back if the plug-in under test doesn't.
         * Then power.off=N or device_action 10. A boot drops it. */
        int secs = atoi(line + 11);
        if (!muse_board->power_on_timer) {
            printf("@power.wake unsupported\n");
        } else if (secs < 10 || secs > 3600) {
            printf("@power.wake usage: power.wake=10..3600\n");
        } else if (muse_board->power_on_timer(secs) != ESP_OK) {
            printf("@power.wake failed\n");
        } else {
            printf("@power.wake in %d s\n", secs);
            ESP_LOGW(TAG, "bench: the power chip turns the board on in %d s", secs);
        }
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "f2.arm") || !strncmp(line, "f2.arm=", 7)) {
        /* The F2 flash check: the next boot on battery checks flash reads
         * (muse_f2.h); "f2.arm=N" also restarts in N s, time for the bench to
         * cut USB power first (usbpower) so that boot is on battery. */
        muse_f2_arm(line[6] == '=' ? atoi(line + 7) : 0);
        return true;
    }
    if (!strcmp(line, "f2.result")) {
        muse_f2_print_result();
        return true;
    }
    if (!strcmp(line, "setup.reset")) {
        /* The setup button's 5 s hold, for a test rig (a phone of another
         * kind takes the gadget over): the console is the USB cable, the
         * same physical access as the button. Wipes Wi-Fi and the pairing. */
        printf("@setup.reset\n");
        fflush(stdout);
        muse_link_reset_setup();
        return true;
    }
    if (!strcmp(line, "pair.confirm")) {
#if CONFIG_MUSE_PAIR_CONFIRM_CONSOLE
        /* The talk button's press, for automated setup tests: the console is
         * the USB cable, so physical access either way. Never push-to-talk.
         * Bench builds only (Kconfig MUSE_PAIR_CONFIRM_CONSOLE). */
        bool confirmed = muse_link_bench_confirm();
        printf("@pair.confirm %s\n", confirmed ? "confirmed" : "none");
#else
        /* A shipping build confirms setup with the button only. */
        printf("@pair.confirm disabled\n");
#endif
        fflush(stdout);
        return true;
    }
#if LV_USE_SNAPSHOT
#if CONFIG_MUSE_LATEST_REPLY
    if (muse_latest_reply_bench(line)) {
        set_asleep(false, "reply review");
        return true;
    }
#endif
    if (!strcmp(line, "ui.demo=1") || !strcmp(line, "ui.demo=0")) {
        muse_ui_bench_demo(!strcmp(line, "ui.demo=1"));
        return true;
    }
    if (!strcmp(line, "ui=settings") || !strcmp(line, "ui=face")) {
        muse_ui_bench_page(!strcmp(line, "ui=settings"));
        return true;
    }
    if (!strncmp(line, "ui.settings=", 12)) {
        if (muse_settings_ui_bench_open(line + 12)) {
            muse_ui_bench_page(true);
        } else {
            printf("@ui.error unknown settings page \"%s\": home wifi muse ble sound sleep battery talk about "
                   "developer passcode watchface tests vibration\n", line + 12);
            fflush(stdout);
        }
        return true;
    }
#if CONFIG_MUSE_AVATAR_SYNC && CONFIG_MUSE_UI_THEME
    if (!strncmp(line,"ui.settings.action=",19)) {
        if (muse_settings_ui_bench_action(line+19)) muse_ui_bench_page(true);
        else {printf("@ui.error settings action must be primary or back, with no action pending\n");fflush(stdout);}
        return true;
    }
#endif
#endif
#if CONFIG_MUSE_WAKEWORD_ENABLE
    if (!strcmp(line, "wake") || !strcmp(line, "wake.on") || !strcmp(line, "wake.off")) {
        if (line[4] == '.') {
            muse_wakeword_set_enabled(!strcmp(line, "wake.on"));
        }
        muse_wakeword_status_t st;
        muse_wakeword_status(&st);
        printf("@wake {\"enabled\":%s,\"listening\":%s,\"runner\":\"%s\",\"wakes\":%u,\"cancels\":%u,"
               "\"submits\":%u,\"drops\":%u}\n",
               st.enabled ? "true" : "false", st.listening ? "true" : "false", st.runner,
               (unsigned)st.wakes, (unsigned)st.cancels, (unsigned)st.submits,
               (unsigned)st.dropped_frames);
        fflush(stdout);
        return true;
    }
#endif
#if CONFIG_MUSE_UI_THEME
    if (!strcmp(line,"theme") || !strncmp(line,"theme=",6)) {
        if(line[5]=='=') {
            const char *name=line+6;
            if(!strcmp(name,"auto"))muse_theme_request_mode(MUSE_THEME_AUTO);
            else if(!strcmp(name,"dark"))muse_theme_request_mode(MUSE_THEME_DARK);
            else if(!strcmp(name,"light"))muse_theme_request_mode(MUSE_THEME_LIGHT);
            else {printf("@theme error invalid\n");return true;}
        }
        static const char *const names[]={"auto","dark","light"};
        uint32_t hint=muse_theme_hint();
        printf("@theme {\"mode\":\"%s\",\"hint_valid\":%s,\"hint_rgb\":%u,\"save_error\":%d}\n",
            names[muse_theme_mode()],(hint&0x1000000u)?"true":"false",(unsigned)(hint&0xffffff),muse_theme_save_error());
        return true;
    }
#endif
    if (!strncmp(line, "face=", 5)) {
        set_face(line + 5);
        return true;
    }
    if (muse_perf_console(line)) {
        return true;   /* bench gestures: swipe=, drag=, mark= */
    }
    if (strncmp(line, "chat", 4) != 0) {
        return false;
    }
#if CONFIG_MUSE_HATCH
    if (!strcmp(line, "chat.cancel")) {
        chat_cancel();
        return true;
    }
    bool last = !strncmp(line, "chat=", 5);
    if (last || !strncmp(line, "chat+=", 6)) {
        chat_line(line + (last ? 5 : 6), last, whole);
        return true;
    }
    return false;
#else
    (void)whole;
    muse_hatch_console("error", "THIS BOARD CAN'T CHAT OVER SERIAL", NULL);
    return true;
#endif
}

/*
 * Bench testing over the USB cable: 'd' / 'u' act as the talk button going
 * down / up for push-to-talk only (they never confirm a pairing), so the
 * voice path can be driven without a finger on the button;
 * 'm' plays a built-in MP3 through the reply decoder; 'a' / 's' press the
 * menu's Down / Select; 'p' sends a screenshot; 'z' / 'w' sleep and wake.
 * A line starting with '>' is a setup command, the same "key=value" text as
 * the BLE CMD characteristic, or one of the console's own: "status" prints
 * the device's state, "power" the battery meter (muse_battery.h) and
 * "power.reset" starts it over, "nap" sleeps and leaves Wi-Fi at once (as
 * two minutes asleep on battery would; 'w' rejoins), "pair.confirm" confirms
 * a pending Muse Link setup as the talk button would (answering
 * "@pair.confirm confirmed" or "none"; "@pair.pending" says one is waiting),
 * "face=" shows a face (see set_face), "wake" prints the wake word state,
 * "wake.on" and "wake.off" toggle listening, and "chat=" sends a typed message
 * to Muse (see chat_line and tools/muse/chat.py).
 */
static void serial_task(void *arg)
{
    if (muse_console_install(SERIAL_RX) != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        uint8_t c;
        if (!muse_console_getc(&c)) {
            continue;
        }
        if (c == 'm') {
            muse_voice_request_mp3test();
        } else if (c == 'a' || c == 's') {
            muse_state_poke();
            link_user("key");   /* the bench's key, a user's for link modes */
            muse_menu_key(c == 'a' ? MUSE_MENU_DOWN : MUSE_MENU_SELECT);
        } else if (c == 'p') {
            muse_ui_request_snapshot();
        } else if (c == 'z' || c == 'w') {
            set_asleep(c == 'z', "serial");
            if (c == 'w') {
                link_user("wake");   /* the screen lit by the bench, as by a user */
            }
        } else if (c == '>') {
            char *line = heap_caps_malloc(SERIAL_LINE, MUSE_BIG_CAPS);
            char none[1];   /* no room: the line is read and dropped */
            bool whole = read_line(line ? line : none, line ? SERIAL_LINE : sizeof(none));
            if (line && whole && muse_local_press_console(line)) {
                /* update.arm: the wired console's own word, an update with no press (never the Nordic UART's) */
            } else if (line && !console_command(line, whole)) {
                muse_ble_command(line, MUSE_LINE_WIRED);   /* the physical console port's (the only wired origin) */
            }
            if (line) { volatile char *p = line; for (int i = 0; i < SERIAL_LINE; i++) p[i] = 0; }
            free(line);
        } else if (c == 'd' || c == 'u') {
            muse_state_poke();
            if (c == 'd') {
                link_talk_press();   /* the bench's talk press, as the talk key's (link modes) */
            }
            post(c == 'd' ? MUSE_PTT_DOWN : MUSE_PTT_UP, false);
        }
    }
}

/*
 * SAFE mode's console. Only while
 * in SAFE, and only because SAFE runs no Muse feature (no UI, audio, input,
 * settings or session) for the Muse words to act on: those words (and injected
 * input: tap=, press=, !IN) would reach code that never started. Every word
 * that needs no Muse stays: safe=0 / safe.off, guard?, restart, then the
 * hook's (muse_glue.c glue_link_console: version, the SDK token, hang=, and
 * musegadgets BLE's bench words where SAFE runs it). Leaving SAFE is a restart
 * into the normal console (muse_input_start), every word and injected input
 * back; nothing here applies outside SAFE (muse_input_console_line).
 */
static bool (*s_safe_hook)(const char *line);

void muse_input_set_safe_console_hook(bool (*hook)(const char *line))
{
    s_safe_hook = hook;
}

static bool safe_console_line(const char *line)
{
    char out[64];
    if (muse_bootguard_console_word(line, out, sizeof(out))) {
        if (out[0]) {
            printf("@%s\n", out);
            fflush(stdout);
        }
        return true;
    }
    if (!strcmp(line, "restart")) {
        printf("@restart\n");
        fflush(stdout);
        muse_restart(MUSE_RESTART_CONSOLE);
    }
    if (s_safe_hook && s_safe_hook(line)) {
        return true;
    }
    printf("@%.32s: not in SAFE mode (Muse is off): safe=0 restarts normally\n", line);
    fflush(stdout);
    return false;
}

static void safe_serial_task(void *arg)
{
    (void)arg;
    if (muse_console_install(SERIAL_RX) != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        uint8_t c;
        if (!muse_console_getc(&c) || c != '>') {
            continue;   /* the bench keys need Muse: ignored */
        }
        char line[96];
        if (read_line(line, sizeof(line)) && !muse_local_press_console(line)) {   /* update.arm: wired only */
            safe_console_line(line);
        }
    }
}

esp_err_t muse_input_safe_console_start(void)
{
    /* Internal stack, as the console's (muse_stack.h). */
    return xTaskCreate(safe_serial_task, "muse_safe_con", 3072, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}

bool muse_input_console_line(char *line)
{
    if (muse_bootguard_mode() == MUSE_BOOT_SAFE) {
        return safe_console_line(line);   /* ESP-Iris's console on the ESP-Mosaico lands here too */
    }
    return console_command(line, true);
}

bool muse_input_console_post(const char *line)
{
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    char buf[CONSOLE_POST_LEN];
    size_t n = strlen(line);
    if (!s_post || n >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, line, n + 1);
    if (xQueueSend(s_post, buf, 0) != pdTRUE) {
        return false;
    }
    if (s_input) {
        xTaskNotifyGive(s_input);   /* out of wait_buttons() */
    }
    return true;
#else
    (void)line;
    return false;
#endif
}

void muse_input_set_console_hook(bool (*hook)(const char *line))
{
    s_console_hook = hook;
}

esp_err_t muse_input_start(QueueHandle_t queue)
{
    s_queue = queue;
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    s_post = xQueueCreateStatic(CONSOLE_POST_LINES, CONSOLE_POST_LEN, s_post_buf, &s_post_q);
#endif
    if (xTaskCreate(input_task, "muse_input", 4096, NULL, 6, &s_input) != pdPASS) {
        return ESP_FAIL;
    }
    /* Bench-test and setup console; the input still works if it can't start.
     * Both stacks stay internal (muse_stack.h): the input task powers the
     * chip off and waits in light sleep, and console lines reach most of the
     * firmware (setup, updates, restarts), so the console can't promise it
     * never maps flash (ota? reads the slots' states). 5 KB: 3128 B used at
     * worst on the Watcher (>tasks over Settings' pages, the faces,
     * sub.now, call, tap=call.panel and call=0; 456 B left of the 3.5 KB it
     * had), the rest a margin for words that print or log. */
    xTaskCreate(serial_task, "muse_serial", 5120, NULL, 5, NULL);
    return ESP_OK;
}

void muse_input_wake(const char *why)
{
    if (!muse_state_asleep()) {
        muse_state_poke();
        return;
    }
    ESP_LOGI(TAG, "waking (%s)", why);
    muse_state_set_asleep(false);   /* pokes too */
    if (s_input) {
        xTaskNotifyGive(s_input);   /* out of wait_buttons(): the display resumes now, not at the next poll */
    }
}

void muse_input_keep_lit(const char *why)
{
    (void)why;
    if (!muse_state_asleep()) {
        muse_state_poke();
    }
}

void muse_input_request_power_off(void)
{
    s_power_off_requested = true;
}

void muse_input_request_restart(void)
{
    s_restart_requested = true;
}
