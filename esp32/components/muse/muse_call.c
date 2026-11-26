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
#include "muse_call.h"
#include "muse_call_at.h"
#include "muse_subs.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "muse_touch.h"
#include "muse_touch_log.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#ifdef CONFIG_MUSE_CALL_AT_BOOT   /* (Kconfig always defines it, "" when off; the simulator doesn't) */
#include "esp_app_desc.h"
#include "nvs.h"
#endif

#include "mg_ble.h"
#include "mg_lct.h"
#include "mgcommands.h"
#include "muse_board.h"
#include "muse_call_backstop.h"
#include "muse_call_lock.h"
#include "muse_call_controls.h"
#include "muse_call_stats.h"
#include "muse_home.h"
#include "muse_swipe.h"
#include "muse_icons.h"
#include "muse_menu.h"
#include "muse_passcode.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"
#if CONFIG_MUSE_VIDEO
#include "muse_mic_ring.h"
#include "muse_video.h"
#endif

static const char *TAG = "mg.call";   /* "mg": on the Nordic UART log mirror */
/* That mirror is readable by any central with no pairing, on release gadgets
 * too: never log touch coordinates on TAG. They go to XY_TAG, which only the
 * cable console shows. */
static const char *XY_TAG = "muse_touch";

#define CONTROLS_SHOW_US MUSE_CALL_CONTROLS_SHOW_US   /* the controls hide this long after the last touch */
#define HINT_SHOW_US (5 * 1000000LL)           /* the hint stays this long once the call is live */
/* A live call whose phone never sends a picture: the calling screen gives way
 * to the face after this long, so "Calling…" doesn't stand over a call that's
 * up (video builds only; without video the face is the call's picture). */
#define COVER_LIVE_MAX_US (15 * 1000000LL)
/* Live this long with no first picture from the phone (nothing failed
 * here): the calling screen says so rather than "Calling…". */
#define NO_PICTURE_US (5 * 1000000LL)

#define COLOR_LIVE_BG 0x1d7a45
#define COLOR_LIVE_TEXT 0xffffff

/* The call's phase and whether it came from a central without the connection's token proof (shown only while
 * unlocked and awake, never waking the screen or holding off the lock: muse_call_unproven), as one word
 * (muse_call_lock.h, muse_call_word_*). Written only by compare-and-swap and read in one load, from any task: the BLE
 * worker's client_state and the input and LVGL tasks' starts and ends. */
static atomic_uint s_call;
static volatile int64_t s_ended_us;   /* when the last call ended (muse_call_ended_within) */
static volatile int64_t s_live_us;
/* No answer (30 s) and the connecting backstop (60 s): muse_call_backstop.h. */
static muse_call_timers_t s_timers;
/* The call ended: muse_call_poll closes its video, the stream that was on then (muse_video_gen() + 1; 0: none).
 * A newer call or stream makes it void: a late close never ends what came after. */
static volatile uint32_t s_close_gen;
static volatile bool s_check_volume;  /* the call started: muse_call_poll raises a silent speaker */
static volatile int64_t s_controls_until_us;   /* the controls show until then */
static volatile int64_t s_hint_until_us;       /* ...and the hint line in them */
static bool s_hinted;                          /* this call's first tap has shown the hint */
/* The hint line's text: NULL for how to use the controls, else who changed
 * the volume from outside ("Set by phone", muse_call_volume_by). */
static const char *volatile s_hint_text;

static lv_obj_t *s_pill, *s_pill_lbl;
/* The calling screen: black, "Calling…" centered and nothing else, from the
 * moment a call is asked for until its first picture is drawn. With the avatar
 * face picked and stored, its plain picture full-panel at 40 % behind it (no
 * clock, page dots or scrim); the pixel face or no picture stays
 * black. Bench/debug images (CONFIG_MUSE_BENCH_IMAGE), or any image while
 * the console word call.steps=1 is set (RAM only: off at every boot), add one
 * bottom line: the call start's step and the seconds since the tap. */
static lv_obj_t *s_calling, *s_calling_lbl, *s_calling_face, *s_calling_step;
#define CALLING_FACE_OPA LV_OPA_40
static volatile int64_t s_tap_us;   /* the tap that asked for the call (start_call) */
static volatile bool s_steps_word;   /* call.steps=1: the step line on a shipping image (RAM: off at boot) */
static volatile bool s_calling_up;   /* it's on the screen (the LVGL task sets it) */
static lv_obj_t *s_banner_box, *s_banner_lbl;   /* the end banners, over the home */

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static muse_call_phase_t call_phase(void)
{
    return (muse_call_phase_t)muse_call_word_phase(atomic_load(&s_call));
}

/* The phase, in one store with the unproven mark (none clears it). */
static void call_store_phase(muse_call_phase_t phase)
{
    unsigned w = atomic_load(&s_call);
    while (!atomic_compare_exchange_weak(&s_call, &w, muse_call_word_with_phase(w, phase))) {
    }
}

static void call_store_unproven(bool unproven)
{
    unsigned w = atomic_load(&s_call);
    while (!atomic_compare_exchange_weak(&s_call, &w, muse_call_word_with_unproven(w, unproven))) {
    }
}

const char *muse_call_phase_name(muse_call_phase_t phase)
{
    static const char *const NAMES[] = { "none", "asked", "connecting", "live" };
    return phase <= MUSE_CALL_LIVE ? NAMES[phase] : "?";
}

static void vstats_edge(void);

static void set_phase(muse_call_phase_t phase)
{
    muse_call_phase_t was = call_phase();
    if (phase != was) {
        ESP_LOGI(TAG, "%s -> %s", muse_call_phase_name(was), muse_call_phase_name(phase));
        bool starts = (phase == MUSE_CALL_CONNECTING || phase == MUSE_CALL_LIVE) &&
                      (was == MUSE_CALL_NONE || was == MUSE_CALL_ASKED);
        if (starts) {
            s_close_gen = 0;         /* a new call: an older call's pending close is void */
            s_check_volume = true;   /* NVS writes: not here on the BLE worker */
            s_hinted = false;
        }
        if (starts || phase == MUSE_CALL_NONE || was == MUSE_CALL_NONE) {
            vstats_edge();   /* the stats overlay starts hidden in every call */
        }
#if CONFIG_MUSE_VIDEO
        /* The video view keeps out of the way until the call's first
         * picture, which replaces the calling screen (muse_video_call). */
        muse_video_call(phase == MUSE_CALL_NONE ? MUSE_VIDEO_CALL_NONE
                        : phase == MUSE_CALL_LIVE ? MUSE_VIDEO_CALL_LIVE
                                                  : MUSE_VIDEO_CALL_CALLING);
#endif
        if (phase == MUSE_CALL_NONE) {
            muse_subs_call_end();   /* the subtitles go with the call */
            s_ended_us = now_us();   /* muse_call_ended_within() */
        }
        if (phase != MUSE_CALL_NONE && muse_state_asleep()) {
            /* The call's video is on its way: a sleeping screen refuses it
             * (busy) until it's awake, which cost the first call after a
             * quiet spell about a second of the phone's retries. Wake it
             * now, while the phone gets its stream ready. */
            ESP_LOGI(TAG, "waking the screen for the call");
            muse_state_set_asleep(false);
        }
        call_store_phase(phase);   /* the phase and the unproven mark together: none clears the mark in this store */
    }
}

/* The speaker level as the phone sees it (speaker_volume): 0 while it's off. */
static int volume_now(void)
{
    return muse_settings_speaker_on() ? muse_settings_volume() : 0;
}

/* Sets and keeps it (0 turns the speaker off, keeping the level), and shows it. */
static void volume_set_quiet(int pct, const char *why, bool show);

static void volume_set(int pct, const char *why)
{
    volume_set_quiet(pct, why, true);
}

/* show: the controls come up with the new level (a key, a swipe); not for
 * the call-start raise, which happens with no one touching anything. */
static void volume_set_quiet(int pct, const char *why, bool show)
{
    ESP_LOGI(TAG, "volume %d%% -> %d%% (%s)", volume_now(), pct, why);
    if (pct <= 0) {
        if (muse_settings_speaker_on()) {
            muse_settings_set_speaker_on(false);
        }
    } else {
        muse_settings_set_volume(pct);
        if (!muse_settings_speaker_on()) {
            muse_settings_set_speaker_on(true);
        }
    }
    if (show) {
        s_controls_until_us = now_us() + CONTROLS_SHOW_US;
    }
}

int muse_call_volume(void)
{
    return volume_now();
}

/* The end banners, then the home. */
#define BANNER_ENDED_MS 1500
#define BANNER_PROBLEM_MS 3000
/* The phone needs the user before calls can start (the
 * Muse app's voice intro, the microphone permission): it says so for 5 s. */
#define BANNER_SETUP_MS 5000
#define BANNER_SETUP "Open the Muse app to finish setting up calls"
static const char *volatile s_banner;
static volatile int64_t s_banner_until_us;

/* Over, from either side: back to the face, with its banner. why: what ended
 * it, for the call timeline's summary (mg.lct end=). */
static void finish(const char *banner, int ms, const char *why)
{
    set_phase(MUSE_CALL_NONE);
    mg_lct_end_why(why);
#if CONFIG_MUSE_VIDEO
    s_close_gen = muse_video_gen() + 1;
#endif
    if (banner) {
        s_banner = banner;
        s_banner_until_us = now_us() + ms * 1000LL;
    }
}

/* A live call and a UI stream don't run together yet (the S3's internal RAM
 * for both is unmeasured). */
static bool ui_stream_on(void)
{
#if CONFIG_MUSE_VIDEO
    return muse_video_ui_active();
#else
    return false;
#endif
}

/* The touch that started or ended the call (muse_touch_log.h): the call
 * timeline's by= / end= text, kept until the next call. */
static char s_by_tag[40], s_end_tag[72];

static void start_call(const char *why, const char *by, const char *detail);

void muse_call_start_from(const char *why)
{
    start_call(why, !strncmp(why, "touch", 5) ? "touch" : why, NULL);
}

void muse_call_start_touch(const char *why, int x, int y, uint32_t hold_ms)
{
    char detail[96];
    muse_touch_detail(detail, sizeof(detail), x, y, hold_ms, muse_touch_downs_within(MUSE_TOUCH_DOWNS_WINDOW_MS));
    muse_touch_tag(s_by_tag, sizeof(s_by_tag), x, y, hold_ms);
    start_call(why, s_by_tag, detail);
}

static void start_call(const char *why, const char *by, const char *detail)
{
    if (!muse_call_supported()) {
        ESP_LOGW(TAG, "start (%s): no call on this board", why);   /* any entry point: never ASKED for nothing */
        return;
    }
    muse_call_start_t may = muse_call_may_start(call_phase() == MUSE_CALL_NONE, muse_passcode_locked(), ui_stream_on());
    if (may == MUSE_CALL_START_IN_CALL) {
        return;
    }
    ESP_LOGI(TAG, "start (%s)%s%s", why, detail ? " " : "", detail ? detail : "");
    if (may == MUSE_CALL_START_LOCKED) {
        ESP_LOGW(TAG, "live call refused: locked");
        muse_passcode_request_keypad(MUSE_PASS_WHY_CALL);
        muse_state_poke();
        return;
    }
    if (may == MUSE_CALL_START_UI) {
        ESP_LOGW(TAG, "live call refused: a UI stream is on");
        muse_state_set_caption("Stop the screen stream first");
        return;
    }
    muse_call_timers_asked(&s_timers, now_us());
    s_tap_us = now_us();
    /* Who started it, for the call timeline's summary (mg.lct by=): menu, console, key, or the touch
     * that did it, touch(x,y,hold). */
    mg_lct_begin_by(by);
    call_store_unproven(false);   /* the gadget's own call is the owner's: no mark left from a refused unproven one */
    set_phase(MUSE_CALL_ASKED);
    muse_state_poke();
    mg_ble_live_call(true);   /* pending until a client answers */
}

void muse_call_start(void)
{
    muse_call_start_from("unnamed");
}

void muse_call_end_from(const char *why)
{
    if (call_phase() == MUSE_CALL_NONE) {
        return;
    }
    ESP_LOGI(TAG, "end (%s)", why);
    mg_ble_live_call(false);
    finish("Call ended", BANNER_ENDED_MS, why);   /* closes here even if the phone never answers */
}

void muse_call_end(void)
{
    muse_call_end_from("unnamed");
}

