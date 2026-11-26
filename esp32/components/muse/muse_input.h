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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

/*
 * Buttons, as the board reports them. The talk button is push-to-talk. On touch
 * boards the aux button puts the screen to sleep; holding it 1.5 s powers off,
 * and two quick presses toggle BLE phone setup. Without touch it opens the
 * two-button menu instead (muse_menu.h), where talk selects. On the
 * StopWatch with live calls, the speaker button (blue) hangs up during a
 * push-to-talk call, or once a press that starts one is queued. Either button
 * wakes from sleep; the talk button's press is also posted, marked `wake`, so
 * holding it on through waking records a note. Waking also retries Wi-Fi at
 * once if it's down. Also runs auto-sleep and refreshes battery status into
 * muse_state. The Watcher's single wheel instead uses a click for sleep/wake,
 * a 300 ms hold for talk, and rotation for reviewing reply pages.
 */

typedef enum {
    MUSE_PTT_DOWN,
    MUSE_PTT_UP,
    MUSE_PTT_END_CALL,   /* StopWatch speaker (blue) press in a call; never a talk-button release */
} muse_ptt_t;

typedef struct {
    muse_ptt_t type;
    bool wake;   /* a press that woke the screen: a tap only wakes */
    uint32_t epoch;   /* a dropped call edge invalidates this and all older gestures */
    uint32_t mark;    /* DOWN: its pending-call mark (muse_input_clear_call_pending), or 0 */
    int64_t at_us;    /* when the edge was seen (esp_timer): the hold threshold counts from the press */
#if CONFIG_MUSE_WAKEWORD_ENABLE
    bool synthetic;   /* posted by the wake word glue, not a finger: an
                       * intentional hold, ended by endpointing, and a real
                       * press during it takes over instead of retriggering */
    uint32_t disables;   /* synthetic: the disable generation at the wake */
#endif
} muse_input_event_t;

/*
 * With CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP: takes the CPU's light-sleep
 * retention memory (about 15 KB of internal RAM on an S3) for good, before
 * Wi-Fi, BLE and the display fragment internal RAM, and turns light sleep on
 * for the rest of the boot behind a power lock that set_cpu_low() lets go of
 * only while resting. If the memory isn't there it logs once and light sleep
 * keeps the CPU powered, as without the option. Call once, early in boot.
 */
void muse_input_power_init(void);

/* Whether the CPU powers down in light sleep (muse_input_power_init() got its memory). */
bool muse_input_cpu_pd(void);
/* The display is stopped and the CPU may drop to the crystal and light-sleep (on battery, dark, resting). */
bool muse_input_cpu_low(void);

/* PTT events are posted to `queue` (items are muse_input_event_t). */
esp_err_t muse_input_start(QueueHandle_t queue);
/* The voice task has entered the call this DOWN's `mark` started, or dropped
 * or rejected that press. Clears the mark only if no newer press has set its
 * own since. */
void muse_input_clear_call_pending(uint32_t mark);
/* Sticky generation fence: queue overflow invalidates a call without relying
 * on another queued event. Only a fresh DOWN in the new epoch can start one. */
uint32_t muse_input_call_epoch(void);

/* CONFIG_MUSE_VOICE_NOTE_TOGGLE: a toggled note is recording, so the other
 * buttons act as if talk were held (no menu, no sleep). From the voice task. */
void muse_input_note_toggled(bool on);

/*
 * Console '>' lines go to hook first, without the '>'; it returns false for a
 * line that isn't its own. The app sets the SDK token this way
 * (main/sdk_token_console.h). Set it before muse_input_start().
 */
void muse_input_set_console_hook(bool (*hook)(const char *line));
/* One console line (without the '>'), as if typed: false if nothing took it. */
bool muse_input_console_line(char *line);
/* SAFE mode's USB console: the recovery words alone (safe=0 / safe.off,
 * guard?, restart), in place of muse_input_start's, which needs Muse. */
esp_err_t muse_input_safe_console_start(void);
/* SAFE's console passes what it doesn't take itself to hook (the words that
 * reach no Muse feature). */
