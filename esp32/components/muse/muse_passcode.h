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
 * The passcode lock:
 * device-local, set and entered on the gadget, off by default. This is the
 * gadget's one copy of the rules (muse_passcode_core.h) behind a mutex, so
 * the LVGL task (the screens), the input task (a talk press while locked),
 * the console and the pairing's erase (main/app.c) can all reach it.
 *
 * What it never does: send, print or log the code, its salt or its hash;
 * gate a console word or injected input (`pass lock` is a bench word,
 * `pass` a diagnostic); sit in front of a recovery tier (the BLE, USB
 * and button tiers run below the UI; SAFE mode never starts it).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "muse_home_ui.h"
#include "muse_passcode_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What opened the keypad: its prompt says why. */
typedef enum {
    MUSE_PASS_WHY_TOUCH = 1,   /* "Enter passcode" */
    MUSE_PASS_WHY_TALK,        /* "Unlock to talk" */
    MUSE_PASS_WHY_UPDATE,      /* "Unlock to update": an update over Bluetooth asks for its confirm press */
    MUSE_PASS_WHY_CALL,        /* "Unlock to call" */
    MUSE_PASS_WHY_PHONE_CALL,  /* "Call from phone": the phone started a call */
} muse_pass_why_t;

#if MUSE_HOME_UI
/* Loads the record (a passcode that's on starts locked: a reboot locks).
 * Once, before the UI starts. */
void muse_passcode_init(void);

/* The board shows the lock: the v1.0 home on a touch panel (muse_ui.c sets
 * it once its screens are up). Without it nothing locks. */
void muse_passcode_set_supported(bool supported);
bool muse_passcode_supported(void);

/* Any task, cheap. */
bool muse_passcode_on(void);
bool muse_passcode_locked(void);
/* A talk press (the input task): "After 5 min" applied first if it lapsed
 * while the screen was dark (the UI's tick or screen-on may not have run
 * yet), then whether it's locked. */
bool muse_passcode_locked_at_press(void);

/* Any task: a talk or call press found the gadget locked. The press itself
 * is dropped (never replayed); the keypad opens with its reason. */
void muse_passcode_request_keypad(muse_pass_why_t why);
/* The UI takes the request (0: none). */
int muse_passcode_take_keypad_request(void);

/* The UI's view of the lock rules (LVGL task). */
void muse_passcode_screen_off(bool busy);
void muse_passcode_screen_on(void);
void muse_passcode_tick(uint32_t idle_ms, bool stays_on, bool busy);
muse_pass_result_t muse_passcode_check(const char digits[MUSE_PASS_DIGITS]);
int64_t muse_passcode_wait_left_ms(void);
/* A new code, on: false if the salt can't be drawn (no radio on) or the
 * hash failed. */
bool muse_passcode_set(const char digits[MUSE_PASS_DIGITS]);
void muse_passcode_turn_off(void);
muse_pass_lock_t muse_passcode_lock_mode(void);
void muse_passcode_set_lock_mode(muse_pass_lock_t mode);

/* Any task: `pass lock` (bench). Nothing without a passcode. */
void muse_passcode_lock_now(void);
/* Any task: the pairing's erase (Reset pairing, the app's Forget over a
 * proven link, a setup reset, a completed Link setup, a factory reset):
 * the passcode lives and dies with the pairing. */
void muse_passcode_forget(const char *why);
/* The stored record is what RAM says: nothing waiting to be saved (a failed save stays waiting). An owner change's
 * mark retires only then (app.c owner_wipe_retire). */
bool muse_passcode_store_clean(void);
/* Any task: `pass clear` (a bench console word): erases
 * only the passcode (its wrong count and wait too); the pairing stays. */
void muse_passcode_clear(void);
/* Any task: the console's `pass` line, "on locked tries=0 wait=0" or "off". */
void muse_passcode_status(char *out, size_t n);

/* The platform (muse_passcode_esp.c on the gadget, the simulator's stand-in
 * on a desktop): the settings store's record, SHA-256 and the salt. */
bool muse_passcode_platform_load(muse_pass_rec_t *rec);
bool muse_passcode_platform_save(const muse_pass_rec_t *rec);
bool muse_passcode_platform_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
bool muse_passcode_platform_salt(uint8_t salt[MUSE_PASS_SALT_LEN]);
/* ...and every lock change (locked or not, the board showing the lock):
 * the video view holds a UI stream or camera view while locked. */
void muse_passcode_platform_locked(bool locked);
#else
/* No passcode without the v1.0 home (muse_home_ui.h): never on, never locked,
 * nothing stored. */
static inline void muse_passcode_init(void) {}
static inline void muse_passcode_set_supported(bool supported) { (void)supported; }
static inline bool muse_passcode_supported(void) { return false; }
static inline bool muse_passcode_on(void) { return false; }
static inline bool muse_passcode_locked(void) { return false; }
static inline bool muse_passcode_locked_at_press(void) { return false; }
static inline void muse_passcode_request_keypad(muse_pass_why_t why) { (void)why; }
static inline void muse_passcode_screen_off(bool busy) { (void)busy; }
static inline void muse_passcode_screen_on(void) {}
static inline void muse_passcode_tick(uint32_t idle_ms, bool stays_on, bool busy)
{
    (void)idle_ms;
    (void)stays_on;
    (void)busy;
}
static inline muse_pass_lock_t muse_passcode_lock_mode(void) { return MUSE_PASS_LOCK_SCREEN_OFF; }
static inline void muse_passcode_set_lock_mode(muse_pass_lock_t mode) { (void)mode; }
static inline void muse_passcode_lock_now(void) {}
static inline void muse_passcode_forget(const char *why) { (void)why; }
static inline bool muse_passcode_store_clean(void) { return true; }
static inline void muse_passcode_clear(void) {}
static inline void muse_passcode_status(char *out, size_t n)
{
    if (n) {
        out[0] = 0;
        strncat(out, "off (no lock screen on this board)", n - 1);
    }
}
#endif

#ifdef __cplusplus
}
#endif