void muse_call_end_touch(const char *why, int x, int y, uint32_t hold_ms)
{
    if (call_phase() == MUSE_CALL_NONE) {
        return;
    }
    char detail[96], tag[40];
    muse_touch_detail(detail, sizeof(detail), x, y, hold_ms, muse_touch_downs_within(MUSE_TOUCH_DOWNS_WINDOW_MS));
    muse_touch_tag(tag, sizeof(tag), x, y, hold_ms);
    snprintf(s_end_tag, sizeof(s_end_tag), "%s %s", why, tag);
    ESP_LOGI(TAG, "end (%s) %s", why, detail);
    mg_ble_live_call(false);
    finish("Call ended", BANNER_ENDED_MS, s_end_tag);
}

bool muse_call_active(void)
{
    return call_phase() != MUSE_CALL_NONE;
}

bool muse_call_ended_within(int ms)
{
    int64_t t = s_ended_us;
    return call_phase() == MUSE_CALL_NONE && t && now_us() - t < (int64_t)ms * 1000;
}

muse_call_phase_t muse_call_phase(int *live_s)
{
    muse_call_phase_t phase = call_phase();
    if (live_s) {
        *live_s = phase == MUSE_CALL_LIVE ? (int)((now_us() - s_live_us) / 1000000) : 0;
    }
    return phase;
}

bool muse_call_supported(void)
{
    /* The call's picture, its controls and its subtitles live in PSRAM: a
     * board without it can't show a call, so it doesn't offer one (a feature
     * shows only where it works). */
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

bool muse_call_available(void)
{
    return mg_ble_live_call_client();
}

/*
 * A call the phone starts while the
 * gadget is locked waits behind the keypad ("Call from phone"): nothing from
 * it reaches the gadget (no phase, so no picture, subtitles, speaker audio
 * or mic) until it's unlocked, then the call view opens with the phone's
 * latest state; not unlocked within the call-setup wait (60 s), the
 * request ends as declined ([live_call, end]). The state the phone last
 * reported, 0 while none waits.
 */
#define HELD_DECLINE_US (60 * 1000000LL)
static volatile uint8_t s_held_state;
static int64_t s_held_us;

static void client_state(uint8_t state, bool unproven);

/* A call that waited behind the keypad and never opened (ended, failed, the link gone, declined): what it may have
 * left goes with it, as finish() does for a call that opened: its subtitles (kept, hidden by the lock) and a video
 * stream it started meanwhile. Its phase never left NONE, so set_phase can't do it. */
static void held_teardown(void)
{
    s_held_state = 0;
    mg_ble_live_call_held(false);
    muse_subs_call_end();
#if CONFIG_MUSE_VIDEO
    s_close_gen = muse_video_gen() + 1;
#endif
}

static void held_poll(void)
{
    uint8_t st = s_held_state;
    if (!st) {
        return;
    }
    if (!muse_passcode_locked()) {
        s_held_state = 0;
        mg_ble_live_call_held(false);
        ESP_LOGI(TAG, "unlocked: the phone's call opens");
        client_state(st, false);   /* (only a proven call waits behind the keypad) */
    } else if (now_us() - s_held_us >= HELD_DECLINE_US) {
        held_teardown();
        ESP_LOGW(TAG, "a call from the phone, still locked after 60 s: declined");
        mg_ble_live_call(false);
    }
}

bool muse_call_unproven(void)
{
    return muse_call_word_unproven(atomic_load(&s_call));
}

bool muse_call_owner_active(void)
{
    return muse_call_word_owner(atomic_load(&s_call));
}

void muse_call_client_state(uint8_t state)
{
    client_state(state, mg_ble_live_call_unproven());
}

static void client_state(uint8_t state, bool unproven)
{
    bool starts = state == mg_live_call_state_connecting || state == mg_live_call_state_live;
    if (unproven) {
        bool owner_call = muse_call_owner_active() || s_held_state;
        if (!muse_call_unproven_shows(owner_call, muse_passcode_locked(), muse_state_asleep())) {
            ESP_LOGI(TAG, "call state %u from an unproven central: not shown", state);
            return;
        }
        if (starts && call_phase() == MUSE_CALL_NONE) {
            call_store_unproven(true);
        }
    } else if (starts) {
        call_store_unproven(false);   /* proven now (or the answer to the gadget's own start): the owner's call */
    }
    muse_call_phase_t phase = call_phase();
    if (muse_call_keypad_holds(phase == MUSE_CALL_NONE || phase == MUSE_CALL_ASKED, starts, muse_passcode_locked())) {
        if (!s_held_state) {
            s_held_us = now_us();
            ESP_LOGI(TAG, "a call from the phone while locked: waiting behind the keypad");
            mg_ble_live_call_held(true);   /* its reply audio waits too */
            muse_passcode_request_keypad(MUSE_PASS_WHY_PHONE_CALL);
            if (muse_state_asleep()) {
                muse_state_set_asleep(false);   /* the screen wakes to the keypad (pokes too) */
            } else {
                muse_state_poke();
            }
        }
        s_held_state = state;
        return;
    }
    if (s_held_state && !starts) {
        held_teardown();   /* the phone gave up first: nothing to open, nothing of it kept */
        ESP_LOGI(TAG, "the phone's call ended while it waited behind the keypad");
        return;
    }
    switch (state) {
    case mg_live_call_state_connecting:
        if (ui_stream_on()) {
            /* The phone started one anyway: ask it to end it (busy). */
            ESP_LOGW(TAG, "live call refused: a UI stream is on");
            mg_ble_live_call(false);
            break;
        }
        /* Every connecting, a re-sent one too, restarts the 60 s backstop. */
        muse_call_timers_connecting(&s_timers, now_us());
        set_phase(MUSE_CALL_CONNECTING);
        break;
    case mg_live_call_state_live:
        if (call_phase() == MUSE_CALL_NONE && ui_stream_on()) {
            ESP_LOGW(TAG, "live call refused: a UI stream is on");
            mg_ble_live_call(false);
            break;
        }
        if (call_phase() != MUSE_CALL_LIVE) {
            s_live_us = now_us();
            set_phase(MUSE_CALL_LIVE);
            if (!muse_call_unproven()) {
                muse_state_poke();   /* (an unproven call never keeps the screen up) */
            }
            /* No controls until a tap (nothing over the face as the
             * call starts); the first tap's carry the hint (show_controls). */
        }
        break;
    case mg_live_call_state_ended:
        if (call_phase() != MUSE_CALL_NONE) {
            finish("Call ended", BANNER_ENDED_MS, "phone");
        }
        break;
    case mg_live_call_state_failed:
        if (call_phase() != MUSE_CALL_NONE) {
            if (mg_ble_live_call_failure_reason() == mg_live_call_failure_setup_required) {
                /* Not a failure the user can retry here: the phone needs them first. */
                finish(BANNER_SETUP, BANNER_SETUP_MS, "phone failed: setup required");
                break;
            }
            /* Live and then lost, or never started. */
            finish(call_phase() == MUSE_CALL_LIVE ? "Call dropped" : "Couldn't start the call", BANNER_PROBLEM_MS,
                   "phone failed");
        }
        break;
    default:
        /* Idle: no call. A start the client hasn't answered yet stays (mg_proto
         * keeps it pending); a call it had is over (it disconnected, say). */
        if (call_phase() == MUSE_CALL_CONNECTING || call_phase() == MUSE_CALL_LIVE) {
            finish(call_phase() == MUSE_CALL_LIVE ? "Call dropped" : "Couldn't start the call", BANNER_PROBLEM_MS,
                   "disconnect");
        }
        break;
    }
}

static void poll_video_touch(void);

static void vstats_poll(void);

/* The bench's timed call (console call.at=, muse_call_at.h). */
static muse_call_at_t s_at;

void muse_call_at_set(unsigned delay_s, unsigned len_s)
{
    muse_call_at_arm(&s_at, delay_s, len_s, muse_call_available());
}

void muse_call_at_describe(char *out, size_t cap)
{
    if (!s_at.armed) {
        snprintf(out, cap, "off");
    } else if (s_at.end_us) {
        snprintf(out, cap, "in a call, ends in %d s", (int)((s_at.end_us - now_us()) / 1000000));
    } else if (s_at.fire_us) {
        snprintf(out, cap, "starts in %d s, %u s long", (int)((s_at.fire_us - now_us()) / 1000000),
                 (unsigned)s_at.len_s);
    } else {
        snprintf(out, cap, "armed: %u s after the next client is ready, %u s long", (unsigned)s_at.delay_s,
                 (unsigned)s_at.len_s);
    }
}

/* CONFIG_MUSE_CALL_AT_BOOT: call.at armed at the first poll, once per image
 * (its ELF sha256's first 8 bytes in NVS once the call started). */
#define CALL_AT_NVS_NS "muse_bench"
#define CALL_AT_NVS_KEY "call_at_img"
#define CALL_AT_BOOT_MAX_VOLUME 30   /* a call the gadget starts by itself never plays louder (bench rule) */
static bool s_at_boot;   /* armed at boot, not yet started or given up */

#ifdef CONFIG_MUSE_CALL_AT_BOOT
static void call_at_boot(void)
{
    static bool done;
    if (done) return;
    done = true;
    unsigned d = 0, l = 0;
    if (!CONFIG_MUSE_CALL_AT_BOOT[0] || sscanf(CONFIG_MUSE_CALL_AT_BOOT, "%u,%u", &d, &l) != 2 || !l) return;
    uint8_t had[8] = { 0 };
    size_t n = sizeof(had);
    nvs_handle_t h;
    if (nvs_open(CALL_AT_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, CALL_AT_NVS_KEY, had, &n) != ESP_OK) memset(had, 0, sizeof(had));
        nvs_close(h);
    }
    if (!memcmp(had, esp_app_get_description()->app_elf_sha256, sizeof(had))) {
        ESP_LOGI(TAG, "call.at at boot: this image's timed call already ran");
        return;
    }
    muse_call_at_arm(&s_at, d, l, false);   /* a client already connected at boot counts */
    s_at.strict = true;
    s_at_boot = true;
    ESP_LOGW(TAG, "call.at at boot (bench image): a %u s call %u s after the next client is ready", l, d);
}

static void call_at_boot_ran(void)
{
    if (!s_at_boot) return;
    s_at_boot = false;
    nvs_handle_t h;
    if (nvs_open(CALL_AT_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, CALL_AT_NVS_KEY, esp_app_get_description()->app_elf_sha256, 8);
        nvs_commit(h);
        nvs_close(h);
    }
}
#else
static void call_at_boot(void) {}
static void call_at_boot_ran(void) {}
#endif

static void shown_down_flush(void);

