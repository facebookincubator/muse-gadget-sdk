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

#include "muse_input.h"
#include "i18n.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include "muse_battery.h"
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_console.h"
#include "muse_link.h"
#include "muse_mem.h"
#include "muse_menu.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_WATCHER_CAMERA
#include "boards/watcher_camera.h"
#endif

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
#define HINT_TICKS 60          /* 0.6 s: show the BOOT-key actions */
#define LONG_TICKS 200         /* 2 s: release to enter deep sleep */
#define RESET_TICKS 500        /* 5 s: release to reset pairing and Wi-Fi */
#define SLEEP_CHECK_MS 100

#define SERIAL_RX 1024         /* the driver drops what doesn't fit, so a console line must */
#define SERIAL_LINE 1024
#define CHAT_MAX (192 * 1024)  /* a typed message, assembled from "chat+=" lines */

static QueueHandle_t s_queue;
static TaskHandle_t s_input;
static bool s_talk_down;
static bool s_touch_talk_down;
static bool s_touch_talk_swallow;
static bool s_power_key_down;
static bool s_power_key_swallow;
static bool s_power_key_menu;
static TickType_t s_power_key_started;
static int s_power_key_hint;
static char s_power_key_saved_caption[64];
static bool s_cpu_low;      /* display stopped and the CPU allowed to sleep */
static volatile bool s_power_off_requested;
static volatile bool s_nap_now;   /* ">nap": asleep, as if on battery, nap without waiting WIFI_NAP_MS */

static void post(muse_ptt_t type, bool wake)
{
    muse_input_event_t ev = { .type = type, .wake = wake };
    ESP_LOGI(TAG, "PTT %s%s", type == MUSE_PTT_DOWN ? "down" : "up", wake ? " (waking)" : "");
    xQueueSend(s_queue, &ev, 0);
}

static bool update_power(void);

static void power_off(void)
{
    ESP_LOGI(TAG, "shutting down");
    muse_state_set_asleep(false);
    update_power();
    muse_state_set_progress(0);
    muse_state_set_level(0);
    muse_state_set_mode(MUSE_MODE_OFF);
    muse_state_set_caption(tr("GOODBYE!"));
    vTaskDelay(pdMS_TO_TICKS(GOODBYE_MS));
    esp_err_t err = muse_board->power_off();
    /* Only reached if the board couldn't power off. */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGE(TAG, "power-off failed (%s)", esp_err_to_name(err));
    muse_state_set_mode(MUSE_MODE_IDLE);
    muse_state_set_caption(tr("COULDN'T POWER OFF"));
}

