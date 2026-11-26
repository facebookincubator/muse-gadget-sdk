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
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Boot guard: a crash loop escapes by itself, and
 * Muse turns itself off after 2 crash resets until it's turned on again.
 *
 * - A boot is pending from its start until it's healthy (BLE up and 30 s
 *   without a reset: muse_bootguard_healthy()). A boot that begins while the
 *   last one was still pending, and not out of a power-on or a planned
 *   restart, is a failed boot.
 * - After MUSE_BOOTGUARD_FAILED_BOOTS failed boots in a row: with a USB host
 *   attached, the chip restarts into its ROM download mode (flash it over the
 *   cable as usual); otherwise SAFE mode.
 * - After MUSE_BOOTGUARD_CRASHES crash resets (panic, task or interrupt
 *   watchdog, other watchdogs) without 10 minutes of uptime between them:
 *   SAFE mode, kept across boots (NVS) until re-enabled (the BLE console's
 *   "safe.off", the USB console's ">safe=0") or a different firmware image
 *   boots.
 * - After MUSE_BOOTGUARD_LOST restarts in a row out of a running boot that
 *   stopped being reachable (muse_bootguard_tick_reachable(); each of
 *   those boots got healthy first, so the failed-boot count never sees them):
 *   SAFE mode, until a boot stays up 10 minutes (the stable time).
 * - SAFE mode: no Muse UI, audio or input; Link setup, musegadgets BLE (token
 *   proof, the Nordic UART console) and the BLE firmware update stay up. On a
 *   build without musegadgets (Wi-Fi Muse, Link only) Link stays up instead:
 *   Wi-Fi and the Noise session with device.health; on a
 *   Link-only build its own commands are off (CONFIG_MUSE_BOOTGUARD_SAFE_LINK_QUIET).
 * - The user's way into SAFE: the board's SAFE key (muse_board_t.safe_key,
 *   Link's button) pressed within MUSE_BOOTGUARD_ENTRY_WINDOW_MS of boot and
 *   held MUSE_BOOTGUARD_ENTRY_HOLD_MS (muse_bootguard_entry_watch; the key
 *   must be seen up first, so a key held or stuck through the boot never
 *   counts). A planned restart follows; the record carries the request across
 *   it, and that one boot is SAFE with the cause MUSE_BOOT_WHY_BUTTON
 *   (muse_bootguard_safe_why()). After the boot's 3 s, the key does what it
 *   always does. A post-boot hold, not one at reset: on several boards the
 *   key is a strapping pin (GPIO0 on the S3, GPIO9 on the C6), and held at
 *   reset it starts the ROM's download mode.
 * - Leaving SAFE: the BLE console's "safe.off" and the USB console's
 *   ">safe=0" (muse_bootguard_console_word: Muse on, then a planned restart
 *   MUSE_BOOTGUARD_SAFE_OFF_RESTART_MS later), a new image, or the user
 *   holding the talk / boot key MUSE_BOOTGUARD_SAFE_HOLD_MS
 *   (muse_bootguard_safe_watch: Muse on and a planned restart; no UI in SAFE).
 *   The key must be seen up first, so a key held
 *   through the boot doesn't count.
 * - Healthy: on builds with musegadgets BLE, BLE advertised or connected by
 *   then (CONFIG_MUSE_BOOTGUARD_BLE_REACHABLE, and the stay-reachable
 *   check); elsewhere 30 s of uptime alone, never a reset for being out of
 *   reach (a Wi-Fi or VM outage isn't a crash).
 *
 * The record lives in RTC memory (every reset but a power loss), checked
 * against its complement. The decision is pure (muse_bootguard_core.c) so the
 * host tests run it (tests/test_muse_bootguard.py).
 */

#define MUSE_BOOTGUARD_FAILED_BOOTS 3
#define MUSE_BOOTGUARD_CRASHES 2
#define MUSE_BOOTGUARD_LOST 3
#define MUSE_BOOTGUARD_HEALTHY_MS 30000
#define MUSE_BOOTGUARD_STABLE_MS (10 * 60 * 1000)
#define MUSE_BOOTGUARD_SAFE_HOLD_MS 5000   /* the key hold that leaves SAFE */
#define MUSE_BOOTGUARD_SAFE_OFF_RESTART_MS 500   /* safe.off / >safe=0 in SAFE: the answer, then the restart */
#define MUSE_BOOTGUARD_ENTRY_WINDOW_MS 3000   /* the SAFE key's press must begin this soon after boot */
#define MUSE_BOOTGUARD_ENTRY_HOLD_MS 10000    /* ...and be held this long: SAFE, by the user's button */

typedef enum {
    MUSE_BOOT_NORMAL = 0,
    MUSE_BOOT_SAFE,       /* recovery only: BLE, setup, update */
    MUSE_BOOT_DOWNLOAD,   /* restart into the ROM's download mode (USB host present) */
} muse_boot_mode_t;

typedef enum {
    MUSE_BOOT_WHY_NONE = 0,
    MUSE_BOOT_WHY_FAILED_BOOTS,   /* boots in a row that never got healthy */
    MUSE_BOOT_WHY_CRASHES,        /* crash resets: Muse off until re-enabled */
    MUSE_BOOT_WHY_KEPT_OFF,       /* still off from an earlier boot */
    MUSE_BOOT_WHY_LOST,           /* running boots that kept going unreachable */
    MUSE_BOOT_WHY_BUTTON,         /* the user held the SAFE key after boot (muse_bootguard_entry_watch) */
} muse_boot_why_t;

typedef struct {
    uint32_t magic;
    uint32_t pending;   /* this boot hasn't got healthy yet */
    uint32_t failed;    /* failed boots in a row */
    uint32_t crashes;   /* crash resets since 10 minutes of uptime */
    uint32_t lost;      /* restarts in a row out of a running boot that went unreachable */
    uint32_t button;    /* the user asked for SAFE (the key hold): the next boot only */
    uint32_t check;     /* ~(magic ^ pending ^ failed ^ crashes ^ lost ^ button) */
} muse_bootguard_rec_t;

#define MUSE_BOOTGUARD_MAGIC 0x4D424755u   /* the record with the button request */

typedef struct {
    int reason;            /* this boot's reset reason, muse_restart_boot_reason() (MUSE_RST_*) */
    bool intentional;      /* the last boot ended in muse_restart() */
    bool usb_host;         /* a USB host is on the chip's USB Serial/JTAG */
    bool can_download;     /* the chip can be sent to download mode from software */
    bool kept_off;         /* NVS: Muse is off from an earlier boot's crashes */
} muse_bootguard_in_t;

typedef struct {
    muse_boot_mode_t mode;
    muse_boot_why_t why;
    bool keep_off;         /* write the "off until re-enabled" flag */
} muse_bootguard_out_t;

/* The pure decision; updates the record for this boot. */
muse_bootguard_out_t muse_bootguard_decide(muse_bootguard_rec_t *r, const muse_bootguard_in_t *in);
void muse_bootguard_rec_healthy(muse_bootguard_rec_t *r);
void muse_bootguard_rec_stable(muse_bootguard_rec_t *r);
/* A running boot went unreachable and is about to restart. */
void muse_bootguard_rec_lost(muse_bootguard_rec_t *r);
/* The user's key hold asked for SAFE: the next boot is SAFE, MUSE_BOOT_WHY_BUTTON, once. */
void muse_bootguard_rec_button(muse_bootguard_rec_t *r);
bool muse_bootguard_rec_valid(const muse_bootguard_rec_t *r);
const char *muse_bootguard_why_name(muse_boot_why_t why);

/* The key hold that leaves SAFE (pure: the host tests run it). Feed it the key's
 * state and the time; true once, when the key has been held
 * MUSE_BOOTGUARD_SAFE_HOLD_MS since it went down after being seen up. */
typedef struct {
    bool seen_up;        /* the key was up once since the watch began */
    bool down;
    bool fired;          /* this hold already left SAFE */
    bool released_early; /* entry: a press begun in the entry window let go before the SAFE hold (setup mode) */
    uint32_t down_ms;
} muse_bootguard_hold_t;
bool muse_bootguard_hold_step(muse_bootguard_hold_t *h, bool down, uint32_t now_ms);
/* The key hold into SAFE (pure): feed it the key's state and the time since boot; true once, when a press that
 * began after the key was seen up and within MUSE_BOOTGUARD_ENTRY_WINDOW_MS of boot has been held
 * MUSE_BOOTGUARD_ENTRY_HOLD_MS. A press begun later never counts, nor one released early; such a press, released
 * before the SAFE hold, sets released_early: setup mode for a board whose only key is its talk key
 * (muse_bootguard_setup_press). */
bool muse_bootguard_entry_step(muse_bootguard_hold_t *h, bool down, uint32_t since_boot_ms);
/* This normal boot's SAFE key press began in the entry window and was let go before the SAFE hold: the user held
 * the key at power-on and released it early, setup mode (protocols/README.md, The press). Never in SAFE. */
bool muse_bootguard_setup_press(void);
/* In a SAFE boot the guard entered by itself, the key does both (pure): a press begun within the entry window
 * is the way into a button SAFE (10 s, MUSE_BOOTGUARD_KEY_ENTER) and never leaves SAFE; a press begun later is
 * the 5 s way out (MUSE_BOOTGUARD_KEY_LEAVE). */
typedef struct {
    muse_bootguard_hold_t entry, leave;
    bool window_press;   /* the press now down began in the entry window */
    bool was_down;
} muse_bootguard_keys_t;
enum { MUSE_BOOTGUARD_KEY_NONE = 0, MUSE_BOOTGUARD_KEY_ENTER, MUSE_BOOTGUARD_KEY_LEAVE };
int muse_bootguard_keys_step(muse_bootguard_keys_t *k, bool down, uint32_t since_boot_ms);

/* The consoles' SAFE words (USB ">safe=0" and "safe.off", "guard?"): true if
 * line is one; out gets the answer for "@..." ("" for guard?, which logs).
 * In SAFE, safe=0 / safe.off restart normally MUSE_BOOTGUARD_SAFE_OFF_RESTART_MS
 * after answering. */
bool muse_bootguard_console_word(const char *line, char *out, size_t n);
/* The same for a console whose words may be unproven (trusted false: muse_bootguard_reenable_from). */
bool muse_bootguard_console_word_from(const char *line, char *out, size_t n, bool trusted);

/* ---- on the device ---------------------------------------------------------- */

/* First thing in app_main, before any app code: decides, and for
 * MUSE_BOOT_DOWNLOAD restarts into the ROM's download mode (never returns).
 * Returns NORMAL or SAFE. Without CONFIG_MUSE_BOOTGUARD: always NORMAL. */
muse_boot_mode_t muse_bootguard_begin(void);
/* This boot's mode, after muse_bootguard_begin(). */
muse_boot_mode_t muse_bootguard_mode(void);
/* Why this boot is SAFE (MUSE_BOOT_WHY_NONE in a normal boot): MUSE_BOOT_WHY_BUTTON when the user's key hold
 * entered it, else the guard's own cause. */
muse_boot_why_t muse_bootguard_safe_why(void);
/* Starts the timers: healthy after MUSE_BOOTGUARD_HEALTHY_MS (call once the
 * recovery tier, BLE, is up), crash count cleared after the stable time. */
void muse_bootguard_arm(void);
/* What "reachable" means for this boot: at the healthy
 * time the boot counts healthy only if it says true; else it's a failed boot
 * (an unmarked restart in a normal boot, so the guard escalates to SAFE or
 * download mode; logged only in SAFE mode, the floor). NULL: no check. */
void muse_bootguard_set_reachable(bool (*reachable)(void));
/* Whether the gadget is reachable right now, for muse_bootguard_tick_reachable(). */
void muse_bootguard_set_still_reachable(bool (*reachable_now)(void));
/* This boot reached healthy (or the guard is off). */
bool muse_bootguard_is_healthy(void);
/* Called every second while running ("stay reachable"): once the boot
 * is healthy, 30 s in a row with the reachable check false counts as a failed
 * boot (the reachable-now check: muse_bootguard_set_still_reachable), as at the healthy time (an unmarked restart in a normal boot; logged
 * in SAFE mode). Without CONFIG_MUSE_BOOTGUARD: nothing. */
void muse_bootguard_tick_reachable(void);
/* Logs this boot's mode and the record's counts (the BLE console's "guard?"). */
void muse_bootguard_log_state(void);
/* Muse on again after crashes kept it off: clears the NVS flag; the next
 * boot is normal. True if it was off. */
bool muse_bootguard_reenable(void);
/* The same from a word that may be unproven: trusted false (the Nordic UART in SAFE, which runs no token proof)
 * erases the keep-off record but leaves the running-loss count. */
bool muse_bootguard_reenable_from(bool trusted);
/* In SAFE mode: a low-priority task polls key_down every 50 ms; held
 * MUSE_BOOTGUARD_SAFE_HOLD_MS (muse_bootguard_hold_step), Muse is re-enabled
 * and the gadget restarts normally (MUSE_RESTART_SAFE_OFF), cue first if given
 * (a light or motor where the board has one; NULL: none). Nothing outside
 * SAFE or without the guard. */
void muse_bootguard_safe_watch(bool (*key_down)(void), void (*cue)(void));
/* In a normal boot: a low-priority task polls key_down every 50 ms for the first
 * MUSE_BOOTGUARD_ENTRY_WINDOW_MS + MUSE_BOOTGUARD_ENTRY_HOLD_MS (muse_bootguard_entry_step); a hold records
 * the request and restarts (MUSE_RESTART_SAFE_ON) into SAFE. Nothing in SAFE or without the guard. */
void muse_bootguard_entry_watch(bool (*key_down)(void));
/* The same hold's action for builds that read the key themselves (Link's button): true if it was taken (the
 * gadget restarts into SAFE; never returns), false outside the window or in SAFE. */
bool muse_bootguard_enter_safe(const char *why);
/* Whether a press beginning now is still in the SAFE entry window (Link's button: such a press doesn't run the
 * key's usual long press). */
bool muse_bootguard_entry_open(void);
/* The key hold's action, for builds that read the key themselves (Link's
 * button): re-enable and restart normally. Never returns in SAFE mode; false
 * (nothing done) outside it. */
bool muse_bootguard_leave_safe(const char *why);

#ifdef __cplusplus
}
#endif