void muse_call_poll(void)
{
    shown_down_flush();   /* (noted by a draw: logged here, on the UI task) */
    held_poll();
    call_at_boot();
    bool boot = s_at_boot;
    switch (muse_call_at_tick(&s_at, muse_call_available(), now_us())) {
    case MUSE_CALL_AT_START:
        call_at_boot_ran();
        if (boot && call_phase() != MUSE_CALL_NONE) {
            /* A gadget starting a call by itself: never over one that's up (it gives up, never later). */
            s_at.armed = false;
            ESP_LOGW(TAG, "call.at: boot-armed call given up: a call is already up");
        } else if (boot && volume_now() > CALL_AT_BOOT_MAX_VOLUME) {
            s_at.armed = false;
            ESP_LOGW(TAG, "call.at: boot-armed call given up: the speaker is at %d (over %d)", volume_now(),
                     CALL_AT_BOOT_MAX_VOLUME);
        } else {
            ESP_LOGI(TAG, "call.at: %s call starting (%u s)", boot ? "boot-armed" : "timed", (unsigned)s_at.len_s);
            muse_call_start_from("call.at");
        }
        break;
    case MUSE_CALL_AT_END:
        ESP_LOGI(TAG, "call.at: %s call ended", s_at.strict ? "boot-armed" : "timed");
        muse_call_end_from("call.at");
        break;
    case MUSE_CALL_AT_GIVE_UP:
        call_at_boot_ran();   /* once per image, even given up */
        ESP_LOGW(TAG, "call.at: boot-armed call given up: the phone went before its start");
        break;
    default:
        break;
    }
    if (muse_call_unproven() && muse_call_unproven_closes(muse_passcode_locked(), muse_state_asleep())) {
        ESP_LOGI(TAG, "the unproven central's call view goes: the gadget %s",
                 muse_passcode_locked() ? "locked" : "slept");
        finish(NULL, 0, "unproven: locked or asleep");
    }
    poll_video_touch();
    vstats_poll();
    muse_call_phase_t phase = call_phase();
    switch (muse_call_timers_due(&s_timers, phase == MUSE_CALL_ASKED, phase == MUSE_CALL_CONNECTING, now_us())) {
    case MUSE_CALL_DUE_NO_ANSWER:
        ESP_LOGW(TAG, "no answer from the phone");
        mg_ble_live_call(false);
        finish("No answer", BANNER_PROBLEM_MS, "timeout");
        break;
    case MUSE_CALL_DUE_BACKSTOP:
        /* The connecting backstop (mgcommands.h): 60 s of
         * connecting with nothing newer from the phone. Ended as the user's
         * end is ([live_call, end]: the phone cancels its connect), with the
         * failure's banner for 3 s, then home. */
        ESP_LOGW(TAG, "connecting for 60 s with nothing newer from the phone: ending the call");
        mg_ble_live_call(false);
        finish("Couldn't start the call", BANNER_PROBLEM_MS, "connecting backstop");
        break;
    default:
        break;
    }
    if (s_check_volume) {
        s_check_volume = false;
        if (volume_now() == 0) {
            /* A call never raises a muted device: muted
             * is the user's choice, and the phone sends subtitles instead. */
            ESP_LOGI(TAG, "call started with the speaker at 0: no volume raise");
        }
        mg_lct_mark(MG_LCT_TVOL);
    }
    uint32_t close_gen = s_close_gen;
    if (close_gen) {
        s_close_gen = 0;
#if CONFIG_MUSE_VIDEO
        muse_video_close_gen(close_gen - 1);   /* the call's video goes with it, not a newer stream */
#endif
    }
}

/* ---- the controls: End call, volume − / +, the level bar --------------- */

/*
 * The controls panel (muse_call_controls.h): one LVGL object on the
 * screen that draws it shape by shape when LVGL draws (lean_draw), and the
 * same shapes drawn into each video piece while video plays and LVGL is
 * paused (lean_overlay): nothing kept but the panel's place, on every board
 * (no canvas). Taps come from
 * LVGL's pointer events, and from reading the touch controller here while
 * video plays.
 */
static lv_obj_t *s_ctl;            /* the panel's object; NULL: no panel (small screen) */
static lv_obj_t *s_end_btn;        /* without the panel, on a touch board: the compact End call button */
static muse_call_end_btn_t s_eb;
static muse_call_panel_t s_P;      /* its place and hit areas on this screen */
static int s_ctl_x, s_ctl_y;       /* the panel's top left on the screen */
static int s_rendered_vol = -1;    /* what the picture shows */
static bool s_rendered_hint;
static const char *s_rendered_hint_text;
static lv_indev_t *s_touch;        /* the touch screen, if any */
/* Where the controls come up (muse_call_touch_down, muse_call_controls.h): the touch-down off a talk
 * surface, a tap's lift on one. s_shown_down_us: when a touch-down brought them up, for the log line of when
 * they were first drawn (lean_draw, controls_overlay). */
static muse_call_touch_t s_touch_t;
static volatile int64_t s_shown_down_us;

/* A card that asks for a press or a code is up (muse_call_card): the controls stay off it. */
static volatile bool s_card;

static bool controls_shown(void)
{
    return s_ctl && !s_card && call_phase() != MUSE_CALL_NONE && muse_call_controls_up(now_us(), s_controls_until_us);
}


bool muse_call_controls_at(int x, int y)
{
    return controls_shown() && muse_call_ctl_hit(&s_P, x - s_ctl_x, y - s_ctl_y) != MUSE_CALL_HIT_NONE;
}

static void show_controls(void)
{
    int64_t now = now_us();
    s_controls_until_us = now + CONTROLS_SHOW_US;
    if (!s_hinted && call_phase() == MUSE_CALL_LIVE && !s_calling_up) {   /* the calling screen has no hint */
        s_hinted = true;   /* how to use them, the first time in a call */
        s_hint_text = NULL;
        s_hint_until_us = now + HINT_SHOW_US;
        s_controls_until_us = s_hint_until_us;
    }
}

static void volume_step(int steps, const char *why)
{
    volume_set(muse_call_volume_step(volume_now(), steps), why);
}

bool muse_call_volume_by(const char *by)
{
    if (call_phase() == MUSE_CALL_NONE || !s_ctl) {
        return false;   /* no call, or no panel on this screen: the caller shows it */
    }
    int64_t now = now_us();
    s_hint_text = by;
    s_hint_until_us = now + CONTROLS_SHOW_US;
    s_controls_until_us = s_hint_until_us;
    ESP_LOGI(TAG, "volume %d%% (%s): the controls show it", volume_now(), by);
    return true;
}

void muse_call_volume_key(int steps)
{
    if (call_phase() == MUSE_CALL_NONE) {
        return;
    }
    muse_state_poke();
    volume_step(steps, steps > 0 ? "key +" : "key -");
}

static void draw_rect(lv_layer_t *layer, int x0, int y0, int x1, int y1, uint32_t color, int radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = LV_OPA_COVER;
    d.radius = radius;
    lv_area_t a = { x0, y0, x1 - 1, y1 - 1 };
    lv_draw_rect(layer, &d, &a);
}

static void draw_text(lv_layer_t *layer, int x0, int y0, int x1, int y1, const lv_font_t *font, uint32_t color,
                      const char *text)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    d.font = font;
    d.color = lv_color_hex(color);
    d.align = LV_TEXT_ALIGN_CENTER;
    int h = lv_font_get_line_height(font);
    int top = y0 + (y1 - y0 - h) / 2;
    lv_area_t a = { x0, top, x1 - 1, top + h - 1 };
    lv_draw_label(layer, &d, &a);
}

/* A standard icon (muse_icons.h) in white, centered at cx, cy. */
#define CTL_ICON_PX 28
static void draw_icon(lv_layer_t *layer, muse_icon_t icon, int cx, int cy)
{
    const lv_image_dsc_t *src = muse_icon(icon, CTL_ICON_PX);
    if (!src) {
        return;
    }
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = src;
    d.recolor = lv_color_white();
    d.recolor_opa = LV_OPA_COVER;
    int w = src->header.w, h = src->header.h;
    lv_area_t a = { cx - w / 2, cy - h / 2, cx - w / 2 + w - 1, cy - h / 2 + h - 1 };
    lv_draw_image(layer, &d, &a);
}

/* The bar as the video overlay redraws it each frame, so both look the same. */
#define COLOR_BAR_TRACK 0x3a3355
#define COLOR_BAR_FILL 0x2fbf71
#define COLOR_BUTTON 0x2e2552
#define COLOR_END 0xe0393b
#define COLOR_HINT_TEXT 0xf2efff

#define HINT_TEXT_DEFAULT "Hold to talk " LV_SYMBOL_BULLET " tap for controls"
#define END_TEXT "End call"
#define END_FONT (&lv_font_montserrat_20)
#define END_GAP 8                  /* between the call_end glyph and "End call", centered together in the pill */

/* LVGL task, LVGL running: the object redraws itself (lean_draw) when what it shows changed. */
static void render_controls(void)
{
    s_rendered_vol = volume_now();
    s_rendered_hint = now_us() < s_hint_until_us;
    s_rendered_hint_text = s_hint_text;
    lv_obj_invalidate(s_ctl);
}

/* The panel's object follows controls_shown() (muse_call_tick, and a touch-down at once). LVGL task. */
static void ctl_sync(void)
{
    if (s_end_btn) {
        /* Shown for the whole call (asked, connecting, live) whenever LVGL has the screen: no show/hide. */
        bool drawing = false;
#if CONFIG_MUSE_VIDEO
        drawing = muse_video_drawing();
#endif
        bool on = call_phase() != MUSE_CALL_NONE && !drawing;
        if (lv_obj_has_flag(s_end_btn, LV_OBJ_FLAG_HIDDEN) == on) {
            lv_obj_set_flag(s_end_btn, LV_OBJ_FLAG_HIDDEN, !on);
            if (on) {
                lv_obj_move_foreground(s_end_btn);   /* over the calling screen */
            }
        }
    }
    if (!s_ctl) {
        return;
    }
    bool shown = controls_shown();
    if (shown && (s_rendered_vol != volume_now() || s_rendered_hint != (now_us() < s_hint_until_us) ||
                  s_rendered_hint_text != s_hint_text)) {
        render_controls();
    }
    if (lv_obj_has_flag(s_ctl, LV_OBJ_FLAG_HIDDEN) == shown) {
        lv_obj_set_flag(s_ctl, LV_OBJ_FLAG_HIDDEN, !shown);
        if (shown) {
            lv_obj_move_foreground(s_ctl);
        }
    }
}

/* The controls' touch-down latency, once per show: the panel first drawn (into LVGL's layer, or into a video frame)
 * this long after the touch-down that brought it up. One line per show, no per-frame logging. Only noted here:
 * the draw may run deep on a panel's send task (muse_lcd_bands' lcd_send, under a video window's fill), where a
 * log line's formatting and sinks overflowed its stack (on the W175); muse_call_poll logs it. */
static const char *volatile s_shown_where;
static volatile int32_t s_shown_ms;

static void shown_down_log(const char *where)
{
    int64_t t0 = s_shown_down_us;
    if (t0) {
        s_shown_down_us = 0;
        s_shown_ms = (int32_t)((now_us() - t0) / 1000);
        s_shown_where = where;
    }
}

static void shown_down_flush(void)
{
    const char *where = s_shown_where;
    if (where) {
        s_shown_where = NULL;
        ESP_LOGI(TAG, "call controls drawn %s %ld ms after the touch-down", where, (long)s_shown_ms);
    }
}

#if CONFIG_MUSE_VIDEO

/* Over a video frame (muse_video's show task): the panel's picture, then the
 * level bar at the volume now (the picture may be older: LVGL is paused). */
static void lean_overlay(uint8_t *dst, int stride, int x, int y, int w, int rows);

static void controls_overlay(uint8_t *dst, int stride, int x, int y, int w, int rows)
{
    if (!controls_shown()) {
        return;
    }
    lean_overlay(dst, stride, x, y, w, rows);
    shown_down_log("over video");
}


/*
 * The lean panel over a piece of a video frame (no canvas): the
 * shapes render_controls draws, computed per pixel inside each shape's box
 * only: rounded rectangles and circles with a soft one-pixel edge, the
 * standard icons (A8, in flash) and the labels' glyphs (the fonts' own bits,
 * in flash). The piece holds panel rows y .. y + rows, columns x .. x + w.
 */
typedef struct {
    uint8_t *dst;
    int stride, x, y, w, rows;
} piece_t;

#define blend_rgb muse_call_blend_rgb

/* The piece's rows and columns a box covers (its coordinates from ox, oy on the screen: the panel's top left,
 * or 0, 0); false: none. */
static bool lean_clip(const piece_t *p, int ox, int oy, int x0, int y0, int x1, int y1, int *r0, int *r1, int *c0,
                      int *c1)
{
    *r0 = y0 + oy - p->y;
    *r1 = y1 + oy - p->y;
    *c0 = x0 + ox - p->x;
    *c1 = x1 + ox - p->x;
    *r0 = *r0 < 0 ? 0 : *r0;
    *r1 = *r1 > p->rows ? p->rows : *r1;
    *c0 = *c0 < 0 ? 0 : *c0;
    *c1 = *c1 > p->w ? p->w : *c1;
    return *r0 < *r1 && *c0 < *c1;
}

static void lean_rect(const piece_t *p, int x0, int y0, int x1, int y1, int rad, uint32_t rgb)
{
    /* Panel coordinates of the piece's top left: the shape's coordinates are the panel's (muse_call_controls.h). */
    muse_call_rrect(p->dst, p->stride, p->x - s_ctl_x, p->y - s_ctl_y, p->w, p->rows, x0, y0, x1, y1, rad, rgb);
}

/*
 * The panel's icons and glyphs as a list of sprites (an alpha bitmap in flash,
 * where it goes, its color), made once for what's shown (the hint and its text
 * change it) instead of looking each glyph up in every piece of every frame.
 */