static void set_asleep(bool asleep, const char *why)
{
    if (asleep != muse_state_asleep()) {
        ESP_LOGI(TAG, "%s (%s)", asleep ? "sleeping" : "waking", why);
        muse_state_set_asleep(asleep);
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
    muse_settings_set_ble_on(on);
    muse_ble_status_t b;
    muse_ble_status(&b);
    if (on && b.name[0]) {
        muse_state_set_caption("PHONE SETUP: %s", b.name);
    } else {
        muse_state_set_caption("PHONE SETUP %s", on ? "ON" : "OFF");
    }
}

/*
 * The 1.85B has one app-readable key (BOOT). A tap toggles the display sleep;
 * a 2-5 s hold enters deep sleep, and a 5+ s hold resets pairing/Wi-Fi.
 * Pairing confirmation takes precedence. Touch provides push-to-talk.
 */
static void power_key(unsigned ev)
{
    if ((ev & MUSE_BTN_TALK_PRESS) && !s_power_key_down && !s_power_key_swallow) {
        if (muse_link_talk_press()) {
            muse_state_poke();
            s_power_key_swallow = true;
            return;
        }
        if (muse_state_asleep()) {
            set_asleep(false, muse_board->talk_button);
            s_power_key_swallow = true;
            return;
        }
        s_power_key_down = true;
        s_power_key_started = xTaskGetTickCount();
        s_power_key_hint = -1;
        s_power_key_menu = muse_menu_is_open();
        uint32_t version = UINT32_MAX;
        muse_state_caption(s_power_key_saved_caption, sizeof(s_power_key_saved_caption), &version);
    }

    if ((ev & MUSE_BTN_TALK_RELEASE) && s_power_key_swallow) {
        s_power_key_swallow = false;
        return;
    }
    if ((ev & MUSE_BTN_TALK_RELEASE) && s_power_key_down) {
        TickType_t held = xTaskGetTickCount() - s_power_key_started;
        s_power_key_down = false;
        if (held >= pdMS_TO_TICKS(RESET_TICKS * POLL_MS)) {
            muse_state_set_caption(tr("RESETTING SETUP"));
            muse_link_reset_setup();
        } else if (held >= pdMS_TO_TICKS(LONG_TICKS * POLL_MS)) {
            power_off();
        } else if (s_power_key_menu) {
            muse_menu_key(MUSE_MENU_SELECT);
        } else {
            if (s_power_key_hint) {
                muse_state_set_caption("%s", s_power_key_saved_caption);
            }
            set_asleep(true, muse_board->talk_button);
        }
        s_power_key_menu = false;
        s_power_key_hint = 0;
    }
}

static void power_key_tick(void)
{
    if (!s_power_key_down || s_power_key_menu) return;
    TickType_t held = xTaskGetTickCount() - s_power_key_started;
    int hint = held >= pdMS_TO_TICKS(RESET_TICKS * POLL_MS) ? 2
             : held >= pdMS_TO_TICKS(LONG_TICKS * POLL_MS) ? 1
             : held >= pdMS_TO_TICKS(HINT_TICKS * POLL_MS) ? 0 : -1;
    if (hint < 0 || hint == s_power_key_hint) return;
    s_power_key_hint = hint;
    muse_state_set_caption(hint == 2 ? tr("RELEASE TO RESET SETUP")
                           : hint == 1 ? tr("RELEASE TO POWER OFF")
                                       : tr("HOLD 2s OFF / 5s RESET"));
}

/*
 * Aux button: short press sleeps, a hold powers off, any press wakes.
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
        held++;
        if (held == HINT_TICKS) {
            uint32_t v = UINT32_MAX;
            muse_state_caption(saved_caption, sizeof(saved_caption), &v);
            muse_state_set_caption(tr("HOLD TO POWER OFF"));
            hinted = true;
        } else if (held == LONG_TICKS) {
            swallow = true;
            power_off();
        }
        return;
    }
    if (edge && !pressed && !swallow) {
        if (hinted) {
            muse_state_set_caption("%s", saved_caption);   /* let go early: cancel */
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
    } else if (!s_talk_down) {
        muse_state_poke();
        muse_menu_key(MUSE_MENU_DOWN);
    }
}

static void aux_key(bool pressed, bool edge)
{
    if (muse_board->touch) {
        aux_button(pressed, edge);
    } else {
        menu_button(pressed, edge);
    }
}

/* Talk button: push-to-talk, or Select while the menu is open. Asleep, the
 * press wakes and is posted as a waking one: muse_voice records only if it's
 * still held once awake. */
static void talk_button(unsigned ev)
{
    bool talk_down = s_talk_down;
    static bool swallow;
#if CONFIG_MUSE_WATCHER_CAMERA
    static TickType_t last_release;
    if ((ev & MUSE_BTN_TALK_PRESS) && !muse_state_asleep()
        && !muse_menu_is_open() && last_release
        && xTaskGetTickCount() - last_release <= pdMS_TO_TICKS(350)) {
        ESP_LOGI(TAG, "wheel double-click: camera preview/shutter");
        last_release = 0;
        watcher_camera_preview_toggle();
        swallow = true;
        return;
    }
#endif

    /* A quick tap can latch press and release in the same poll, and a release
     * can land just before the next press; keep them ordered. */
    bool released = ev & MUSE_BTN_TALK_RELEASE;
#if CONFIG_MUSE_WATCHER_CAMERA
    bool saw_release = released;
#endif
    if ((talk_down || swallow) && released) {
        if (talk_down) {
            post(MUSE_PTT_UP, false);
        }
        talk_down = swallow = false;
        released = false;
    }
    if (!talk_down && !swallow && (ev & MUSE_BTN_TALK_PRESS)) {
        if (muse_link_talk_press()) {
            /* Confirmed a Muse app pairing (Link's setup button). */
            muse_state_poke();
            swallow = true;
        } else if (muse_state_asleep()) {
            set_asleep(false, muse_board->talk_button);
            post(MUSE_PTT_DOWN, true);
            talk_down = true;
        } else if (muse_menu_is_open()) {
            muse_state_poke();
            if (!muse_board->keyboard) muse_menu_key(MUSE_MENU_SELECT);
            swallow = true;
        } else {
            post(MUSE_PTT_DOWN, false);
            talk_down = true;
        }
    }
    if ((talk_down || swallow) && released) {
        if (talk_down) {
            post(MUSE_PTT_UP, false);
        }
        talk_down = swallow = false;
    }
#if CONFIG_MUSE_WATCHER_CAMERA
    if (saw_release && !swallow) {
        last_release = xTaskGetTickCount();
    }
#endif
    s_talk_down = talk_down;
}

