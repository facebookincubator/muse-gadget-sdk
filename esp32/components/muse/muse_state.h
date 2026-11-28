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

/*
 * Shared state between the voice pipeline (writer) and the UI (reader).
 * Scalars are plain word-sized stores; the caption is guarded by a spinlock.
 */

typedef enum {
    MUSE_MODE_BOOT = 0,
    MUSE_MODE_IDLE,
    MUSE_MODE_LISTENING,
    MUSE_MODE_THINKING,
    MUSE_MODE_SPEAKING,
    MUSE_MODE_ERROR,
    MUSE_MODE_OFF,       /* powering down */
    MUSE_MODE_COUNT,
} muse_mode_t;

typedef struct {
    int battery_pct;   /* -1 when no battery is connected */
    int battery_mv;    /* 0 when the board can't measure it */
    bool charging;
    bool usb;
    /* The charger reports charge done (its done or termination status), on
     * boards whose muse_board_t.charged is MUSE_CHARGED_DONE
     * (muse_charged.h). Never derived from the percentage. */
    bool charge_done;
    /* What a fuel gauge adds (the ESP-Mosaico's BQ27220); 0 / false where the
     * board can't measure it. For report_battery_status. */
    bool has_current;
    int current_100ua;    /* negative discharging */
    bool has_temp;
    int temp_c;
    int remaining_mah, full_mah;
} muse_power_t;

void muse_state_init(void);

void muse_state_set_mode(muse_mode_t mode);
muse_mode_t muse_state_mode(float *secs_in_mode);

/* The turn on now is the gadget's own: its talk press started it
 * (the panel stays lit through its thinking and responding until it's done;
 * a phone's turn alone never wakes or holds the panel). The voice task sets
 * it as a press's turn starts and clears it for a client's start_mic; a
 * mode back to idle, boot or off clears it. */
void muse_state_set_own_turn(bool own);
bool muse_state_own_turn(void);

/* Live audio level (mic while listening, playback while speaking), 0..1. */
void muse_state_set_level(float level);
float muse_state_level(void);

/* Progress of the current phase (recording length or playback), 0..1. */
void muse_state_set_progress(float progress);
float muse_state_progress(void);

/* Room for a page of reply text; longer captions are cut short. */
#define MUSE_CAPTION_MAX 400

void muse_state_set_caption(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Copies the caption if it changed since *version; returns true on change. */
bool muse_state_caption(char *out, size_t out_len, uint32_t *version);

/* A transient hint caption: cleared once MUSE_HINT_MS passes. Plain
 * captions (replies, status) never expire; a hint set replaces any
 * caption, and any later plain caption cancels the pending expiry. */
#define MUSE_HINT_MS 5000
void muse_state_set_hint(const char *text);
/* Whether the caption is a hint that hasn't expired yet. */
bool muse_state_hint_pending(void);

/* How much reply text the screen shows at once, set by the UI: lines of up to
 * `cols` characters. The replies are wrapped and paged to fit. A screen that
 * draws CJK larger than its other caption text sets a page for replies with CJK
 * in them too, and one with its accented letters' font (muse_text_has_latin)
 * for replies with those; without one they get the usual page. */
void muse_state_set_page(int cols, int lines);
void muse_state_set_cjk_page(int cols, int lines);
void muse_state_set_latin_page(int cols, int lines);
/* The page for `text`. */
void muse_state_page(const char *text, int *cols, int *lines);

void muse_state_set_power(const muse_power_t *power);
muse_power_t muse_state_power(void);
/* A battery and no USB power (charger or computer), or a bench nap's as-if
 * battery (below). Power saving asleep (the voice task resting, the display
 * paused, the Wi-Fi nap) is only for then; a change nudges
 * (muse_state_wait_awake). Never for cutting power: muse_state_on_battery_real. */
bool muse_state_on_battery(void);
/* Really on battery: a battery and no USB power, whatever a bench nap says.
 * Everything that cuts power (a power-off a board only allows on battery, the
 * low-battery shutdown) goes by this, so a nap on USB makes the gadget
 * behave as on battery but never power itself off. */
bool muse_state_on_battery_real(void);
/* Bench tests over USB (">nap"): count as on battery until turned off, or
 * for MUSE_AS_IF_BATTERY_US at most (logged), so a bench that lost the
 * device gets it back. */
void muse_state_set_as_if_battery(bool on);

/* ---- the pieces above as plain C (host tests) ---- */

#define MUSE_AS_IF_BATTERY_US (10LL * 60 * 1000000)   /* a bench nap's as-if battery ends by itself */

typedef struct {
    bool on;
    int64_t until_us;
} muse_as_if_t;

static inline void muse_as_if_set(muse_as_if_t *a, bool on, int64_t now_us)
{
    a->on = on;
    if (on) {
        a->until_us = now_us + MUSE_AS_IF_BATTERY_US;
    }
}

/* Still on at now_us. Its time run out, it goes off; *expired says it did just now. */
static inline bool muse_as_if_on(muse_as_if_t *a, int64_t now_us, bool *expired)
{
    *expired = a->on && now_us >= a->until_us;
    if (*expired) {
        a->on = false;
    }
    return a->on;
}

static inline bool muse_power_real_battery(const muse_power_t *p)
{
    return !p->usb && p->battery_pct >= 0;
}

/* A software power-off may go ahead: not on a board whose latch USB holds on
 * (power_off_battery_only, the ESP-Mosaico) unless it's really on battery. */
static inline bool muse_power_off_ok(bool battery_only, const muse_power_t *p)
{
    return !battery_only || muse_power_real_battery(p);
}

/* Any user interaction: keeps Muse awake. */
void muse_state_poke(void);
/* Called on every poke (a touch, a key, a wake, a call): the BLE link
 * policy's actions. Any task; keep it cheap. */
void muse_state_set_poke_listener(void (*fn)(void));
float muse_state_idle_secs(void);

/* Screen sleep (display dark, rendering paused). Waking also pokes. */
void muse_state_set_asleep(bool asleep);
bool muse_state_asleep(void);
/* For a task with nothing to do asleep: returns once awake, nudged
 * (muse_state_nudge, which doesn't wake the screen) or timeout_ms passes. */
void muse_state_wait_awake(uint32_t timeout_ms);
void muse_state_nudge(void);

/* A reply from Muse arrived: wakes the screen and flashes it twice
 * (muse_state_flash_level, read by the UI each frame). */
void muse_state_flash(void);
float muse_state_flash_level(void);   /* 0..1, 0 when no flash is running */

/* Touch/pet reaction. */
void muse_state_make_happy(void);
float muse_state_happiness(void);
/* Counts muse_state_make_happy calls: each one restarts the avatar's bounce,
 * whichever control took the tap. */
uint32_t muse_state_happy_seq(void);