typedef struct {
    const uint8_t *bits;
    int16_t x, y, w, h;      /* panel coordinates (the bench mark's: the screen's) */
    uint16_t stride_bits;
    uint8_t bpp;
    uint32_t rgb;            /* its color */
} sprite_t;
#define LEAN_SPRITES 48      /* the hint's 34 glyphs, three icons and "End call"'s 8 glyphs */
static sprite_t s_spr[LEAN_SPRITES];
static int s_nspr = -1;              /* -1: to be made */
static bool s_spr_hint;
static const char *s_spr_hint_text;
/* Where spr_add appends: the panel's list (spr_make) or the bench mark's (bench_make). */
static sprite_t *s_tgt = s_spr;
static int *s_tgt_n = &s_nspr;
static int s_tgt_cap = LEAN_SPRITES;

static void spr_add(const uint8_t *bits, int x, int y, int w, int h, int bpp, int stride_bits, uint32_t rgb)
{
    if (*s_tgt_n < s_tgt_cap && bits && w > 0 && h > 0 && bpp) {
        s_tgt[(*s_tgt_n)++] = (sprite_t){ bits, (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (uint16_t)stride_bits,
                                          (uint8_t)bpp, rgb };
    }
}

static void spr_icon(muse_icon_t icon, int cx, int cy, int px)
{
    const lv_image_dsc_t *img = muse_icon(icon, px);
    if (img && img->header.cf == LV_COLOR_FORMAT_A8) {
        int w = img->header.w, h = img->header.h, st = img->header.stride ? img->header.stride : w;
        spr_add(img->data, cx - w / 2, cy - h / 2, w, h, 8, st * 8, 0xffffff);
    }
}

static int lean_bpp(lv_font_glyph_format_t f)
{
    return f == LV_FONT_GLYPH_FORMAT_A1 ? 1 : f == LV_FONT_GLYPH_FORMAT_A2 ? 2 : f == LV_FONT_GLYPH_FORMAT_A4 ? 4
         : f == LV_FONT_GLYPH_FORMAT_A8 ? 8 : 0;
}

/* The next UTF-8 code point of s from *i (our labels: ASCII and LVGL's symbols). */
static uint32_t lean_next_cp(const char *s, uint32_t *i)
{
    const unsigned char *u = (const unsigned char *)s + *i;
    uint32_t cp = u[0];
    int n = cp < 0x80 ? 1 : (cp & 0xe0) == 0xc0 ? 2 : (cp & 0xf0) == 0xe0 ? 3 : (cp & 0xf8) == 0xf0 ? 4 : 1;
    if (n > 1) {
        cp &= 0x3f >> (n - 1);
        for (int k = 1; k < n; k++) {
            if ((u[k] & 0xc0) != 0x80) {
                n = k;
                break;
            }
            cp = cp << 6 | (u[k] & 0x3f);
        }
    }
    *i += (uint32_t)n;
    return cp;
}

/* The text's width in pixels, with kerning. */
static int lean_text_w(const lv_font_t *font, const char *s)
{
    int w = 0;
    for (uint32_t i = 0; s[i];) {
        uint32_t cp = lean_next_cp(s, &i), j = i, nx = s[i] ? lean_next_cp(s, &j) : 0;
        w += lv_font_get_glyph_width(font, cp, nx);
    }
    return w;
}

/* The text's glyphs as sprites, its line's top left at x0, top (panel coordinates). */
static void spr_text(const lv_font_t *font, uint32_t rgb, int x0, int top, const char *s)
{
    int x = x0;
    for (uint32_t i = 0; s[i];) {
        uint32_t cp = lean_next_cp(s, &i), j = i, nx = s[i] ? lean_next_cp(s, &j) : 0;
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(font, &g, cp, nx)) {
            continue;
        }
        int bpp = lean_bpp(g.format);
        if (bpp && g.box_w && g.box_h && g.resolved_font && g.resolved_font->get_glyph_bitmap) {
            g.req_raw_bitmap = 1;
            const uint8_t *bm = g.resolved_font->get_glyph_bitmap(&g, NULL);
            spr_add(bm, x + g.ofs_x, top + (font->line_height - font->base_line) - g.box_h - g.ofs_y, g.box_w,
                    g.box_h, bpp, g.stride ? g.stride * 8 : g.box_w * bpp, rgb);
        }
        x += g.adv_w;
    }
}

/* Where End call's glyph starts: the glyph, the gap and the text centered together in the pill. */
static int end_x0(void)
{
    return (MUSE_CALL_END_X0 + MUSE_CALL_END_X1 - (CTL_ICON_PX + END_GAP + lean_text_w(END_FONT, END_TEXT))) / 2;
}

static void spr_make(bool hint, const char *hint_text)
{
    s_tgt = s_spr, s_tgt_n = &s_nspr, s_tgt_cap = LEAN_SPRITES;
    s_nspr = 0;
    s_spr_hint = hint;
    s_spr_hint_text = hint_text;
    if (hint) {
        const char *t = hint_text ? hint_text : HINT_TEXT_DEFAULT;
        const lv_font_t *f = &lv_font_montserrat_14;
        spr_text(f, COLOR_HINT_TEXT, (MUSE_CALL_HINT_X0 + MUSE_CALL_HINT_X1 - lean_text_w(f, t)) / 2,
                 (MUSE_CALL_HINT_H - lv_font_get_line_height(f)) / 2, t);
    }
    const int by = MUSE_CALL_BTN_Y, bw = MUSE_CALL_BTN;
    spr_icon(MUSE_ICON_REMOVE, bw / 2, by + bw / 2, CTL_ICON_PX);
    spr_icon(MUSE_ICON_ADD, MUSE_CALL_CTL_W - bw / 2, by + bw / 2, CTL_ICON_PX);
    int x0 = end_x0();
    spr_icon(MUSE_ICON_CALL_END, x0 + CTL_ICON_PX / 2, (MUSE_CALL_END_Y0 + MUSE_CALL_END_Y1) / 2, CTL_ICON_PX);
    spr_text(END_FONT, 0xffffff, x0 + CTL_ICON_PX + END_GAP,
             MUSE_CALL_END_Y0 + (MUSE_CALL_END_Y1 - MUSE_CALL_END_Y0 - lv_font_get_line_height(END_FONT)) / 2, END_TEXT);
}

/* A sprite into a piece; its coordinates from ox, oy on the screen. */
static void spr_draw(const piece_t *p, const sprite_t *sp, int ox, int oy)
{
    int r0, r1, c0, c1;
    if (!lean_clip(p, ox, oy, sp->x, sp->y, sp->x + sp->w, sp->y + sp->h, &r0, &r1, &c0, &c1)) {
        return;
    }
    const int bpp = sp->bpp, maxv = (1 << bpp) - 1;
    const int scale = 255 / maxv;   /* 255, 85, 17, 1: exact for 1, 2, 4 and 8 bits */
    const uint32_t rgb = sp->rgb;
    for (int r = r0; r < r1; r++) {
        int gr = p->y + r - oy - sp->y;
        uint8_t *d = p->dst + (size_t)r * p->stride + (size_t)c0 * 2;
        int bit = gr * sp->stride_bits + (p->x + c0 - ox - sp->x) * bpp;
        for (int c = c0; c < c1; c++, d += 2, bit += bpp) {
            int v = bpp == 8 ? sp->bits[bit >> 3] : (sp->bits[bit >> 3] >> (8 - bpp - (bit & 7))) & maxv;
            if (v) {
                blend_rgb(d, rgb, (unsigned)(v * scale));
            }
        }
    }
}

static void lean_overlay(uint8_t *dst, int stride, int x, int y, int w, int rows)
{
    if (y + rows <= s_ctl_y || y >= s_ctl_y + MUSE_CALL_CTL_H) {
        return;   /* not the panel's rows */
    }
    bool hint = now_us() < s_hint_until_us;
    if (s_nspr < 0 || hint != s_spr_hint || (hint && s_hint_text != s_spr_hint_text)) {
        spr_make(hint, s_hint_text);
    }
    const piece_t p = { dst, stride, x, y, w, rows };
    if (hint) {
        lean_rect(&p, MUSE_CALL_HINT_X0, 0, MUSE_CALL_HINT_X1, MUSE_CALL_HINT_H, MUSE_CALL_HINT_H / 2, 0x000000);
    }
    const int by = MUSE_CALL_BTN_Y, bw = MUSE_CALL_BTN;
    lean_rect(&p, 0, by, bw, by + bw, bw / 2, COLOR_BUTTON);
    lean_rect(&p, MUSE_CALL_CTL_W - bw, by, MUSE_CALL_CTL_W, by + bw, bw / 2, COLOR_BUTTON);
    /* A dark band behind the bar so it reads over video too. */
    lean_rect(&p, MUSE_CALL_BAR_X0 - 8, MUSE_CALL_BAR_Y0 - 8, MUSE_CALL_BAR_X1 + 8, MUSE_CALL_BAR_Y1 + 8, 8, 0x000000);
    lean_rect(&p, MUSE_CALL_BAR_X0, MUSE_CALL_BAR_Y0, MUSE_CALL_BAR_X1, MUSE_CALL_BAR_Y1, 0, COLOR_BAR_TRACK);
    int fill = muse_call_bar_fill(volume_now());
    if (fill > 0) {
        lean_rect(&p, MUSE_CALL_BAR_X0, MUSE_CALL_BAR_Y0, MUSE_CALL_BAR_X0 + fill, MUSE_CALL_BAR_Y1, 0, COLOR_BAR_FILL);
    }
    lean_rect(&p, MUSE_CALL_END_X0, MUSE_CALL_END_Y0, MUSE_CALL_END_X1, MUSE_CALL_END_Y1,
              (MUSE_CALL_END_Y1 - MUSE_CALL_END_Y0) / 2, COLOR_END);
    for (int i = 0; i < s_nspr; i++) {
        spr_draw(&p, &s_spr[i], s_ctl_x, s_ctl_y);
    }
}

/* ---- the call video's stats overlay (muse_call_stats.h) ----------------
 *
 * Five taps on the call video show it, one hides it, and it starts hidden in
 * every call. Once a second (the input task, vstats_poll) the numbers
 * muse_video and mg.play already keep become a few lines of fixed-width
 * text, rendered into a 1-bit picture of the box (double-buffered: the show
 * task reads the front one while the back one is drawn), which the show task
 * blends into each video frame's pieces on their way to the panel, as the
 * subtitles band and the controls are: no redraw of its own, nothing that
 * makes a frame wait, nothing on the audio path. The same lines go to the USB
 * console (@vstats), never to the Nordic UART mirror. Console word: vstats
 * (toggle), vstats=1 / vstats=0, vstats?.
 */
#define VSTATS_PAD 4
static muse_vstats_taps_t s_vt;             /* the taps; the input task's */
static volatile bool s_vstats_on;           /* shown: the show task blends it */
static volatile bool s_vstats_reset;        /* a call started or ended (set on the BLE worker) */
static volatile int s_vstats_req;           /* the console's: 1 show, 2 hide */
static muse_vstats_acc_t s_vacc;
static int64_t s_vstats_next_us;
static const lv_font_t *s_vfont;
static muse_vstats_box_t s_vbox;
static int s_vstride;                       /* bytes a row of the 1-bit picture */
static uint8_t *s_vbits[2];                 /* the box's 1-bit pictures: front and back */
static volatile int s_vfront = -1;          /* the one shown (-1: none) */
static volatile uint32_t s_vstats_flips;    /* the front picture's changes (video_overlay_rects' gen) */
static volatile int s_vused_w, s_vused_h;   /* what the front one's text covers, px from the box's corner */
static bool s_vgeom_tried;

static void vstats_edge(void)
{
    s_vstats_on = false;   /* gone from the very next frame */
    s_vstats_reset = true;
}

/* The box for this board: the fixed-width font that fits, placed clear of the
 * mic ring at the top (the subtitles band and the controls are at the
 * bottom), and its two pictures. False: no room or no memory. */