/* Keyboard menus don't repurpose Space/GO as Select. Navigation wakes the
 * screen without accidentally changing a setting; Enter can confirm pairing. */
static void keyboard_buttons(unsigned ev)
{
    const unsigned mask = MUSE_BTN_UP | MUSE_BTN_DOWN | MUSE_BTN_LEFT |
                          MUSE_BTN_RIGHT | MUSE_BTN_ENTER | MUSE_BTN_ESCAPE;
    if (!(ev & mask) || s_talk_down) return;
    if ((ev & MUSE_BTN_ENTER) && muse_link_talk_press()) {
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

/* A pairing prompt wakes the screen and keeps it on; otherwise idle sleeps. */
static void check_sleep(void)
{
    muse_ble_status_t ble;
    muse_ble_status(&ble);
    bool prompt = ble.passkey || muse_link_state() == MUSE_LINK_CONFIRM;
    if (prompt) {
        set_asleep(false, "pairing");
        return;
    }
    int after = muse_settings_sleep_s();
    float mode_t;
    if (after && !muse_state_asleep() && muse_state_mode(&mode_t) == MUSE_MODE_IDLE
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
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power management: %s", esp_err_to_name(err));
    }
#else
    (void)low;
#endif
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
    if (!muse_state_asleep() && s_nap_now) {
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

static TickType_t s_single_press;   /* 单键板：说话键按下的时刻 */

static void input_task(void *arg)
{
    (void)arg;
    bool aux_down = false;
    bool paused = false;
    TickType_t checked = xTaskGetTickCount() - pdMS_TO_TICKS(SLEEP_CHECK_MS);
    TickType_t powered = xTaskGetTickCount() - pdMS_TO_TICKS(POWER_MS);

    for (;;) {
        unsigned ev = muse_board->poll_buttons();
        if (ev & (MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE)) {
            ESP_LOGI(TAG, "talk key:%s%s", ev & MUSE_BTN_TALK_PRESS ? " press" : "",
                     ev & MUSE_BTN_TALK_RELEASE ? " release" : "");
            /* 单键板（1.85B 只有 BOOT 可读）：这颗键是唯一能按的东西，全给"按住说话"
             * 的话，短按等于一次被丢弃的录音，按键看起来就是没作用。所以短按（这里定
             * 350ms，和 watcher 双击窗一致）额外当一次菜单键；语音层对这种过短录音本来
             * 就会自己丢弃并提示，不需要动它的状态机。 */
            if (muse_board->single_button && !muse_state_asleep()) {
                if (ev & MUSE_BTN_TALK_PRESS) {
                    s_single_press = xTaskGetTickCount();
                } else if (s_single_press &&
                           xTaskGetTickCount() - s_single_press < pdMS_TO_TICKS(350)) {
                    s_single_press = 0;
                    /* 触摸板上"菜单"就是设置页；muse_menu_key() 那条队列在这里没起 */
                    bool to_settings = muse_ui_toggle_settings();
                    ESP_LOGI(TAG, "short tap: %s", to_settings ? "settings" : "home");
                } else {
                    s_single_press = 0;
                }
            }
            if (muse_board->button_power_controls) power_key(ev);
            else talk_button(ev);
        }
        power_key_tick();
        keyboard_buttons(ev);
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

        if (s_power_off_requested) {
            s_power_off_requested = false;
            power_off();
        }

        TickType_t now = xTaskGetTickCount();
        if (now - checked >= pdMS_TO_TICKS(SLEEP_CHECK_MS)) {
            checked = now;
            check_sleep();
        }

        if (now - powered >= pdMS_TO_TICKS(paused ? REST_POWER_MS : POWER_MS)) {
            powered = now;
            muse_power_t p = { .battery_pct = -1 };
            if (muse_board->read_power && muse_board->read_power(&p) == ESP_OK) {
                muse_state_set_power(&p);
                muse_battery_note_power(&p, muse_state_on_battery());
            }
        }

        paused = update_power();
        bool napping = update_wifi_nap(now, paused);
        muse_battery_note_state(muse_state_asleep(), s_cpu_low);
        /* Paused, anything but a button (a pairing prompt, an image) is
         * noticed within REST_WAIT_MS. Napping, nothing comes over the
         * network; USB power arriving is noticed within NAP_WAIT_MS. */
        if (paused && muse_board->wait_buttons) {
            muse_board->wait_buttons(napping ? NAP_WAIT_MS : REST_WAIT_MS);
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
    const char *err = !whole ? tr("LINE TOO LONG") : !s_chat ? tr("OUT OF MEMORY") : s_chat_len + n >= CHAT_MAX ? tr("TOO LONG") : NULL;
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

/*
 * Console-only commands; false for setup commands. Their buffers are taken
 * per command: without PSRAM, static ones would hold internal RAM for good.
 */
static muse_console_hook_t s_console_hook;

void muse_console_set_local_hook(muse_console_hook_t hook)
{
    s_console_hook = hook;
}

static bool console_command(char *line, bool whole)
{
    if (s_console_hook && s_console_hook(line)) {
        return true;
    }

    if (!strcmp(line, "status")) {
        size_t cap = 1024;   /* long SSID, host and VM names escaped: past 512 */
        char *json = heap_caps_malloc(cap, MUSE_BIG_CAPS);
        if (json) {
            muse_ble_status_json(json, cap);
            printf("@status {\"board\":\"%s\",\"chat\":%s,\"device\":%s}\n", muse_board->name,
                   CHAT_OVER_SERIAL, json);
            fflush(stdout);
            free(json);
        }
        return true;
    }
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
    if (!strncmp(line, "face=", 5)) {
        set_face(line + 5);
        return true;
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
    muse_hatch_console("error", tr("THIS BOARD CAN'T CHAT OVER SERIAL"), NULL);
    return true;
#endif
}

/*
 * Bench testing over the USB cable: 'd' / 'u' act as the talk button going
 * down / up, so the voice path can be driven without a finger on the button;
 * 'm' plays a built-in MP3 through the reply decoder; 'a' / 's' press the
 * menu's Down / Select; 'p' sends a screenshot; 'z' / 'w' sleep and wake.
 * A line starting with '>' is a setup command, the same "key=value" text as
 * the BLE CMD characteristic, or one of the console's own: "status" prints
 * the device's state, "power" the battery meter (muse_battery.h) and
 * "power.reset" starts it over, "nap" sleeps and leaves Wi-Fi at once (as
 * two minutes asleep on battery would; 'w' rejoins), "face=" shows a face
 * (see set_face), and "chat=" sends a typed message to Hatch (see chat_line
 * and tools/muse/chat.py).
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
            muse_menu_key(c == 'a' ? MUSE_MENU_DOWN : MUSE_MENU_SELECT);
        } else if (c == 'p') {
            muse_ui_request_snapshot();
        } else if (c == 'z' || c == 'w') {
            set_asleep(c == 'z', "serial");
        } else if (c == '>') {
            char *line = heap_caps_malloc(SERIAL_LINE, MUSE_BIG_CAPS);
            char none[1];   /* no room: the line is read and dropped */
            bool whole = read_line(line ? line : none, line ? SERIAL_LINE : sizeof(none));
            if (line && !console_command(line, whole)) {
                muse_ble_command(line);
            }
            free(line);
        } else if (c == 'd' || c == 'u') {
            muse_state_poke();
            post(c == 'd' ? MUSE_PTT_DOWN : MUSE_PTT_UP, false);
        }
    }
}

esp_err_t muse_input_start(QueueHandle_t queue)
{
    s_queue = queue;
    if (xTaskCreate(input_task, "muse_input", 4096, NULL, 6, &s_input) != pdPASS) {
        return ESP_FAIL;
    }
    /* Bench-test and setup console; the input still works if it can't start. */
    xTaskCreate(serial_task, "muse_serial", 3584, NULL, 5, NULL);
    return ESP_OK;
}

void muse_input_request_power_off(void)
{
    s_power_off_requested = true;
}

void muse_input_touch_ptt(bool down)
{
    if (!s_queue) return;
    if (down) {
        if (s_touch_talk_down || s_touch_talk_swallow) return;
        if (muse_link_talk_press()) {
            muse_state_poke();
            s_touch_talk_swallow = true;
        } else if (muse_state_asleep()) {
            set_asleep(false, "touch");
            s_touch_talk_swallow = true;
        } else if (muse_menu_is_open()) {
            muse_menu_key(MUSE_MENU_SELECT);
            s_touch_talk_swallow = true;
        } else {
            post(MUSE_PTT_DOWN, false);
            s_touch_talk_down = true;
        }
    } else if (s_touch_talk_swallow) {
        s_touch_talk_swallow = false;
    } else if (s_touch_talk_down) {
        post(MUSE_PTT_UP, false);
        s_touch_talk_down = false;
    }
}
