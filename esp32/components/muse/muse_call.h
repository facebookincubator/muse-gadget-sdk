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
 * Live call: the client is the Muse app, which implements the call on its own session.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "muse_touch.h"

/*
 * A live call with the Muse (CONFIG_MUSE_GADGET_BLE_LIVE_CALL): the user
 * starts it from Settings (or the menu), the musegadgets client (the phone)
 * holds the call, and reports its state back (mg_command_live_call). From
 * the moment the call is asked for until its first picture is drawn the
 * screen is black with "Calling…" centered and nothing else ("Waiting for
 * phone…" while no app is ready); the first picture
 * replaces it directly. Without video the face takes over once the call is
 * live, with a "Live m:ss" pill under the state line; push-to-talk and
 * replies work as outside a call. On-screen controls (End call, volume − / +
 * and a level bar; muse_call_controls.h) show only after a tap, for 3 s after
 * the last touch, over the calling screen, the face and video alike; up, a
 * tap away from them dismisses them at once and a tap on one acts and
 * re-arms the 3 s, but the touch that showed
 * them never dismisses them on its own lift; where the picture isn't a talk
 * surface (the video, the calling screen, the face with Settings > Talk on
 * Button) they come up at the touch-down, a drag or a hold keeping them. A vertical swipe sets the volume too, and the board's
 * volume keys (muse_call_volume_key). Video the phone streams during the
 * call shows at once, no button closes it, and it closes when the call
 * ends. In a call the face doesn't slide off to settings. A call never
 * raises a muted speaker. Over, it shows the end banner, then the home.
 */

typedef enum {
    MUSE_CALL_NONE,
    MUSE_CALL_ASKED,        /* the user started one; the phone hasn't answered yet */
    MUSE_CALL_CONNECTING,   /* the phone is connecting it */
    MUSE_CALL_LIVE,
} muse_call_phase_t;

/* From any task. */
void muse_call_start(void);
/* The same, logging what started it ("menu", "console", "touch: Settings"). */
void muse_call_start_from(const char *why);
void muse_call_end(void);
/* The same, saying what ended it ("menu", "console", "End call control"):
 * logged, and the call timeline's summary carries it (mg.lct end=). */
void muse_call_end_from(const char *why);
/* The bench's timed call (console call.at=DELAY_S,LEN_S, muse_call_at.h): armed (len_s > 0) or off. */
void muse_call_at_set(unsigned delay_s, unsigned len_s);
void muse_call_at_describe(char *out, size_t cap);
/* A touch started or ended it: its place, its hold and the touch-downs of the
 * last 5 s go on the mg.call line, and touch(x,y,hold) in the call
 * timeline's summary (mg.lct by= / end=). */
void muse_call_start_touch(const char *why, int x, int y, uint32_t hold_ms);
void muse_call_end_touch(const char *why, int x, int y, uint32_t hold_ms);
/* The call on screen came from a central without the connection's token proof: it
 * doesn't hold off the passcode lock, and goes once the gadget locks or sleeps. */
bool muse_call_unproven(void);
/* A call is on and it's the owner's (not an unproven central's): both from one read of the call's state, so a call
 * on its way out is never seen as on and the owner's. */
bool muse_call_owner_active(void);
/* Asked, connecting or live. */
bool muse_call_active(void);
/* The call's controls panel is up and screen point x, y is on one of its hit
 * areas (End call, − or +): the home's hold region doesn't start a talk turn
 * there (a long press on End call never talks, then hangs up). LVGL task. */
bool muse_call_controls_at(int x, int y);
/* A card that asks for a press or a code is up or gone (LVGL's task, muse_ui.c): while it's up the call's
 * controls are hidden, over video too, and a touch is the card's, never the call's. */
void muse_call_card(bool up);
/* A call ended less than ms ago (none on now). A client capture that
 * starts in that window belongs to the call (the app's codec switch back),
 * never a talk turn. */
bool muse_call_ended_within(int ms);
/* The phase, and how long the call has been live (seconds). */
muse_call_phase_t muse_call_phase(int *live_s);
const char *muse_call_phase_name(muse_call_phase_t phase);
/* The calling screen's step line (muse_call_steps_visible):
 * what the BLE call start is doing ("Asked the phone", "The phone is
 * connecting the call", "Live, waiting for the phone's picture"); "" with no
 * call. app_ready: a phone app can take the call (muse_call_available). */
const char *muse_call_step_name(muse_call_phase_t phase, bool app_ready);
/* A client that can take a call is connected. */
bool muse_call_available(void);
/* The board can show a Live call at all (PSRAM for its picture, controls and
 * subtitles); without, no Call button or menu row. */
bool muse_call_supported(void);
/* The speaker level as the call hears it (0-100, 0 while the speaker is off). */
int muse_call_volume(void);
/* A volume key in a call (the input task): steps sixteenths and shows the controls. */
void muse_call_volume_key(int steps);
/* A volume change the gadget didn't make: in a call
 * the controls show it, with by ("Set by phone") on their hint line. False:
 * no call, or no controls panel on this screen. LVGL task. */
bool muse_call_volume_by(const char *by);

/* The client's mg_live_call_state_t (mg_ble's live_call; the BLE worker, so it doesn't block). */
void muse_call_client_state(uint8_t state);

/* The input task, every poll: what the client's state left to do (closing the video). */
void muse_call_poll(void);

/* LVGL task: the calling screen (on the face's screen), the pill on the
 * face, centered at y from the top, the controls (over both, and over video
 * through muse_video_set_overlay) and their touches, and keeping them current. */
void muse_call_build(lv_obj_t *face, int y, bool small);
/* True while a call is up (the face's name line gives way to it). */
bool muse_call_tick(void);
/* Bench: where a named call control is, or a swipe (muse_touch.h; any task:
 * plain arithmetic, so it works over the call's video too). */
bool muse_call_bench_point(const char *what, muse_touch_gesture_t *g);
/* The console word for the call video's stats overlay: "vstats" toggles
 * it, "vstats=1" / "vstats=0" set it, "vstats?" reports; each answers
 * "@vstats on|off" on the USB console, where the overlay's lines print once a
 * second while it's up ("@vstats ..."). False: not the word. Video builds. */
bool muse_call_stats_word(const char *line);
/* The calling screen's step line: shown on a bench/debug image
 * (CONFIG_MUSE_BENCH_IMAGE) or while the console word call.steps=1 is set. */
bool muse_call_steps_visible(bool bench_image, bool word_on);
/* The console word: "call.steps=1" / "call.steps=0" set it (RAM only: off at
 * every boot), "call.steps?" reports; each answers "@call.steps on|off".
 * False: not the word. Live call builds. */
bool muse_call_steps_word(const char *line);