static bool vstats_geometry(void)
{
    if (s_vgeom_tried) {
        return s_vbits[0] != NULL;
    }
    s_vgeom_tried = true;
    const lv_font_t *fonts[2] = { NULL, NULL };
    int nf = 0;
    int w = muse_board->width, h = muse_board->height, small = w < h ? w : h;
#if CONFIG_LV_FONT_UNSCII_16
    if (small >= 300) {
        fonts[nf++] = &lv_font_unscii_16;
    }
#endif
#if CONFIG_LV_FONT_UNSCII_8
    fonts[nf++] = &lv_font_unscii_8;
#endif
    int ring = muse_mic_ring_width(w, h);
    for (int i = 0; i < nf && !s_vfont; i++) {
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(fonts[i], &g, 'M', 0)) {
            continue;
        }
        int bw = (MUSE_VSTATS_COLS - 1) * g.adv_w + 2 * VSTATS_PAD;
        int bh = MUSE_VSTATS_LINES * lv_font_get_line_height(fonts[i]) + 2 * VSTATS_PAD;
        if (muse_vstats_place(w, h, muse_board->round, muse_board->corner_px, ring, VSTATS_PAD, bw, bh, &s_vbox)) {
            s_vfont = fonts[i];
        }
    }
    if (!s_vfont) {
        ESP_LOGW(TAG, "stats overlay: no room on this panel");
        return false;
    }
    s_vstride = (s_vbox.w + 7) / 8;
    size_t n = (size_t)s_vstride * s_vbox.h;
    for (int i = 0; i < 2; i++) {
        s_vbits[i] = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_vbits[i]) {
            s_vbits[i] = heap_caps_malloc(n, MALLOC_CAP_8BIT);
        }
    }
    if (!s_vbits[0] || !s_vbits[1]) {
        free(s_vbits[0]);
        free(s_vbits[1]);
        s_vbits[0] = s_vbits[1] = NULL;
        ESP_LOGW(TAG, "stats overlay: no memory");
        return false;
    }
    return true;
}

/* The lines into the back picture (the font's own bits, any bpp: on at half
 * coverage), then it becomes the front. Input task. */
static void vstats_render(char lines[][MUSE_VSTATS_COLS], int n)
{
    int back = s_vfront == 0 ? 1 : 0;
    uint8_t *bits = s_vbits[back];
    memset(bits, 0, (size_t)s_vstride * s_vbox.h);
    int lh = lv_font_get_line_height(s_vfont), used_w = 0;
    for (int l = 0; l < n; l++) {
        int x = VSTATS_PAD, top = VSTATS_PAD + l * lh;
        for (const char *c = lines[l]; *c; c++) {
            lv_font_glyph_dsc_t g;
            if (!lv_font_get_glyph_dsc(s_vfont, &g, (uint32_t)(uint8_t)*c, 0)) {
                continue;
            }
            int bpp = g.format == LV_FONT_GLYPH_FORMAT_A1 ? 1 : g.format == LV_FONT_GLYPH_FORMAT_A2 ? 2
                      : g.format == LV_FONT_GLYPH_FORMAT_A4 ? 4 : g.format == LV_FONT_GLYPH_FORMAT_A8 ? 8 : 0;
            g.req_raw_bitmap = 1;
            const uint8_t *bm = bpp && g.box_w && g.box_h && g.resolved_font && g.resolved_font->get_glyph_bitmap
                                    ? g.resolved_font->get_glyph_bitmap(&g, NULL)
                                    : NULL;
            int gx = x + g.ofs_x, gy = top + (s_vfont->line_height - s_vfont->base_line) - g.box_h - g.ofs_y;
            int stride_bits = g.stride ? g.stride * 8 : g.box_w * bpp, maxv = (1 << bpp) - 1;
            for (int r = 0; bm && r < g.box_h; r++) {
                int py = gy + r;
                if (py < 0 || py >= s_vbox.h) {
                    continue;
                }
                for (int col = 0; col < g.box_w; col++) {
                    int px = gx + col;
                    int bit = r * stride_bits + col * bpp;
                    int v = (bm[bit >> 3] >> (8 - bpp - (bit & 7))) & maxv;
                    if (px >= 0 && px < s_vbox.w && 2 * v > maxv) {
                        bits[py * s_vstride + (px >> 3)] |= (uint8_t)(0x80 >> (px & 7));
                    }
                }
            }
            x += g.adv_w;
        }
        used_w = x > used_w ? x : used_w;
    }
    s_vused_w = n ? (used_w + VSTATS_PAD > s_vbox.w ? s_vbox.w : used_w + VSTATS_PAD) : 0;
    s_vused_h = n ? (VSTATS_PAD * 2 + n * lh > s_vbox.h ? s_vbox.h : VSTATS_PAD * 2 + n * lh) : 0;
    s_vfront = back;
    s_vstats_flips++;
}

/* The overlay over a piece of a video frame (the show task): the text white,
 * the box under it at half brightness. */
static void vstats_overlay(uint8_t *dst, int stride, int x, int y, int w, int rows)
{
    int f = s_vfront;
    if (!s_vstats_on || f < 0) {
        return;
    }
    const uint8_t *bits = s_vbits[f];
    int bw = s_vused_w, bh = s_vused_h;
    int r0 = s_vbox.y - y, r1 = r0 + bh, c0 = s_vbox.x - x, c1 = c0 + bw;
    r0 = r0 < 0 ? 0 : r0;
    r1 = r1 > rows ? rows : r1;
    c0 = c0 < 0 ? 0 : c0;
    c1 = c1 > w ? w : c1;
    for (int r = r0; r < r1; r++) {
        int by = y + r - s_vbox.y;
        const uint8_t *row = bits + (size_t)by * s_vstride;
        uint8_t *d = dst + (size_t)r * stride + (size_t)c0 * 2;
        for (int c = c0; c < c1; c++, d += 2) {
            int bx = x + c - s_vbox.x;
            if (row[bx >> 3] & (0x80 >> (bx & 7))) {
                d[0] = 0xff;
                d[1] = 0xff;
            } else {
                unsigned v = ((unsigned)d[0] << 8 | d[1]) >> 1 & 0x7BEF;   /* half of each of R, G, B */
                d[0] = (uint8_t)(v >> 8);
                d[1] = (uint8_t)v;
            }
        }
    }
}

/* Shows (on) or hides it; why for the log. Input task. */
static void vstats_set(bool on, const char *why)
{
    if (on && !vstats_geometry()) {
        on = false;
    }
    s_vt.shown = on;   /* the taps' idea of it (a refused show included) */
    if (on == s_vstats_on) {
        return;
    }
    if (on) {
        muse_vstats_acc_open(&s_vacc);    /* the call's totals stay (from its start); rates start over */
        s_vfront = -1;
        s_vstats_next_us = 0;             /* the first lines at once */
    }
    s_vstats_on = on;
    ESP_LOGI(TAG, "stats overlay %s (%s)", on ? "on" : "off", why);
}

/* The numbers now, from muse_video and mg_ble (plain reads under their own locks). */
static void vstats_sample(muse_vstats_sample_t *s)
{
    memset(s, 0, sizeof(*s));
    s->t_ms = now_us() / 1000;
    muse_video_stats_t v;
    muse_video_stats(&v);
    s->video = v.streaming;
    s->codec = v.vp8 ? "VP8" : "H.264";
    s->coded_w = v.coded_w;
    s->coded_h = v.coded_h;
    s->shown_w = v.shown_w;
    s->shown_h = v.shown_h;
    s->rx_bytes = v.rx_bytes;
    s->shown = v.shown;
    s->dec_errors = v.dec_errors;
    s->drops = v.dropped;
    s->kf_requests = v.kf_requests;
    mg_ble_call_stats_t b;
    mg_ble_call_stats(&b);
    s->link = b.link;
    s->itvl_1250us = b.itvl;
    s->link_mode = b.mode;
    s->play = b.play;
    s->play_stream = b.stream;
    s->play_underruns = b.underruns;
    s->play_depth_ms = b.depth_ms;
}

/* The input task, every 10 ms: the call's edges, the console's asks, and the
 * once-a-second refresh while it's up. */
static void vstats_poll(void)
{
    if (s_vstats_reset) {
        s_vstats_reset = false;
        muse_vstats_call_edge(&s_vt);
        vstats_set(false, "call edge");
        /* The call's totals count from its start (its edges: a start, or the end before the next). */
        muse_vstats_sample_t base;
        vstats_sample(&base);
        muse_vstats_acc_reset(&s_vacc);
        muse_vstats_acc_base(&s_vacc, &base);
    }
    int req = s_vstats_req;
    if (req) {
        s_vstats_req = 0;
        vstats_set(req == 1, "console");
    }
    int64_t now = now_us();
    if (!s_vstats_on || now < s_vstats_next_us) {
        return;
    }
    s_vstats_next_us = now + 1000000;
    muse_vstats_sample_t smp;
    vstats_sample(&smp);
    char lines[MUSE_VSTATS_LINES][MUSE_VSTATS_COLS];
    int n = muse_vstats_lines(&s_vacc, &smp, lines);
    vstats_render(lines, n);
    /* The same lines on the USB console only (printf: the Nordic UART mirrors log lines). */
    char out[MUSE_VSTATS_LINES * (MUSE_VSTATS_COLS + 3) + 1];
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        o += (size_t)snprintf(out + o, sizeof(out) - o, "%s%s", i ? " | " : "", lines[i]);
        if (o >= sizeof(out)) {
            break;
        }
    }
    printf("@vstats %s\n", n ? out : "(nothing measured)");
    fflush(stdout);
}

bool muse_call_stats_word(const char *line)
{
    if (strcmp(line, "vstats") && strcmp(line, "vstats=1") && strcmp(line, "vstats=0") && strcmp(line, "vstats?")) {
        return false;
    }
    bool on = s_vstats_on;
    if (line[6] == '=') {
        on = line[7] == '1';
    } else if (!line[6]) {
        on = !on;   /* the toggle */
    }
    if (line[6] != '?') {
        s_vstats_req = on ? 1 : 2;   /* applied on the input task (vstats_poll) */
    }
    printf("@vstats %s\n", on ? "on" : "off");
    fflush(stdout);
    return true;
}

/* Over a video frame: the controls, then the stats overlay. */
/*
 * The bench mark over a video frame (LVGL paused, so the home's label
 * can't draw): "Bench" where it stands on the screen and in its color
 * (muse_home_bench_mark), glyph by glyph from the font, made once. Mic
 * output can go to an unproven central while it shows (the mark is visible
 * wherever the mic can send).
 */
#define BENCH_SPRITES 8
static sprite_t s_bspr[BENCH_SPRITES];
static int s_nbspr = -1;   /* -1: to be made */

static void bench_overlay(uint8_t *dst, int stride, int x, int y, int w, int rows)
{
    int bx, by;
    const lv_font_t *font;
    uint32_t rgb;
    if (!muse_home_bench_mark(&bx, &by, &font, &rgb)) {
        return;
    }
    if (y + rows <= by || y >= by + lv_font_get_line_height(font)) {
        return;   /* not its rows */
    }
    if (s_nbspr < 0) {
        s_tgt = s_bspr, s_tgt_n = &s_nbspr, s_tgt_cap = BENCH_SPRITES;
        s_nbspr = 0;
        spr_text(font, rgb, bx, by, MUSE_HOME_BENCH_TEXT);
        s_tgt = s_spr, s_tgt_n = &s_nspr, s_tgt_cap = LEAN_SPRITES;
    }
    const piece_t p = { dst, stride, x, y, w, rows };
    for (int i = 0; i < s_nbspr; i++) {
        spr_draw(&p, &s_bspr[i], 0, 0);
    }
}

static void video_overlay(uint8_t *dst, int stride, int x, int y, int w, int rows)
{
    controls_overlay(dst, stride, x, y, w, rows);
    vstats_overlay(dst, stride, x, y, w, rows);
    bench_overlay(dst, stride, x, y, w, rows);   /* last: never under the controls or the stats */
}