void muse_input_set_safe_console_hook(bool (*hook)(const char *line));
/* The same from another task (the Nordic UART's !IN): queued and run on the
 * input task, never on the caller's. False: too long, or the queue is full. */
bool muse_input_console_post(const char *line);
/* From any task: the screen on now (and its idle timer reset), as a press
 * would, for something the user must see (a reply's text). */
void muse_input_wake(const char *why);
/* An update over Bluetooth asks for its confirm press: wake the screen (to the keypad with "Unlock to update"
 * when locked). Any task. */
void muse_input_update_wake(bool locked);
/* Keeps a lit panel lit (counts as activity), and never wakes a dark one
 * (a reply on the gadget holds the panel, it doesn't turn it on). */
void muse_input_keep_lit(const char *why);
/* Display > Sleep when charged at Never holding a lit screen now: on
 * USB with the battery charged, by the board's own measure
 * (muse_board_t.charged, muse_charged.h). The passcode's "screen stays on"
 * follows it. Any task. */
bool muse_input_charged_hold(void);

/* A user's touch on the panel (its touch-down; a bench press or injected
 * touch alike), from any task, the LVGL read included: the input task hands
 * it to link modes (sleep -> ready) without waiting on BLE here. */
void muse_input_user_touch(void);

/* LVGL microphone press/release edges; handled by the existing input task. */
void muse_input_touch_talk(bool pressed);
/* A board's own touch went down (a hold on the face that may talk): the pairing prompt's showing then, for the press
 * it may report later (MUSE_BTN_TALK_HELD). muse_input_touch_talk(true) notes its own. */
void muse_input_touch_down(void);
/* No person in the loop: a test or automation image (a bench image is both) takes injected input for every human
 * gate (a setup's touch confirm, the press a client's start_mic waits for, the talk press): an automated run never
 * waits. A release image takes only the gadget's own. The one predicate: mg_ble.c defines the same expression
 * (MG_NO_PERSON; test_mg_bench_input checks they match). */
#if (defined(CONFIG_MUSE_GADGET_TEST_BUILD) && CONFIG_MUSE_GADGET_TEST_BUILD) \
    || (defined(CONFIG_MUSE_GADGET_AUTOMATION) && CONFIG_MUSE_GADGET_AUTOMATION)
#define MUSE_NO_PERSON 1
#else
#define MUSE_NO_PERSON 0
#endif
/* A talk press the gadget itself detected: its talk key, or a finger on a talk surface (muse_touch_real()). Passed
 * to the hook (muse_glue: mg_ble_press), which opens a client's start_mic for a few seconds on a release build.
 * Console words, bench keys and bench touches never call it. */
typedef enum { MUSE_PRESS_KEY = 1, MUSE_PRESS_PANEL = 2 } muse_press_t;
void muse_input_press(muse_press_t src);
void muse_input_set_press_hook(void (*hook)(muse_press_t src));
/* Whether a talk press may open the mic. real: the gadget's own press (its talk key, or a finger on a talk surface:
 * muse_touch_real()). On a release image an injected talk press (a bench press or tap on a talk surface, a Nordic
 * UART line's ptt=1 or key=talk) never does; the wired console's ptt=1 and key=talk do in every build (muse_input.c
 * line_talk_ok); test and automation builds take every source (MUSE_INJECTED_TALK_OK). */
bool muse_input_talk_ok(bool real);
/* The console line being run came over the Nordic UART (on the input task). */
bool muse_input_line_wireless(void);
/* The home's hold region saw a tap (released before the talk threshold):
 * the voice task's "nothing sent" answer is the happy bounce, not "Hold to
 * talk". Set before the release is posted; taken once. */
void muse_input_note_touch_tap(void);
bool muse_input_take_touch_tap(void);
/* Settings > Talk applies: the board has a talk key and the
 * home's screen hold region. */
bool muse_input_talk_choice(void);

/* MUSE_BTN_* edges, consumed by the existing input task. */
void muse_input_inject(unsigned events);

/* Plays the goodbye animation and powers off (from the input task). */
void muse_input_request_power_off(void);

/* Ends any live call, says so on screen and restarts the chip (from the input task). */
void muse_input_request_restart(void);
