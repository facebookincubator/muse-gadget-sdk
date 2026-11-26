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

/*
 * The passcode lock's screens, on
 * the v1.0 home's touch panels: the locked face (the home with its buttons
 * gone and a lock glyph for the page dots, muse_home_set_locked), the keypad
 * (the digit ring on round panels, the 3 x 4 grid on rectangles), the wait
 * after 5 wrong codes and Forgot passcode. Settings > Passcode drives the
 * same keypad (muse_settings_ui.c). All on the LVGL task.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "muse_home_ui.h"
#include "muse_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSE_PASS_FLOW_SETUP,    /* Choose, Confirm: "Passcode on" */
    MUSE_PASS_FLOW_CHANGE,   /* the current code, then Choose, Confirm */
    MUSE_PASS_FLOW_OFF,      /* the current code: "Passcode off" */
} muse_pass_flow_t;

#if MUSE_HOME_UI
/* Once, from muse_ui.c's overlays, under the Link setup prompt and the
 * sleep cover (both stay over the lock). */
void muse_passcode_ui_build(void);

/* Every lit frame, before anything is drawn: the lock's visuals follow the
 * lock (a lock at screen-off shows at the first lit frame), requests from
 * the input task open the keypad. face: the home's tile is the one on show. */
void muse_passcode_ui_tick(bool face);

/* The screen turned off: the keypad, the wait and Forgot close (a wake
 * shows the locked face), and a Settings flow ends unfinished. */
void muse_passcode_ui_screen_off(void);

/* The locked face is up (no buttons, no page dots, no menus, no reply text
 * or drawn files): muse_ui.c and muse_home.c hold those back. */
bool muse_passcode_ui_locked_face(void);
/* A passcode screen covers the panel (the keypad, the wait, Forgot). */
bool muse_passcode_ui_covering(void);

/* Settings > Passcode's flows over the keypad; done(true) once it changed
 * something (on, a new code, off), done(false) if it was left. */
void muse_passcode_ui_flow(muse_pass_flow_t flow, void (*done)(bool changed));

/* What's on show for the bench (muse_ui_page_name): "keypad", "wait",
 * "forgot", "locked", or NULL. */
const char *muse_passcode_ui_page_name(void);
/* Bench: where a passcode control is (tap=pass.<what>): pass.0 - pass.9
 * (the key's center), pass.del (delete or Cancel), pass.forgot (the wait
 * screen's card), pass.reset (Forgot's Reset pairing). */
/* tap=pass.swipe.<k1>,<k2>,<k3>: the keys' centres for one stroke through
 * them (2..max keys), as a finger draws it; 0 if the keypad isn't up or a
 * key isn't a digit. LVGL task. */
int muse_passcode_ui_bench_path(const char *keys, int *xs, int *ys, int max);
bool muse_passcode_ui_bench_point(const char *what, muse_touch_gesture_t *g);
#else
/* No passcode screens without the v1.0 home (muse_home_ui.h). */
static inline void muse_passcode_ui_build(void) {}
static inline void muse_passcode_ui_tick(bool face) { (void)face; }
static inline void muse_passcode_ui_screen_off(void) {}
static inline bool muse_passcode_ui_locked_face(void) { return false; }
static inline bool muse_passcode_ui_covering(void) { return false; }
static inline void muse_passcode_ui_flow(muse_pass_flow_t flow, void (*done)(bool changed))
{
    (void)flow;
    (void)done;
}
static inline const char *muse_passcode_ui_page_name(void) { return NULL; }
static inline int muse_passcode_ui_bench_path(const char *keys, int *xs, int *ys, int max)
{
    (void)keys;
    (void)xs;
    (void)ys;
    (void)max;
    return 0;
}
static inline bool muse_passcode_ui_bench_point(const char *what, muse_touch_gesture_t *g)
{
    (void)what;
    (void)g;
    return false;
}
#endif

#ifdef __cplusplus
}
#endif