static void rect_union(muse_board_win_t *r, int x0, int y0, int x1, int y1)
{
    if (r->w <= 0 || r->h <= 0) {
        *r = (muse_board_win_t){ (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
        return;
    }
    int ax = r->x < x0 ? r->x : x0, ay = r->y < y0 ? r->y : y0;
    int bx = r->x + r->w > x1 ? r->x + r->w : x1, by = r->y + r->h > y1 ? r->y + r->h : y1;
    *r = (muse_board_win_t){ (int16_t)ax, (int16_t)ay, (int16_t)(bx - ax), (int16_t)(by - ay) };
}

/*
 * What video_overlay draws over the picture now (changed-only redraw,
 * muse_video_set_overlay_rects): the controls panel, the stats box and the
 * bench mark, each as the rectangle it can touch (panel coordinates); gen
 * changes whenever any of them shows, hides or changes what it shows (its
 * low three bits: which of them show, for the shadow compare's log). Any
 * task; plain reads.
 */
static int video_overlay_rects(muse_board_win_t *out, int max, uint32_t *gen)
{
    int n = 0;
    uint32_t bits = 0, h = 0;
    if (controls_shown()) {
        muse_board_win_t r = { 0, 0, 0, 0 };
        /* The shapes lean_draw draws (a pixel of soft edge around), and its sprites. */
        int ph = MUSE_CALL_CTL_H > MUSE_CALL_END_Y1 ? MUSE_CALL_CTL_H : MUSE_CALL_END_Y1;
        rect_union(&r, s_ctl_x - 2, s_ctl_y - 2, s_ctl_x + MUSE_CALL_CTL_W + 2, s_ctl_y + ph + 2);
        for (int i = 0; i < s_nspr && i < LEAN_SPRITES; i++) {
            const sprite_t *sp = &s_spr[i];
            rect_union(&r, s_ctl_x + sp->x - 1, s_ctl_y + sp->y - 1, s_ctl_x + sp->x + sp->w + 1,
                       s_ctl_y + sp->y + sp->h + 1);
        }
        if (n < max) {
            out[n++] = r;
        }
        bits |= 1u;
        h = h * 31u + (uint32_t)volume_now();
        bool hint = now_us() < s_hint_until_us;   /* as lean_overlay decides it */
        h = h * 31u + (uint32_t)hint;
        h = h * 31u + (hint ? (uint32_t)(uintptr_t)s_hint_text : 0u);
    }
    int vf = s_vfront;
    if (s_vstats_on && vf >= 0) {
        if (n < max) {
            out[n++] = (muse_board_win_t){ (int16_t)s_vbox.x, (int16_t)s_vbox.y, (int16_t)s_vbox.w, (int16_t)s_vbox.h };
        }
        bits |= 2u;
        h = h * 31u + s_vstats_flips;
    }
    int bx, by;
    const lv_font_t *font;
    uint32_t rgb;
    if (muse_home_bench_mark(&bx, &by, &font, &rgb)) {
        /* Its rows, the screen's width (its glyphs stay inside them). */
        int lh = lv_font_get_line_height(font);
        if (n < max) {
            out[n++] = (muse_board_win_t){ 0, (int16_t)(by - 1), (int16_t)muse_board->width, (int16_t)(lh + 2) };
        }
        bits |= 4u;
        h = h * 31u + (uint32_t)bx;
    }
    if (gen) {
        *gen = h << 3 | bits;   /* bit 0: the controls, 1: the stats, 2: the bench mark; the rest: what they show */
    }
    return n;
}

#endif /* CONFIG_MUSE_VIDEO */

#if !CONFIG_MUSE_VIDEO
static void vstats_edge(void) {}
static void vstats_poll(void) {}
#endif

/* ---- touch: taps on the controls and the volume swipe ------------------- */

static struct {
    bool down;        /* a press being followed */
    bool moved;       /* ...that's gone past a tap */
    int x0, y0;       /* where it went down */
    int applied;      /* volume steps it has applied */
    int64_t t0_us;    /* when (the End call tap's hold, for its log) */
} s_press;

void muse_call_card(bool up)
{
    if (up == s_card) {
        return;
    }
    s_card = up;
    if (up) {
        s_controls_until_us = 0;   /* they come back with the next tap */
        s_hint_until_us = 0;
        s_press.down = false;      /* a touch already down is the card's */
    }
    if (s_ctl) {
        ctl_sync();
    }
}


static void ctl_sync(void);

/* A tap's lift; was_up: the controls were up at its touch-down (the press acts on the control under it). */
static void tap(int x, int y, bool was_up)
{
    if (s_end_btn && !lv_obj_has_flag(s_end_btn, LV_OBJ_FLAG_HIDDEN) && muse_call_end_button_hit(&s_eb, x, y)) {
        ESP_LOGI(TAG, "tap: End call (the compact button)");
        ESP_LOGI(XY_TAG, "tap at %d,%d", x, y);
        muse_call_end_touch("End call button", x, y, (uint32_t)((now_us() - s_press.t0_us) / 1000));
        return;
    }
    muse_call_hit_t hit = s_ctl ? muse_call_ctl_hit(&s_P, x - s_ctl_x, y - s_ctl_y) : MUSE_CALL_HIT_NONE;
#if CONFIG_MUSE_VIDEO
    /* Five taps on the call video show the stats overlay, one hides
     * it; a tap on a shown control button is the button's. */
    if (muse_video_drawing() || s_vt.shown) {
        switch (muse_vstats_tap(&s_vt, now_us() / 1000, was_up && hit != MUSE_CALL_HIT_NONE)) {
        case MUSE_VSTATS_TAP_HIDE:
            vstats_set(false, "tap");
            return;   /* consumed */
        case MUSE_VSTATS_TAP_SWALLOW:
            return;   /* taps 2-4 of a run don't toggle the controls */
        case MUSE_VSTATS_TAP_SHOW:
            s_controls_until_us = 0;   /* the fifth hides the controls */
            s_hint_until_us = 0;
            vstats_set(true, "five taps");
            return;
        default:
            break;
        }
    }
#endif
    /* Hidden, a tap brings them up; up when this touch went down, a tap away from them dismisses them
     * at once and a tap on one acts and re-arms the 3 s; the touch that showed them (was_up false) never
     * dismisses them on its own lift (muse_call_tap). */
    long long until = s_controls_until_us;
    bool held = now_us() - s_press.t0_us >= MUSE_CALL_TALK_HOLD_US;   /* a hold isn't a tap away */
    muse_call_tap_t what = muse_call_tap(now_us(), &until, was_up && controls_shown(), hit, held);
    if (what == MUSE_CALL_TAP_SHOWN) {
        ESP_LOGI(TAG, "tap: controls shown%s", s_touch_t.early ? " (at its touch-down)" : "");
        ESP_LOGI(XY_TAG, "tap at %d,%d: controls shown", x, y);
        show_controls();   /* the first tap only brings them up */
        return;
    }
    s_controls_until_us = until;
    if (what == MUSE_CALL_TAP_DISMISSED) {
        ESP_LOGI(TAG, "tap: controls dismissed (a tap away from them)");
        ESP_LOGI(XY_TAG, "tap at %d,%d: controls dismissed", x, y);
        s_hint_until_us = 0;
        return;
    }
    ESP_LOGI(XY_TAG, "tap at %d,%d", x, y);
    ESP_LOGI(TAG, "tap: %s",
             hit == MUSE_CALL_HIT_END ? "End call" : hit == MUSE_CALL_HIT_MINUS ? "volume -" : hit == MUSE_CALL_HIT_PLUS ? "volume +" : "a hold on the picture (controls stay, 3 s from it)");
    switch (hit) {
    case MUSE_CALL_HIT_END:
        muse_call_end_touch("End call control", x, y, (uint32_t)((now_us() - s_press.t0_us) / 1000));
        return;
    case MUSE_CALL_HIT_MINUS:
        volume_step(-1, "-");
        break;
    case MUSE_CALL_HIT_PLUS:
        volume_step(1, "+");
        break;
    default:
        break;
    }
    show_controls();
}

/* talk_surface: a hold here is a talk press (the home's hold region with Settings > Talk on Screen or Both;
 * never the call video or the calling screen). lvgl: on the LVGL task (the face's touches), where the panel
 * can be put on the screen at once. */
static void press_down(int x, int y, bool talk_surface, bool lvgl)
{
    s_press.down = call_phase() != MUSE_CALL_NONE;
    s_press.moved = false;
    s_press.x0 = x;
    s_press.y0 = y;
    s_press.applied = 0;
    s_press.t0_us = now_us();
    if (!s_press.down) {
        return;
    }
    bool was_up = controls_shown();
    long long until = s_controls_until_us;
    if (muse_call_touch_down(&s_touch_t, s_press.t0_us, &until, was_up, talk_surface) && s_ctl) {
        s_controls_until_us = until;
        if (!was_up) {
            show_controls();   /* the hint the first time in a call, as a tap's */
            s_shown_down_us = s_press.t0_us;
            ESP_LOGI(XY_TAG, "touch-down at %d,%d", x, y);
            ESP_LOGI(TAG, "touch-down: controls shown (not a talk surface)");
            if (lvgl) {
                ctl_sync();   /* on the screen in this LVGL pass, not the next frame tick */
            }
        }
    }
}

/* Steps the volume as the finger goes further up or down. */
static void press_move(int x, int y)
{
    if (!s_press.down) {
        return;
    }
    int dx = x - s_press.x0, dy = y - s_press.y0;
    if (muse_swipe_past_slop(dx, dy)) {
        s_press.moved = true;   /* a swipe (muse_swipe.h): no tap at its lift, on a control or off one */
    }
    if (s_touch_t.early && controls_shown()) {
        long long until = s_controls_until_us;
        muse_call_touch_held(&s_touch_t, now_us(), &until);   /* a drag or a hold keeps them, 3 s from now */
        s_controls_until_us = until;
    }
    int want = muse_call_swipe_steps(dx, dy);
    /* Only further: easing back on the way doesn't undo steps. */
    if ((want > 0 && want > s_press.applied) || (want < 0 && want < s_press.applied)) {
        int steps = want - s_press.applied;
        s_press.applied = want;
        volume_step(steps, "swipe");
    }
}

static void press_up(int x, int y)
{
    if (!s_press.down) {
        return;
    }
    press_move(x, y);
    s_press.down = false;
    long long until = s_controls_until_us;
    bool early_up = s_touch_t.early && controls_shown();
    muse_call_lift_t lift = muse_call_touch_up(&s_touch_t, now_us(), &until, s_press.moved);
    if (early_up) {
        s_controls_until_us = until;   /* up since the touch-down: 3 s from this lift */
    }
    if (lift == MUSE_CALL_LIFT_TAP && call_phase() != MUSE_CALL_NONE) {
        tap(s_press.x0, s_press.y0, s_touch_t.was_up);
    }
}

/* A hold here would be a talk press: the home's hold region takes it (the face showing, no
 * calling screen over it, Settings > Talk not Button), so the controls wait for a tap's lift. LVGL task. */
static bool press_talks(int x, int y)
{
    return !s_calling_up && muse_home_talk_at(x, y);
}

/* The face's touches, from LVGL: only during a call, on the face, awake. */
static bool face_touch_ok(void)
{
    bool video = false, drawing = false;
#if CONFIG_MUSE_VIDEO
    video = muse_video_is_open();
    drawing = muse_video_drawing();
#endif
    if (call_phase() == MUSE_CALL_NONE || muse_ui_dark() || drawing) {
        return false;   /* while pictures are drawn LVGL is paused: poll_video_touch reads the screen */
    }
    /* The calling screen takes taps whether or not the video view is open
     * underneath: LVGL runs until the first picture. A view whose pictures
     * stopped shows the face with LVGL running again: its taps come here too
     * (an End call there once went nowhere, neither path took it). */
    return s_calling_up || video || muse_ui_face_active();
}

static void on_touch(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(s_touch, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        if (face_touch_ok() && !s_card) {   /* a touch on a card is the card's, never the call's */
            press_down(p.x, p.y, press_talks(p.x, p.y), true);
        }
    } else {
        press_up(p.x, p.y);
    }
}

/* While video plays LVGL is paused and reads no touch: read the controller
 * here (the input task, every 10 ms). */
static void poll_video_touch(void)
{
#if CONFIG_MUSE_VIDEO
    static bool was_down;
    static lv_point_t last;
    if (!s_touch || call_phase() == MUSE_CALL_NONE || !muse_video_drawing()) {
        if (was_down) {
            was_down = false;
            s_press.down = false;
        }
        return;
    }
    lv_indev_read_cb_t read = lv_indev_get_read_cb(s_touch);
    if (!read) {
        return;
    }
    lv_indev_data_t d = { 0 };
    read(s_touch, &d);
    bool down = d.state == LV_INDEV_STATE_PRESSED;
    if (down && !was_down) {
        press_down(d.point.x, d.point.y, false, false);   /* the call's video: never a talk surface */
    } else if (down) {
        press_move(d.point.x, d.point.y);
    } else if (was_down) {
        press_up(last.x, last.y);
    }
    if (down) {
        last = d.point;
        muse_state_poke();
    }
    was_down = down;
#endif
}

/* Bench (the console's tap=call.*): "minus", "plus", "end" (the panel's buttons:
 * a first tap only brings the panel up, as a finger's does), "minus.edge" /
 * "plus.edge" (their rows at the screen's side edge), "top" (the picture,
 * away from the panel: dismisses it), "panel" (its
 * level bar: brings it up or keeps it up), "swipe.down" / "swipe.up" (a
 * vertical swipe from the middle of the screen, three sixteenths). Only
 * during a call. Any task: plain arithmetic from the panel's place. */
bool muse_call_bench_point(const char *what, muse_touch_gesture_t *g)
{
    if (call_phase() == MUSE_CALL_NONE) {
        return false;
    }
    int w = muse_board->width, h = muse_board->height;
    if (!strncmp(what, "swipe.", 6)) {
        int dy = h * 35 / 100;
        bool down = !strcmp(what + 6, "down");
        if (!down && strcmp(what + 6, "up")) {
            return false;
        }
        *g = (muse_touch_gesture_t){ w / 2, h / 2 - (down ? dy / 2 : -dy / 2), w / 2,
                                     h / 2 + (down ? dy / 2 : -dy / 2), MUSE_TOUCH_DRAG_MS };
        return true;
    }
    if (!s_ctl) {
        return false;   /* no panel on this screen */
    }
    int x, y;
    /* "minus.edge" / "plus.edge": at the screen's side edge on their row. */
    if (!strcmp(what, "minus.edge")) {
        x = 2 - s_ctl_x, y = MUSE_CALL_BTN_Y + MUSE_CALL_BTN / 2;
    } else if (!strcmp(what, "plus.edge")) {
        x = muse_board->width - 3 - s_ctl_x, y = MUSE_CALL_BTN_Y + MUSE_CALL_BTN / 2;
    } else if (!strcmp(what, "minus")) {
        x = MUSE_CALL_BTN / 2;
        y = MUSE_CALL_BTN_Y + MUSE_CALL_BTN / 2;
    } else if (!strcmp(what, "plus")) {
        x = MUSE_CALL_CTL_W - MUSE_CALL_BTN / 2;
        y = MUSE_CALL_BTN_Y + MUSE_CALL_BTN / 2;
    } else if (!strcmp(what, "panel")) {
        x = (MUSE_CALL_BAR_X0 + MUSE_CALL_BAR_X1) / 2;
        y = (MUSE_CALL_BAR_Y0 + MUSE_CALL_BAR_Y1) / 2;
    } else if (!strcmp(what, "end")) {
        x = (MUSE_CALL_END_X0 + MUSE_CALL_END_X1) / 2;   /* End call (a first tap only brings the panel up) */
        y = (MUSE_CALL_END_Y0 + MUSE_CALL_END_Y1) / 2;
    } else if (!strcmp(what, "top")) {
        x = muse_board->width / 2 - s_ctl_x, y = muse_board->height / 6 - s_ctl_y;   /* the picture: dismisses them */
    } else {
        return false;
    }
    *g = (muse_touch_gesture_t){ s_ctl_x + x, s_ctl_y + y, s_ctl_x + x, s_ctl_y + y, MUSE_TOUCH_TAP_MS };
    return true;
}

/* ---- the calling screen, the pill and the panel ------------------------ */

/* LVGL's Montserrat builds stop at ASCII and its symbols: the ellipsis comes
 * from a one-glyph fallback converted from the same font
 * (tools/muse/gen_ellipsis_font.sh). */
LV_FONT_DECLARE(muse_font_ellipsis_20)
LV_FONT_DECLARE(muse_font_ellipsis_28)

static const lv_font_t *calling_font(bool small)
{
    static lv_font_t f20, f28;
    lv_font_t *f = small ? &f20 : &f28;
    if (!f->get_glyph_dsc) {
        *f = small ? lv_font_montserrat_20 : lv_font_montserrat_28;
        f->fallback = small ? &muse_font_ellipsis_20 : &muse_font_ellipsis_28;
    }
    return f;
}

/* The automation line's step for the BLE call: asked (the phone hasn't
 * answered: no app ready yet, or asked), connecting, live (no picture yet). */
const char *muse_call_step_name(muse_call_phase_t phase, bool app_ready)
{
    switch (phase) {
    case MUSE_CALL_ASKED: return app_ready ? "Asked the phone" : "Asked, no phone app ready yet";
    case MUSE_CALL_CONNECTING: return "The phone is connecting the call";
    case MUSE_CALL_LIVE: return "Live, waiting for the phone's picture";
    default: return "";
    }
}

#if CONFIG_MUSE_BENCH_IMAGE
#define MUSE_CALL_BENCH_IMAGE true
#else
#define MUSE_CALL_BENCH_IMAGE false
#endif

/* The step line shows on a bench/debug image, or while call.steps=1 is set. */
bool muse_call_steps_visible(bool bench_image, bool word_on)
{
    return bench_image || word_on;
}

bool muse_call_steps_word(const char *line)
{
    if (strcmp(line, "call.steps=1") && strcmp(line, "call.steps=0") && strcmp(line, "call.steps?")) {
        return false;
    }
    if (line[10] == '=') {
        s_steps_word = line[11] == '1';   /* RAM only: every boot starts with it off */
    }
    printf("@call.steps %s%s\n", s_steps_word ? "on" : "off", MUSE_CALL_BENCH_IMAGE ? " (bench image: always shown)" : "");
    fflush(stdout);
    return true;
}

/* Whether the calling screen stands: a call is asked for, connecting, or live
 * with no picture drawn yet. */
static bool calling_screen(muse_call_phase_t phase)
{
    if (phase == MUSE_CALL_NONE) {
        return false;
    }
#if CONFIG_MUSE_VIDEO
    if (muse_video_supported()) {
        /* The first picture replaces it, whatever the phase. */
        /* A video failure here keeps it up with the reason, in the
         * picture's place for the rest of the call. */
        if (muse_video_call_failure()) return true;
        return muse_video_call_waiting() && (phase != MUSE_CALL_LIVE || now_us() - s_live_us < COVER_LIVE_MAX_US);
    }
#endif
    return phase != MUSE_CALL_LIVE;   /* no video: the face is the call's picture */
}

/* The panel on the screen (LVGL running): one object that draws it straight
 * into LVGL's layer when it's drawn, the same shapes lean_overlay draws over
 * video (no child objects, no canvas). */
static void lean_draw(lv_event_t *e)
{
    shown_down_log("on the screen");
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(s_ctl, &a);
    const int ox = a.x1, oy = a.y1;
    if (now_us() < s_hint_until_us) {
        draw_rect(layer, ox + MUSE_CALL_HINT_X0, oy, ox + MUSE_CALL_HINT_X1, oy + MUSE_CALL_HINT_H, 0x000000,
                  MUSE_CALL_HINT_H / 2);
        draw_text(layer, ox + MUSE_CALL_HINT_X0, oy, ox + MUSE_CALL_HINT_X1, oy + MUSE_CALL_HINT_H,
                  &lv_font_montserrat_14, COLOR_HINT_TEXT, s_hint_text ? s_hint_text : HINT_TEXT_DEFAULT);
    }
    const int by = oy + MUSE_CALL_BTN_Y, bw = MUSE_CALL_BTN;
    draw_rect(layer, ox, by, ox + bw, by + bw, COLOR_BUTTON, LV_RADIUS_CIRCLE);
    draw_icon(layer, MUSE_ICON_REMOVE, ox + bw / 2, by + bw / 2);
    draw_rect(layer, ox + MUSE_CALL_CTL_W - bw, by, ox + MUSE_CALL_CTL_W, by + bw, COLOR_BUTTON, LV_RADIUS_CIRCLE);
    draw_icon(layer, MUSE_ICON_ADD, ox + MUSE_CALL_CTL_W - bw / 2, by + bw / 2);
    /* A dark band behind the bar so it reads over video too. */
    draw_rect(layer, ox + MUSE_CALL_BAR_X0 - 8, oy + MUSE_CALL_BAR_Y0 - 8, ox + MUSE_CALL_BAR_X1 + 8,
              oy + MUSE_CALL_BAR_Y1 + 8, 0x000000, 8);
    draw_rect(layer, ox + MUSE_CALL_BAR_X0, oy + MUSE_CALL_BAR_Y0, ox + MUSE_CALL_BAR_X1, oy + MUSE_CALL_BAR_Y1,
              COLOR_BAR_TRACK, 0);
    int fill = muse_call_bar_fill(volume_now());
    if (fill > 0) {
        draw_rect(layer, ox + MUSE_CALL_BAR_X0, oy + MUSE_CALL_BAR_Y0, ox + MUSE_CALL_BAR_X0 + fill,
                  oy + MUSE_CALL_BAR_Y1, COLOR_BAR_FILL, 0);
    }
    draw_rect(layer, ox + MUSE_CALL_END_X0, oy + MUSE_CALL_END_Y0, ox + MUSE_CALL_END_X1, oy + MUSE_CALL_END_Y1,
              COLOR_END, LV_RADIUS_CIRCLE);
    /* The call_end glyph, then "End call", centered together in the pill. */
    int tw = 0;
    for (const char *t = END_TEXT; *t; t++) {
        tw += lv_font_get_glyph_width(END_FONT, (uint32_t)*t, (uint32_t)t[1]);   /* ASCII, with kerning */
    }
    int x0 = ox + (MUSE_CALL_END_X0 + MUSE_CALL_END_X1 - (CTL_ICON_PX + END_GAP + tw)) / 2;
    draw_icon(layer, MUSE_ICON_CALL_END, x0 + CTL_ICON_PX / 2, oy + (MUSE_CALL_END_Y0 + MUSE_CALL_END_Y1) / 2);
    draw_text(layer, x0 + CTL_ICON_PX + END_GAP, oy + MUSE_CALL_END_Y0, x0 + CTL_ICON_PX + END_GAP + tw + 1,
              oy + MUSE_CALL_END_Y1, END_FONT, 0xffffff, END_TEXT);
}

static void lean_build(lv_obj_t *scr)
{
    s_ctl = lv_obj_create(scr);   /* on the screen, so it can stand over the calling screen */
    lv_obj_remove_style_all(s_ctl);
    lv_obj_set_pos(s_ctl, s_ctl_x, s_ctl_y);
    lv_obj_set_size(s_ctl, MUSE_CALL_CTL_W, MUSE_CALL_CTL_H);
    lv_obj_remove_flag(s_ctl, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ctl, lean_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(s_ctl, LV_OBJ_FLAG_HIDDEN);
}

void muse_call_build(lv_obj_t *face, int top, bool small)
{
    s_pill = lv_obj_create(face);
    lv_obj_remove_style_all(s_pill);
    lv_obj_set_size(s_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_pill, small ? 6 : 14, 0);
    lv_obj_set_style_pad_ver(s_pill, small ? 1 : 4, 0);
    lv_obj_remove_flag(s_pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_pill, LV_ALIGN_TOP_MID, 0, top);
    s_pill_lbl = lv_label_create(s_pill);
    lv_obj_set_style_text_font(s_pill_lbl, small ? &lv_font_montserrat_14 : &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_color(s_pill, lv_color_hex(COLOR_LIVE_BG), 0);
    lv_obj_set_style_text_color(s_pill_lbl, lv_color_hex(COLOR_LIVE_TEXT), 0);
    lv_label_set_text(s_pill_lbl, "");
    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);

    s_banner_box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_banner_box);
    lv_obj_set_size(s_banner_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_banner_box, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_banner_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_banner_box, lv_color_hex(0x1a1530), 0);
    lv_obj_set_style_pad_hor(s_banner_box, small ? 10 : 22, 0);
    lv_obj_set_style_pad_ver(s_banner_box, small ? 4 : 10, 0);
    lv_obj_remove_flag(s_banner_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_banner_box);
    s_banner_lbl = lv_label_create(s_banner_box);
    lv_obj_set_style_text_font(s_banner_lbl, small ? &lv_font_montserrat_14 : &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_banner_lbl, lv_color_hex(0xf2efff), 0);
    /* The long ones (the setup line) wrap, centered, inside the round panel's middle. */
    lv_obj_set_width(s_banner_lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_banner_lbl, muse_board->width * 7 / 10, 0);
    lv_label_set_long_mode(s_banner_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_banner_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_banner_lbl, "");
    lv_obj_add_flag(s_banner_box, LV_OBJ_FLAG_HIDDEN);

    /* The calling screen: over everything on the screen (the face, its rim
     * hints and page dots, the video view's black), under the controls, the
     * banners and the sleep cover on the top layer. Opaque, so LVGL draws
     * nothing under it, and clickable, so a tap reaches no button under it
     * (the touch itself comes to on_touch from the pointer). */
    lv_obj_t *scr = lv_obj_get_screen(face);
    s_calling = lv_obj_create(scr);
    lv_obj_remove_style_all(s_calling);
    lv_obj_set_size(s_calling, muse_board->width, muse_board->height);
    lv_obj_set_pos(s_calling, 0, 0);
    lv_obj_set_style_bg_color(s_calling, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_calling, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_calling, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_calling, LV_OBJ_FLAG_CLICKABLE);
    s_calling_face = lv_image_create(s_calling);   /* first: under the words */
    lv_obj_set_pos(s_calling_face, 0, 0);
    lv_obj_set_style_image_opa(s_calling_face, CALLING_FACE_OPA, 0);
    lv_obj_remove_flag(s_calling_face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_calling_face, LV_OBJ_FLAG_HIDDEN);
    s_calling_lbl = lv_label_create(s_calling);
    lv_obj_set_style_text_font(s_calling_lbl, calling_font(small), 0);
    lv_obj_set_style_text_color(s_calling_lbl, lv_color_hex(0xf2efff), 0);   /* the text color */
    lv_label_set_text(s_calling_lbl, "Calling…");
    /* The failure reasons are longer than "Calling…": wrap them, centered,
     * inside the round panel's middle. */
    lv_obj_set_width(s_calling_lbl, muse_board->width * 7 / 10);
    lv_label_set_long_mode(s_calling_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_calling_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_calling_lbl);
    s_calling_step = lv_label_create(s_calling);   /* hidden unless muse_call_steps_visible() */
    lv_obj_set_style_text_font(s_calling_step, small ? &lv_font_montserrat_14 : &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_calling_step, lv_color_hex(0xb8b2d6), 0);
    lv_obj_set_width(s_calling_step, muse_board->width * 7 / 10);
    lv_label_set_long_mode(s_calling_step, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_calling_step, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_calling_step, "");
    lv_obj_align(s_calling_step, LV_ALIGN_BOTTOM_MID, 0, -(muse_board->height / 14));   /* off the face */
    lv_obj_add_flag(s_calling_step, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_calling, LV_OBJ_FLAG_HIDDEN);

    /* The touch screen's pointer: presses anywhere, whichever object they land on. */
    if (muse_board->touch) {
        for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
            if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER) {
                s_touch = i;
                lv_indev_add_event_cb(i, on_touch, LV_EVENT_PRESSED, NULL);
                lv_indev_add_event_cb(i, on_touch, LV_EVENT_RELEASED, NULL);
                break;
            }
        }
    }

#if CONFIG_MUSE_VIDEO
    muse_video_set_overlay(video_overlay);   /* the controls (if any) and the stats overlay */
    muse_video_set_overlay_rects(video_overlay_rects);   /* where it draws (changed-only redraw) */
#endif
    /* The panel, the same pixels on every board; this panel's pixels per mm (its width in mm, else its
     * diagonal) only size the hit areas' 7 mm floor (muse_call_controls.h). */
    int w = muse_board->width, h = muse_board->height;
    float diag_px = muse_board->round ? (float)w : sqrtf((float)(w * w + h * h));
    int ppm10 = muse_board->panel_w_mm > 0 ? (int)(w * 10.0 / muse_board->panel_w_mm + 0.5)
              : muse_board->diagonal_in > 0 ? (int)(diag_px * 10.0f / (muse_board->diagonal_in * 25.4f) + 0.5f) : 0;
    if (!muse_call_panel(w, h, muse_board->round, muse_board->corner_px, ppm10, &s_P)) {
        /* No room for the panel: on a touch board the compact End call button (only it ends the call: a stray
         * tap elsewhere never does); the pill, swipes and the volume keys as before. */
        if (muse_board->touch && muse_call_end_button(w, h, muse_board->round, ppm10, &s_eb)) {
            s_end_btn = lv_obj_create(scr);   /* on the screen, so it can stand over the calling screen */
            lv_obj_remove_style_all(s_end_btn);
            lv_obj_set_pos(s_end_btn, s_eb.x, s_eb.y);
            lv_obj_set_size(s_end_btn, s_eb.w, s_eb.h);
            lv_obj_set_style_bg_color(s_end_btn, lv_color_hex(COLOR_END), 0);
            lv_obj_set_style_bg_opa(s_end_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(s_end_btn, LV_RADIUS_CIRCLE, 0);
            lv_obj_remove_flag(s_end_btn, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);   /* tap() takes it */
            lv_obj_t *t = lv_label_create(s_end_btn);
            lv_obj_set_style_text_font(t, END_FONT, 0);
            lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), 0);
            lv_label_set_text(t, END_TEXT);
            lv_obj_center(t);
            lv_obj_add_flag(s_end_btn, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "call: a compact End call button at %d,%d %dx%d (no room for the panel)", s_eb.x, s_eb.y,
                     s_eb.w, s_eb.h);
        }
        return;
    }
    s_ctl_x = s_P.x;
    s_ctl_y = s_P.y;
    lean_build(scr);
    ESP_LOGI(TAG, "call controls panel at %d,%d (%d.%d px/mm): hit rows - / + up %d, End call down %d (7 mm = %d px)",
             s_ctl_x, s_ctl_y, ppm10 / 10, ppm10 % 10, s_P.btn_up, s_P.end_down, s_P.min_px);
}

bool muse_call_tick(void)
{
    if (!s_pill) {
        return false;
    }
    if (s_touch && s_press.down && face_touch_ok() && lv_indev_get_state(s_touch) == LV_INDEV_STATE_PRESSED) {
        lv_point_t p;
        lv_indev_get_point(s_touch, &p);
        press_move(p.x, p.y);   /* LVGL tells the pointer only press and release, not the moves */
    }
    int secs;
    muse_call_phase_t phase = muse_call_phase(&secs);
    bool up = phase != MUSE_CALL_NONE;

    /* The end banner: "Call ended" 1.5 s, the problems 3 s. During a
     * live call whose pictures stopped, Muse animates in their place and,
     * after 5 s, this line joins him at the bottom. */
    const char *banner = s_banner;
    bool show_banner = banner && !up && now_us() < s_banner_until_us;
    static bool line_pos;   /* the box sits low for the line, centered for the banners */
    bool line = false;
#if CONFIG_MUSE_VIDEO
    if (up && phase == MUSE_CALL_LIVE && muse_video_supported()) {
        int64_t since = muse_video_call_stalled_since();
        line = since && now_us() - since >= NO_PICTURE_US;
    }
#endif
    if (line) {
        banner = "No picture from the phone";
        show_banner = true;
    }
    if (line != line_pos) {
        line_pos = line;
        if (line) {
            lv_obj_align(s_banner_box, LV_ALIGN_BOTTOM_MID, 0, -muse_board->height / 6);
        } else {
            lv_obj_center(s_banner_box);
        }
    }
    if (show_banner && strcmp(lv_label_get_text(s_banner_lbl), banner) != 0) {
        lv_label_set_text(s_banner_lbl, banner);
    }
    if (lv_obj_has_flag(s_banner_box, LV_OBJ_FLAG_HIDDEN) == show_banner) {
        lv_obj_set_flag(s_banner_box, LV_OBJ_FLAG_HIDDEN, !show_banner);
    }

    /* In a call the face stays: no sliding off to settings. */
    static bool was_up;
    if (up != was_up) {
        was_up = up;
        if (up) {
            muse_ui_show_face();
        }
        muse_ui_set_swipe_enabled(!up);
    }

    /* The calling screen: "Calling…" on black until the first picture;
     * "Waiting for phone…" while no app is ready to take the call. */
    bool calling = calling_screen(phase);
    if (calling) {
        const char *text = phase == MUSE_CALL_ASKED && !muse_call_available() ? "Waiting for phone…"
                                                                              : "Calling…";
#if CONFIG_MUSE_VIDEO
        /* Video that fails says so, never a blank screen. */
        const char *fail = muse_video_supported() ? muse_video_call_failure() : NULL;
        if (fail) {
            text = fail;
        } else if (phase == MUSE_CALL_LIVE && muse_video_supported() && now_us() - s_live_us >= NO_PICTURE_US) {
            text = "No picture from the phone";
        }
#endif
        if (strcmp(lv_label_get_text(s_calling_lbl), text) != 0) {
            lv_label_set_text(s_calling_lbl, text);
        }
        /* The avatar's plain picture behind it at 40 %, when the avatar face
         * is picked and stored (mg_face.c hands it over; none: black). */
        static uint32_t face_ver = UINT32_MAX;
        uint32_t ver = 0;
        const void *face_img = muse_ui_face_image(&ver);
        if (ver != face_ver) {
            face_ver = ver;
            lv_image_set_src(s_calling_face, NULL);
            if (face_img) {
                lv_image_set_src(s_calling_face, face_img);
            }
        }
        if (lv_obj_has_flag(s_calling_face, LV_OBJ_FLAG_HIDDEN) == (face_img != NULL)) {
            lv_obj_set_flag(s_calling_face, LV_OBJ_FLAG_HIDDEN, !face_img);
        }
        /* Bench/debug images, or call.steps=1: the BLE call start's step and the time since the tap. */
        bool steps = muse_call_steps_visible(MUSE_CALL_BENCH_IMAGE, s_steps_word);
        if (s_calling_step && lv_obj_has_flag(s_calling_step, LV_OBJ_FLAG_HIDDEN) == steps) {
            lv_obj_set_flag(s_calling_step, LV_OBJ_FLAG_HIDDEN, !steps);
        }
        if (s_calling_step && steps) {
            char step[80];
            int64_t tap = s_tap_us;
            uint32_t ms = tap ? (uint32_t)((now_us() - tap) / 1000) : 0;
            snprintf(step, sizeof(step), "%s\n%lu.%lu s", muse_call_step_name(phase, muse_call_available()),
                     (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 100));
            /* Above the "Bench" mark while it shows (the page dots' place, muse_home.c). */
            static int lifted = -1;
            int lift = muse_board->height / 14, bx, by;
            const lv_font_t *bf;
            uint32_t brgb;
            if (muse_home_bench_mark(&bx, &by, &bf, &brgb) && muse_board->height - by + 2 > lift) {
                lift = muse_board->height - by + 2;
            }
            if (strcmp(lv_label_get_text(s_calling_step), step) != 0 || lift != lifted) {
                lv_label_set_text(s_calling_step, step);
                lv_obj_align(s_calling_step, LV_ALIGN_BOTTOM_MID, 0, -lift);
                lifted = lift;
            }
        }
    }
    if (calling != s_calling_up) {
        lv_obj_set_flag(s_calling, LV_OBJ_FLAG_HIDDEN, !calling);
        if (calling) {
            muse_menu_close();   /* the menu's Live call row started it: over everything */
            lv_obj_move_foreground(s_calling);
            if (s_ctl) {
                lv_obj_move_foreground(s_ctl);
            }
        } else if (phase == MUSE_CALL_LIVE) {
            bool picture = false;
#if CONFIG_MUSE_VIDEO
            picture = muse_video_supported() && !muse_video_call_waiting();
#endif
            ESP_LOGI(TAG, "calling screen down: %s", picture ? "first picture" : "live, no picture");
        }
        s_calling_up = calling;
    }

    ctl_sync();

    /* The pill only once the call is live on the face (no video, or a phone
     * that sends none): before that the calling screen says it all. */
    bool pill = up && !calling && phase == MUSE_CALL_LIVE;
    if (lv_obj_has_flag(s_pill, LV_OBJ_FLAG_HIDDEN) == pill) {
        lv_obj_set_flag(s_pill, LV_OBJ_FLAG_HIDDEN, !pill);
    }
    if (!up) {
        return false;
    }
    if (pill) {
        char text[40];
        snprintf(text, sizeof(text), LV_SYMBOL_CALL " Live %d:%02d", secs / 60, secs % 60);
        if (strcmp(lv_label_get_text(s_pill_lbl), text) != 0) {
            lv_label_set_text(s_pill_lbl, text);
        }
    }
    return true;
}
