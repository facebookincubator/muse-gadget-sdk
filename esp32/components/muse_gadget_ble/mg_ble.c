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

#include "mg_ble.h"
#include "mg_linkpol.h"
#include "mg_smp_gate.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "secure_random.h"
#include "mg_unlock.h"
#include "mg_voice.h"
#include "muse_restart.h"
#include "muse_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "muse_wdt.h"
#if CONFIG_MUSE_GADGET_BLE_SMP
#include "mg_fifo.h"
#include "mg_smp_esp.h"
#include "mg_smp_front.h"
#include "muse_bootguard.h"
#include "muse_local_press.h"
#endif
#include "host/ble_gap.h"
#include "mg_link_close.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "mg_nus.h"
#include "host/ble_l2cap.h"
#include "host/ble_uuid.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "mg_ble_priv.h"
#include "mg_haptic_test_core.h"   /* the haptics core (mg_haptics.h) and the vibration tests */
#include "mg_codec.h"
#include "mg_chain.h"   /* the board's speech chain on the player's output */
#include "mg_play.h"
#include "mg_config.h"
#include "mg_ui.h"
#include "mgcommands.h"

/* The sealed session needs ATT_MTU >= MG_MIN_ATT_MTU (its one-write messages, credentials_saved among them);
 * a device that can't reach it doesn't declare mg_session_cap_sealed. Every
 * build here declares it, so every build must ask for at least that much (a board lowering it fails here). */
_Static_assert(CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU >= MG_MIN_ATT_MTU,
               "musegadgets' sealed session needs ATT MTU >= MG_MIN_ATT_MTU (100)");
#if CONFIG_MUSE_GADGET_BLE_HID
#include "mg_hogp.h"
#endif

#if CONFIG_MUSE_GADGET_BLE_SESSION && !CONFIG_MUSE_GADGET_BLE_HID
/* The bond store kept in NVS (CONFIG_BT_NIMBLE_NVS_PERSIST); NimBLE doesn't declare it. */
void ble_store_config_init(void);
#endif

static const char *TAG = "mg.ble";

/* No person in the loop: the same expression as muse_input.h's MUSE_NO_PERSON (this component can't include it;
 * test_mg_bench_input checks they match). The person gates use it: the press a client's start_mic waits for, and
 * the update's confirm press (SMP_RELEASE). */
#if (defined(CONFIG_MUSE_GADGET_TEST_BUILD) && CONFIG_MUSE_GADGET_TEST_BUILD) \
    || (defined(CONFIG_MUSE_GADGET_AUTOMATION) && CONFIG_MUSE_GADGET_AUTOMATION)
#define MG_NO_PERSON 1
#else
#define MG_NO_PERSON 0
#endif
static const char *PLAY_TAG = "mg.play";   /* on the Nordic UART mirror (tag prefix "mg") */

#define NVS_NS "mg"
#define NVS_SETTINGS "settings"
#define SETTINGS_VERSION 2   /* 2: + capture_sample_rate (13), capture_bitrate_kbps (14); 1 read too */
#define QUEUE_PART_SUBTYPE 0x4D   /* partitions_muse.csv: mg_queue */
/* The token proof's HMACs (PSA) run on the worker. */
#if CONFIG_MUSE_GADGET_BLE_SMP
/* ... and an SMP upload's esp_ota_end(), which checks the image's RSA signature. */
#define WORKER_STACK 8192
#else
#define WORKER_STACK 6144
#endif
#define PLAYER_STACK 4096
#define PLAY_CHUNK (MG_PLAY_OUT_RATE * (MG_PLAY_CHUNK_US / 1000) / 1000)   /* 10 ms at 16 kHz: what the player writes at a time */
#define PLAY_TAIL_CHUNKS 5                /* silence after a stream, so the DMA holds no stale audio */
#define PLAY_FLUSH_FADE (MG_PLAY_OUT_RATE / 200)   /* a flush fades the FIFO's first 5 ms out, then silence */
#define NUS_RING 2048
#define NUS_LINE 160

#define STR_(x) #x
#define STR(x) STR_(x)

/* ---- state ---------------------------------------------------------------- */

static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(MG_SERVICE_UUID_LE_BYTES);
static const ble_uuid128_t CONTROL_UUID = BLE_UUID128_INIT(MG_CONTROL_UUID_LE_BYTES);
static const ble_uuid128_t DATA_UUID = BLE_UUID128_INIT(MG_DATA_UUID_LE_BYTES);
static const ble_uuid128_t NUS_UUID = BLE_UUID128_INIT(MG_NUS_SERVICE_UUID_LE_BYTES);
static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(MG_NUS_RX_UUID_LE_BYTES);
static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(MG_NUS_TX_UUID_LE_BYTES);
static const uint8_t ADV_UUID[16] = { MG_SERVICE_UUID_LE_BYTES };

static mg_ble_platform_t s_platform;
static bool s_ready;
static SemaphoreHandle_t s_lock;
/* Held from a credentials_update's take to its store (session_store_pending), and by a setup change, so
 * an old session's pair is never stored after the setup that ended it (mg_ble_setup_changed). */
static SemaphoreHandle_t s_store_lock;
static QueueHandle_t s_events;
static TaskHandle_t s_worker;
static TaskHandle_t s_player;
static mg_proto_t s_proto;
static mg_play_t s_play;
/* The client's state (mg_link.h), re-read after anything that can change it. */
static volatile mg_link_t s_link = MG_LINK_NEVER;
/* The console's bench words: !PAIR AUTO, !RESTART (mg_unlock.h). No owner
 * unlock: open in automation builds, a proven connection in release builds. */
static mg_unlock_t s_unlock;

/*
 * Planned restarts (fast start): the link is closed cleanly first
 * (LL terminate), so the phone sees the disconnect at once instead of after
 * its supervision timeout, then the restart follows the disconnect event
 * (300 ms at most). restart_after(ms, why) waits ms first.
 */
static esp_timer_handle_t s_restart_timer;
static volatile int s_restart_why;
static volatile bool s_restart_on_disc, s_restart_closing;

static void restart_fire(void *arg);

#if CONFIG_MUSE_GADGET_BLE_SMP
static bool s_update_ui_attached;   /* a screen draws the update card */
static int64_t s_restart_hold_until;
#endif

static void restart_after(uint32_t ms, int why)
{
#if CONFIG_MUSE_GADGET_BLE_SMP
    s_restart_hold_until = esp_timer_get_time() + 500 * 1000;   /* the longest wait for "Installing" */
#endif
    if (!s_restart_timer) {
        const esp_timer_create_args_t a = { .callback = restart_fire, .name = "mg_restart" };
        if (esp_timer_create(&a, &s_restart_timer) != ESP_OK) {
            muse_restart((muse_restart_why_t)why);
        }
    }
    s_restart_why = why;
    s_restart_closing = false;
    esp_timer_stop(s_restart_timer);
    esp_timer_start_once(s_restart_timer, (uint64_t)(ms ? ms : 1) * 1000);
}
static volatile bool s_set_up;   /* the gadget has been set up: Link setup left a proof key, or its marker */
static volatile bool s_smp_forgot;   /* Forget erased the token: no proof opens a firmware update until the restart */
static mg_capture_req_t s_capture;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_handle[MG_CH_COUNT];
static uint16_t s_bas_handle, s_nus_tx_handle;
/* The log mirror's owner gate (nus_owner_refresh): a release build of a set-up gadget mirrors only to the
 * proven phone. */
#if CONFIG_MUSE_GADGET_AUTOMATION || CONFIG_MUSE_GADGET_TEST_BUILD
#define NUS_MIRROR_RELEASE false
#else
#define NUS_MIRROR_RELEASE true
#endif
static volatile bool s_nus_owner = !NUS_MIRROR_RELEASE;
/* Identity and status stay open: while the Nordic UART answers status, version, guard? or ota?, the
 * lines its handler logs (on this task only) go to the requesting connection, proven or not, in SAFE too. */
static TaskHandle_t volatile s_nus_reply_task;
static portMUX_TYPE s_nus_mux = portMUX_INITIALIZER_UNLOCKED;
/* The Nordic UART log mirror (its ring, filled by the logger, drained by the worker). */
static char *s_nus_ring;
static size_t s_nus_head, s_nus_len;
static uint32_t s_nus_gen;                 /* +1 each time the ring is emptied (a new link): a drain's copy is stale */
static void nus_ring_reset(void);
static volatile uint8_t s_nus_inflight;    /* NUS notifies in the stack (mg_nus.h: at most MG_NUS_INFLIGHT_MAX) */
static volatile uint32_t s_nus_dropped;    /* log lines with no room in the ring (the status line's nus_dropped) */
static volatile uint32_t s_host_closed;    /* links NimBLE closed itself (the status line's host_closed, mg_link_close.h) */
static volatile int64_t s_last_rx_us;      /* the peer's last write to any of our characteristics (esp_timer) */
static volatile bool s_bas_sub, s_nus_sub;
static int s_bas_last = -1;

static muse_vstream_t s_vstream;
#if CONFIG_MUSE_GADGET_BLE_VIDEO
static mg_video_caps_t s_video_caps;   /* what stream_video's capabilities report (mg_ble_init) */
#endif
#define MG_VIDEO_L2CAP_PSM 0x0081   /* LE dynamic range */
#if CONFIG_MUSE_GADGET_BLE_QUEUE
static mg_queue_t s_queue;
static const esp_partition_t *s_queue_part;
static uint32_t s_forget_ms;   /* the last mg_ble_queue_forget (mg_now_ms), once s_forgot */
static bool s_forgot;
#endif
static bool s_have_queue;

typedef enum {
    EV_WRITE,
    EV_CLIP,
    EV_KICK,
    EV_RUN,     /* a bench word's work (mg_ble_run_on_worker) */
} ev_type_t;

typedef struct {
    ev_type_t type;
    uint8_t ch;
    uint8_t codec;
    uint32_t len;
    uint32_t start_ms;
    uint32_t conn_gen;   /* EV_WRITE: the connection it came on (s_conn_gen) */
    union {
        uint64_t start_unix_ms;
        void (*run)(void *arg);   /* EV_RUN: called with data, which it owns */
    };
    uint8_t *data;
} ev_t;

bool mg_active(void)
{
    return s_ready;
}

void mg_lock(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void mg_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

mg_proto_t *mg_proto_get(void)
{
    return &s_proto;
}

mg_capture_req_t *mg_capture_req(void)
{
    return &s_capture;
}

uint32_t mg_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

mg_queue_t *mg_queue_get(void)
{
#if CONFIG_MUSE_GADGET_BLE_QUEUE
    return s_have_queue ? &s_queue : NULL;
#else
    return NULL;
#endif
}

void mg_play_kick(void)
{
    if (s_player) {
        xTaskNotifyGive(s_player);
    }
}

/* ---- haptics (the haptics core in haptics/) ----------------------------- */

static mgh_t s_mgh;
static mgh_caps_t s_mgh_caps;            /* the core keeps it by pointer */
static bool s_mgh_on;                    /* the core runs (the board gave an actuator) */
static bool s_motor;                     /* what the motor was last told */
static esp_timer_handle_t s_mgh_timer;

static bool haptics_core(void)
{
    return s_mgh_on && s_platform.haptic_actuator && s_platform.haptic_motor;
}

/* The states the core asks for at every request (mgh_env_t). */
static void haptic_env(mgh_env_t *e, bool own)
{
    memset(e, 0, sizeof(*e));
    /* A client's pattern follows its connection's haptics_enabled; the
     * device's own cues follow it too while a client is connected, else the
     * device's own setting. */
    e->enabled = s_proto.connected || !own ? mg_proto_haptics(&s_proto)
                                           : (s_platform.haptics_on ? s_platform.haptics_on() : 1);
#if CONFIG_MUSE_GADGET_BLE_SMP
    uint8_t pct;
    mg_smp_ui_t ui = mg_smp_esp_ui(&pct);
    e->updating = ui == MG_SMP_UI_WRITING || ui == MG_SMP_UI_INSTALLING;
#endif
    mg_battery_status_t b;
    if (s_platform.battery_status && s_platform.battery_status(&b) && (b.flags & mg_battery_status_flag_present) &&
        !(b.flags & mg_battery_status_flag_charging)) {
        e->low_battery = b.soc_percent <= 10;
    }
    e->asleep = 0;   /* the esp_timer and the motor's expander both run in light sleep */
    e->in_turn = s_proto.turn;
}

static void haptic_send(const mgh_status_t *s, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        mg_proto_haptic_status(&s_proto, s[i].id, s[i].status, s[i].reason);
    }
}

/* After every core call (under mg_lock): the motor follows mgh_motor(), and
 * the timer comes back after the next_ms the core asked for. */
static void haptic_drive(uint32_t next_ms)
{
    static uint16_t s_played;
    static uint8_t s_step;
    bool on = mgh_motor(&s_mgh);
    /* A new on step while the motor runs (a pattern replacing one mid-step) renews the board's own stop too. */
    bool renew = on && s_motor && (s_mgh.played != s_played || s_mgh.step != s_step);
    if (on != s_motor || renew) {
        s_motor = on;
        s_platform.haptic_motor(on, MG_HAPTIC_MAX_ON_MS);   /* the board's own stop if a tick were ever late */
    }
    s_played = s_mgh.played;
    s_step = s_mgh.step;
    esp_timer_stop(s_mgh_timer);
    if (s_mgh.active) {
        esp_timer_start_once(s_mgh_timer, (uint64_t)(next_ms ? next_ms : 1) * 1000);
    }
}

/* The steps that ended by `now` accounted, before anything else reads or rolls the duty buckets at `now`: the core
 * clears its whole duty history if a step's end is added in a bucket older than the newest. */
static void haptic_settle(uint32_t now)
{
    if (s_mgh.active) {
        uint32_t next = 0;
        mgh_status_t s[1];
        haptic_send(s, mgh_tick(&s_mgh, now, s, &next));
    }
}

static void haptic_tick_locked(void)
{
    uint32_t now = (uint32_t)mg_now_ms();
    mgh_env_t e;
    haptic_env(&e, s_mgh.src != MGH_SRC_CLIENT);
    mgh_status_t s[1];
    haptic_settle(now);
    if (s_mgh.active && (e.updating || e.low_battery)) {
        haptic_send(s, mgh_interrupt(&s_mgh, e.updating ? mg_haptic_reason_updating : mg_haptic_reason_low_battery,
                                     now, s));
    }
    uint32_t next = 0;
    haptic_send(s, mgh_tick(&s_mgh, now, s, &next));
    haptic_drive(next);
}

/* The next step is due: the worker runs it (haptic_poll), as the component's other timers do, so a motor
 * write (an I/O expander, I2C retries) or a wait on the lock never holds up the esp_timer task. */
static volatile bool s_mgh_due;

static void kick(void);

static void haptic_timer(void *arg)
{
    (void)arg;
    s_mgh_due = true;
    kick();
}

static void haptic_poll(void)
{
    if (s_mgh_due) {
        s_mgh_due = false;
        mg_lock();
        haptic_tick_locked();
        mg_unlock();
    }
}

/* haptic_play / haptic_stop from the client (mg_proto, under mg_lock). */
static uint16_t op_haptic(void *ctx, const uint8_t *a, size_t n)
{
    (void)ctx;
    uint32_t now = (uint32_t)mg_now_ms();
    haptic_settle(now);
    if (a[0] == mg_device_action_haptic_stop) {
        uint16_t err = mgh_parse_stop(a + 1, (unsigned)(n - 1));
        if (err) {
            return err;
        }
        mgh_status_t s[1];
        haptic_send(s, mgh_stop(&s_mgh, now, s));
    } else {
        mgh_req_t r;
        uint16_t err = mgh_parse_play(s_mgh.caps, a + 1, (unsigned)(n - 1), &r);
        if (err) {
            return err;
        }
        mgh_env_t e;
        haptic_env(&e, false);
        mgh_status_t s[2];
        haptic_send(s, mgh_play(&s_mgh, &r, MGH_SRC_CLIENT, &e, now, s));
    }
    uint32_t next = 0;
    mgh_status_t s[1];
    haptic_send(s, mgh_tick(&s_mgh, now, s, &next));
    haptic_drive(next);
    return 0;
}

bool mg_haptic_cue(uint8_t src, uint16_t ms)
{
    if (!haptics_core()) {
        return false;
    }
    mgh_req_t r = { .repeat = 1, .count = 1 };
    r.steps[0] = ms;
    mg_lock();
    uint32_t now = (uint32_t)mg_now_ms();
    haptic_settle(now);
    mgh_env_t e;
    haptic_env(&e, true);
    e.in_turn = 0;   /* the talk buzz is the turn's own cue */
    mgh_status_t s[2];
    haptic_send(s, mgh_play(&s_mgh, &r, src, &e, now, s));
    uint32_t next = 0;
    mgh_status_t t[1];
    haptic_send(t, mgh_tick(&s_mgh, now, t, &next));
    haptic_drive(next);
    mg_unlock();
    return true;
}

bool mg_ble_haptic_talk_cue(uint16_t ms)
{
    return s_ready && mg_haptic_cue(MGH_SRC_TALK, ms);
}

void mg_haptic_interrupt(uint8_t reason)
{
    if (!haptics_core()) {
        return;
    }
    mg_lock();
    uint32_t now = (uint32_t)mg_now_ms();
    haptic_settle(now);
    if (s_mgh.active && s_mgh.src == MGH_SRC_CLIENT) {
        mgh_status_t s[1];
        haptic_send(s, mgh_interrupt(&s_mgh, reason, now, s));
        haptic_drive(0);
    }
    mg_unlock();
}

/* ---- vibration tests (mg_haptic_test.h) ---------------------------------
 * Developer > Tests > Vibration and the console's vib words: the same core,
 * gates, timer and motor as a client's pattern, as MGH_SRC_TEST (skips
 * haptics_enabled only). Called from the LVGL task (the page) and the input
 * task (console words, keys); never from a timer of its own. */

static mght_t s_mght;

bool mg_ble_haptic_test_caps(mght_caps_t *c)
{
    memset(c, 0, sizeof(*c));
    if (!haptics_core()) {
        return false;
    }
    mg_lock();
    mght_caps(&s_mght, c);
    c->default_strength = s_proto.settings.buzz_volume;   /* ptt_buzz_volume_percent: the page's starting strength */
    mg_unlock();
    return true;
}

static void haptic_test_after(const mgh_status_t *s, unsigned n, uint32_t now)
{
    haptic_send(s, n);   /* a client's pattern a test cut: (interrupted, in_use) */
    uint32_t next = 0;
    mgh_status_t t[1];
    haptic_send(t, mgh_tick(&s_mgh, now, t, &next));
    haptic_drive(next);
}

mght_result_t mg_ble_haptic_test(const mght_cmd_t *c)
{
    if (!haptics_core()) {
        return MGHT_INVALID;
    }
    mg_lock();
    uint32_t now = (uint32_t)mg_now_ms();
    haptic_settle(now);
    mgh_env_t e;
    haptic_env(&e, true);   /* the update, battery and talk-turn gates; a test skips haptics_enabled (mgh_play) */
    mgh_status_t s[2];
    unsigned n;
    mght_result_t r = mght_run(&s_mght, c, &e, now, s, &n);
    haptic_test_after(s, n, now);
    mg_unlock();
    return r;
}

void mg_ble_haptic_test_status(mght_status_t *st)
{
    memset(st, 0, sizeof(*st));
    if (!haptics_core()) {
        return;
    }
    mg_lock();
    uint32_t now = (uint32_t)mg_now_ms();
    mgh_status_t s[1];
    uint32_t next = 0;
    if (s_mgh.active) {
        haptic_send(s, mgh_tick(&s_mgh, now, s, &next));   /* settle a step the timer is about to end */
        haptic_drive(next);
    }
    mght_status(&s_mght, now, st);
    mg_unlock();
}

bool mg_ble_haptic_test_key(void)
{
    if (!haptics_core()) {
        return false;
    }
    mg_lock();
    uint32_t now = (uint32_t)mg_now_ms();
    bool stopped = mght_stop(&s_mght, now);
    if (stopped) {
        haptic_test_after(NULL, 0, now);
        ESP_LOGI("mg.bench", "vib: stopped by a key");
    }
    mg_unlock();
    return stopped;
}

bool mg_ble_haptic_busy(void)
{
    return haptics_core() && s_mgh.active;   /* one byte, read without the lock: a sleep check's hint */
}

void mg_ble_haptic_console(const char *line, char *out, size_t n)
{
    if (!haptics_core()) {
        snprintf(out, n, "vib: no vibration motor on this board");
        return;
    }
    mg_lock();
    uint32_t now = (uint32_t)mg_now_ms();
    mgh_env_t e;
    haptic_env(&e, true);
    mgh_status_t s[2];
    unsigned k;
    mght_console(&s_mght, line, &e, now, s, &k, out, n);
    haptic_test_after(s, k, now);
    mg_unlock();
}

static void haptic_init(void)
{
    if (!s_platform.haptic_actuator || !s_platform.haptic_motor) {
        return;
    }
    const uint8_t *a = s_platform.haptic_actuator;
    s_mgh_caps = (mgh_caps_t){ .kind = a[0], .flags = a[1], .min_on_ms = a[2], .strength_steps = a[3],
                               .max_on_ms = MG_HAPTIC_MAX_ON_MS };
    mgh_init(&s_mgh, &s_mgh_caps);
    mght_init(&s_mght, &s_mgh, 0);
    const esp_timer_create_args_t t = { .callback = haptic_timer, .name = "mg_haptic" };
    s_mgh_on = esp_timer_create(&t, &s_mgh_timer) == ESP_OK;
}

bool mg_vibrate(uint16_t freq_hz, uint16_t ms, uint8_t volume_pct)
{
    if (haptics_core()) {
        (void)freq_hz;   /* an ERM has one speed; strength per the actuator's flags */
        (void)volume_pct;
        return mg_haptic_cue(MGH_SRC_TALK, ms);
    }
    if (!s_platform.vibrate) {
        return false;
    }
    s_platform.vibrate(freq_hz, ms, volume_pct);
    return true;
}

static void link_turn_end(bool error);

static void op_assistant_state(void *ctx, uint8_t state)
{
    (void)ctx;
    if (state == mg_assistant_state_done || state == mg_assistant_state_error) {
        link_turn_end(state == mg_assistant_state_error);   /* the gadget's turn is over */
    }
    s_platform.assistant_state(state);
}

static void op_play_kick(void *ctx)
{
    (void)ctx;
    mg_play_kick();
    if (s_platform.attention) {
        s_platform.attention();   /* keeps the codecs powered for the stream */
    }
}

/*
 * Link modes (mgcommands.h "Link modes", mg_linkpol.h):
 * sleep 100/160/4/600, ready 24/36/5/400, active 12/12/0/400. Ready at
 * connect (after the central's first update or 1 s); active on the device's
 * own activity (its talk press at touch-down, a
 * stream the app starts, push-to-talk capture, Link setup, an MCUmgr update)
 * and back to the app's mode (setting link_mode 16) 5 s after it ends; sleep
 * by itself after 10 min in ready; a user's wake or touch (a key, a touch,
 * the screen lit by the user, bench input alike) moves sleep to ready and
 * restarts the 10 min (mg_ble_link_user); the app's active expires after 30 s
 * without activity. The gadget's own turn (mgcommands.h) is
 * activity from its talk press the client heard (mg_ble_link_turn) until its
 * reply has played, assistant_state done or error, or 20 s with no reply.
 * Each effective change is logged on mg.ble and notified (link_mode); each
 * grant is logged and readable as link_params 17. A refused request is
 * asked once more after 5 s; one refused for an LL procedure collision
 * (0x2A, status 554) after 300 ms; one with no answer at all in 2 s is asked
 * again then, BLE_HS_EALREADY (NimBLE still holding it, up to 40 s)
 * meaning again 2 s later, for at most 45 s a mode. A video stream
 * starting while active toggles idle -> fast: the idle half
 * 24/36/0/400, then, once its connection update event came (granted or
 * refused), the fast half no sooner than 500 ms after it and 2 s after the
 * idle half; no answer to the idle half in 2 s skips the toggle. At most one
 * request per 2 s, none while one is unanswered, none within 500 ms of a
 * connection update. 2M PHY and 251-byte packets with the first request.
 * The worker runs it (link_policy_check); a one-shot timer (s_lp_timer)
 * wakes the worker when the policy's next round falls due, so the NimBLE
 * host task only records the update event and kicks.
 */
_Static_assert((int)MG_LINKPOL_SLEEP == (int)mg_link_mode_sleep && (int)MG_LINKPOL_READY == (int)mg_link_mode_ready &&
                   (int)MG_LINKPOL_ACTIVE == (int)mg_link_mode_active,
               "mg_linkpol's modes are mgcommands.h's mg_link_mode_t");
static mg_linkpol_t s_linkpol;
static bool s_link_phy_asked;           /* 2M + DLE asked on this connection */
static uint16_t s_conn_itvl;            /* the interval in effect, 1.25 ms units */
static uint32_t s_conn_gen;             /* connections so far: a queued write's */
static uint16_t s_conn_latency;         /* the latency in effect */
static esp_timer_handle_t s_lp_timer;   /* wakes the worker when the policy's next request is due */
#if CONFIG_MUSE_GADGET_BLE_VIDEO
static uint16_t s_video_ce = 0;      /* max connection event length, 0.625 ms units (0: the controller's) */
/*
 * A UI stream sends frames only when the picture changes: it counts as
 * active while frames come (each Data write) and for UI_TOUCH_FAST_US after
 * a touch (the tap's answer is on its way), not while the picture is still.
 * A camera stream is always active.
 */
#define UI_TOUCH_FAST_US (6 * 1000 * 1000)
#define UI_FRAME_ACTIVE_US (1000 * 1000)   /* a UI frame keeps it busy this long */
static volatile bool s_ui_stream;    /* the stream that's on is a UI stream */
static volatile int64_t s_last_data_us;   /* the last Data write (stream bytes) */
static volatile int64_t s_last_touch_us;
#endif

static void lp_timer_fire(void *arg)
{
    (void)arg;
    kick();   /* the worker's next round runs the policy */
}

/* The policy's next round at at_us (0: none). The worker; any task may stop it. */
static void link_wake_at(int64_t at_us)
{
    if (!s_lp_timer) {
        const esp_timer_create_args_t a = { .callback = lp_timer_fire, .name = "mg_linkpol" };
        if (esp_timer_create(&a, &s_lp_timer) != ESP_OK) {
            s_lp_timer = NULL;
            return;   /* the worker's own 1 s rounds still run it, just later */
        }
    }
    esp_timer_stop(s_lp_timer);   /* not running: ESP_ERR_INVALID_STATE, fine */
    if (at_us) {
        int64_t d = at_us - esp_timer_get_time();
        esp_timer_start_once(s_lp_timer, (uint64_t)(d > 0 ? d : 1));
    }
}

static int link_request(const mg_linkpol_params_t *w)
{
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return BLE_HS_ENOTCONN;
    }
    if (!s_link_phy_asked) {
        s_link_phy_asked = true;
        ble_gap_set_prefered_le_phy(conn, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_CODED_ANY);
        ble_gap_set_data_len(conn, 251, 2120);
    }
    struct ble_gap_upd_params up = {
        .itvl_min = w->itvl_min,   /* fast 12 = 12: Apple grants 15 ms only for exactly that */
        .itvl_max = w->itvl_max,
        .latency = w->latency,
        .supervision_timeout = w->timeout,
#if CONFIG_MUSE_GADGET_BLE_VIDEO
        .min_ce_len = s_video_ce,
        .max_ce_len = s_video_ce,
#endif
    };
    int rc = ble_gap_update_params(conn, &up);
    ESP_LOGI(TAG, "link: asked %u-%u x1.25 ms, latency %u, timeout %u ms (%s: %s) rc=%d", w->itvl_min,
             w->itvl_max, w->latency, w->timeout * 10, mg_linkpol_mode_name(s_linkpol.mode),
             s_linkpol.req_why ? s_linkpol.req_why : "-", rc);
    return rc;
}

/*
 * Listening at every event during a switch (mg_linkpol.h): from each request until its set applies, a refusal, the
 * stack refusing to send it, the link's loss or 20 s, the peripheral ignores
 * its slave latency, so the central's answer is heard at the next event. The
 * policy opens and closes the window (mg_linkpol_wake_sync); the mechanism is
 * the controller's:
 * - ESP32-S3 and C3, experimental (Espressif's RW-based "BTDM" controller,
 *   libbtdm_app.a, checked in ESP-IDF v6.0.3):
 *   the undocumented vendor command 0xFC13, Set Preferred Slave Latency
 *   (parameters connection handle, latency; returns status, handle; in the
 *   controller's hci_cmd_desc_tab_vs and llc_hci_command_handler_tab, handler
 *   hci_vs_set_pref_slave_latency_cmd_handler -> lld_con_pref_slave_latency_set,
 *   also in the S3 ROM at 0x40002bec / 0x400042c0). It sets the latency in use
 *   to min(in use, preferred): 0 wakes at every event. It can't raise it
 *   again: the controller reloads the granted latency only at a connection
 *   update instant (lld_con_sched) or a new connection. So a grant restores it
 *   by itself, but after a refusal, the stack refusing to send, 20 s or
 *   link.wake=0 it keeps listening at every event until the next connection
 *   update (logged; the README's Link modes says what that costs).
 * - The others (C5, C6, C61, H2, the S31's controller): no override (no
 *   vendor command, no API; the C6's BT_LE_CTRL_SLV_FAST_RX_CONN_DATA_EN, on
 *   in its overlay, is build-time, for the whole controller, and only after
 *   a received data PDU). A peripheral with data queued transmits at every event whatever its
 *   latency, so a 1-byte notification is queued every half interval for the
 *   window (mg_linkpol_wake_period_us): the Battery Level's current value if
 *   the client subscribed to it (a valid, idempotent report), else an empty
 *   Nordic UART TX notification if subscribed, else nothing (logged once a
 *   window). Never a notification the client didn't subscribe to. Also the
 *   S3/C3's way if the vendor command is refused.
 * The worker and the GAP handler run it (link_wake_sync); the bench switch is
 * link.wake=0|1 (mg_ble_link_wake_word).
 */
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3
#define LINK_WAKE_VSC 1
#define MG_VS_SET_PREF_SLAVE_LATENCY 0xFC13   /* OGF 0x3F, OCF 0x013 */
/* NimBLE's own HCI command call (ble_hs_hci_priv.h; ble_hs_hci_send_vs_cmd needs BT_NIMBLE_VS_SUPPORT). */
int ble_hs_hci_cmd_tx(uint16_t opcode, const void *cmd, uint8_t cmd_len, void *rsp, uint8_t rsp_len);
#else
#define LINK_WAKE_VSC 0
#endif
static int battery_level(void);
static mg_linkpol_wake_t s_wake;           /* the override as the worker left it */
static esp_timer_handle_t s_wake_timer;    /* the pending notification's period (no controller override) */
static volatile bool s_wake_tx_due;
static uint32_t s_wake_sent, s_wake_unsent;   /* this window's pending notifications */
static bool s_wake_none_logged;
#if LINK_WAKE_VSC
static bool s_wake_vsc;          /* the controller's override is in (until the next connection update instant) */
static bool s_wake_vsc_stale;    /* ...and outlived its window */
static bool s_wake_vsc_refused;  /* the controller refused it once: notifications for the rest of the boot */

static int link_wake_vsc(uint16_t conn, uint16_t latency)
{
    const uint8_t cmd[4] = { (uint8_t)conn, (uint8_t)(conn >> 8), (uint8_t)latency, (uint8_t)(latency >> 8) };
    /* No response buffer: NimBLE then takes any return parameters as complete (a length it didn't expect would
     * reset the host). A nonzero status comes back as BLE_HS_HCI_ERR(status). */
    return ble_hs_hci_cmd_tx(MG_VS_SET_PREF_SLAVE_LATENCY, cmd, sizeof(cmd), NULL, 0);
}
#endif

static const char *link_wake_mechanism(void)
{
#if LINK_WAKE_VSC
    if (!s_wake_vsc_refused) {
        return "controller: vendor command 0xFC13, preferred slave latency 0";
    }
#endif
    return "a notification kept pending";
}

static void link_wake_timer_fire(void *arg)
{
    (void)arg;
    s_wake_tx_due = true;
    kick();   /* the worker sends it (link_wake_tick) */
}

static void link_wake_timer_stop(void)
{
    s_wake_tx_due = false;
    if (s_wake_timer) {
        esp_timer_stop(s_wake_timer);   /* not running: ESP_ERR_INVALID_STATE, fine */
    }
}

static void link_wake_timer_start(uint16_t itvl)
{
    if (!s_wake_timer) {
        const esp_timer_create_args_t a = { .callback = link_wake_timer_fire, .name = "mg_linkwake" };
        if (esp_timer_create(&a, &s_wake_timer) != ESP_OK) {
            s_wake_timer = NULL;
            ESP_LOGW(TAG, "link: no timer for the pending notification: latency kept");
            return;
        }
    }
    esp_timer_stop(s_wake_timer);
    s_wake_tx_due = true;   /* one at once */
    esp_timer_start_periodic(s_wake_timer, mg_linkpol_wake_period_us(itvl));
}

/* The worker, each round: the pending notification when its period came. */
static void link_wake_tick(void)
{
    if (!s_wake_tx_due) {
        return;
    }
    s_wake_tx_due = false;
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct os_mbuf *om = NULL;
    uint16_t h = 0;
    if (s_bas_sub) {
        uint8_t level = (uint8_t)(s_bas_last >= 0 ? s_bas_last : battery_level());   /* what it reports anyway */
        om = ble_hs_mbuf_from_flat(&level, 1);
        h = s_bas_handle;
    } else if (s_nus_sub) {
        static const uint8_t none;
        om = ble_hs_mbuf_from_flat(&none, 0);   /* an empty line on the log mirror: nothing a client prints */
        h = s_nus_tx_handle;
    } else {
        if (!s_wake_none_logged) {
            s_wake_none_logged = true;
            ESP_LOGW(TAG, "link: nothing subscribed to keep a notification pending: latency kept");
        }
        return;
    }
    if (om && ble_gatts_notify_custom(conn, h, om) == 0) {   /* takes om either way */
        s_wake_sent++;
        if (h == s_nus_tx_handle) {
            s_nus_inflight++;   /* its NOTIFY_TX takes it back */
        }
    } else {
        s_wake_unsent++;   /* out of buffers: data is queued anyway */
    }
}

/* The window's override, as the policy has it now. The worker (link_policy_check) and the GAP handler (a
 * disconnect); not under mg_lock. */
static void link_wake_sync(void)
{
    mg_lock();
    mg_linkpol_wake_act_t act = mg_linkpol_wake_sync(&s_linkpol, &s_wake);
    mg_linkpol_mode_t mode = s_linkpol.wake_mode;
    const char *why = s_linkpol.wake_why ? s_linkpol.wake_why : "-";
    long long held_ms = (long long)(s_linkpol.wake_held_us / 1000);
    uint16_t itvl = s_conn_itvl, latency = s_conn_latency;
    mg_unlock();
    uint16_t conn = s_conn;
    if (act == MG_LINKPOL_WAKE_APPLY || act == MG_LINKPOL_WAKE_REAPPLY) {
        if (act == MG_LINKPOL_WAKE_APPLY) {
            s_wake_sent = s_wake_unsent = 0;
            s_wake_none_logged = false;
            ESP_LOGI(TAG, "link: listening every event (switch to %s)", mg_linkpol_mode_name(mode));
        }
#if LINK_WAKE_VSC
        if (!s_wake_vsc_refused) {
            int rc = link_wake_vsc(conn, 0);
            if (rc == 0) {
                s_wake_vsc = true;
                s_wake_vsc_stale = false;
                return;
            }
            s_wake_vsc_refused = true;
            ESP_LOGW(TAG, "link: the controller refused Set Preferred Slave Latency (rc=%d): a notification kept "
                     "pending instead, for this boot", rc);
        }
#endif
        (void)conn;
        link_wake_timer_start(itvl);
    } else if (act == MG_LINKPOL_WAKE_RELEASE) {
        link_wake_timer_stop();
#if LINK_WAKE_VSC
        if (s_wake_vsc) {
            s_wake_vsc = false;
            /* Only a connection update instant (a grant, the central's own) or the link's end gives the granted
             * latency back: the controller can't raise it on its own. */
            if (strcmp(why, "granted") && strcmp(why, "disconnect") && strncmp(why, "the central's own", 17)) {
                s_wake_vsc_stale = true;
                ESP_LOGW(TAG, "link: listening every event until the next connection update (%lld ms; %s: the "
                         "controller can't lift its override)", held_ms, why);
                return;
            }
            ESP_LOGI(TAG, "link: latency back (%lld ms): %s, latency %u", held_ms, why, latency);
            return;
        }
#endif
        ESP_LOGI(TAG, "link: latency back (%lld ms): %s, latency %u (%u notifications pending, %u unsent)", held_ms,
                 why, latency, (unsigned)s_wake_sent, (unsigned)s_wake_unsent);
    }
}

/* A connection update event (status 0: its instant passed) or the link's end: a stale controller override
 * (LINK_WAKE_VSC) is gone with it. The GAP handler. */
static void link_wake_link_changed(bool ended)
{
#if LINK_WAKE_VSC
    if (s_wake_vsc_stale) {
        s_wake_vsc_stale = false;
        if (!ended) {
            ESP_LOGI(TAG, "link: latency back (the controller's override ended at the connection update)");
        }
    }
#else
    (void)ended;
#endif
}

bool mg_ble_link_wake_word(const char *line, char *out, size_t n)
{
    bool set = !strcmp(line, "link.wake=0") || !strcmp(line, "link.wake=1");
    if (!set && strcmp(line, "link.wake?")) {
        if (!strncmp(line, "link.wake", 9)) {
            snprintf(out, n, "link.wake: link.wake=0|1 or link.wake?");
            ESP_LOGW(TAG, "%s", out);
            return true;
        }
        return false;
    }
    mg_lock();
    if (set) {
        mg_linkpol_set_wake(&s_linkpol, line[10] == '1', esp_timer_get_time());
    }
    bool on = !s_linkpol.wake_off;
    bool open = s_linkpol.on && s_linkpol.wake;
    long long open_ms = open ? (long long)((esp_timer_get_time() - s_linkpol.wake_since_us) / 1000) : 0;
    const char *last = s_linkpol.wake_why;
    long long last_ms = (long long)(s_linkpol.wake_held_us / 1000);
    mg_unlock();
    if (set) {
        kick();   /* an open window closes in the worker's next round */
    }
    if (open) {
        snprintf(out, n, "link.wake %d (%s): listening every event for %lld ms", on, link_wake_mechanism(),
                 open_ms);
    } else if (last) {
        snprintf(out, n, "link.wake %d (%s): not switching (last window %lld ms, %s)", on, link_wake_mechanism(),
                 last_ms, last);
    } else {
        snprintf(out, n, "link.wake %d (%s): not switching", on, link_wake_mechanism());
    }
    ESP_LOGI(TAG, "%s", out);
    return true;
}

bool mg_ble_link_sleep_word(const char *line, char *out, size_t n)
{
    if (strncmp(line, "link.sleep", 10)) {
        return false;
    }
    mg_lock();
    bool r = mg_linkpol_sleep_word(&s_linkpol, line, esp_timer_get_time(), out, n);
    mg_unlock();
    if (r) {
        kick();   /* a new set while asleep goes in the worker's next round */
        ESP_LOGI(TAG, "%s", out);
    }
    return r;
}

/* The device's own activity (its talk press at touch-down, a stream's
 * start, an update's write): active at once; the worker's next round asks
 * for it, right away when it wasn't active. Any task. */
static void link_action(const char *why)
{
    mg_lock();
    bool was = mg_linkpol_active(&s_linkpol);
    mg_linkpol_action(&s_linkpol, esp_timer_get_time(), why);
    mg_unlock();
    if (!was) {
        kick();
    }
}

void mg_ble_link_action(const char *why)
{
    if (s_ready) {
        link_action(why);
    }
}

void mg_ble_link_turn(void)
{
    if (!s_ready || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    mg_lock();
    mg_linkpol_turn(&s_linkpol, esp_timer_get_time());
    mg_unlock();
    kick();   /* the worker's next round asks for active (if it wasn't) and times the turn */
}

/* assistant_state done or error: the gadget's turn is over (mgcommands.h). Under mg_lock. */
static void link_turn_end(bool error)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    mg_linkpol_turn_end(&s_linkpol, esp_timer_get_time(), error);
    kick();
}

/* A reply that starts on a link that isn't active starts at 150 ms held (mg_play_t.slow_link);
 * once active is granted, at the normal 30 ms. Under mg_lock. */
static void play_link_rule(void)
{
    s_play.slow_link = !mg_linkpol_link_active(&s_linkpol);
}

void mg_ble_link_activity(void)
{
    if (!s_ready) {
        return;
    }
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        /* No app connected: advertise fast for 30 s so it finds the gadget. */
        if (s_platform.adv_fast) {
            s_platform.adv_fast("talk press");
        }
        return;
    }
    link_action("talk press");
}

/* A user's wake or touch on the gadget (mgcommands.h): sleep ->
 * ready (not active) and the 10 min restarted; the worker's next round asks
 * for ready when it was asleep, paced as every request. Any task. */
void mg_ble_link_user(const char *why)
{
    if (s_ready && s_platform.user_action && (!why || strcmp(why, "wake") != 0)) {
        /* A talk press, a key or a tap that may lead to an agent action
         * is an active task, link or not; the screen lit alone isn't (a glance
         * at the time shouldn't wake the phone's app). */
        s_platform.user_action();
    }
    if (!s_ready || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    mg_lock();
    mg_linkpol_mode_t was = s_linkpol.mode;
    mg_linkpol_user(&s_linkpol, esp_timer_get_time(), why);
    bool changed = s_linkpol.mode != was;
    mg_unlock();
    if (changed) {
        kick();   /* the worker logs "link mode sleep -> ready (touch)", notifies and asks */
    }
}

/* Reports a change of the effective mode: the log (mg.ble, on the Nordic
 * UART mirror) and the link_mode notification. Under mg_lock. */
static void link_mode_report(void)
{
    uint8_t from;
    mg_linkpol_mode_t to;
    const char *why;
    if (!mg_linkpol_take_change(&s_linkpol, &from, &to, &why)) {
        return;
    }
    if (from == MG_LINKPOL_NO_MODE) {
        ESP_LOGI(TAG, "link mode %s (%s)", mg_linkpol_mode_name(to), why);
    } else {
        ESP_LOGI(TAG, "link mode %s -> %s (%s)", mg_linkpol_mode_name(from), mg_linkpol_mode_name(to), why);
    }
    mg_proto_report_setting(&s_proto, mg_setting_parameter_link_mode);   /* nothing before the client is ready */
}

static void link_policy_check(const char *busy)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    /* Link setup counts for its whole duration, and a gadget that
     * isn't set up yet is there for its setup. */
    if (!busy && (!s_set_up || (s_platform.setup_active && s_platform.setup_active()))) {
        busy = "setup";
    }
    mg_linkpol_params_t w;
    mg_lock();
    /* A reply playing ends the gadget's turn when it stops: a reply stream open. */
    mg_linkpol_in_t in = { .busy = busy, .reply = mg_play_active(&s_play) };
    bool go = mg_linkpol_step(&s_linkpol, esp_timer_get_time(), &in, &w);
    const char *note = s_linkpol.note;
    s_linkpol.note = NULL;
    const char *mode_note = s_linkpol.mode_note;
    s_linkpol.mode_note = NULL;
    link_mode_report();
    play_link_rule();
    int64_t next_us = s_linkpol.next_us;
    mg_unlock();
    if (mode_note) {
        ESP_LOGI(TAG, "link mode %s", mode_note);   /* "link mode active kept (turn: waiting for the reply)" */
    }
    if (note) {
        ESP_LOGI(TAG, "link: %s", note);
    }
    if (go) {
        link_wake_sync();   /* the window opened with the request: listening before it goes */
    }
    int rc = go ? link_request(&w) : 0;
    if (rc != 0) {
        mg_lock();
        if (rc == BLE_HS_EALREADY) {
            /* NimBLE still holds our unanswered request (it keeps one 40 s and
             * ends it without an event): not a refusal, asked again in 2 s. */
            mg_linkpol_request_busy(&s_linkpol, esp_timer_get_time());
        } else {
            mg_linkpol_request_failed(&s_linkpol, esp_timer_get_time());
        }
        note = s_linkpol.note;
        s_linkpol.note = NULL;
        mg_unlock();
        if (note) {
            ESP_LOGW(TAG, "link: %s", note);
        }
    }
    link_wake_sync();   /* a grant, refusal, 20 s or link.wake=0 closed it; the central's own update re-applies */
    link_wake_at(next_us);
}

/* start_streaming taken: active asked now, ahead of the stream's first buffer_update, so a
 * reply on a ready link waits as little as it can for the central's grant. Under mg_lock (mg_proto),
 * on the worker. Busy as the playback (what the worker's next round says too), so the check never asks
 * setup_active: that takes the pairing lock, which a setup's commit holds while it takes mg_lock. */
static void op_stream_starting(void *ctx)
{
    (void)ctx;
    link_action("audio playback");
    link_policy_check("audio playback");
}

/* Settings link_mode 16 and link_params 17. Under mg_lock (mg_proto). */
static uint8_t op_link_mode(void *ctx)
{
    (void)ctx;
    return (uint8_t)s_linkpol.mode;
}

static bool op_set_link_mode(void *ctx, uint8_t mode)
{
    (void)ctx;
    if (!mg_linkpol_app_mode(&s_linkpol, esp_timer_get_time(), mode)) {
        return false;
    }
    ESP_LOGI(TAG, "link: the app asks %s", mg_linkpol_mode_name(mode));
    kick();   /* the worker reports the change and asks for its set */
    return true;
}

static void op_link_params(void *ctx, uint8_t out[6])
{
    (void)ctx;
    mg_linkpol_link_params(&s_linkpol, out);
}

#if CONFIG_MUSE_GADGET_BLE_VIDEO
/* A stream starting or ending: the policy sees it on the worker's next round. */
static void video_link_want(bool fast)
{
    if (fast) {
        /* A stream's start: an action, and a video stream starting while the
         * link is already fast toggles it so the central re-plans (the
         * idle half 24/36/0/400, the fast half >= 500 ms after its grant). */
        if (s_ui_stream) {
            link_action("UI stream");
        } else {
            mg_lock();
            mg_linkpol_stream_start(&s_linkpol, esp_timer_get_time(), "video stream");
            mg_unlock();
            kick();
        }
    }
}

#if MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM) > 0
/*
 * The stream on an L2CAP connection-oriented channel: the same bytes as the
 * Data writes, into the same ring, but credit-based at the link layer, so a
 * phone sends as fast as the link goes instead of one GATT write at a time.
 */
/* The receive window is one SDU's worth of credits (MTU / MPS), refilled as
 * each SDU arrives: 8 KB keeps the phone sending across the credit round
 * trip (4 KB measured ~280 kb/s, the link stalling on credits). */
#define VIDEO_COC_MTU 8192
#define VIDEO_COC_BLOCK 2048
#define VIDEO_COC_BLOCKS 10
static os_membuf_t *s_coc_mem;   /* in PSRAM */
static struct os_mempool s_coc_mempool;
static struct os_mbuf_pool s_coc_pool;
static bool s_coc_server;

static void coc_give_buffer(struct ble_l2cap_chan *chan)
{
    struct os_mbuf *sdu = os_mbuf_get_pkthdr(&s_coc_pool, 0);
    if (!sdu || ble_l2cap_recv_ready(chan, sdu) != 0) {
        ESP_LOGW(TAG, "video channel: no receive buffer");
        if (sdu) {
            os_mbuf_free_chain(sdu);
        }
    }
}

static int coc_event(struct ble_l2cap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_L2CAP_EVENT_COC_CONNECTED:
        ESP_LOGI(TAG, "video channel %s (status %d)", ev->connect.status ? "failed" : "open", ev->connect.status);
        return 0;
    case BLE_L2CAP_EVENT_COC_DISCONNECTED:
        ESP_LOGI(TAG, "video channel closed");
        return 0;
    case BLE_L2CAP_EVENT_COC_ACCEPT:
        if (ev->accept.conn_handle != s_conn) {
            return BLE_HS_ENOTCONN;
        }
        coc_give_buffer(ev->accept.chan);
        return 0;
    case BLE_L2CAP_EVENT_COC_DATA_RECEIVED: {
        struct os_mbuf *om = ev->receive.sdu_rx;
        /* Video only: the channel isn't ordered with Control, so it can't
         * take part in change_data_type interleaving. */
        mg_lock();
        bool video = mg_proto_video_active(&s_proto);
        mg_unlock();
        for (struct os_mbuf *seg = om; video && seg; seg = SLIST_NEXT(seg, om_next)) {
            if (seg->om_len) {
                muse_vstream_write(&s_vstream, seg->om_data, seg->om_len);
            }
        }
        os_mbuf_free_chain(om);
        coc_give_buffer(ev->receive.chan);
        return 0;
    }
    default:
        return 0;
    }
}

static void coc_server_start(void)
{
    if (s_coc_server) {
        return;
    }
    s_coc_mem = heap_caps_malloc(OS_MEMPOOL_BYTES(VIDEO_COC_BLOCKS, VIDEO_COC_BLOCK),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_coc_mem ||
        os_mempool_init(&s_coc_mempool, VIDEO_COC_BLOCKS, VIDEO_COC_BLOCK, s_coc_mem, "mg_video_coc") != 0 ||
        os_mbuf_pool_init(&s_coc_pool, &s_coc_mempool, VIDEO_COC_BLOCK, VIDEO_COC_BLOCKS) != 0) {
        ESP_LOGE(TAG, "video channel: no buffer pool");
        return;
    }
    int rc = ble_l2cap_create_server(MG_VIDEO_L2CAP_PSM, VIDEO_COC_MTU, coc_event, NULL);
    s_coc_server = rc == 0;
    ESP_LOGI(TAG, "video channel server on PSM 0x%02x: rc=%d", MG_VIDEO_L2CAP_PSM, rc);
}
#endif

static bool op_video_begin(void *ctx, const mg_video_params_t *vp)
{
    (void)ctx;
    if (!s_platform.video_begin || !s_platform.video_begin(vp)) {
        return false;
    }
    s_ui_stream = vp->content == mg_video_content_ui;
    ESP_LOGI(TAG, "%s stream %ux%u %u fps %u kb/s%s", s_ui_stream ? "UI" : "video", vp->width, vp->height, vp->fps,
             vp->kbps, vp->tiles == 2 ? ", two bands" : "");
    video_link_want(true);
    kick();
    return true;
}

static void op_video_end(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "video stream ended: %u bytes, %u resyncs, %u overrun; Data discarded so far %u bytes "
             "(selected type without a stream)", (unsigned)s_vstream.received, (unsigned)s_vstream.resyncs,
             (unsigned)s_vstream.overruns, (unsigned)mg_proto_rx_discarded(&s_proto));
    mg_ble_framed_log();
    if (s_platform.video_end) {
        s_platform.video_end();
    }
    s_ui_stream = false;
    video_link_want(false);
}

/* A Data write while a stream is on: active (latency 0 asked if it was idle). */
static void video_data_arrived(void)
{
    int64_t now = esp_timer_get_time();
    s_last_data_us = now;
    if (!mg_linkpol_active(&s_linkpol)) {
        link_action("UI stream");   /* not active: active now, not at the next round */
    }
}
#endif

/* mgcommands.h: after the device ended a stream, Data selected for it was discarded until the
 * client's change_data_type: told once per episode. */
static void op_rx_held(void *ctx, uint32_t bytes)
{
    (void)ctx;
    ESP_LOGI(TAG, "Data discarded after device end: %lu bytes until change_data_type", (unsigned long)bytes);
}


/* The clock is no video: builds without CONFIG_MUSE_GADGET_BLE_VIDEO take it too. */
static void op_clock_set(void *ctx, int64_t ms, bool has_off, int16_t off, bool has_flags, uint8_t flags, bool store)
{
    (void)ctx;
    s_platform.clock_set(ms, has_off, off, has_flags, flags, store);
}

static void op_clock_get(void *ctx, int64_t *ms, int16_t *off, uint8_t *flags)
{
    (void)ctx;
    s_platform.clock_get(ms, off, flags);
}

static bool op_battery_status(void *ctx, mg_battery_status_t *out)
{
    (void)ctx;
    return s_platform.battery_status(out);
}

static void op_power_off(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "power_off from the client");
    s_platform.power_off();
}

static bool op_power_off_ready(void *ctx)
{
    (void)ctx;
    return s_platform.power_off_ready();
}

/* Restart normally from a recovery mode (device_action reboot). */
static void op_reboot(void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "recovery: restarting normally (device_action reboot from the client)");
    restart_after(200, MUSE_RESTART_CONSOLE);   /* closes the link cleanly first */
}

void mg_ble_ui_tune(uint16_t latency)
{
    /* Bench: ready's latency (5 by default). The stream-start toggle's idle half
     * doesn't follow it: 24/36/0/400 always. */
    if (!s_ready) {
        return;
    }
    mg_lock();
    s_linkpol.ready_latency = latency <= 30 ? latency : 30;
    mg_unlock();
    ESP_LOGI(TAG, "link: latency %u in ready (the stream-start toggle's idle half stays %u-%u, latency %u)",
             s_linkpol.ready_latency, (unsigned)MG_LINKPOL_TOGGLE_ITVL_MIN, (unsigned)MG_LINKPOL_TOGGLE_ITVL_MAX,
             (unsigned)MG_LINKPOL_TOGGLE_LATENCY);
}

void mg_ble_video_tune(uint16_t itvl, uint16_t ce)
{
#if CONFIG_MUSE_GADGET_BLE_VIDEO
    /* Bench: the interval the policy asks for (12 by default) and the event length. */
    if (!s_ready) {
        return;
    }
    mg_lock();
    if (itvl >= 6 && itvl <= 80) {
        s_linkpol.itvl = itvl;
    }
    s_video_ce = ce;
    mg_linkpol_stream_start(&s_linkpol, esp_timer_get_time(), "bench: link tuned");   /* a real update */
    mg_unlock();
    ESP_LOGI(TAG, "link: interval %u (x1.25 ms), event length %u (x0.625 ms)", s_linkpol.itvl, s_video_ce);
    kick();
#else
    (void)itvl;
    (void)ce;
#endif
}

muse_vstream_t *mg_ble_vstream(void)
{
    return s_vstream.buf ? &s_vstream : NULL;
}

void mg_ble_video_request(bool want)
{
    if (!s_ready) {
        return;
    }
    mg_lock();
    mg_proto_video_request(&s_proto, want);
    mg_unlock();
}

void mg_ble_video_keyframe(void)
{
    if (!s_ready) {
        return;
    }
    mg_lock();
    mg_proto_video_request_keyframe(&s_proto);
    mg_unlock();
}

void mg_ble_video_freed(uint16_t shown, uint16_t dropped, uint8_t decode_ms)
{
    if (!s_ready) {
        return;
    }
    mg_lock();
    mg_proto_video_stats(&s_proto, shown, dropped, decode_ms);
    mg_unlock();
    kick();
}

void mg_ble_video_shown(uint16_t number, uint32_t age_ms, uint16_t shown)
{
    if (!s_ready) {
        return;
    }
    mg_lock();
    mg_proto_video_shown(&s_proto, number, mg_now_ms() - age_ms);
    s_proto.video.shown = shown;
    mg_unlock();
    kick();
}

void mg_ble_video_stop(void)
{
    if (!s_ready) {
        return;
    }
    mg_lock();
    mg_proto_video_stop(&s_proto);
    mg_unlock();
}

bool mg_ble_output_ok(void)
{
    if (!s_ready) {
        return false;
    }
    mg_lock();
    bool ok = s_proto.connected && mg_proto_output_ok(&s_proto);
    mg_unlock();
    return ok;
}

/* The bench mark (the token proof's bench override): on while a central is connected and either console's switch is on;
 * the platform hears each change (never under the lock). */
static void bench_mark_sync(void)
{
    static bool shown;
    mg_lock();
    bool on = s_proto.connected && mg_proto_output_bench(&s_proto);
    mg_unlock();
    if (on != shown) {
        shown = on;
        if (s_platform.bench_mark) {
            s_platform.bench_mark(on);
        }
    }
}

/* The Nordic UART's bench.out=1|0 / bench.out? (the bench override over Bluetooth):
 * the switch for this connection only, off at every boot and every connect, ended by the disconnect; one log
 * line for each on and off naming the connection. False: not the word. NimBLE host. */
static bool nus_bench_out(const char *line)
{
    bool set = !strcmp(line, "bench.out=0") || !strcmp(line, "bench.out=1");
    if (!set && strcmp(line, "bench.out?")) {
        if (!strncmp(line, "bench.out", 9)) {
            ESP_LOGI(TAG, "bench.out: bench.out=0|1 or bench.out?");
            return true;
        }
        return false;
    }
    mg_lock();
    bool was = mg_proto_output_bench_conn(&s_proto);
    if (set) {
        mg_proto_set_output_bench_conn(&s_proto, line[10] == '1');
    }
    bool on = mg_proto_output_bench_conn(&s_proto), usb = s_proto.out_bench;
    const char *state = mg_proto_output_state(&s_proto);
    bool pending = mg_proto_capture_pending(&s_proto);
    uint32_t gen = s_conn_gen;
    uint16_t conn = s_conn;
    mg_unlock();
    if (set && on != was) {
        if (on) {
            ESP_LOGW(TAG, "bench.out on (Nordic UART, connection %u, handle %u): mic output goes to this central "
                     "without its token proof until it disconnects (bench override, RAM only)",
                     (unsigned)gen, (unsigned)conn);
        } else {
            ESP_LOGW(TAG, "bench.out off (Nordic UART, connection %u, handle %u): bench.out=0", (unsigned)gen,
                     (unsigned)conn);
        }
        if (on && pending) {
            kick();   /* a hold the switch released: the worker drains it */
        }
        bench_mark_sync();
    }
    ESP_LOGI(TAG, "bench.out %d (%s; this connection's%s)", on, state, usb ? ", the USB console's on too" : "");
    return true;
}

bool mg_ble_bench_out_word(const char *line, char *out, size_t n)
{
    bool set = !strcmp(line, "bench.out=0") || !strcmp(line, "bench.out=1");
    if (!set && strcmp(line, "bench.out?")) {
        if (!strncmp(line, "bench.out", 9)) {
            snprintf(out, n, "bench.out: bench.out=0|1 or bench.out?");
            return true;
        }
        return false;
    }
    if (!s_ready) {
        snprintf(out, n, "bench.out: no musegadgets BLE on this build");
        return true;
    }
    mg_lock();
    if (set) {
        mg_proto_set_output_bench(&s_proto, line[10] == '1');
    }
    bool on = mg_proto_output_bench(&s_proto);
    const char *state = mg_proto_output_state(&s_proto);
    bool pending = mg_proto_capture_pending(&s_proto);
    mg_unlock();
    if (set) {
        ESP_LOGW(TAG, "bench.out %s: mic output %s (bench switch, RAM only)", on ? "on" : "off",
                 on ? "goes to any central, proven or not" : "only to a central that passed this connection's token proof");
        if (pending) {
            kick();   /* a hold the switch released: the worker drains it */
        }
        bench_mark_sync();
    }
    snprintf(out, n, "bench.out %d (%s)", on, state);
    return true;
}

/* Framed Data's state and counters since connect (mg_proto.h): the console's framed? and the video stream's
 * end line. Counts only, never a line per write. */
void mg_ble_framed_log(void)
{
    mg_lock();
    bool on = s_proto.framed;
    uint32_t sel = s_proto.framed_selects, wv = s_proto.framed_wr_video, wa = s_proto.framed_wr_audio,
             disc = s_proto.framed_disc, sw = s_proto.rx_switches;
    mg_unlock();
    ESP_LOGI(TAG, "framed: %s; since connect: %lu selects, writes video %lu audio %lu, %lu discarded; "
             "plain change_data_type %lu", on ? "on" : "off", (unsigned long)sel, (unsigned long)wv,
             (unsigned long)wa, (unsigned long)disc, (unsigned long)sw);
}

/* Framed Data on/off, once per change (the worker looks every pass): a line per stream or reply, never per write. */
static void framed_watch(void)
{
    static bool was;
    bool on = s_proto.framed;
    if (on != was) {
        was = on;
        ESP_LOGI(TAG, "framed Data %s (selects %lu, plain change_data_type %lu)", on ? "on" : "off",
                 (unsigned long)s_proto.framed_selects, (unsigned long)s_proto.rx_switches);
    }
}

void mg_ble_credit_coalesce(bool on)
{
    mg_lock();
    s_proto.credit_coalesce = on;   /* bench A/B, RAM only (request answers stay immediate by default) */
    s_proto.update_owed = false;
    mg_unlock();
    ESP_LOGI(TAG, "credit.coalesce %s: a request_buffer_update in a live-depth reply is answered %s", on ? "on" : "off",
             on ? "by the stream tick's next update (<= 10 ms)" : "at once");
}

/* speaker_volume reports logged as they leave (owed until then); the
 * worker logs an owed one that left later. */
static uint32_t s_spk_logged;

void mg_ble_report_setting(uint8_t param)
{
    if (!s_ready) {
        return;
    }
    mg_lock();
    mg_proto_report_setting(&s_proto, param);
    bool went = s_proto.spk_sent != s_spk_logged;
    s_spk_logged = s_proto.spk_sent;
    mg_unlock();
    if (param == mg_setting_parameter_speaker_volume && s_platform.speaker_volume) {
        /* On the Nordic UART mirror: a Mac client sees each report. */
        ESP_LOGI(TAG, "speaker_volume %d reported%s", s_platform.speaker_volume(),
                 went ? "" : s_conn == BLE_HS_CONN_HANDLE_NONE ? " later (owed: no client)" : " later (owed: the link is busy)");
    }
}

void mg_ble_press(uint8_t src)
{
    if (!s_ready) {
        return;
    }
    mg_lock();
    mg_proto_press(&s_proto, src);
    mg_unlock();
}

void mg_ble_report_battery(void)
{
    if (!s_ready) {
        return;
    }
    mg_lock();
    mg_proto_report_battery(&s_proto);
    mg_unlock();
}

bool mg_ble_video_client(void)
{
    return s_ready && s_vstream.buf && mg_ble_link() >= MG_LINK_SESSION;
}

/* ---- UI input: touch events and the HID ------------------------------------ */

static mg_touch_track_t s_touch;
static mg_hid_t *s_hid;        /* the touch-to-HID engine (in PSRAM), with HID */
static uint32_t s_hid_sent, s_hid_busy;

#if CONFIG_MUSE_GADGET_BLE_HID
/* Call with mg_lock held or not: it takes it. */
static void hid_status_changed(void)
{
    uint8_t flags = mg_hogp_status();
    mg_lock();
    uint8_t mode = s_hid ? s_hid->map.mode : mg_hid_mode_off;
    mg_proto_hid_status(&s_proto, mode, flags);
    mg_unlock();
    ESP_LOGI(TAG, "HID: mode %u,%s%s", mode, flags & mg_hid_status_bonded ? " bonded" : " not bonded",
             flags & mg_hid_status_host ? ", host attached" : "");
}

#define NVS_HID_MAP "hid_map"
#define NVS_HID_SHOWN "hid_shown"
static bool s_hid_saved_shown;   /* NVS_HID_SHOWN: a HID host's service shown (for bonded peers) */

/* The map survives restarts: the phone's HID host reconnects on its own, app or not. */
static void hid_map_save(const mg_hid_map_t *m)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_HID_MAP, m, sizeof(*m));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void hid_map_load(void)
{
    nvs_handle_t h;
    mg_hid_map_t m;
    size_t n = sizeof(m);
    if (!s_hid || nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, NVS_HID_MAP, &m, &n) == ESP_OK && n == sizeof(m) && m.mode <= mg_hid_mode_mouse_rel) {
        mg_hid_set_map(s_hid, &m);
    }
    uint8_t shown = 0;
    nvs_get_u8(h, NVS_HID_SHOWN, &shown);
    s_hid_saved_shown = shown;
    mg_hogp_set_visible(shown);   /* the first connection decides again (hid_visibility_for) */
    nvs_close(h);
}

/* The HID service shown a moment after hid_status has gone: a client that
 * sees Service Changed rediscovers, and may lose the notification that was
 * in flight (macOS does). Once shown it stays (in NVS): a phone's HID host
 * that bonded loses its input device if the service goes away under it, so
 * mode off only stops the reports. */
static volatile int8_t s_hid_show = -1;
static int64_t s_hid_show_at;

static void hid_shown_save(uint8_t shown);

static void hid_visibility_check(void)
{
    if (s_hid_show >= 0 && esp_timer_get_time() >= s_hid_show_at) {
        if (s_hid_show == 1 && !mg_hogp_visible()) {
            mg_hogp_set_visible(true);
            hid_shown_save(1);
        }
        s_hid_show = -1;
        if (s_platform.refresh_advertising) {
            s_platform.refresh_advertising();
        }
    }
}

static bool mg_ble_hid_key_locked(uint16_t usage);
static void hid_shown_save(uint8_t shown);
static void hid_map_save(const mg_hid_map_t *m);

/* HID mode off (saved) and the service hidden. A phone that bonded keeps
 * its keys, but no HID service: forget the gadget there too. */
static void hid_off_locked(void)
{
    if (!s_hid) {
        return;
    }
    mg_lock();
    mg_hid_map_t m = s_hid->map;
    m.mode = mg_hid_mode_off;
    mg_hid_set_map(s_hid, &m);
    hid_map_save(&m);
    s_proto.hid_mode = mg_hid_mode_off;
    mg_unlock();
    mg_hogp_set_visible(false);
    hid_shown_save(0);
    ESP_LOGI(TAG, "HID: off and hidden until a hid_map turns it on");
}

static void hid_shown_save(uint8_t shown)
{
    s_hid_saved_shown = shown;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_HID_SHOWN, shown);
        nvs_commit(h);
        nvs_close(h);
    }
}

/*
 * The HID service for a new connection, before any discovery: shown only to a
 * bonded peer while a HID host's service is saved as shown, hidden from
 * anyone else whatever the saved map says (mg_hid_show_at_connect). An
 * unbonded phone gets it once a hid_map with HID on arrives on its connection
 * (hid_visibility_check). A bonded host the controller couldn't resolve at
 * connect gets it at encryption (hid_show_on_encryption). With no bond left at
 * all and HID off, the saved flag is cleared too.
 */
static void hid_visibility_for(uint16_t conn)
{
    if (!s_hid) {
        return;
    }
    s_hid_show = -1;   /* a show pending from the last connection is that connection's */
    bool bonded = mg_hogp_peer_bonded(conn);
    if (s_hid_saved_shown && s_hid->map.mode == mg_hid_mode_off) {
        ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
        int bonds = 0;
        if (ble_store_util_bonded_peers(peers, &bonds, MYNEWT_VAL(BLE_STORE_MAX_BONDS)) == 0 && bonds == 0) {
            hid_shown_save(0);
        }
    }
    bool show = mg_hid_show_at_connect(bonded, s_hid_saved_shown);
    if (!show && mg_hogp_visible()) {
        ESP_LOGI(TAG, "HID: hidden from this %s peer until a hid_map turns HID on", bonded ? "bonded" : "unbonded");
    }
    mg_hogp_set_visible(show);   /* not connected yet for mg_hogp: applied at its connect, no Service Changed */
}

/* Encryption with a bond: a HID host's service if it should have had it at
 * connect but its address wasn't resolved then. */
static void hid_show_on_encryption(void)
{
    if (s_hid && s_hid_saved_shown && !mg_hogp_visible() && (mg_hogp_status() & mg_hid_status_bonded)) {
        mg_hogp_set_visible(true);   /* with Service Changed */
        ESP_LOGI(TAG, "HID: shown to the bonded host at encryption");
    }
}

static bool op_hid_key(void *ctx, uint16_t usage)
{
    (void)ctx;
    /* Called with the lock held (the protocol's): do it on the event task. */
    return mg_ble_hid_key_locked(usage);
}

static uint8_t op_hid_map(void *ctx, const mg_hid_map_t *m, uint8_t *mode)
{
    (void)ctx;
    if (s_hid) {
        mg_hid_set_map(s_hid, m);
        hid_map_save(m);
        s_hid_show_at = esp_timer_get_time() + 200 * 1000;
        s_hid_show = m->mode != mg_hid_mode_off;
        kick();
    }
    *mode = s_hid ? m->mode : mg_hid_mode_off;
    ESP_LOGI(TAG, "hid_map: mode %u, square %u,%u %ux%u /65535%s, rel %ux%u", m->mode, m->x, m->y, m->w, m->h,
             m->flags & mg_hid_map_fisheye ? ", fisheye" : "", m->rel_w, m->rel_h);
    return mg_hogp_status();
}

/* Sends what the engine queued, as fast as the stack takes it. True while more waits. */
static bool hid_pump(void)
{
    if (!s_hid) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        mg_lock();
        const mg_hid_report_t *r = mg_hid_peek(s_hid);
        mg_hid_report_t rep;
        if (r) {
            rep = *r;
        }
        mg_unlock();
        if (!r) {
            return false;
        }
        if (!mg_hogp_ready(rep.id)) {
            mg_lock();
            mg_hid_clear(s_hid);   /* no host for it: nothing to keep */
            mg_unlock();
            return false;
        }
        if (!mg_hogp_send(&rep)) {
            s_hid_busy++;
            return true;   /* the stack is full: next round */
        }
        s_hid_sent++;
        mg_lock();
        mg_hid_pop(s_hid);
        mg_unlock();
    }
    return true;
}
#endif

/* The touch gesture under way works the client's menu (to the client only),
 * and the tap-or-pan gate for the edge ring and pan mode: mg_ble_touch. */
static bool s_gesture_client;
static mg_edge_gate_t s_gate;

bool mg_ble_run_on_worker(void (*fn)(void *arg), void *arg)
{
    ev_t ev = { .type = EV_RUN, .run = fn, .data = arg };
    return fn && s_events && xQueueSend(s_events, &ev, 0) == pdTRUE;
}

bool mg_ble_ui_menu_toggle(void)
{
    if (!s_ready) {
        return false;
    }
    mg_lock();
    uint8_t open = !mg_proto_ui_control(&s_proto, mg_ui_control_menu);
    bool sent = mg_proto_ui_control_set(&s_proto, mg_ui_control_menu, open);
    mg_unlock();
    ESP_LOGI(TAG, "UI menu %s%s", open ? "open" : "closed", sent ? "" : " (not sent)");
    return sent;
}

bool mg_ble_ui_controls(void)
{
    return s_ready && (s_proto.cfg.ui_input & (1u << mg_ui_input_feature_controls)) && mg_proto_ready(&s_proto);
}

bool mg_ble_touch_wanted(void)
{
    if (!s_ready) {
        return false;
    }
    mg_lock();
    bool want = mg_proto_touch_enabled(&s_proto) || (s_hid && s_hid->map.mode != mg_hid_mode_off);
    mg_unlock();
    return want;
}

void mg_ble_touch(bool pressed, uint16_t x, uint16_t y)
{
    if (!s_ready) {
        return;
    }
    uint32_t now = mg_now_ms();
    mg_touch_event_t ev;
    /* A passcode entry on the gadget's screen: no touch leaves it (mgcommands.h, UI input). */
    bool held = s_platform.touch_private && s_platform.touch_private();
    mg_lock();
    bool got = mg_touch_feed_held(&s_touch, held, pressed, x, y, now, &ev);
    mg_touch_event_t out[2];
    bool pan = false;
    int n = 0;
    if (got && ev.action == mg_touch_down) {
        /* With the menu open the whole gesture is the menu's: the client's
         * only. Otherwise one that may pan (it went down in the edge ring,
         * or pan mode is on) waits until it moves (a pan: the client's only,
         * flagged) or lifts or holds still (the phone's, as any touch). */
        s_gesture_client = mg_proto_ui_control(&s_proto, mg_ui_control_menu);
        bool gated = !s_gesture_client &&
                     (mg_proto_ui_control(&s_proto, mg_ui_control_pan) ||
                      (mg_proto_ui_control(&s_proto, mg_ui_control_edge_pan) &&
                       mg_ui_in_edge_ring(ev.x, ev.y, s_platform.touch_w, s_platform.touch_h, s_platform.touch_round)));
        n = mg_edge_gate_feed(&s_gate, &ev, gated, now, out, &pan);
    } else if (got && s_gesture_client) {
        out[0] = ev;
        n = 1;
    } else {
        n = mg_edge_gate_feed(&s_gate, got ? &ev : NULL, false, now, out, &pan);
    }
    for (int i = 0; i < n; i++) {
        mg_proto_touch(&s_proto, &out[i], now, pan ? MG_TOUCH_ID_PAN : 0);
        if (s_hid && !s_gesture_client && !pan) {
            mg_hid_touch(s_hid, &out[i]);
        }
    }
    if (got && ev.action == mg_touch_up) {
        s_gesture_client = false;
    }
#if CONFIG_MUSE_GADGET_BLE_VIDEO
    if (got) {
        s_last_touch_us = esp_timer_get_time();
    }
    if (got && ev.action == mg_touch_down && s_ui_stream) {
        /* A touch on a still UI stream: the picture it changes is on its way,
         * so the link goes active now, while the phone works, rather than
         * when the first byte of the answer comes (up to 165 ms later). */
        video_data_arrived();
    }
#endif
    mg_unlock();
    if (got) {
        static const char *const names[] = { "down", "move", "up" };
        if (ev.action != mg_touch_move) {
            ESP_LOGI(TAG, "touch %s", names[ev.action]);   /* never where: a keypad's taps spell its code */
        }
    }
    if (n && pan && out[0].action == mg_touch_down) {
        ESP_LOGI(TAG, "touch: a pan (moved %d px from the edge)", MG_UI_EDGE_SLOP);
    }
    if (n && s_hid) {
        kick();
    }
}

/* With the lock held. */
static bool mg_ble_hid_key_locked(uint16_t usage)
{
    if (!s_ready || !s_hid) {
        return false;
    }
#if CONFIG_MUSE_GADGET_BLE_HID
    bool on = s_hid->map.mode != mg_hid_mode_off && mg_hogp_ready(MG_HID_REPORT_CONSUMER);
#else
    bool on = false;   /* no HID service: nothing to send it on */
#endif
    if (on) {
        mg_hid_key(s_hid, usage);
        kick();
    }
    ESP_LOGI(TAG, "HID key 0x%03x%s", usage, on ? "" : ": no HID host for it");
    return on;
}

bool mg_ble_hid_key(uint16_t usage)
{
    mg_lock();
    bool on = mg_ble_hid_key_locked(usage);
    mg_unlock();
    return on;
}

static void kick(void)
{
    if (s_events) {
        ev_t ev = { .type = EV_KICK };
        xQueueSend(s_events, &ev, 0);
    }
}

void mg_ble_kick(void)
{
    kick();
}

int64_t mg_unix_ms_now(void)
{
    int64_t ms = INT64_MIN;
    int16_t off = 0;
    uint8_t flags = 0;
    if (s_platform.clock_get) {
        s_platform.clock_get(&ms, &off, &flags);
    }
    return ms == INT64_MIN || ms < 0 ? 0 : ms;
}

bool mg_queue_submit(uint8_t codec, uint32_t start_ms, uint64_t start_unix_ms, uint8_t *data, uint32_t bytes)
{
    ev_t ev = { .type = EV_CLIP, .codec = codec, .start_ms = start_ms, .start_unix_ms = start_unix_ms, .len = bytes, .data = data };
    if (!s_events || xQueueSend(s_events, &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(data);
        return false;
    }
    return true;
}

/* ---- persistence ---------------------------------------------------------- */

static void load_settings(mg_settings_t *s)
{
    mg_settings_defaults(s);
    /* A BLE-only gadget keeps presses made while no client is connected until
     * one is; clients can turn that off (audio_queue_enabled). */
    s->queue_enabled = MG_STANDALONE;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t b[12];
    size_t n = sizeof(b);
    if (nvs_get_blob(h, NVS_SETTINGS, b, &n) == ESP_OK &&
        ((n == 8 && b[0] == 1) || (n == 12 && b[0] == SETTINGS_VERSION))) {
        s->buzz_freq_hz = (uint16_t)(b[1] | b[2] << 8);
        s->buzz_ms = (uint16_t)(b[3] | b[4] << 8);
        s->buzz_volume = b[5];
        s->codec = b[6];   /* audio_codec (7), kept again; mg_proto_init checks it */
        s->queue_enabled = b[7];
        if (n == 12) {
            s->capture_rate = (uint16_t)(b[8] | b[9] << 8);   /* 13 and 14; mg_proto_init checks them */
            s->capture_kbps = (uint16_t)(b[10] | b[11] << 8);
        }
    }
    nvs_close(h);
}

void mg_ble_settings_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_erase_key(h, NVS_SETTINGS);
    if ((err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK) {
        ESP_LOGI(TAG, "settings back to defaults (factory reset)");
    }
    nvs_close(h);
}

static void save_settings(const mg_settings_t *s)
{
    uint8_t b[12] = {
        SETTINGS_VERSION, (uint8_t)s->buzz_freq_hz, (uint8_t)(s->buzz_freq_hz >> 8), (uint8_t)s->buzz_ms,
        (uint8_t)(s->buzz_ms >> 8), s->buzz_volume, s->codec, s->queue_enabled,
        (uint8_t)s->capture_rate, (uint8_t)(s->capture_rate >> 8), (uint8_t)s->capture_kbps, (uint8_t)(s->capture_kbps >> 8),
    };
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, NVS_SETTINGS, b, sizeof(b)) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "settings not saved");
    }
    nvs_close(h);
}

/* ---- token proof -----------------------------------------------------------
 * K comes from Link's config (the platform's proof_key); it never stays in
 * this component's memory longer than one HMAC.
 */

static bool op_random(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    /* A security draw: the hardware RNG only with a radio on, else the proof
     * fails (mg_proto.c: busy) with nothing drawn (components/secure_random). */
    return secure_random(out, n);
}

static bool op_proof_key(void *ctx, uint8_t k[MG_TOKEN_PROOF_KEY_LEN])
{
    (void)ctx;
    return s_platform.proof_key(k);
}

static bool op_set_up(void *ctx)
{
    (void)ctx;
    return s_set_up;
}

static void op_proof_clear(void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "token proof clear: resetting setup");
    /* Set up until the erase succeeds (mg_ble_proof_clear_done): a Forget that fails must not open the owner's
     * reads and the mirror. */
    s_platform.proof_clear();
}

/* The sealed clear's erase runs on the platform's own task (it waits for the setup operation gate): the
 * platform calls mg_ble_proof_clear_done() with its result, which the device session answers before the
 * restart. */
static bool op_proof_clear_async(void *ctx)
{
    op_proof_clear(ctx);
    return true;
}

void mg_ble_proof_clear_done(bool erased)
{
    if (erased) {
        s_set_up = false;   /* the key and the marker are gone (a restart follows) */
        /* Forget ends the link's proof and the update's binding (mg-update-v2.json "Forget ends the binding", "...
         * the link's proof"): until the restart an update asks for the press; the wired console's arm stays. */
        s_smp_forgot = true;
        muse_local_press_forget();
    } else {
        uint8_t k[MG_TOKEN_PROOF_KEY_LEN];   /* failed: what storage still says */
        s_set_up = (s_platform.proof_key && s_platform.proof_key(k)) || (s_platform.set_up && s_platform.set_up());
        mg_token_proof_wipe(k, sizeof(k));
    }
    if (!s_ready) {
        return;
    }
    mg_lock();
    mg_proto_clear_done(&s_proto, erased);
    mg_unlock();
    kick();   /* the answer goes out from the worker; the restart waits for it (mg_ble_proof_clear_owed) */
}

bool mg_ble_proof_clear_owed(void)
{
    if (!s_ready) {
        return false;
    }
    mg_lock();
    bool owed = mg_proto_clear_owed(&s_proto);
    mg_unlock();
    return owed;
}

/* ---- device session ---------------------------------------------------------
 * The gadget's VM session over BLE (mg_command_session): the credential comes
 * from Link's config through the platform, messages go to and from the
 * session code in main (noise_control.cpp) through mg_ble_session_*.
 */

/* The device session's credential and its refreshed pair (the VM session
 * on BLE-only builds, the credentials-only session on Wi-Fi gadgets). */
static bool op_session_credentials(void *ctx, mg_session_creds_t *c)
{
    (void)ctx;
    return s_platform.session_credentials(c);
}

static bool op_session_store_tokens(void *ctx, const char *access_token, const char *refresh_token,
                                    uint64_t expires)
{
    (void)ctx;
    bool ok = s_platform.session_store_tokens && s_platform.session_store_tokens(access_token, refresh_token, expires);
    ESP_LOGI(TAG, "session: refreshed tokens %s (%s expiry)", ok ? "saved" : "not saved", expires ? "with" : "no");
    return ok;
}

static bool op_session_busy(void *ctx)
{
    (void)ctx;
    return s_platform.session_busy && s_platform.session_busy();
}

static bool op_client_refreshes(void *ctx)
{
    (void)ctx;
    return s_platform.client_refreshes();
}

#if CONFIG_MUSE_GADGET_BLE_SESSION
static mg_session_config_t s_session_cfg;

/* Everything else the session allocates, per message (out-queue copies,
 * credentials, keys, the incremental tag): PSRAM first, quietly. Freed with
 * free(), which takes either heap. */
static void *session_msg_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static void session_msg_release(void *p)
{
    free(p);
}

/* The session's receive ring, for an open session only: PSRAM where the
 * board has some, else internal RAM (the C6). */
static void *session_ring_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(n);
    }
    ESP_LOGI(TAG, "session: ring of %u bytes %s", (unsigned)n, p ? "taken" : "not available");
    return p;
}

static void op_session_state(void *ctx, uint8_t state, uint16_t epoch)
{
    (void)ctx;
    ESP_LOGI(TAG, "session: state %u, epoch %u", state, epoch);
    s_platform.session_state(state, epoch);
}

static void op_session_message(void *ctx)
{
    (void)ctx;
    s_platform.session_message();
}

#if CONFIG_MUSE_GADGET_BLE_SESSION
/* On mg.session, the device session's tag (mirrored on the Nordic UART). */
static void op_session_data_refused(void *ctx, const char *why, uint32_t len)
{
    (void)ctx;
    ESP_LOGW("mg.session", "refused a data message (%u bytes): %s", (unsigned)len, why);
}
#endif


/* Session writes the host task handles itself (data, credit, abort, state,
 * credentials_update's fragments): a fragment lost to a full worker queue
 * would break its message, and lost credit would stall the session. hello
 * reads NVS and goes to the worker; so does storing a credentials_update's
 * pair (store_later: session_store_pending() on the worker, outside the
 * lock). */
static bool session_on_host(uint8_t sub)
{
    return sub == mg_session_data || sub == mg_session_credit || sub == mg_session_abort || sub == mg_session_state
           || sub == mg_session_credentials_update;
}
#endif

bool mg_ble_session_open(void)
{
    if (!s_ready) {
        return false;
    }
    mg_lock();
    bool open = mg_proto_session_open(&s_proto);
    mg_unlock();
    return open;
}

bool mg_ble_session_sealed(void)
{
    if (!s_ready) {
        return false;
    }
    mg_lock();
    bool sealed = mg_proto_session_sealed(&s_proto);
    mg_unlock();
    return sealed;
}

uint32_t mg_ble_session_id(void)
{
    if (!s_ready) {
        return 0;
    }
    mg_lock();
    uint32_t id = mg_proto_session_id(&s_proto);
    mg_unlock();
    return id;
}

void mg_ble_session_reask_marks(uint32_t *hellos, uint32_t *saved)
{
    *hellos = *saved = 0;
    if (!s_ready) {
        return;
    }
    mg_lock();
    mg_proto_session_reask_marks(&s_proto, hellos, saved);
    mg_unlock();
}

bool mg_ble_session_send(uint32_t session, const uint8_t *msg, size_t len)
{
    if (!s_ready) {
        return false;
    }
    mg_lock();
    bool ok = mg_proto_session_send(&s_proto, session, msg, len);
    mg_unlock();
    if (ok) {
        kick();   /* the worker retries what a full stack holds back */
    }
    return ok;
}

size_t mg_ble_session_room(void)
{
    if (!s_ready) {
        return 0;
    }
    mg_lock();
    size_t n = mg_proto_session_room(&s_proto);
    mg_unlock();
    return n;
}

uint32_t mg_ble_session_client_max(void)
{
    if (!s_ready) {
        return 0;
    }
    mg_lock();
    uint32_t n = mg_proto_session_client_max(&s_proto);
    mg_unlock();
    return n;
}

size_t mg_ble_session_take(uint32_t session, uint8_t *out, size_t cap)
{
    if (!s_ready) {
        return 0;
    }
    mg_lock();
    size_t n = mg_proto_session_take(&s_proto, session, out, cap);
    mg_unlock();
    return n;
}

void mg_ble_session_request(bool open)
{
    if (!s_ready) {
        return;
    }
    mg_lock();
    mg_proto_session_request(&s_proto, open);
    mg_unlock();
}

size_t mg_ble_session_max_message(void)
{
#if CONFIG_MUSE_GADGET_BLE_SESSION
    return s_ready && s_proto.cfg.session ? s_session_cfg.max_message : 0;
#else
    return 0;
#endif
}

/* mg_session_cap_sealed_clear (and an unsealed token_proof clear refused) with CONFIG_MUSE_GADGET_SEALED_CLEAR,
 * off by default; CMakeLists.txt sets the layer-2 twin. */
#if CONFIG_MUSE_GADGET_SEALED_CLEAR
#define MG_SEALED_CLEAR true
#else
#define MG_SEALED_CLEAR false
#endif

/* ---- protocol ops ------------------------------------------------------- */

static void terminate_cb(void *arg)
{
    (void)arg;
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void mg_ble_forget_bonds(void)
{
    if (!s_ready) {
        return;
    }
    int rc = ble_store_clear();
    ESP_LOGW(TAG, "forgot every bonded phone (rc %d): dropping the connection", rc);
#if CONFIG_MUSE_GADGET_BLE_HID
    mg_hogp_forget_addr_records();   /* ble_store_clear() leaves them */
    hid_off_locked();   /* no host is left to drive: HID off and hidden, until a hid_map */
#endif
    terminate_cb(NULL);
}

void mg_ble_hid_off(void)
{
#if CONFIG_MUSE_GADGET_BLE_HID
    if (s_ready) {
        hid_off_locked();
        hid_status_changed();
    }
#endif
}

static bool op_notify(void *ctx, mg_ch_t ch, const uint8_t *data, size_t len)
{
    (void)ctx;
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_handle[ch]) {
        return false;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return false;
    }
    return ble_gatts_notify_custom(conn, s_handle[ch], om) == 0;
}

/*
 * The capture's outbox (mg_capq_t, mg_proto.h; on the Watcher a
 * talk press on a sleep link, its request for active unanswered, lost 156
 * frames and its stop_mic, and the phone waited 120 s for the end). A
 * capture's audio is held in PSRAM (CONFIG_MUSE_GADGET_BLE_CAPTURE_HOLD_S of
 * its format), or without PSRAM in internal RAM up to
 * CONFIG_MUSE_GADGET_BLE_CAPTURE_HOLD_INTERNAL_KB while 32 KB stays free past
 * it, and given back once everything has gone.
 */
#define CAPQ_INTERNAL_KEEP (32 * 1024)
/* Held audio leaves the stack this many msys blocks for control messages
 * (stop_mic, buffer_update, the session): audio waits in the hold buffer, not
 * in the stack's pool. */
#define CAPQ_MSYS_KEEP 4

static void *op_capq_alloc(void *ctx, size_t want, size_t *got)
{
    (void)ctx;
    void *b = heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (b) {
        *got = want;
        return b;
    }
    size_t cap = (size_t)CONFIG_MUSE_GADGET_BLE_CAPTURE_HOLD_INTERNAL_KB * 1024;
    bool whole = mg_proto_capture_whole(&s_proto);
    if (whole && CONFIG_MUSE_GADGET_BLE_CAPTURE_GUARD_INTERNAL_KB > CONFIG_MUSE_GADGET_BLE_CAPTURE_HOLD_INTERNAL_KB) {
        /* A turn held whole until it's released (by the bench guard, ptt.minrise, mg_proto_turn_hold, until
         * it rises; or for the token proof, until it matches) keeps all of itself: more of the
         * internal heap, one allocation at the capture's start (never per frame), still only while
         * CAPQ_INTERNAL_KEEP stays free. Under mg_lock. */
        cap = (size_t)CONFIG_MUSE_GADGET_BLE_CAPTURE_GUARD_INTERNAL_KB * 1024;
    }
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t room = largest > CAPQ_INTERNAL_KEEP ? largest - CAPQ_INTERNAL_KEEP : 0;
    if (cap > want) {
        cap = want;
    }
    if (cap > room) {
        cap = room;
    }
    b = cap ? heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : NULL;
    *got = b ? cap : 0;
    return b;
}

bool mg_ble_can_hold(size_t bytes)
{
    /* What op_capq_alloc would give a capture held whole (the held live turn needs MG_HELD_TURN_MIN_HOLD_MS
     * where there's no clip queue): PSRAM, else the internal cap while CAPQ_INTERNAL_KEEP stays free. */
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= bytes) {
        return true;
    }
    size_t cap = (size_t)CONFIG_MUSE_GADGET_BLE_CAPTURE_GUARD_INTERNAL_KB * 1024;
    if ((size_t)CONFIG_MUSE_GADGET_BLE_CAPTURE_HOLD_INTERNAL_KB * 1024 > cap) {
        cap = (size_t)CONFIG_MUSE_GADGET_BLE_CAPTURE_HOLD_INTERNAL_KB * 1024;
    }
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t room = largest > CAPQ_INTERNAL_KEEP ? largest - CAPQ_INTERNAL_KEEP : 0;
    return cap >= bytes && room >= bytes;
}

static void op_capq_free(void *ctx, void *buf)
{
    (void)ctx;
    free(buf);
}

static bool op_data_room(void *ctx)
{
    (void)ctx;
    return os_msys_num_free() >= CAPQ_MSYS_KEEP;
}

/* Logs an end of utterance the outbox delivered since the last call, on
 * mg.voice (the Nordic UART mirror). Under mg_lock. */
static uint32_t s_capq_ends_logged;

void mg_capture_note(void)
{
    const mg_capq_t *q = &s_proto.capq;
    if (q->ends_sent == s_capq_ends_logged) {
        return;
    }
    s_capq_ends_logged = q->ends_sent;
    char line[256];
    mg_proto_format_utterance(&s_proto, line, sizeof(line));
    ESP_LOGI("mg.voice", "%s", line);
    if (q->ctl_lost) {
        ESP_LOGW("mg.voice", "capture outbox full: %u control messages sent past it", (unsigned)q->ctl_lost);
    }
}

static void op_disconnect(void *ctx)
{
    (void)ctx;
    /* A moment later, so the error just queued goes out first. */
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = { .callback = terminate_cb, .name = "mg_dc" };
        if (esp_timer_create(&a, &t) != ESP_OK) {
            terminate_cb(NULL);
            return;
        }
    }
    esp_timer_stop(t);
    esp_timer_start_once(t, 150 * 1000);
}

static void op_capture(void *ctx, bool start, const mg_audio_format_t *fmt)
{
    (void)ctx;
    s_capture.requested = start;
    s_capture.armed_us = start ? esp_timer_get_time() : 0;
    if (fmt) {
        s_capture.fmt = *fmt;
    }
    if (start) {
        ESP_LOGI("mg.voice", "client started capture");
        if (s_platform.attention) {
            s_platform.attention();
        }
    }
}

static void op_mic_gain(void *ctx, uint8_t gain)
{
    (void)ctx;
    if (s_platform.mic_gain) {
        s_platform.mic_gain(gain);
    }
}

static void op_settings_changed(void *ctx)
{
    (void)ctx;
    save_settings(mg_proto_settings(&s_proto));
}

static uint8_t op_speaker_volume(void *ctx)
{
    (void)ctx;
    int v = s_platform.speaker_volume();
    return (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
}

static void op_set_speaker_volume(void *ctx, uint8_t volume)
{
    (void)ctx;
    s_platform.set_speaker_volume(volume);
    ESP_LOGI(TAG, "speaker volume %u%s", volume, volume ? "" : " (speaker off)");
}

static uint32_t op_now(void *ctx)
{
    (void)ctx;
    return mg_now_ms();
}

static int64_t op_mono(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time() / 1000;   /* 64-bit: the press window never comes back around */
}

/* ---- GATT ----------------------------------------------------------------- */

/*
 * The largest write a client sends: ATT_MTU - 3 bytes (mgcommands.h),
 * whatever the overlay asks. NimBLE holds only prepared writes to
 * 512 (BLE_ATT_ATTR_MAX_LEN), not plain writes or writes without response,
 * and iOS's maximumWriteValueLength(.withoutResponse) is ATT_MTU - 3, so a
 * write past 512 is taken, not dropped with ATT success (a lost session
 * fragment would break its message and its credit would never come back).
 */
#define MG_WRITE_MAX (BLE_ATT_MTU_MAX - 3)

static int mg_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    s_last_rx_us = esp_timer_get_time();   /* the last-data age a host close reports (mg_link_close.h) */
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (s_platform.muse_gatt_refused && s_platform.muse_gatt_refused()) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;   /* SAFE on the public address */
    }
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (!s_ready || !len || len > MG_WRITE_MAX || conn != s_conn) {
        return 0;
    }
    /* A GATT write alone isn't an action: streams, the mic and calls
     * hold the link fast as busy, and starts are actions of their own. */
    mg_ch_t ch = (mg_ch_t)(uintptr_t)arg;
    if (ch == MG_CH_DATA) {
        /*
         * Playback audio: straight into the ring, here on the host task, so
         * no write is ever lost to a full queue (the client's flow control
         * counts on every byte arriving) and none overtakes the Control
         * command before it. The host task is the only writer of s_rx.
         */
        static uint8_t s_rx[MG_WRITE_MAX];
        uint16_t n = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, s_rx, sizeof(s_rx), &n) == 0 && n) {
            mg_lock();
            mg_proto_write(&s_proto, ch, s_rx, n);
#if CONFIG_MUSE_GADGET_BLE_VIDEO
            bool video = mg_proto_video_active(&s_proto);
#endif
            mg_unlock();
#if CONFIG_MUSE_GADGET_BLE_VIDEO
            if (video) {
                video_data_arrived();
            }
#endif
        }
        return 0;
    }
#if CONFIG_MUSE_GADGET_BLE_SESSION
    uint8_t head[2];
    if (ch == MG_CH_CONTROL && len >= 2 && os_mbuf_copydata(ctxt->om, 0, 2, head) == 0
        && head[0] == mg_command_session && session_on_host(head[1])) {
        static uint8_t s_ctl[MG_WRITE_MAX];   /* the host task is its only writer */
        uint16_t n = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, s_ctl, sizeof(s_ctl), &n) == 0 && n) {
            mg_lock();
            mg_proto_write(&s_proto, ch, s_ctl, n);
            mg_unlock();
            if (head[1] == mg_session_credentials_update) {
                kick();   /* a whole pair is stored on the worker */
            }
        }
        return 0;
    }
#endif
    if (ch == MG_CH_CONTROL && len <= 16) {
        /*
         * change_data_type picks which stream the Data writes after it
         * belong to (video and audio playback interleaved), so it must take
         * effect between them, here on the host task like the Data writes,
         * not later on the worker.
         */
        uint8_t small[16];
        uint16_t sn = 0;
        /* So must an A/V anchor: it names the first sample of the next Data write, so it is applied
         * between the Data writes around it. On the worker it would come after them (the iOS greeting's
         * anchor, written before its audio, would be applied after the first audio write had gone into the ring,
         * and only measured, not held). It sends nothing (an error at most). And a flush
         * (mg_stream_audio_flush, barge-in): queued to the worker it ran after the next reply's first Data
         * writes and dropped them (the reply started clipped); here it applies between them, and its
         * buffer_update goes from the worker (mg_proto.c flush_update_owed). */
        if (ble_hs_mbuf_to_flat(ctxt->om, small, sizeof(small), &sn) == 0 && sn &&
            (small[0] == mg_command_change_data_type ||
             (sn >= 2 && small[0] == mg_command_stream_audio &&
              (small[1] == mg_stream_audio_video_anchor || small[1] == mg_stream_audio_flush)))) {
            mg_lock();
            mg_proto_write(&s_proto, ch, small, sn);
            bool owed = s_proto.flush_update_owed;
            mg_unlock();
            if (owed) {
                kick();   /* the flush's buffer_update goes from the worker */
            }
            return 0;
        }
    }
    uint8_t *buf = malloc(len);
    if (!buf) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &n) != 0) {
        free(buf);
        return BLE_ATT_ERR_UNLIKELY;
    }
    ev_t ev = { .type = EV_WRITE, .ch = (uint8_t)(uintptr_t)arg, .len = n, .data = buf, .conn_gen = s_conn_gen };
    if (xQueueSend(s_events, &ev, 0) != pdTRUE) {
        free(buf);   /* the worker is swamped; like a lost write-without-response */
    }
    return 0;
}

static int battery_level(void)
{
    int pct = s_platform.battery_pct ? s_platform.battery_pct() : -1;
    return pct < 0 ? 100 : pct > 100 ? 100 : pct;
}

static int bas_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t level = (uint8_t)battery_level();
    return os_mbuf_append(ctxt->om, &level, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

enum { DIS_MANUFACTURER, DIS_MODEL, DIS_FIRMWARE, DIS_SOFTWARE };

static int dis_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    const char *s = "";
    switch ((int)(uintptr_t)arg) {
    case DIS_MANUFACTURER: s = CONFIG_MUSE_GADGET_BLE_MANUFACTURER; break;
    case DIS_MODEL: s = s_platform.model && s_platform.model() ? s_platform.model() : CONFIG_IDF_TARGET; break;
    case DIS_FIRMWARE: {
        s = esp_app_get_description()->version;
        if (s_platform.safe_mode) {
            /* SAFE shows in the app's device info. Build metadata (after '+')
             * doesn't change a version's precedence, so update checks don't. */
            static char safe[48];
            snprintf(safe, sizeof(safe), "%s%ssafe", s, strchr(s, '+') ? "." : "+");
            s = safe;
        }
        break;
    }
    case DIS_SOFTWARE: s = "mg" STR(MG_SPEC_VERSION); break;
    }
    return os_mbuf_append(ctxt->om, s, strlen(s)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static void nus_command(char *line);

static int nus_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    s_last_rx_us = esp_timer_get_time();
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    char line[64];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, line, sizeof(line) - 1, &n) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    line[n] = '\0';
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[--n] = '\0';
    }
    if (s_ready) {
        bool open_read = !strcmp(line, "status") || !strcmp(line, "version") || !strcmp(line, "guard?") ||
                         !strcmp(line, "ota?");
        if (open_read) {
            s_nus_reply_task = xTaskGetCurrentTaskHandle();   /* the reply goes back to this connection */
        }
        nus_command(line);
        s_nus_reply_task = NULL;
    }
    return 0;
}

#if CONFIG_MUSE_GADGET_BLE_SMP
/* ---- firmware update: the MCUmgr SMP service (SMP core, smp/) ---- */
static const ble_uuid128_t SMP_SVC_UUID = BLE_UUID128_INIT(MG_SMP_SERVICE_UUID_LE_BYTES);
static const ble_uuid128_t SMP_CHR_UUID = BLE_UUID128_INIT(MG_SMP_CHAR_UUID_LE_BYTES);
static uint16_t s_smp_handle;

/*
 * The core runs on the worker (flash writes stay off the host task), inline:
 * one request at a time in one slot. Requests are at most SMP_BUF bytes
 * (mcumgr_params' buf_size): 512, the largest attribute value GATT allows,
 * because the Android app cuts each request into writes of MTU - 3 bytes and
 * Android refuses a write over 512 (at MTU 517 a larger request's first
 * write is 514 bytes: the update fails at once). The client's window
 * lives in a FIFO between the host task and the worker that holds whole ATT
 * writes: room for SMP_WINDOW requests even in 20-byte writes (4 bytes of
 * length apiece), so mcumgr_params can say buf_count 3 (Android's window)
 * and mean it. A write that still finds no room after SMP_FIFO_WAIT_MS (a
 * client past its window) is counted and lost; the core resyncs on the next
 * request and the client's timeout recovers.
 */
#define SMP_BUF 512                          /* buf_size: the largest request (GATT's attribute maximum) */
#define SMP_SLOT 1024                        /* the core's slot: a request, or the image state response */
/* A request the front can't hold is one the core answers EMSGSIZE from its header alone (larger than the slot), so the
 * front hands it just the header and drops the rest (mg_smp_front.h). */
_Static_assert(SMP_SLOT == MG_SMP_FRONT_MAX, "the request front holds what the core's slot does");
#define SMP_WINDOW 3                         /* buf_count */
#define SMP_WRITE_MAX 520                    /* the largest ATT write (MTU 517 less 3, and some) */
/* The ring: the window in 20-byte writes (2 bytes of length apiece), and two whole writes more. */
#define SMP_FIFO (SMP_WINDOW * (SMP_BUF + 26 * 2) + 2 * (SMP_WRITE_MAX + 3))
#define SMP_FIFO_WAIT_MS 300
#define SMP_HEALTHY_FLOOR_US ((int64_t)MUSE_BOOTGUARD_HEALTHY_MS * 1000LL)

/*
 * Memory (no idle memory, and update support never takes memory a feature
 * needs): nothing of the server's is held between updates. The SMP core
 * (smp-v3) keeps one pointer and takes its state and its request slot
 * through mem_get/mem_put only while an update runs (and the state alone for
 * resume_hold_s after a link drop mid-upload). The glue adds one block, the
 * FIFO (the window, the host task's and the worker's write buffers), made by
 * the host task at the first SMP write and freed by the worker once the link
 * is gone and it's empty; the session's bookkeeping (lost writes, the
 * reset's notification count) lives in it too, and so does the request front's
 * reassembly (MG_SMP_FRONT_MAX: an update change on a link that needs the
 * confirm press is asked about whole, mg_smp_front.h). A small slot tracker
 * (mg_smp_front_slot_t: the bound upload's running SHA-256) is made at the first
 * change the press is asked for and freed with the FIFO block unless a press
 * still binds an upload. All of it from PSRAM where the
 * board has it (the internal heap only on the C6, which has none). The host
 * task only creates the FIFO block; the worker only frees it, under mg_lock()
 * with no host-side user (users), so neither side sees it go mid-use. The
 * FIFO is a ring of length-prefixed writes in flash code (not a FreeRTOS
 * message buffer, whose functions ESP-IDF places in IRAM: 246 B of base
 * memory on the C6); one producer (the host task), one consumer
 * (the worker), its indices under mg_lock(). What stays static: two
 * pointers, the link generation and three flags.
 */
typedef struct {
    mg_fifo_t idx;                   /* ring[]'s writes in and out (mg_fifo.h, under mg_lock) */
    uint8_t users;                   /* host-side uses in progress (under mg_lock) */
    bool reset_pending;
    uint32_t drops;                  /* writes lost on this link */
    uint32_t tx_queued, tx_done, reset_at;   /* an OS reset waits for its response to leave */
    uint8_t in[1 + SMP_WRITE_MAX];   /* host task: link generation + one write */
    uint8_t out[1 + SMP_WRITE_MAX];  /* worker: one write out of the FIFO */
    mg_smp_front_rx_t front;         /* worker: the request the front is putting together (mg_smp_front.h) */
    uint8_t ring[SMP_FIFO];
} smp_fifo_mem_t;

/* Host task: one write in, if there's room for it now. */
static bool fifo_push(smp_fifo_mem_t *f, const uint8_t *msg, uint16_t n)
{
    mg_lock();
    bool ok = mg_fifo_push(&f->idx, f->ring, SMP_FIFO, msg, n);
    mg_unlock();
    return ok;
}

/* Worker: the oldest write out: its length, 0 for an empty write, -1 when
 * the FIFO is empty (longer than cap can't happen: writes are at most SMP_WRITE_MAX + 1; dropped). */
static int fifo_pop(smp_fifo_mem_t *f, uint8_t *out, size_t cap)
{
    mg_lock();
    int n = mg_fifo_pop(&f->idx, f->ring, SMP_FIFO, out, cap);
    mg_unlock();
    return n;
}

static smp_fifo_mem_t *volatile s_smp_fifo;    /* host creates, worker frees */
static mg_smp_front_slot_t *s_smp_slot;        /* worker: the slot the press's upload writes (mg_smp_front.h) */
static mg_smp_front_up_t *s_smp_up;            /* worker: the length of the upload the core takes (while it runs) */
static bool s_smp_front_ok;                    /* worker: the front let the request the core is reading through */
static const mg_smp_env_t *s_smp_env;          /* the env the boot probe picked; NULL = off */
static volatile uint8_t s_smp_gen;             /* +1 at every disconnect (host task) */
static uint8_t s_smp_seen_gen;                 /* worker */
#define SMP_HEALTHY 0x01u                      /* s_smp_marks (worker): the boot's healthy mark seen */
#define SMP_MARKED 0x02u                       /* ... and the core told of it, the keep done (bits 2-7: its retries,
                                                * mg_smp_esp_keep_due) */
static uint8_t s_smp_marks;
static bool s_smp_fallback;                    /* host task: no memory for the FIFO this boot */

static void *smp_alloc(size_t n)
{
    void *p = heap_caps_aligned_calloc(8, 1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_aligned_calloc(8, 1, n, MALLOC_CAP_8BIT);
}

/* Host task: the FIFO block, made if need be, held until smp_fifo_give(). */
static smp_fifo_mem_t *smp_fifo_take(void)
{
    mg_lock();
    smp_fifo_mem_t *f = s_smp_fifo;
    if (f) {
        f->users++;
    }
    mg_unlock();
    if (f || s_smp_fallback) {
        return f;
    }
    smp_fifo_mem_t *n = smp_alloc(sizeof(*n));   /* zeroed: an empty ring */
    if (!n) {
        heap_caps_free(n);
        s_smp_fallback = true;   /* for the rest of the boot */
        ESP_LOGW(TAG, "SMP: no memory for the write FIFO and its request front: firmware updates refused this boot");
        return NULL;
    }
    n->users = 1;
    mg_lock();
    s_smp_fifo = n;   /* only this task creates it, and it was NULL */
    mg_unlock();
    return n;
}

static void smp_fifo_give(smp_fifo_mem_t *f)
{
    mg_lock();
    f->users--;
    mg_unlock();
}

static int smp_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    s_last_rx_us = esp_timer_get_time();
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || !s_smp_env) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    uint16_t n = 0;
    if (len > SMP_WRITE_MAX) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    link_action("firmware update");   /* link modes: active */
    smp_fifo_mem_t *f = smp_fifo_take();
    if (!f) {
        /* No memory for the FIFO block, so none for the request front either: nothing reaches the core unchecked
         * (mg_smp_front.h, every request's nesting; the confirm press), so the write is refused. */
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    f->in[0] = s_smp_gen;
    if (ble_hs_mbuf_to_flat(ctxt->om, f->in + 1, len, &n) != 0) {
        f->drops++;
        smp_fifo_give(f);
        return BLE_ATT_ERR_UNLIKELY;
    }
    /* Room for the client's whole window; a client past it waits a little. */
    for (uint32_t waited = 0; !fifo_push(f, f->in, (uint16_t)(1 + n)); waited += 2) {
        if (waited >= SMP_FIFO_WAIT_MS) {
            f->drops++;   /* the core resyncs, the client resends */
            break;
        }
        kick();
        vTaskDelay(pdMS_TO_TICKS(2) ? pdMS_TO_TICKS(2) : 1);
    }
    smp_fifo_give(f);
    kick();
    return 0;
}

/* SMP OS reset (restart-to-connected, PERFORMANCE.md): restart 100 ms after
 * the stack reports the response's notifications sent (BLE_GAP_EVENT_
 * NOTIFY_TX), not after a blind 500 ms; 500 ms is the fallback when no such
 * event comes. */
#define SMP_RESET_AFTER_TX_MS 100
#define SMP_RESET_FALLBACK_MS 500
static void smp_reset_arm(uint32_t ms)
{
    restart_after(ms, MUSE_RESTART_OTA);   /* closes the link cleanly first */
}

/* The core's orderly reset: its last notification went to the stack just before. */
static void smp_reset(void *ctx)
{
    (void)ctx;
    smp_fifo_mem_t *f = s_smp_fifo;   /* worker: only it frees the block */
    bool sent = false;
    if (f) {
        mg_lock();
        f->reset_at = f->tx_queued;
        f->reset_pending = true;
        sent = f->tx_done >= f->reset_at;
        mg_unlock();
    }
    smp_reset_arm(sent ? SMP_RESET_AFTER_TX_MS : SMP_RESET_FALLBACK_MS);
}

static void smp_notify_tx(uint16_t attr)
{
    if (attr != s_smp_handle) return;
    bool arm = false;
    mg_lock();
    smp_fifo_mem_t *f = s_smp_fifo;
    if (f) {
        f->tx_done++;
        if (f->reset_pending && f->tx_done >= f->reset_at) {
            f->reset_pending = false;
            arm = true;
        }
    }
    mg_unlock();
    if (arm) {
        smp_reset_arm(SMP_RESET_AFTER_TX_MS);
    }
}

/* One notification of at most MTU - 3 bytes. A full TX queue is waited out
 * here (up to a second) rather than handed back as MG_SMP_EAGAIN: the core
 * runs inline in one slot, and a response left half-sent would hold it. */
static int smp_send(void *ctx, const uint8_t *buf, uint16_t n)
{
    (void)ctx;
    uint16_t conn = s_conn;
    for (int tries = 0;; tries++) {
        if (conn == BLE_HS_CONN_HANDLE_NONE || s_conn != conn) {
            return MG_SMP_EUNKNOWN;   /* the link went: the response goes with it */
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, n);
        if (om) {
            int rc = ble_gatts_notify_custom(conn, s_smp_handle, om);   /* takes om either way */
            if (rc == 0) {
                if (s_smp_fifo) {
                    mg_lock();
                    s_smp_fifo->tx_queued++;   /* worker: the block can't go meanwhile */
                    mg_unlock();
                }
                return 0;
            }
            if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) {
                return MG_SMP_EUNKNOWN;
            }
        }
        if (tries >= 100) {
            return MG_SMP_EUNKNOWN;
        }
        /* Up to 1 s per notification on the worker, whoever the peer: the watchdog stays fed, so a central
         * that doesn't take its replies (no proof needed for SMP) can't time it out. */
        muse_wdt_feed(MUSE_WDT_MG_WORKER);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static uint16_t smp_mtu(void *ctx)
{
    (void)ctx;
    uint16_t conn = s_conn;
    uint16_t mtu = conn != BLE_HS_CONN_HANDLE_NONE ? ble_att_mtu(conn) : 0;
    /* The core sends MTU - 3 per notification: no more than an attribute value
     * may hold (MG_MAX_ATT_VALUE, mgcommands.h), whatever the link's MTU. */
    if (mtu > MG_MAX_ATT_VALUE + 3) {
        mtu = MG_MAX_ATT_VALUE + 3;
    }
    return mtu >= 23 ? mtu : 23;
}

/* One OTA at a time: the BLE-only builds have no other front end (no Wi-Fi
 * OTA); the claim is held through the commit until the reset. */
static bool smp_claim(void *ctx, bool claim)
{
    (void)ctx;
    (void)claim;   /* nothing else writes OTA here */
    return true;
}

/* Stop playback, video and a client's capture before OTA data (shared
 * buffers). The apps download offline notes first and stream nothing
 * during an update; this covers anything else. */
static void smp_media_stop(void *ctx)
{
    (void)ctx;
    mg_lock();
    if (mg_play_active(&s_play)) {
        mg_play_stop(&s_play, false);
        s_play.state = MG_PLAY_DONE;   /* notifies stop_streaming drop */
        mg_proto_stream_finished(&s_proto);
    }
    mg_proto_video_stop(&s_proto);
    s_capture.requested = false;
    mg_unlock();
}

/* The boot's healthy mark: the boot guard's (reachable and 30 s without
 * a reset). It gates nothing (who may update is smp_may_change's). It only times the confirm,
 * which the core does: a client's confirm takes effect here, and a test
 * image that gets here confirms itself, phone or not. A test image that
 * never gets here is reset by the boot guard and the bootloader goes back
 * to the old one. */
static bool smp_healthy(void *ctx)
{
    (void)ctx;
    /* The mark, asked only until it came (boot_healthy may read flash: device.ota's record). */
    bool mark = !(s_smp_marks & SMP_HEALTHY) &&
                (s_platform.boot_healthy ? s_platform.boot_healthy() : muse_bootguard_is_healthy());
    if (!(s_smp_marks & SMP_HEALTHY) && mark && esp_timer_get_time() >= SMP_HEALTHY_FLOOR_US) {
        s_smp_marks |= SMP_HEALTHY;
        mg_smp_esp_settle();
        ESP_LOGI(TAG, "SMP: healthy mark%s", mg_smp_esp_pending_verify() ? ": this updated image confirms itself" : "");
    }
    return (s_smp_marks & SMP_HEALTHY) != 0;
}

/* The core's state and request slot, while an update runs (no idle memory). */
static void *smp_mem_get(void *ctx, uint8_t kind, uint32_t size)
{
    (void)ctx;
    (void)kind;
    return smp_alloc(size);
}

static void smp_mem_put(void *ctx, uint8_t kind, void *block)
{
    (void)ctx;
    heap_caps_free(block);
    /* No update open: the other slot is free for a Wi-Fi OTA. A committed one keeps it until the reset (the
     * core's commit mark), so nothing overwrites the image selected for boot. */
    if (kind == MG_SMP_MEM_STATE && !mg_smp_upload_active()) {
        mg_smp_esp_session_end();
        heap_caps_free(s_smp_up);   /* no upload: no length to check against */
        s_smp_up = NULL;
    }
}

static void smp_wake(void *ctx)
{
    (void)ctx;
    kick();
}

static uint32_t smp_now_ms(void *ctx)
{
    (void)ctx;
    return mg_now_ms();
}

static void smp_log(void *ctx, const char *line)
{
    (void)ctx;
    ESP_LOGI("mg.smp", "%s", line);
}

/* mg_smp_gate.h, protocols/test-vectors/mg-update-v2.json: an upload, state
 * write, reset or erase runs at once for a connection whose token proof
 * matched, in a SAFE the user entered with the key, and on bench, automation
 * and test builds; from any other link, set up or not, it asks for ONE confirm
 * press on the gadget (muse_local_press_ask: "Press to confirm update", only a
 * press during it counts; the wired console's update.arm counts as it). The
 * request front asks about each change whole, before the core sees it
 * (mg_smp_front.h: the core asks its hooks only at a transfer's start): rc 10
 * while it asks (the client retries), rc 11 for what can't be asked (an upload
 * naming no SHA-256, a chunk with no upload started on its link), once the
 * prompt timed out or was cancelled, or for a write that isn't the bound
 * image's; and in a SAFE the gadget entered itself (no way to prompt). No
 * phone. Reads never ask. */
#if MG_NO_PERSON   /* no person in the loop: bench, automation and test images */
#define SMP_RELEASE false   /* bench, automation and test builds: no run waits on a person */
#else
#define SMP_RELEASE true
#endif
static int s_smp_refusal = MG_SMP_EACCESSDENIED;   /* the last refusal's rc (the SMP worker's alone) */

static mg_smp_gate_t smp_gate_now(void)
{
    bool safe = s_platform.safe_mode || s_platform.recovery_mode;
    mg_lock();
    /* This connection's matched token proof, never owner_ok: that is true on a gadget that isn't set up, and the
     * press is asked for set up or not (mgcommands.h, Token proof). Forget ends it with the token. */
    bool ok = !safe && !s_smp_forgot && mg_proto_proof_matched(&s_proto);
    mg_unlock();
    return mg_smp_gate(safe, s_platform.safe_updates_open, ok, SMP_RELEASE);
}

static const char *smp_op_name(uint8_t op)
{
    return op == MG_SMP_OP_UPLOAD ? "upload" : op == MG_SMP_OP_STATE ? "image state write"
           : op == MG_SMP_OP_RESET ? "reset" : "erase";
}

/* The core's hooks: what the front let through goes on; a change that didn't come through it (the fail-soft path, a
 * request too large to hold) is refused on a link that needs the press. */
static bool smp_gate_ask(uint8_t op)
{
    mg_smp_gate_t g = smp_gate_now();
    if (g == MG_SMP_GATE_OPEN || (g == MG_SMP_GATE_NEEDS_PRESS && s_smp_front_ok)) {
        return true;
    }
    s_smp_refusal = MG_SMP_EACCESSDENIED;
    if (g == MG_SMP_GATE_CLOSED) {
        ESP_LOGW(TAG, "SMP: %s refused: a SAFE the gadget entered itself can't ask for the confirm press: hold "
                      "the SAFE key 10 s right after boot, or update over USB", smp_op_name(op));
    } else {
        ESP_LOGW(TAG, "SMP: %s refused: it didn't come through the confirm press's request front", smp_op_name(op));
    }
    return false;
}

static bool smp_may_change(void *ctx, uint8_t op)
{
    (void)ctx;
    return smp_gate_ask(op);
}

static bool smp_may_upload(void *ctx, uint32_t len, const uint8_t *sha32)
{
    (void)ctx;
    (void)len;
    (void)sha32;   /* the front read the upload itself */
    return smp_gate_ask(MG_SMP_OP_UPLOAD);
}

static int smp_refusal_rc(void *ctx)
{
    (void)ctx;
    return s_smp_refusal;
}

static muse_update_answer_t smp_front_ask(void *ctx, const muse_update_req_t *req)
{
    (void)ctx;
    return (muse_update_answer_t)s_platform.local_press_ask(req);
}

static bool smp_front_bound(void *ctx, muse_update_ident_t *id)
{
    (void)ctx;
    return muse_local_press_bound(id);
}

/* One whole request (worker): every request nested deeper than MG_SMP_FRONT_MAX_DEPTH, or one the walk can't read
 * to its end (mg_smp_front_depth_ok fails closed), is refused before any decoder sees it (the core's own skip is
 * bounded too, smp/mg_smp.c cr_skip_body); on a link that needs the press,
 * the front asks about an update change before the core sees it, and answers a refusal itself. */
static void smp_front_whole(void *ctx, const uint8_t *req, size_t n)
{
    (void)ctx;
    if (!mg_smp_front_depth_ok(req, n)) {
        uint8_t r[13];
        ESP_LOGW(TAG, "SMP: a request nested deeper than %d levels, or malformed, refused (rc %d)",
                 MG_SMP_FRONT_MAX_DEPTH, MG_SMP_EINVAL);
        (void)smp_send(NULL, r, (uint16_t)mg_smp_front_refusal(req, MG_SMP_EINVAL, r));
        return;
    }
    /* An upload write past its upload's length: rc 11 and the upload ends, however the link was opened (smp-v12; the
     * vendored smp-v11 core would drop the extra bytes and answer rc 0). */
    bool past = mg_smp_front_past_len(req, n, s_smp_up, mg_smp_upload_active());
    bool ended = false;
    int rc = 0;
    bool gated = smp_gate_now() == MG_SMP_GATE_NEEDS_PRESS && s_platform.local_press_ask;
    if (gated) {
        if (!s_smp_slot) {
            s_smp_slot = smp_alloc(sizeof(*s_smp_slot));   /* none: no last chunk can be the bound image's */
        }
        rc = mg_smp_front_gate(req, n, s_smp_slot, mg_smp_upload_active(), past, smp_front_ask, smp_front_bound, NULL,
                               &ended);
    } else if (past) {
        rc = MG_SMP_EACCESSDENIED;
    }
    if (ended || (past && rc == MG_SMP_EACCESSDENIED)) {
        mg_smp_abort(s_smp_env);   /* the binding ended, or a write past the length: the upload goes */
    }
    if (rc > 0) {
        uint8_t r[13];
        ESP_LOGW(TAG, "SMP: an update change %s (rc %d)",
                 rc == MG_SMP_EBUSY ? "waits for the confirm press on the gadget"
                 : past ? "refused: it writes past its upload's length" : "refused", rc);
        (void)smp_send(NULL, r, (uint16_t)mg_smp_front_refusal(req, rc, r));
        return;
    }
    if (mg_smp_front_up_note(req, n, s_smp_up) && !s_smp_up) {
        s_smp_up = smp_alloc(sizeof(*s_smp_up));   /* an upload starts: freed with the core's state (smp_mem_put) */
        (void)mg_smp_front_up_note(req, n, s_smp_up);
    }
    s_smp_front_ok = gated && rc == 0;
    mg_smp_rx(s_smp_env, req, (uint16_t)n);
    s_smp_front_ok = false;
}

static const mg_smp_config_t s_smp_cfg = {
    .slot_size = SMP_SLOT,
    .buf_size = SMP_BUF,
    .rx_slots = 1,
    .buf_count = SMP_WINDOW,
    .inline_rx = 1,
    .resume_hold_s = 120,   /* fleet value: an upload's state waits this long for a resume */
    .be_scratch = MG_SMP_ESP_SCRATCH,   /* the backend's open OTA session, with the state */
};

static const mg_smp_platform_t s_smp_pf = {
    .send = smp_send,
    .mtu = smp_mtu,
    .record_get = mg_smp_esp_record_get,
    .record_put = mg_smp_esp_record_put,
    .running_id = mg_smp_esp_running_id,
    .reset = smp_reset,
    .ota_claim = smp_claim,
    .media_stop = smp_media_stop,
    .boot_healthy = smp_healthy,
    .may_change = smp_may_change,
    .may_upload = smp_may_upload,
    .refusal_rc = smp_refusal_rc,
    .mem_get = smp_mem_get,
    .mem_put = smp_mem_put,
    .wake = smp_wake,
    .now_ms = smp_now_ms,
    .log = smp_log,
};

/* One env per backend: the boot probe picks whether this unit can revert. */
static const mg_smp_env_t s_smp_env_revert = { &s_smp_cfg, &s_smp_pf, &mg_smp_esp_backend_revert };
static const mg_smp_env_t s_smp_env_permanent = { &s_smp_cfg, &s_smp_pf, &mg_smp_esp_backend_permanent };

/* Worker: free the FIFO block once the link is gone and it's empty. */
static void smp_release_fifo(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    if (!s_smp_fifo) {
        return;   /* only the host task makes one, and only on a link */
    }
    mg_lock();
    smp_fifo_mem_t *f = s_smp_fifo;
    if (f->users || !mg_fifo_empty(&f->idx)) {
        f = NULL;
    } else {
        s_smp_fifo = NULL;
    }
    mg_unlock();
    if (f) {
        if (f->drops) {
            ESP_LOGW(TAG, "SMP: %u writes lost on the last link", (unsigned)f->drops);
        }
        heap_caps_free(f);
        muse_update_ident_t id;
        if (s_smp_slot && !muse_local_press_bound(&id)) {
            heap_caps_free(s_smp_slot);   /* no press binds an upload: nothing to resume */
            s_smp_slot = NULL;
        }
    }
}

static void smp_front_whole(void *ctx, const uint8_t *req, size_t n);

static void smp_front_raw(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    mg_smp_rx(s_smp_env, p, (uint16_t)n);   /* a header too large to hold, alone: the core answers it (EMSGSIZE) */
}

static const mg_smp_front_out_t s_smp_front_out = { smp_front_whole, smp_front_raw, NULL };

/* One write, tagged with its link's generation, through the request front (rx: its reassembly, in the FIFO block)
 * into the core (worker). */
static void smp_feed(mg_smp_front_rx_t *rx, const uint8_t *msg, size_t n)
{
    if (n < 1) {
        return;
    }
    if (msg[0] != s_smp_seen_gen) {
        if (msg[0] != s_smp_gen) {
            return;   /* written on a link that has gone since */
        }
        s_smp_seen_gen = msg[0];   /* a link newer than the round's start */
        mg_smp_disconnect(s_smp_env);
        mg_smp_front_rx_reset(rx);
    }
    mg_smp_front_rx(rx, msg + 1, n - 1, mg_now_ms(), &s_smp_front_out);
}

/* On the worker, each round: the link's generation, the queued writes, the
 * healthy mark (an event for the core, once), the core's own work, and the
 * FIFO block back when it's done. */
static void smp_service(void)
{
    if (!s_smp_env) {
        return;
    }
    uint8_t g = s_smp_gen;
    if (g != s_smp_seen_gen) {
        s_smp_seen_gen = g;
        mg_smp_disconnect(s_smp_env);   /* a partial request goes; an upload resumes */
        if (s_smp_fifo) {
            mg_smp_front_rx_reset(&s_smp_fifo->front);
        }
        if (s_restart_why != MUSE_RESTART_OTA) {
            mg_smp_esp_ui_session_gone();   /* the link went, no reset coming: "Installing" stops, saying what's installed */
        }
    }
    smp_fifo_mem_t *f = s_smp_fifo;   /* only this task frees it */
    int n;
    while (f && (n = fifo_pop(f, f->out, sizeof(f->out))) >= 0) {
        smp_feed(&f->front, f->out, (size_t)n);
    }
    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (!(s_smp_marks & SMP_MARKED) && smp_healthy(NULL) && mg_smp_esp_keep_due(s_smp_marks, up_s)) {
        int rc = mg_smp_healthy(s_smp_env);   /* confirms a running test image */
        if (rc != MG_SMP_ENOMEM && rc != MG_SMP_EBUSY) {   /* else again next round */
            if (!mg_smp_esp_keep()) {
                s_smp_marks |= SMP_MARKED;   /* kept, or nothing to keep */
            } else if (mg_smp_esp_keep_spent(s_smp_marks)) {
                s_smp_marks |= SMP_MARKED;
                ESP_LOGE(TAG, "SMP: the keep's writes failed %u times (logged above): given up for this boot",
                         (unsigned)MG_SMP_ESP_KEEP_TRIES + 1);
            } else {
                s_smp_marks = mg_smp_esp_keep_tried(s_smp_marks, up_s);   /* a failed write: again in a few s */
            }
        }
    }
    mg_smp_poll(s_smp_env);   /* returns at once while the core holds nothing */
    smp_release_fifo();
}

static void smp_start(void)
{
    /* The backend probes the unit once (this boot's OTA state says whether
     * its bootloader can roll back); nothing is allocated here. */
    const mg_smp_env_t *env = mg_smp_esp_probe() ? &s_smp_env_revert : &s_smp_env_permanent;
    if (mg_smp_check(env) != MG_SMP_EOK) {
        ESP_LOGE(TAG, "SMP server: bad configuration");
        return;
    }
    s_smp_env = env;
    ESP_LOGI(TAG, "firmware update over BLE (SMP) on%s",
             mg_smp_esp_pending_verify() ? ": this updated image confirms itself at the healthy mark" : "");
}

/* Const, so the table stays in flash (no RAM for update support
 * while no update runs). */
#define SMP_SVC_DEF                                                                         \
    {                                                                                       \
        .type = BLE_GATT_SVC_TYPE_PRIMARY,                                                  \
        .uuid = &SMP_SVC_UUID.u,                                                            \
        .characteristics = (const struct ble_gatt_chr_def[]){                               \
            {                                                                               \
                .uuid = &SMP_CHR_UUID.u,                                                    \
                .access_cb = smp_access,                                                    \
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY, \
                .val_handle = &s_smp_handle,                                                \
            },                                                                              \
            { 0 },                                                                          \
        },                                                                                  \
    }
#if CONFIG_MUSE_GADGET_BLE_HID
static const struct ble_gatt_svc_def s_smp_svc[] = { SMP_SVC_DEF, { 0 } };
#endif
#endif

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &CONTROL_UUID.u,
                .access_cb = mg_access,
                .arg = (void *)(uintptr_t)MG_CH_CONTROL,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_handle[MG_CH_CONTROL],
            },
            {
                .uuid = &DATA_UUID.u,
                .access_cb = mg_access,
                .arg = (void *)(uintptr_t)MG_CH_DATA,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_handle[MG_CH_DATA],
            },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(MG_BAS_SERVICE_UUID16),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(MG_BAS_BATTERY_LEVEL_UUID16),
                .access_cb = bas_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_bas_handle,
            },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(MG_DIS_SERVICE_UUID16),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(MG_DIS_MANUFACTURER_NAME_UUID16), .access_cb = dis_access,
              .arg = (void *)DIS_MANUFACTURER, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(MG_DIS_MODEL_NUMBER_UUID16), .access_cb = dis_access,
              .arg = (void *)DIS_MODEL, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(MG_DIS_FIRMWARE_REVISION_UUID16), .access_cb = dis_access,
              .arg = (void *)DIS_FIRMWARE, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(MG_DIS_SOFTWARE_REVISION_UUID16), .access_cb = dis_access,
              .arg = (void *)DIS_SOFTWARE, .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &NUS_RX_UUID.u, .access_cb = nus_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &NUS_TX_UUID.u, .access_cb = nus_access, .flags = BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_nus_tx_handle },
            { 0 },
        },
    },
#if CONFIG_MUSE_GADGET_BLE_SMP && !CONFIG_MUSE_GADGET_BLE_HID
    SMP_SVC_DEF,   /* last, so no earlier handle moves (append-only); in flash, no copy */
#endif
    { 0 },
};

const struct ble_gatt_svc_def *mg_ble_services(void)
{
#if CONFIG_MUSE_GADGET_BLE_HID
    /* The HID and SMP services after ours, in that order, so no handle of an
     * earlier service moves (the GATT layout is append-only). */
    static struct ble_gatt_svc_def all[sizeof(s_svcs) / sizeof(s_svcs[0]) + 2];
    if (!all[0].type) {
        size_t n = sizeof(s_svcs) / sizeof(s_svcs[0]) - 1;
        memcpy(all, s_svcs, n * sizeof(s_svcs[0]));
        all[n++] = mg_hogp_service()[0];
#if CONFIG_MUSE_GADGET_BLE_SMP
        all[n++] = s_smp_svc[0];
#endif
    }
    return all;
#else
    return s_svcs;
#endif
}

uint8_t mg_ble_update_ui(uint8_t *pct)
{
#if CONFIG_MUSE_GADGET_BLE_SMP
    uint8_t ui = (uint8_t)mg_smp_esp_ui(pct);
    if (!ui && muse_local_press_prompting()) {
        *pct = 0;
        return MG_BLE_UPDATE_UI_ASK;   /* the confirm prompt */
    }
    return ui;
#else
    *pct = 0;
    return 0;
#endif
}

void mg_ble_update_ui_shown(uint8_t state)
{
#if CONFIG_MUSE_GADGET_BLE_SMP
    if (state == MG_BLE_UPDATE_UI_ASK) {
        return;   /* the prompt isn't the core's */
    }
    s_update_ui_attached = true;
    mg_smp_esp_ui_shown((mg_smp_ui_t)state);
#else
    (void)state;
#endif
}

const char *mg_ble_update_stop_text(void)
{
#if CONFIG_MUSE_GADGET_BLE_SMP
    return mg_smp_esp_stop_text(mg_smp_esp_ui_installed());
#else
    return "Nothing was installed";
#endif
}

void mg_ble_update_ui_clear(void)
{
#if CONFIG_MUSE_GADGET_BLE_SMP
    mg_smp_esp_ui_clear();
#endif
}

void mg_ble_ota_log(void)
{
#if CONFIG_MUSE_GADGET_BLE_SMP
    char line[160];
    mg_smp_esp_status(line, sizeof(line));
    ESP_LOGI("mg.smp", "%s", line);
    mg_smp_esp_status_bootloader(line, sizeof(line));
    ESP_LOGI("mg.smp", "%s", line);
    mg_lock();
    unsigned fifo = s_smp_fifo ? (unsigned)sizeof(smp_fifo_mem_t) : 0, lost = s_smp_fifo ? (unsigned)s_smp_fifo->drops : 0;
    mg_unlock();
    ESP_LOGI("mg.smp", "ota: SMP holds %u B now; %u writes lost on this link; boot %s",
             (unsigned)(s_smp_env ? mg_smp_held(s_smp_env) : 0) + fifo, lost,
             (s_smp_marks & SMP_HEALTHY) ? "healthy" : "not healthy yet (a confirm takes effect at the healthy mark)");
#else
    ESP_LOGI("mg.smp", "ota: no firmware updates over BLE in this build");
#endif
}

void mg_ble_configure_host(void)
{
#if CONFIG_MUSE_GADGET_BLE_HID
    mg_hogp_configure_host();   /* bonding, as the device session needs too */
#elif CONFIG_MUSE_GADGET_BLE_SESSION
    /* The device session's bonding (Just Works, as for the HID) with both
     * sides' identity keys, so a phone on a private address finds its bond
     * again after a restart. No hello depends on it: only a sealed one is
     * answered. */
    if (!ble_hs_cfg.sm_mitm) {
        ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    }
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    if (!ble_hs_cfg.store_read_cb) {
        ble_store_config_init();
    }
#endif
}

const uint8_t *mg_ble_adv_uuid128(void)
{
    return ADV_UUID;
}

uint16_t mg_ble_adv_uuid16(void)
{
#if CONFIG_MUSE_GADGET_BLE_HID
    return s_hid && mg_hogp_visible() ? 0x1812 : 0;   /* HID on: phones' Bluetooth settings list an input device */
#else
    return 0;
#endif
}

uint16_t mg_ble_adv_appearance(void)
{
#if CONFIG_MUSE_GADGET_BLE_HID
    return s_hid && mg_hogp_visible() ? 0x03C0 : 0;   /* generic HID */
#else
    return 0;
#endif
}

/* The worker's heartbeat, where it feeds its watchdog (at least once a
 * second). Missed for 5 s: stalled. */
#define WORKER_STALL_US 5000000
static volatile int64_t s_worker_beat_us;

bool mg_ble_stalled(void)
{
    int64_t beat = s_worker_beat_us;
    return beat && esp_timer_get_time() - beat > WORKER_STALL_US;
}

bool mg_ble_wants_advertising(void)
{
    return s_ready && s_conn == BLE_HS_CONN_HANDLE_NONE;
}

bool mg_ble_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

/* ---- who is connected (Status page) ---------------------------------
 *
 * client_app (19) lives in mg_proto (per connection). The name the page
 * shows is only the one the gadget reads itself (client_name, 18, is
 * reserved): the central's GAP
 * Device Name (0x2A00). While the read is pending, or after it fails, the
 * page says "Connected: phone", and the failure is counted (s_gap_name_fails,
 * on the console's status line; never the name). Read once per connection with
 * NimBLE's GATT client, 1.5 s after connect (past the central's own
 * discovery and first updates); the read is asynchronous (the worker starts
 * it, its answer comes on the host task), up to 3 tries 2 s apart when the
 * stack can't start it, and none after an ATT error (a central that guards
 * it). Kept for the connection; cut to MG_CLIENT_NAME_MAX at a whole
 * character; a value that isn't text (mg_proto_utf8_text_ok) is dropped.
 * The name itself is never logged (the Nordic UART mirror carries the log),
 * only its length. */
#if CONFIG_MUSE_GADGET_BLE_PEER_NAME
#define GAP_NAME_UUID16 0x2A00            /* GAP Device Name */
#define GAP_NAME_DELAY_US (1500 * 1000)
#define GAP_NAME_RETRY_US (2000 * 1000)
#define GAP_NAME_TRIES 3
enum { GAP_NAME_WAIT, GAP_NAME_ASKED, GAP_NAME_SETTLED };
static char s_gap_name[MG_CLIENT_NAME_MAX + 1];   /* this connection's ("" none); mg_lock */
static uint8_t s_gap_name_state = GAP_NAME_SETTLED;
static uint8_t s_gap_name_tries;
static uint32_t s_gap_name_conn_gen;     /* the connection a read belongs to */
static int64_t s_gap_name_at_us;         /* when to ask */
static esp_timer_handle_t s_gap_name_timer;
static uint32_t s_gap_name_fails;        /* reads that settled with no name, since boot; mg_lock */
#define PEER_NAME_FAILS() s_gap_name_fails   /* under mg_lock */

static void gap_name_timer_fire(void *arg)
{
    (void)arg;
    kick();   /* the worker starts the read (gap_name_tick) */
}

/* Under mg_lock. */
static void gap_name_arm(int64_t at_us)
{
    s_gap_name_at_us = at_us;
    if (!s_gap_name_timer) {
        const esp_timer_create_args_t a = { .callback = gap_name_timer_fire, .name = "mg_gapname" };
        if (esp_timer_create(&a, &s_gap_name_timer) != ESP_OK) {
            s_gap_name_timer = NULL;
            return;
        }
    }
    int64_t in = at_us - esp_timer_get_time();
    esp_timer_stop(s_gap_name_timer);
    esp_timer_start_once(s_gap_name_timer, in > 0 ? (uint64_t)in : 1);
}

/* A new connection (under mg_lock): nothing known, the read 1.5 s on. */
static void gap_name_connect(void)
{
    s_gap_name[0] = 0;
    s_gap_name_conn_gen++;
    s_gap_name_state = GAP_NAME_WAIT;
    s_gap_name_tries = 0;
    gap_name_arm(esp_timer_get_time() + GAP_NAME_DELAY_US);
}

/* The link's end (under mg_lock): the name goes with it; a late answer is stale. */
static void gap_name_disconnect(void)
{
    s_gap_name[0] = 0;
    s_gap_name_conn_gen++;
    s_gap_name_state = GAP_NAME_SETTLED;
    if (s_gap_name_timer) {
        esp_timer_stop(s_gap_name_timer);
    }
}

/* The read's answer, on the host task. */
static int gap_name_read_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    int status = err ? err->status : 0;
    if (status == 0 && attr && attr->om) {
        uint8_t b[MG_CLIENT_NAME_MAX + 1];
        uint16_t total = OS_MBUF_PKTLEN(attr->om);
        uint16_t take = total < sizeof(b) ? total : (uint16_t)sizeof(b);
        if (os_mbuf_copydata(attr->om, 0, take, b) != 0) {
            take = 0;
        }
        size_t n = take < MG_CLIENT_NAME_MAX ? take : MG_CLIENT_NAME_MAX;
        if (total > MG_CLIENT_NAME_MAX) {
            while (n && (b[n] & 0xC0) == 0x80) {
                n--;   /* cut at a whole character */
            }
        }
        while (n && (b[n - 1] == 0 || b[n - 1] == ' ')) {
            n--;   /* a terminating NUL or padding some centrals send */
        }
        bool ok = n && mg_proto_utf8_text_ok(b, n);
        mg_lock();
        if (gen == s_gap_name_conn_gen && s_gap_name_state == GAP_NAME_ASKED) {
            s_gap_name_state = GAP_NAME_SETTLED;
            if (ok) {
                memcpy(s_gap_name, b, n);
                s_gap_name[n] = 0;
                s_proto.client_gen++;   /* the Status page's row changes */
            } else {
                s_gap_name_fails++;     /* empty or not text: "Connected: phone" */
            }
        }
        mg_unlock();
        ESP_LOGI(TAG, "peer: GAP name %s (%u bytes)", ok ? "read" : "not text, not shown", (unsigned)total);
        return 0;
    }
    mg_lock();
    bool mine = gen == s_gap_name_conn_gen && s_gap_name_state == GAP_NAME_ASKED;
    if (mine) {
        s_gap_name_state = GAP_NAME_SETTLED;   /* done with none, or an ATT error: not asked again */
        s_gap_name_fails++;
    }
    mg_unlock();
    if (mine && status != BLE_HS_EDONE) {
        ESP_LOGI(TAG, "peer: no GAP name (status %d)", status);
    }
    return 0;
}

/* The worker: starts the read once it's due. */
static void gap_name_tick(void)
{
    mg_lock();
    bool due = s_gap_name_state == GAP_NAME_WAIT && s_conn != BLE_HS_CONN_HANDLE_NONE &&
               esp_timer_get_time() >= s_gap_name_at_us;
    uint16_t conn = s_conn;
    uint32_t gen = s_gap_name_conn_gen;
    if (due) {
        s_gap_name_state = GAP_NAME_ASKED;
    }
    mg_unlock();
    if (!due) {
        return;
    }
    int rc = ble_gattc_read_by_uuid(conn, 1, 0xFFFF, BLE_UUID16_DECLARE(GAP_NAME_UUID16), gap_name_read_cb,
                                    (void *)(uintptr_t)gen);
    if (rc == 0) {
        return;
    }
    mg_lock();
    bool again = false;
    if (gen == s_gap_name_conn_gen && s_gap_name_state == GAP_NAME_ASKED) {
        again = ++s_gap_name_tries < GAP_NAME_TRIES;
        s_gap_name_state = again ? GAP_NAME_WAIT : GAP_NAME_SETTLED;
        if (again) {
            gap_name_arm(esp_timer_get_time() + GAP_NAME_RETRY_US);
        } else {
            s_gap_name_fails++;   /* never started: "Connected: phone" */
        }
    }
    mg_unlock();
    ESP_LOGW(TAG, "peer: GAP name read not started (rc %d)%s", rc, again ? ", again in 2 s" : "");
}

uint32_t mg_ble_peer_name_failures(void)
{
    mg_lock();
    uint32_t n = s_gap_name_fails;
    mg_unlock();
    return n;
}
#else
#define PEER_NAME_FAILS() 0u
static void gap_name_connect(void) {}
static void gap_name_disconnect(void) {}
static void gap_name_tick(void) {}
uint32_t mg_ble_peer_name_failures(void)
{
    return 0;
}
#endif

void mg_ble_peer(mg_ble_peer_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_ready) {
        return;
    }
    mg_lock();
    if (s_proto.connected) {
        mg_proto_client_app(&s_proto, out->app_name, sizeof(out->app_name), out->app_version,
                            sizeof(out->app_version));
#if CONFIG_MUSE_GADGET_BLE_PEER_NAME
        memcpy(out->gap_name, s_gap_name, sizeof(out->gap_name));
#endif
    }
    mg_unlock();
}

/* A disconnect with reason 0x216 (local host) that our code didn't ask for (no ble_gap_terminate() within
 * MG_LINK_CLOSE_ASK_US): NimBLE closed the link itself, in practice its 30 s L2CAP reassembly timeout (the peer
 * stopped partway through a packet: two link drops on the bench). Logged once and counted
 * (host_closed); the timeout itself is unchanged. */
static void host_close_check(int reason)
{
    if (reason != BLE_HS_ERR_HCI_BASE + BLE_ERR_CONN_TERM_LOCAL) {
        return;
    }
    int64_t now = esp_timer_get_time(), asked = mg_link_close_asked_us(), rx = s_last_rx_us;
    if (asked >= 0 && now - asked <= MG_LINK_CLOSE_ASK_US) {
        return;   /* ours: it logs its own reason */
    }
    s_host_closed++;
    long long age = rx ? (now - rx) / 1000 : -1;
    ESP_LOGW(TAG, "link closed by the BLE host, not by our code: %s; last data from the peer %lld ms ago "
             "(host closes since boot: %lu)",
             age >= 29000 ? "peer stopped mid-packet (30 s ATT/L2CAP timeout)" : "a host timeout", age,
             (unsigned long)s_host_closed);
}

int mg_ble_reply_stats(bool json, char *out, size_t n)
{
    mg_reply_stats_t st;
    mg_lock();
    mg_proto_reply_stats(&s_proto, &st);
    mg_unlock();
    st.nus_dropped = s_nus_dropped;
    st.host_closed = s_host_closed;
    return mg_reply_stats_format(&st, json, out, n);
}

/* Call with mg_lock held. */
static void link_check(void)
{
    mg_link_t l = mg_proto_link(&s_proto, s_set_up);
    if (l != s_link) {
        ESP_LOGI(TAG, "link %s -> %s", mg_link_name(s_link), mg_link_name(l));
        s_link = l;
    }
}

mg_link_t mg_ble_link(void)
{
    return s_ready ? s_link : MG_LINK_NEVER;
}

bool mg_ble_ptt_ready(void)
{
    return mg_ble_link() == MG_LINK_READY;
}

int mg_ble_gap_event(struct ble_gap_event *ev)
{
#if CONFIG_MUSE_GADGET_BLE_VIDEO && MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM) > 0
    if (s_ready && s_vstream.buf && ev->type == BLE_GAP_EVENT_CONNECT && ev->connect.status == 0) {
        coc_server_start();   /* the host is up by the first connection */
    }
#endif
    if (!s_ready) {
        return 0;
    }
#if CONFIG_MUSE_GADGET_BLE_HID
    if (ev->type == BLE_GAP_EVENT_CONNECT && ev->connect.status == 0) {
        hid_visibility_for(ev->connect.conn_handle);   /* before mg_hogp applies it for this connection */
    }
    if (mg_hogp_gap_event(ev)) {
        hid_status_changed();
    }
    if (ev->type == BLE_GAP_EVENT_ENC_CHANGE && ev->enc_change.status == 0) {
        hid_show_on_encryption();
    }
    if (ev->type == BLE_GAP_EVENT_REPEAT_PAIRING) {
        return BLE_GAP_REPEAT_PAIRING_RETRY;   /* mg_hogp dropped the old bond */
    }
#endif
#if CONFIG_MUSE_GADGET_BLE_SMP
    if (ev->type == BLE_GAP_EVENT_NOTIFY_TX && ev->notify_tx.status == 0 && !ev->notify_tx.indication) {
        smp_notify_tx(ev->notify_tx.attr_handle);
    }
#endif
    if (ev->type == BLE_GAP_EVENT_NOTIFY_TX && !ev->notify_tx.indication && ev->notify_tx.attr_handle == s_nus_tx_handle &&
        s_nus_inflight) {
        s_nus_inflight--;   /* sent or failed: out of the stack either way */
        if (s_nus_len) {
            kick();   /* the next line goes now, not at the worker's next round */
        }
    }
    if (ev->type == BLE_GAP_EVENT_NOTIFY_TX && ev->notify_tx.status == 0 && !ev->notify_tx.indication &&
        ev->notify_tx.attr_handle == s_handle[MG_CH_CONTROL]) {
        /* Reply audio's and video's buffer_updates wait for the last one to leave (mg_proto_stream_tick,
         * mg_proto_video_tick): a held video credit goes at once, not at the worker's next 5 ms round. */
        mg_lock();
        mg_proto_control_sent(&s_proto);
        bool video = s_proto.video.active;
        mg_unlock();
        if (video) {
            kick();
        }
    }
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            mg_lock();
            s_conn = ev->connect.conn_handle;
            s_conn_gen++;
            s_capture.requested = false;
            struct ble_gap_conn_desc cd;
            bool found = ble_gap_conn_find(s_conn, &cd) == 0;
            s_conn_itvl = found ? cd.conn_itvl : 0;
            s_conn_latency = found ? cd.conn_latency : 0;
            mg_linkpol_connect(&s_linkpol, esp_timer_get_time(), s_conn_itvl, s_conn_latency,
                               found ? cd.supervision_timeout : 0);
            link_mode_report();   /* "link mode ready (connect)" */
            play_link_rule();
            /* No connection update (nor PHY / data length) from here at
             * connect: the S22 moves the link itself (7.5 ms within 0.7 s),
             * and a peripheral request at connect collided with it and was
             * left unanswered: LL Response Timeout (0x22) 40 s later, the
             * link dropped. The policy asks for ready after the
             * central's first update, or 1 s after connect if none came. */
            s_link_phy_asked = false;
            mg_proto_connect(&s_proto);
            s_nus_owner = !NUS_MIRROR_RELEASE || !s_set_up;   /* a new link waits for its own proof */
            nus_ring_reset();   /* nothing queued for an earlier link goes to this one */
            gap_name_connect();   /* the central's GAP name, read 1.5 s on */
            mg_unlock();
            bench_mark_sync();   /* the USB console's switch, kept across connections, shows again */
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        host_close_check(ev->disconnect.reason);
        if (s_restart_on_disc) {
            muse_restart((muse_restart_why_t)s_restart_why);   /* the planned restart, the link closed */
        }
#if CONFIG_MUSE_GADGET_BLE_SMP
        s_smp_gen++;   /* the worker drops this link's partial request; an upload resumes at its offset */
        kick();
#endif
        mg_lock();
        if (s_mgh_on) {
            mgh_link_drop(&s_mgh, (uint32_t)mg_now_ms());   /* a client's pattern stops with its link */
            haptic_drive(0);
        }
        if (mg_proto_output_bench_conn(&s_proto)) {
            ESP_LOGW(TAG, "bench.out off (Nordic UART, connection %u, handle %u): disconnected", (unsigned)s_conn_gen,
                     (unsigned)s_conn);
        }
        mg_proto_disconnect(&s_proto);   /* ends the Nordic UART's bench switch with its connection */
        gap_name_disconnect();
        s_capture.requested = false;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        mg_linkpol_disconnect(&s_linkpol);   /* a toggle under way ends here */
        if (s_lp_timer) esp_timer_stop(s_lp_timer);
        s_bas_sub = s_nus_sub = false;
        s_nus_inflight = 0;   /* the link took them with it */
        bool held_turn = mg_proto_held_turn(&s_proto);   /* kept for the clip queue */
        mg_unlock();
        bench_mark_sync();
        if (held_turn) {
            kick();   /* the worker (or the voice task, still capturing it) makes it a clip */
        }
        link_wake_sync();             /* a listening window ends with the link: no override left */
        link_wake_link_changed(true);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        uint16_t h = ev->subscribe.attr_handle;
        bool on = ev->subscribe.cur_notify;
        if (h == s_bas_handle) {
            s_bas_sub = on;
            s_bas_last = -1;
        } else if (h == s_nus_tx_handle) {
            s_nus_sub = on;
            s_nus_inflight = 0;
        } else {
            mg_lock();
            for (int ch = 0; ch < MG_CH_COUNT; ch++) {
                if (s_handle[ch] && h == s_handle[ch]) {
                    mg_proto_subscribe(&s_proto, (mg_ch_t)ch, on);
                }
            }
            bool ready = mg_proto_ready(&s_proto);
            mg_unlock();
            if (ready) {
                kick();   /* an owed speaker_volume report goes at the connection's ready, from the worker */
            }
        }
        break;
    }
    case BLE_GAP_EVENT_MTU:
        mg_lock();
        mg_proto_set_mtu(&s_proto, ev->mtu.value);
        mg_unlock();
        break;
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->conn_update.conn_handle, &d) == 0) {
            /* What the central granted: recorded in the log (each device's
             * docs keep what iPhone, Android and Mac give), and
             * readable as link_params (setting 17). */
            if (ev->conn_update.status == 0) {
                ESP_LOGI(TAG, "link: granted %u.%02u ms latency %u, timeout %u ms", d.conn_itvl * 125 / 100,
                         d.conn_itvl * 125 % 100, d.conn_latency, d.supervision_timeout * 10);
            } else {
                ESP_LOGW(TAG, "link: update refused (status %d), still %u.%02u ms latency %u, timeout %u ms",
                         ev->conn_update.status, d.conn_itvl * 125 / 100, d.conn_itvl * 125 % 100,
                         d.conn_latency, d.supervision_timeout * 10);
            }
            mg_lock();
            s_conn_itvl = d.conn_itvl;
            s_conn_latency = d.conn_latency;
            mg_linkpol_conn_update(&s_linkpol, esp_timer_get_time(), ev->conn_update.status, d.conn_itvl,
                                   d.conn_latency, d.supervision_timeout);
            play_link_rule();   /* active granted: a reply still starting starts at 30 ms */
            mg_unlock();
            if (ev->conn_update.status == 0) {
                link_wake_link_changed(false);   /* its instant gave the S3/C3 controller the granted latency back */
            }
            kick();   /* the worker schedules what follows (the toggle's fast half: >= 500 ms), and the window's end */
        }
        break;
    }
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        ESP_LOGI(TAG, "PHY: tx %u rx %u (status %d)", ev->phy_updated.tx_phy, ev->phy_updated.rx_phy,
                 ev->phy_updated.status);
        break;
#ifdef BLE_GAP_EVENT_DATA_LEN_CHG
    case BLE_GAP_EVENT_DATA_LEN_CHG:
        ESP_LOGI(TAG, "data length: tx %u rx %u bytes", ev->data_len_chg.max_tx_octets,
                 ev->data_len_chg.max_rx_octets);
        break;
#endif
    default:
        break;
    }
    mg_lock();
    link_check();   /* connect, disconnect (any reason), (un)subscribe */
    mg_unlock();
    return 0;
}

/* ---- Nordic UART: log mirror ------------------------------------------- */

static vprintf_like_t s_prev_vprintf;

/* The log line's tag is on the allowlist. Lines look like "I (123) tag: text". */
static bool nus_tag_allowed(const char *line)
{
    static const char tags[] = CONFIG_MUSE_GADGET_BLE_NUS_LOG_TAGS;
    const char *t = strchr(line, ')');
    if (!t) {
        return tags[0] == '*';
    }
    t++;
    while (*t == ' ') {
        t++;
    }
    /* The cable-only tags (touch coordinates, passcode state) never reach the mirror, whatever the
     * allowlist says ('*', a "muse" prefix). */
    if (!strncmp(t, "muse_touch", 10) || !strncmp(t, "muse_pass", 9)) {
        return false;
    }
    if (tags[0] == '*') {
        return true;
    }
    for (const char *p = tags; *p;) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        while (n && *p == ' ') {
            p++;
            n--;
        }
        if (n && strncmp(t, p, n) == 0) {
            return true;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return false;
}

/* On a release build of a set-up gadget the log mirror runs only for a
 * connection whose token proof matched; silent until then (lines in between are never queued). The worker keeps it
 * (nus_owner_refresh, under mg_lock); the logger reads it lock-free. Test and automation builds: always. */

static void nus_owner_refresh(void)
{
    s_nus_owner = !NUS_MIRROR_RELEASE || !s_set_up || mg_proto_proof_matched(&s_proto);
}

/* Empties the NUS ring for a new link (any task): its generation moves on, so a drain under way debits nothing. */
static void nus_ring_reset(void)
{
    portENTER_CRITICAL(&s_nus_mux);
    s_nus_len = 0;
    s_nus_gen++;
    portEXIT_CRITICAL(&s_nus_mux);
}

static int nus_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int r = s_prev_vprintf ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);
    TaskHandle_t reply = s_nus_reply_task;
    if (s_nus_sub && s_nus_ring && (s_nus_owner || (reply && reply == xTaskGetCurrentTaskHandle()))) {
        char line[NUS_LINE];
        int n = vsnprintf(line, sizeof(line), fmt, copy);
        if (n > 0 && nus_tag_allowed(line)) {
            size_t len = n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1;
            if (n >= (int)sizeof(line)) {
                /* Cut: still one line (a heartbeat could run into the next line's text). */
                line[len - 2] = '~';
                line[len - 1] = '\n';
            }
            /* Strip colour codes' escape byte so the line reads as text. */
            portENTER_CRITICAL(&s_nus_mux);
            if (NUS_RING - s_nus_len >= len) {
                for (size_t i = 0; i < len; i++) {
                    s_nus_ring[(s_nus_head + s_nus_len++) % NUS_RING] = line[i] == 0x1b ? ' ' : line[i];
                }
            } else {
                s_nus_dropped++;   /* never waits: the line is gone, and counted */
            }
            portEXIT_CRITICAL(&s_nus_mux);
        }
    }
    va_end(copy);
    return r;
}

/* Sends what's queued for NUS, a notification at a time, within its budget
 * (mg_nus.h: two in the stack, and the protocol's msys reserve left free);
 * the worker drains it after the protocol's own sends. True if more is left.
 * A new link empties the ring (nus_ring_reset, on the host task) while a
 * drain may be between its copy and its debit: the ring's generation says
 * so, and the drain then neither sends that copy nor debits it (a debit
 * after the reset would underflow the count, and the retained bytes would
 * flood the new, unproven central). */
static bool nus_drain(void)
{
    uint16_t conn = s_conn;
    if (!s_nus_ring || !s_nus_len) {
        return false;
    }
    if (!s_nus_sub || conn == BLE_HS_CONN_HANDLE_NONE) {
        nus_ring_reset();   /* an unproven central's ring holds only its own status replies (nus_vprintf) */
        return false;
    }
    uint16_t mtu = ble_att_mtu(conn);
    size_t max = mtu > 3 ? mtu - 3 : 20;
    uint8_t buf[244];
    if (max > sizeof(buf)) {
        max = sizeof(buf);
    }
    for (int k = 0; k < 4 && s_nus_len && mg_nus_may_send(s_nus_inflight, os_msys_num_free()); k++) {
        size_t n = 0;
        portENTER_CRITICAL(&s_nus_mux);
        uint32_t gen = s_nus_gen;
        while (n < max && n < s_nus_len) {
            buf[n] = (uint8_t)s_nus_ring[(s_nus_head + n) % NUS_RING];
            n++;
        }
        portEXIT_CRITICAL(&s_nus_mux);
        if (!n || gen != s_nus_gen || conn != s_conn) {
            return s_nus_len != 0;   /* a new link since the copy: those bytes were the last one's */
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, n);
        if (!om || ble_gatts_notify_custom(conn, s_nus_tx_handle, om) != 0) {
            return true;   /* out of buffers: later */
        }
        s_nus_inflight++;
        portENTER_CRITICAL(&s_nus_mux);
        bool same = gen == s_nus_gen;
        if (same) {
            s_nus_head = (s_nus_head + n) % NUS_RING;
            s_nus_len -= n;   /* only what this ring still holds: a reset since the copy took it all */
        }
        portEXIT_CRITICAL(&s_nus_mux);
        if (!same) {
            break;
        }
    }
    return s_nus_len != 0;
}


static void restart_fire(void *arg)
{
    (void)arg;
#if CONFIG_MUSE_GADGET_BLE_SMP
    /* An update's restart waits for the screen to show "Installing"
     * (500 ms at most; boards without a screen don't wait). */
    if (s_restart_why == MUSE_RESTART_OTA && s_update_ui_attached && mg_smp_esp_installing_unshown() &&
        esp_timer_get_time() < s_restart_hold_until) {
        esp_timer_start_once(s_restart_timer, 25 * 1000);
        return;
    }
#endif
    uint16_t conn = s_conn;
    if (!s_restart_closing && conn != BLE_HS_CONN_HANDLE_NONE &&
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM) == 0) {
        /* The restart follows the disconnect event; this timer is the backstop. */
        s_restart_closing = true;
        s_restart_on_disc = true;
        esp_timer_start_once(s_restart_timer, 300 * 1000);
        return;
    }
    muse_restart((muse_restart_why_t)s_restart_why);
}

/* Every other planned restart (setup done or reset, Settings > Restart):
 * muse_restart()'s first step closes the link the same way. */
static void pre_restart(void)
{
    uint16_t conn = s_conn;
    if (s_restart_on_disc || conn == BLE_HS_CONN_HANDLE_NONE ||
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM) != 0) {
        return;
    }
    for (int i = 0; i < 30 && s_conn == conn; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));   /* the disconnect event, or 300 ms */
    }
}

/*
 * !RESTART <interval_ms> <count> (restart-to-connected): the schedule in
 * RTC no-init memory, so it survives its own planned restarts and dies on a
 * power loss. Each boot after a planned console restart takes one off and
 * restarts again interval_ms later (>= 30 s: after the boot is healthy, so the
 * boot guard never counts these); any other boot (a crash, a power-on) clears
 * it.
 */
#define RSCHED_MAGIC 0x4d475253u   /* "MGRS" */
static RTC_NOINIT_ATTR struct {
    uint32_t magic, interval_ms, left, check;
} s_rsched;

static uint32_t rsched_check(void)
{
    return ~(s_rsched.magic ^ s_rsched.interval_ms ^ s_rsched.left);
}

static void restart_schedule_clear(void)
{
    if (s_rsched.magic == RSCHED_MAGIC) {
        ESP_LOGI(TAG, "restart schedule cleared");
    }
    memset(&s_rsched, 0, sizeof(s_rsched));
}

static void restart_schedule_next(void)
{
    s_rsched.left--;
    s_rsched.check = rsched_check();
    ESP_LOGI(TAG, "restart schedule: next restart in %u ms, %u more after it", (unsigned)s_rsched.interval_ms,
             (unsigned)s_rsched.left);
    restart_after(s_rsched.interval_ms, MUSE_RESTART_CONSOLE);
}

static void restart_schedule_arm(uint32_t interval_ms, uint32_t count)
{
    s_rsched.magic = RSCHED_MAGIC;
    s_rsched.interval_ms = interval_ms;
    s_rsched.left = count;
    s_rsched.check = rsched_check();
    restart_schedule_next();
}

/* At start: carry a schedule on only after one of its own restarts. */
static void restart_schedule_boot(void)
{
    muse_restart_why_t why;
    uint32_t core;
    bool valid = s_rsched.magic == RSCHED_MAGIC && s_rsched.check == rsched_check() && s_rsched.left <= 7 &&
                 s_rsched.interval_ms >= 30000 && s_rsched.interval_ms <= 600000;
    if (!valid) {
        memset(&s_rsched, 0, sizeof(s_rsched));
        return;
    }
    if (!muse_restart_boot_intentional(&why, &core) || why != MUSE_RESTART_CONSOLE || s_rsched.left == 0) {
        restart_schedule_clear();   /* a crash, a power-on, another restart, or done */
        return;
    }
    restart_schedule_next();
}

bool mg_ble_pair_auto_armed(void)
{
    return s_ready && mg_unlock_auto_armed(&s_unlock, esp_timer_get_time());
}

bool mg_ble_pair_auto_arm(bool on)
{
    if (!s_ready) return false;
    mg_unlock_pair_auto(&s_unlock, esp_timer_get_time(), on);
    ESP_LOGI(TAG, "PAIR AUTO %s", on ? "on (one pairing, 10 min; USB console)" : "off");
    return true;
}

bool mg_ble_pair_auto_take(void)
{
    if (!s_ready) return false;
    bool take = mg_unlock_auto_take(&s_unlock, esp_timer_get_time());
    if (take) ESP_LOGI(TAG, "pairing auto-confirmed (!PAIR AUTO)");
    return take;
}

/* >ble.pause: the pause itself 300 ms after the word, so its log line
 * reaches a client on the Nordic UART before the link drops. */
#define BLE_PAUSE_DELAY_US (300 * 1000)
static esp_timer_handle_t s_pause_timer;
static volatile uint32_t s_pause_secs;
static uint8_t s_pause_addr[6];
static volatile bool s_pause_only;

static void pause_fire(void *arg)
{
    (void)arg;
    s_platform.bench_pause(s_pause_secs, s_pause_only ? s_pause_addr : NULL);
}

bool mg_ble_bench_pause(uint32_t secs, const uint8_t *only_addr)
{
    if (!s_ready || !s_platform.bench_pause || secs < 1 || secs > 600) {
        ESP_LOGW(TAG, "ble.pause=%u refused: %s", (unsigned)secs,
                 secs < 1 || secs > 600 ? "1-600 s" : "no pause on this build");
        return false;
    }
    if (!s_pause_timer) {
        const esp_timer_create_args_t a = { .callback = pause_fire, .name = "mg_pause" };
        if (esp_timer_create(&a, &s_pause_timer) != ESP_OK) {
            s_pause_timer = NULL;
            ESP_LOGW(TAG, "ble.pause=%u refused: no timer", (unsigned)secs);
            return false;
        }
    }
    esp_timer_stop(s_pause_timer);   /* not running: ESP_ERR_INVALID_STATE, fine */
    s_pause_secs = secs;
    s_pause_only = only_addr != NULL;
    if (only_addr) {
        memcpy(s_pause_addr, only_addr, sizeof(s_pause_addr));
        ESP_LOGW(TAG, "ble.pause: only %02X:%02X:%02X:%02X:%02X:%02X for %u s (link dropped; no unpair, nothing "
                 "stored)", only_addr[0], only_addr[1], only_addr[2], only_addr[3], only_addr[4], only_addr[5],
                 (unsigned)secs);
    } else {
        ESP_LOGW(TAG, "ble.pause: off the phone for %u s (link dropped, no advertising), then as before; "
                 "no unpair, nothing stored", (unsigned)secs);
    }
    esp_timer_start_once(s_pause_timer, BLE_PAUSE_DELAY_US);
    return true;
}

bool mg_ble_bench_pause_text(const char *text)
{
    uint32_t secs;
    uint8_t addr[6];
    bool has_addr;
    if (!mg_unlock_pause_parse(text, &secs, addr, &has_addr)) {
        ESP_LOGW(TAG, "ble.pause=%.24s refused: SECS (1-600) or SECS,AA:BB:CC:DD:EE:FF", text);
        return false;
    }
    return mg_ble_bench_pause(secs, has_addr ? addr : NULL);
}

static void nus_command(char *line)
{
#if !CONFIG_MUSE_GADGET_AUTOMATION
    /* A release build: a word that changes state runs only on a
     * proven connection; reads stay open. The USB console stays open. */
    mg_lock();
    bool proven = mg_proto_proof_matched(&s_proto);
    mg_unlock();
    if (!mg_unlock_remote_ok(line, proven, s_platform.safe_mode)) {
        ESP_LOGW(TAG, "a state-changing word: locked (this connection's proof hasn't matched)");   /* never its text */
        return;
    }
#endif
#if !CONFIG_MUSE_GADGET_AUTOMATION && !CONFIG_MUSE_GADGET_TEST_BUILD
    /* The owner's content (voice activity, image paths) only for the proven phone of a set-up gadget. */
    mg_lock();
    bool owner = mg_unlock_owner_ok_in(line, mg_proto_proof_matched(&s_proto), s_set_up, true);
    mg_unlock();
    if (!owner) {
        ESP_LOGW(TAG, "an owner-content read: proof required");   /* never its text */
        return;
    }
#endif
    if (line[0] == '?' || line[0] == '!') {
        char out[96];
        if (mg_unlock_line(&s_unlock, line, esp_timer_get_time(), out, sizeof(out))) {
            ESP_LOGI(TAG, "%s", out);
            uint32_t count;
            bool clear;
            uint32_t ms = mg_unlock_restart_take(&s_unlock, &count, &clear);
            if (clear) {
                restart_schedule_clear();
            }
            if (ms && count) {
                restart_schedule_arm(ms, count);
            } else if (ms) {
                restart_after(ms, MUSE_RESTART_CONSOLE);
            }
            return;
        }
    }
    if (!strcmp(line, "!SILENCE")) {
        /* Bench: advertising off and the link closed until a restart, to
         * prove the stay-reachable check. */
        bool ok = s_platform.bench_silence != NULL;
        ESP_LOGI(TAG, "SILENCE %s", ok ? "on: unreachable until a restart" : "not on this build");
        if (ok) {
            s_platform.bench_silence();
        }
        return;
    }
    if (!strcmp(line, "!SETUP ADV") || !strcmp(line, "!SETUP ADV 0")) {
        /* Bench: a set-up gadget
         * advertises Link setup again, taking turns with musegadgets, flagged
         * as not set up, for 10 min (RAM only: a restart ends it), so an app
         * that lists only Link setup adverts can set it up again (setup v2;
         * a set_up=0 console word). Nothing else changes. */
        bool off = !strcmp(line, "!SETUP ADV 0");
        bool ok = s_platform.bench_setup_adv != NULL;
        ESP_LOGI(TAG, "SETUP ADV %s", !ok ? "not on this build" : off ? "off" : "on (Link setup advertised, 10 min)");
        if (ok) {
            s_platform.bench_setup_adv(off ? 0 : 600);
        }
        return;
    }
    if (!strcmp(line, "!SAFEID")) {
        /* Bench: one boot in the SAFE identity, back by itself after 120 s
         * (verifies the way back from SAFE first). */
        bool ok = s_platform.bench_safe_identity != NULL;
        ESP_LOGI(TAG, "SAFEID %s", ok ? "on: restarting in the SAFE identity for 120 s" : "not on this build");
        if (ok) {
            s_platform.bench_safe_identity();
            restart_after(200, MUSE_RESTART_CONSOLE);
        }
        return;
    }
    if (!strncmp(line, "!IN ble.pause=", 14)) {
        /* Bench: off the phone for SECS (1-600; the link this line came on
         * drops too), then as before. */
        mg_ble_bench_pause_text(line + 14);
        return;
    }
    if (!strncmp(line, "!IN ", 4)) {
        /* Bench: the device's own screen and buttons pressed for a Mac
         * client's checks (every mute path), read-only diagnostics and
         * the vibration tests (a release build gates the state-changing
         * ones in nus_command), by the
         * platform's allowlist (tap=, press=, key=, page=,
         * ptt=, wake, session.ping, voice?, wake?, vib=, vib?,
         * mic.gain?, mic.gain=, spk.dsp, spk.dsp=, ptt.minrise=, ptt.minrise?),
         * queued to the input task (this is the NimBLE host); each word logs
         * its outcome on mg.bench (session.ping on mg.session). */
        bool ok = s_platform.bench_input != NULL;
        int r = ok ? s_platform.bench_input(line + 4) : 0;
        if (!ok) {
            ESP_LOGI(TAG, "IN: no bench words on this build");
        } else if (r == 0) {
            ESP_LOGI(TAG, "IN %.32s: not a bench word", line + 4);
        } else if (r < 0) {
            ESP_LOGW(TAG, "a bench word: busy");   /* never its text */
        }
        return;
    }
    if (nus_bench_out(line)) {
        return;   /* bench.out=0|1 / bench.out?: this connection's bench switch (the proof override) */
    }
    char wake[160];
    if (mg_ble_link_wake_word(line, wake, sizeof(wake))) {
        return;   /* link.wake=0|1 / link.wake?: answered on mg.ble */
    }
    char sleep[288];
    if (mg_ble_link_sleep_word(line, sleep, sizeof(sleep))) {
        return;   /* link.sleep=ITVL_MIN,ITVL_MAX,LAT / =0 / ? (the sleep A/B): answered on mg.ble */
    }
    if (!strcmp(line, "status")) {
        mg_lock();
        const mg_proto_t *p = &s_proto;
        /* This line goes to whoever holds the Nordic UART, so an unproven
         * central learns nothing of a held capture here: no live flag, no hold counts. */
        bool visible = mg_proto_capture_visible(p);
        ESP_LOGI(TAG, "status: connected=%d ready=%d ptt=%d live=%d mtu=%u queue=%s proof=%s ptt_nothing=%u "
                 "peer_name_fails=%u",
                 p->connected, mg_proto_ready(p), p->ptt_enabled, p->live && visible, p->mtu, s_have_queue ? "yes" : "no",
                 mg_proto_proof_matched(p) ? "matched" : "no", (unsigned)p->ptt_nothing, (unsigned)PEER_NAME_FAILS());
        if (!visible) {
            ESP_LOGI(TAG, "output: %s", mg_proto_output_state(p));
        } else {
            /* Where mic output may go, and what the proof hold did (counts only). */
            ESP_LOGI(TAG, "output: %s held=%d full=%d fills=%u releases=%u discards=%u discarded=%u stale=%u "
                     "stale_bytes=%u no_key=%u start_refused=%u held_turns live=%u queued=%u",
                     mg_proto_output_state(p), mg_proto_output_held(p), p->capq.proof_full,
                     (unsigned)p->capq.proof_fills, (unsigned)p->capq.proof_releases, (unsigned)p->capq.proof_discards,
                     (unsigned)p->capq.proof_discarded, (unsigned)p->capq.proof_stale,
                     (unsigned)p->capq.proof_stale_bytes, (unsigned)p->out_no_key, (unsigned)p->out_start_refused,
                     (unsigned)p->capq.ht_live, (unsigned)p->capq.ht_queued);
        }
        mg_unlock();
        char replies[192];
        mg_ble_reply_stats(false, replies, sizeof(replies));
        ESP_LOGI(TAG, "replies: %s", replies);   /* every drop counted, counts only */
        if (s_hid) {
            ESP_LOGI(TAG, "hid: mode %u, queued %u, sent %u, busy %u, dropped %u", s_hid->map.mode, s_hid->count,
                     (unsigned)s_hid_sent, (unsigned)s_hid_busy, (unsigned)s_hid->dropped);
        }
    } else if (!strcmp(line, "version")) {
        ESP_LOGI(TAG, "version %s, mg%d", esp_app_get_description()->version, MG_SPEC_VERSION);
    } else if (!s_platform.console || !s_platform.console(line)) {
        ESP_LOGI(TAG, "console: status, version");
    }
}

/* ---- worker ------------------------------------------------------------- */

static void check_battery(void)
{
    uint16_t conn = s_conn;
    if (!s_bas_sub || conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    int level = battery_level();
    if (level != s_bas_last) {
        uint8_t b = (uint8_t)level;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&b, 1);
        if (om && ble_gatts_notify_custom(conn, s_bas_handle, om) == 0) {
            s_bas_last = level;
        }
    }
}

#if CONFIG_MUSE_GADGET_BLE_SESSION
/*
 * A credentials_update's refreshed pair, into NVS (one journalled commit:
 * some 30 flash operations) outside the protocol lock, which the NimBLE
 * host task takes for every session write: taken out under the
 * lock, stored without it, and credentials_saved queued under it again only
 * if the session that sent it is still the one open (a hello or the link's
 * end meanwhile: the client sends the pair again).
 */
static void session_store_pending(void)
{
    char *access = NULL, *refresh = NULL;
    uint64_t expires = 0;
    uint32_t gen = 0;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    mg_lock();
    bool have = mg_proto_session_take_update(&s_proto, &access, &refresh, &expires, &gen);
    mg_unlock();
    if (!have) {
        xSemaphoreGive(s_store_lock);
        return;
    }
    /* A re-setup that began since is still holding the tokens here (its setup change waits for this
     * store), so the old session's pair is refused, never stored beside the new setup. */
    bool saved = op_session_store_tokens(NULL, access, refresh, expires);
    xSemaphoreGive(s_store_lock);
    mg_token_proof_wipe(access, MG_SESSION_TOKEN_MAX);
    mg_token_proof_wipe(refresh, MG_SESSION_TOKEN_MAX);
    free(access);
    free(refresh);
    mg_lock();
    mg_proto_session_update_saved(&s_proto, gen, saved);
    mg_unlock();
}
#endif

/* Recorded before the pairing changed hands (queued, or still recording then): the last owner's. Under the lock. */
static bool clip_stale(uint32_t start_ms)
{
#if CONFIG_MUSE_GADGET_BLE_QUEUE
    return s_forgot && (int32_t)(start_ms - s_forget_ms) < 0;
#else
    (void)start_ms;
    return false;
#endif
}

/* An offline clip into the queue, on the worker (only this task appends; clears come under the lock from
 * others); frees data. */
static void save_clip(uint8_t codec, uint32_t start_ms, uint64_t start_unix_ms, uint8_t *data, uint32_t len)
{
    mg_queue_t *q = mg_queue_get();
    bool ok = false;
    if (q) {
        /* The erase first, a sector at a time outside the lock (BLE callbacks wait on it); a clear in
         * between (the pairing changing hands) voids the sector in flight. */
        uint32_t off, gen;
        for (;;) {
            mg_lock();
            bool more = !clip_stale(start_ms) && mg_queue_prepare_next(q, len, &off, &gen);
            mg_unlock();
            if (!more || !q->store.erase(q->store.ctx, off, q->store.sector)) {
                break;
            }
            mg_lock();
            mg_queue_prepare_done(q, off, gen);
            mg_unlock();
        }
        mg_lock();
        ok = !clip_stale(start_ms) && mg_queue_append_at(q, codec, start_ms, start_unix_ms, data, len);
        mg_unlock();
    }
    ESP_LOGI(TAG, "offline clip of %u bytes %s", (unsigned)len, ok ? "saved" : "not saved");
    free(data);
}

/* A held live turn the voice task has finished with, past its deadline (or its link gone): its audio
 * becomes an offline clip here (or, with no queue, it keeps waiting for its client: mg_config.h). Under the
 * lock; the clip is written by the caller, outside it. */
static uint8_t *held_turn_clip(uint32_t *len, uint32_t *start_ms, uint64_t *start_unix_ms)
{
    mg_proto_t *p = &s_proto;
    *len = 0;
    if (!mg_proto_held_turn(p) || p->capq.ht_voice || !mg_proto_held_turn_due(p)) {
        return NULL;
    }
    if (!mg_proto_held_turn_queueable(p)) {
        if (!MG_HELD_TURN_NO_QUEUE_HOLDS) {
            mg_proto_held_turn_take(p, NULL, 0);
        }
        return NULL;
    }
    uint32_t n = mg_proto_held_turn_bytes(p);
    uint8_t *b = n ? heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (n && !b) {
        b = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    }
    *start_ms = p->capq.ht_ms;
    *start_unix_ms = p->capq.ht_unix_ms;
    *len = (uint32_t)mg_proto_held_turn_take(p, b, n);   /* no memory: discarded (logged by the caller) */
    if (!*len) {
        free(b);
        b = NULL;
    }
    return b;
}

static void log_output_note(void)
{
    uint32_t out_bytes = 0;
    uint8_t out_note = mg_proto_take_output_note(&s_proto, &out_bytes);
    if (out_note == MG_OUT_NOTE_RELEASED) {
        ESP_LOGI("mg.voice", "capture held for the client: proven and ready, %u bytes held go first, in order",
                 (unsigned)out_bytes);
    } else if (out_note == MG_OUT_NOTE_DISCARDED) {
        ESP_LOGI("mg.voice", "capture held for the token proof: no valid proof, %u bytes discarded unsent",
                 (unsigned)out_bytes);
    } else if (out_note == MG_OUT_NOTE_STALE) {
        ESP_LOGI("mg.voice", "capture held for the client: not released within %u ms of its end: %u bytes discarded",
                 (unsigned)MG_OUTPUT_STALE_MS, (unsigned)out_bytes);
    } else if (out_note == MG_OUT_NOTE_QUEUED) {
        ESP_LOGI("mg.voice", "held live turn: not proven and ready in %u ms: %u bytes to the offline clip queue",
                 (unsigned)MG_HELD_TURN_MS, (unsigned)out_bytes);
    }
}

static void worker(void *arg)
{
    (void)arg;
    int64_t next_tick = 0, next_battery = 0;
    for (;;) {
#if CONFIG_MUSE_GADGET_BLE_SESSION
        session_store_pending();   /* a refreshed token pair, into NVS, outside the lock */
#endif
        mg_lock();
        framed_watch();
        bool transfer = mg_proto_transfer_active(&s_proto);
        bool streaming = mg_play_active(&s_play) || s_proto.flush_update_owed;   /* a flush's update owed too */
        /* Every reply stream's credit goes out every 10 ms of playout as it changes (mg_proto_stream_tick:
         * a live depth is capped at 100 ms, and a pending notification keeps a ready link
         * listening at every event), so the worker looks every 5 ms while one plays. */
        bool video = mg_proto_video_active(&s_proto);
        bool session = mg_proto_session_pump(&s_proto);   /* what a full stack held back */
        /* A capture's held audio and its end (mg_capq_t): sent as the link takes them, every 5 ms; a
         * token proof answer the stack refused goes ahead of it (busy after 1 s). */
        bool capq = mg_proto_capture_pump(&s_proto);
        /* A held live turn waits for its client (looked at every 20 ms), and past its deadline
         * becomes an offline clip here once the voice task is done with it. */
        uint32_t ht_len = 0, ht_ms = 0;
        uint64_t ht_unix = 0;
        uint8_t *ht_clip = held_turn_clip(&ht_len, &ht_ms, &ht_unix);
        bool held_turn = mg_proto_output_waiting(&s_proto);   /* a held turn's deadline, or a hold's staleness bound */
        log_output_note();
        int spk_owed_sent = s_proto.spk_sent != s_spk_logged ? (int)s_proto.spk_seen : -1;   /* an owed report left */
        s_spk_logged = s_proto.spk_sent;
        bool held_capture = mg_proto_capture_pending(&s_proto);
        /* A capture held for an unproven central doesn't move the link (its mode report and interval
         * would tell the central one exists). */
        bool capture_visible = mg_proto_capture_visible(&s_proto);
        mg_capture_note();
        mg_unlock();
        if (ht_clip) {
            save_clip(mg_data_type_audio_sbc, ht_ms, ht_unix, ht_clip, ht_len);
        }
        if (spk_owed_sent >= 0) {
            ESP_LOGI(TAG, "speaker_volume %d reported (owed)", spk_owed_sent);   /* the Nordic UART mirror */
        }
        bool nus = s_nus_len != 0;
#if CONFIG_MUSE_GADGET_BLE_HID
        bool hid = (s_hid && s_hid->count) || s_hid_show >= 0;
#else
        bool hid = false;
#endif
        /* What keeps the link active: a
         * stream the app started (audio playback, video, a UI stream while
         * its picture changes, a clip transfer or throughput test), the
         * mic capturing, an update. Not the log mirror, nor a GATT write
         * alone (background app traffic would hold it active forever). */
        const char *busy = (s_capture.requested || held_capture) && capture_visible ? "push-to-talk capture"
                           : streaming ? "audio playback"
                           : transfer ? "transfer"
                           : NULL;
#if CONFIG_MUSE_GADGET_BLE_VIDEO
        if (video) {
            int64_t t = esp_timer_get_time();
            bool still = s_ui_stream && t - s_last_data_us > UI_FRAME_ACTIVE_US &&
                         t - s_last_touch_us > UI_TOUCH_FAST_US;
            if (!still && !busy) {
                busy = s_ui_stream ? "UI stream" : "video stream";
            }
            /* A still UI stream needs no 5 ms rounds: Data writes kick the worker. */
            if (still) {
                video = false;
            }
        }
#else
        if (video && !busy) {
            busy = "video stream";
        }
#endif
        /* An update's writes are activity of their own (smp_access). */
        TickType_t wait = transfer || video || hid || session || capq ? pdMS_TO_TICKS(5)
                          : streaming ? pdMS_TO_TICKS(5)
                          : held_turn || nus ? pdMS_TO_TICKS(20)
                                   : pdMS_TO_TICKS(1000);
        ev_t ev;
        muse_wdt_feed(MUSE_WDT_MG_WORKER);   /* at least once a second (the wait's longest) */
        s_worker_beat_us = esp_timer_get_time();   /* mg_ble_stalled */
        if (xQueueReceive(s_events, &ev, wait) == pdTRUE) {
            switch (ev.type) {
            case EV_WRITE:
#if CONFIG_MUSE_GADGET_BLE_SESSION
                if (ev.ch == MG_CH_CONTROL && ev.len >= 2 && ev.data[0] == mg_command_session
                    && ev.data[1] == mg_session_hello) {
                    session_store_pending();   /* a waiting pair first: the credentials carry it */
                }
#endif
                mg_lock();
                if (ev.conn_gen != s_conn_gen || s_conn == BLE_HS_CONN_HANDLE_NONE) {
                    /* Written on a connection that has since ended: never run against the next one. */
                    mg_unlock();
                    memset(ev.data, 0, ev.len);
                    free(ev.data);
                    break;
                }
                mg_proto_write(&s_proto, (mg_ch_t)ev.ch, ev.data, ev.len);
                if (mg_proto_take_ptt_cleared(&s_proto)) {
                    ESP_LOGI(TAG, "push-to-talk off: a new token proof (presses are gesture-only until it's valid "
                                  "and the client enables push-to-talk again)");
                }
                log_output_note();
                link_check();   /* push-to-talk on or off */
                mg_unlock();
                memset(ev.data, 0, ev.len);
                free(ev.data);
                break;
            case EV_CLIP:
                save_clip(ev.codec, ev.start_ms, ev.start_unix_ms, ev.data, ev.len);
                break;
            case EV_KICK:
                break;
            case EV_RUN:
                ev.run(ev.data);
                break;
            }
        }
#if CONFIG_MUSE_GADGET_BLE_SMP
        smp_service();   /* flash writes: on this task, not the host's */
#endif
        if (transfer) {
            mg_lock();
            uint32_t stalls = s_proto.tp.stalls;
            mg_proto_pump(&s_proto);
            bool stalled = s_proto.tp.stalls != stalls;
            mg_unlock();
            if (stalled) {
                ESP_LOGW(TAG, "throughput test: ended, the link took nothing for %u ms", (unsigned)MG_TP_STALL_MS);
            }
        }
        /* Video credit first: when both are due in one pass it takes the one in-flight slot, and a
         * whole-buffer reply's on-change update waits for it. */
        if (video) {
            mg_lock();
            mg_proto_video_tick(&s_proto);    /* credits as the decoder frees room */
            mg_unlock();
        }
        if (streaming) {
            mg_lock();
            mg_proto_stream_tick(&s_proto);   /* buffer_update: 10 ms cadence with a live depth or a pending pre-active start, else on change */
            mg_unlock();
        }
#if CONFIG_MUSE_GADGET_BLE_HID
        hid_pump();
        hid_visibility_check();
#endif
        link_policy_check(busy);   /* link modes */
        link_wake_tick();          /* a switch's pending notification */
        haptic_poll();             /* a haptic step the timer found due */
        gap_name_tick();           /* the central's GAP name, once it's due */
        mg_lock();
        nus_owner_refresh();       /* the log mirror follows this connection's proof */
        mg_unlock();
        nus_drain();
        int64_t now = esp_timer_get_time();
        if (now >= next_tick) {
            next_tick = now + 1000 * 1000;
            mg_lock();
            mg_proto_tick(&s_proto);
            mg_unlock();
        }
        if (now >= next_battery) {
            next_battery = now + 30 * 1000 * 1000;
            check_battery();
        }
    }
}

/* ---- playback -------------------------------------------------------------- */

static const char *codec_name(uint8_t codec)
{
    return codec == mg_data_type_audio_sbc ? "sbc" : codec == mg_data_type_audio_lc3 ? "lc3" : "pcm";
}

/* One line per stream, on the Nordic UART mirror: what a client can check playback by. */
static void log_stream(const char *how, const mg_play_params_t *pp, const mg_play_stats_t *st, const mg_dec_t *d,
                       const mg_level_t *spk)
{
    char text[400];
    mg_play_format_stats(text, sizeof(text), how, pp, st, d, spk);
    for (char *line = text; line;) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        ESP_LOGI(PLAY_TAG, "%s", line);
        line = nl ? nl + 1 : NULL;
    }
}

static bool speaker_muted(void)
{
    return s_platform.speaker_volume && s_platform.speaker_volume() == 0;
}

/* The makeup gain for the speaker level now (mg_ble_platform_t.speaker_boost_db10). */
static mg_boost_t s_boost;

static void boost(int16_t *pcm, size_t n)
{
    int max = s_platform.speaker_boost_db10 ? s_platform.speaker_boost_db10() : 0;
    int vol = s_platform.speaker_volume ? s_platform.speaker_volume() : 100;
    mg_boost_set(&s_boost, mg_boost_for_volume(max, vol));
    mg_boost_run(&s_boost, pcm, n);
}

static void log_boost(void)
{
    if (s_boost.db10 > 0) {
        ESP_LOGI(PLAY_TAG, "stream boost: +%d.%d dB, limiter on %lu ms", s_boost.db10 / 10, s_boost.db10 % 10,
                 (unsigned long)(s_boost.limited / (MG_PLAY_OUT_RATE / 1000)));
    }
}

static void speaker(const int16_t *pcm, size_t n)
{
    if (!s_platform.speaker_write(pcm, n)) {
        vTaskDelay(pdMS_TO_TICKS(n * 1000 / MG_PLAY_OUT_RATE));   /* codecs off: keep the pace */
    }
}

/* The once-a-second playback line (mg_play_format_tick), and the stream's last one at its end, with
 * the samples the drift trim dropped and repeated. */
static void play_tick(bool end, const mg_play_stats_t *st, uint32_t buffered_us, uint16_t queued_ms,
                      const mg_trim_t *trim, uint32_t *prev_underruns, int64_t *tick_t)
{
    int64_t now = esp_timer_get_time();
    char line[192];
    int n = mg_play_format_tick(line, sizeof(line), end, st, *prev_underruns, (uint32_t)((now - *tick_t) / 1000),
                                buffered_us, queued_ms, (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    if (end && n > 0 && (size_t)n < sizeof(line)) {
        mg_play_format_trimmed(line + n, sizeof(line) - (size_t)n, trim->dropped, trim->repeated, trim->repaid);
        size_t used = strlen(line);
        /* Underruns in the reply's first second (the adaptive start's input), still counted above. */
        snprintf(line + used, sizeof(line) - used, " early=%lu", (unsigned long)st->early_underruns);
    }
    ESP_LOGI(PLAY_TAG, "%s", line);
    *prev_underruns = st->underruns;
    *tick_t = now;
}

/*
 * Decodes the client's stream to the speaker: pops about 10 ms of whole
 * frames at a time from the ring (under mg_lock), decodes and resamples them
 * outside it, and writes 10 ms chunks; silence while prebuffering at the
 * start, and a concealed chunk for each one a gap leaves empty
 * (mg_dec_conceal), playing on with the next frame that arrives.
 * speaker_write blocks on the I2S DMA, which paces the loop. After each
 * write it tells the ring what it still holds (mg_play_set_pending), for
 * buffer_update's queued ms. The drift trim (mg_trim_*) looks at
 * the depth once a chunk and trims the decoded audio while the output clock
 * runs off the reply's rate.
 */
/* The audio clock for the picture (mg_ble_av_clock): one writer, the playback task; readers on any task
 * retry while seq is odd or moved (no lock on either side). */
static volatile uint32_t s_av_seq;
static uint32_t s_av_frame;
static int32_t s_av_pos;
static int64_t s_av_t;
static bool s_av_ok;
#define AV_CLOCK_STALE_US 60000

static void av_publish(bool ok, uint32_t frame, int32_t pos_us)
{
    if (!ok && !s_av_ok) {
        return;   /* nothing to take back: no write at all */
    }
    s_av_seq++;
    __sync_synchronize();
    s_av_ok = ok;
    s_av_frame = frame;
    s_av_pos = pos_us;
    s_av_t = esp_timer_get_time();
    __sync_synchronize();
    s_av_seq++;
}

bool mg_ble_av_clock(uint32_t *frame, int32_t *pos_us)
{
    for (int tries = 0; tries < 4; tries++) {
        uint32_t seq = s_av_seq;
        if (seq & 1) {
            continue;
        }
        __sync_synchronize();
        bool ok = s_av_ok;
        uint32_t f = s_av_frame;
        int32_t pos = s_av_pos;
        int64_t t = s_av_t;
        __sync_synchronize();
        if (seq != s_av_seq) {
            continue;
        }
        if (!ok) {
            return false;
        }
        int64_t age = esp_timer_get_time() - t;
        if (age < 0 || age > AV_CLOCK_STALE_US) {
            return false;
        }
        *frame = f;
        *pos_us = pos + (int32_t)age;   /* the speaker plays on in real time since the write */
        return true;
    }
    return false;
}

static void player(void *arg)
{
    (void)arg;
    static const int16_t silence[PLAY_CHUNK];
    static int16_t fifo[MG_PLAY_MAX_OUT + PLAY_CHUNK];
    static uint8_t frames[MG_PLAY_MAX_POP];
    mg_dec_t *dec = NULL;
    uint32_t stream = 0;
    size_t nfifo = 0;
    mg_play_params_t pp = { 0 };
    mg_play_stats_t st = { 0 };
    mg_level_t spk = { 0 };   /* what reached the codec (the speaker's mute zeroes it) */
    int64_t tick_t = 0;           /* the last playback line */
    uint32_t tick_underruns = 0, buffered = 0, held_us = 0, out_us = MG_PLAY_DEPTH_UNKNOWN;
    uint32_t live_us = 0, live_allow_us = 0, live_grown = 0, live_shrunk = 0, live_grown_seen = 0, live_shrunk_seen = 0;   /* the live depth (mg_play.h) */
    uint16_t queued = 0;
    bool trim_playing = false;
    uint32_t flush_seen = 0, flush_seq = 0, flush_us = 0;   /* mg_stream_audio_flush (barge-in) */
    uint32_t gap_seen = 0, idle_seen = 0;   /* the stream's concealed gaps (repaid) and reply boundaries */
    bool was_barge = false;                 /* a talk press's touch-down paused the reply (barge-in echo rule) */
    static int16_t rewind[MG_REWIND_MAX];   /* what the speaker's DMA held unplayed at the touch-down (mg_play.h) */
    size_t nrewind = 0;
    /* The board's speech chain (mg_chain_t, mg_chain.h); its silence goes through the FIFO's last chunk, free
     * whenever silence is written (the FIFO holds less than a chunk then). */
    static mg_chain_t chain;
    mg_chain_init(&chain, s_platform.speaker_chain_reset ? s_platform.speaker_chain : NULL, s_platform.speaker_chain_reset);
    int16_t *const chain_scratch = fifo + MG_PLAY_MAX_OUT;
    _Static_assert(MG_PLAY_MAX_OUT >= PLAY_CHUNK, "the chain's scratch is the FIFO's last chunk");
    static mg_trim_t trim;   /* the drift trim, per stream */
    mg_trim_reset(&trim);
    for (;;) {
        mg_lock();
        mg_play_state_t state = s_play.state;
        bool fresh = state != MG_PLAY_IDLE && (!dec || s_play.stream != stream);
        /* The adaptive start's move at this stream's start, logged with it. */
        const char *start_note = fresh ? s_play.start_note : NULL;
        uint32_t start_ms = mg_play_start_threshold_us(&s_play) / 1000;
        if (start_note) {
            s_play.start_note = NULL;
        }
        uint32_t new_stream = s_play.stream;
        uint32_t new_flush_seq = s_play.flush_seq;
        mg_play_params_t new_pp = s_play.params;
        size_t n = 0;
        /* A push-to-talk turn holds the speaker: the stream waits in
         * the ring and the FIFO, nothing lost (credits still flow while it has
         * room; no starvation counted). */
        bool held = mg_proto_play_held(&s_proto) && (state == MG_PLAY_PREBUFFER || state == MG_PLAY_PLAYING ||
                                                     state == MG_PLAY_DRAINING);
        bool barge = held && !s_proto.turn && mg_proto_barge_held(&s_proto);
        uint32_t kept_us = s_play.buffered_us + s_play.pending_us;
        if (!fresh && !held && (state == MG_PLAY_PREBUFFER || state == MG_PLAY_PLAYING || state == MG_PLAY_DRAINING)) {
            n = mg_play_pop(&s_play, frames, sizeof(frames), MG_PLAY_CHUNK_US);
        }
        bool conceal = !fresh && !held && !n && mg_play_concealing(&s_play);
        if (!fresh && state != MG_PLAY_IDLE) {
            st = s_play.st;   /* this stream's; a replaced one keeps its last */
            buffered = s_play.buffered_us;
            queued = mg_play_queued_ms(&s_play);
            held_us = s_play.buffered_us + s_play.pending_us;
            out_us = s_play.out_us;
            live_us = s_play.live_us;
            live_allow_us = s_play.live_allow_us;
            live_grown = s_play.st.live_grown;
            live_shrunk = s_play.st.live_shrunk;
            flush_seq = s_play.flush_seq;
            flush_us = s_play.flush_us;
            trim_playing = !held && s_play.state == MG_PLAY_PLAYING && !s_play.starving;
        }
        mg_unlock();
        static uint32_t anchors_seen;   /* each resolved A/V anchor, one line */
        if (st.anchors != anchors_seen) {
            anchors_seen = st.anchors;
            if (st.anchors) {
                ESP_LOGI(PLAY_TAG, "A/V anchor: reply %s %lu ms, picture %+ld at its first sample (%lu of %lu held)",
                         st.anchor_measured ? "already playing, measured;" : "held", (unsigned long)(st.anchor_held_us / 1000),
                         (long)st.anchor_offset, (unsigned long)st.anchors_held, (unsigned long)st.anchors);
            }
        }

        if (dec && (fresh || state == MG_PLAY_IDLE)) {
            /* Dropped, or replaced by a new stream. */
            play_tick(true, &st, buffered, queued, &trim, &tick_underruns, &tick_t);
            log_stream("drop", &pp, &st, dec, &spk);
            log_boost();
            mg_dec_close(dec);
            free(dec);
            dec = NULL;
            nfifo = 0;
            if (nrewind) {
                mg_chain_discard(&chain);   /* the touch-down silenced the speaker: none of the chain's tail plays */
            }
            nrewind = 0;   /* a dropped reply's (a hold's stop_streaming drop, a client's): never played */
            for (int i = 0; i < PLAY_TAIL_CHUNKS; i++) {
                speaker(mg_chain_silence(&chain, silence, chain_scratch, PLAY_CHUNK), PLAY_CHUNK);   /* its tail out */
            }
        }
        if (state == MG_PLAY_IDLE) {
            mg_lock();
            mg_play_release(&s_play);
            mg_unlock();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (barge != was_barge && !fresh && dec) {
            was_barge = barge;
            /* Muted at the talk press's touch-down, so the pre-roll holds none of the reply
             * (the barge-in echo rule); a tap resumes it from the same sample, a hold claims the turn. */
            if (barge) {
                /* And at once, not after the ~30 ms the DMA holds: those go quiet in 6 ms (faded, no click)
                 * and come back here, to play first if a tap resumes (no sample lost; mg_play.h). */
                size_t room = MG_REWIND_MAX - nrewind;
                size_t got = s_platform.speaker_clear ? s_platform.speaker_clear(rewind + nrewind, room) : 0;
                nrewind += got;
                ESP_LOGI(PLAY_TAG, "paused at the talk press (%lu ms kept, %lu ms back from the speaker)",
                         (unsigned long)(kept_us / 1000), (unsigned long)(got * 1000 / MG_PLAY_OUT_RATE));
            } else if (!held) {
                ESP_LOGI(PLAY_TAG, "resumed after the talk press (a tap; %lu ms held)", (unsigned long)(kept_us / 1000));
            }
        }
        if (held && !fresh) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));   /* the turn's, the sound's or the press's; their end kicks */
            continue;
        }
        if (fresh) {
            if (start_note) {
                ESP_LOGI(PLAY_TAG, "start threshold %lu ms (%s)", (unsigned long)start_ms, start_note);
            }
            stream = new_stream;
            pp = new_pp;
            memset(&st, 0, sizeof(st));
            memset(&spk, 0, sizeof(spk));
            s_boost.env = 0;
            s_boost.limited = 0;
            mg_chain_discard(&chain);   /* a stream starts the chain afresh (the last one's tail played out) */
            mg_trim_reset(&trim);
            flush_seen = new_flush_seq;
            gap_seen = idle_seen = 0;
            live_grown_seen = live_shrunk_seen = 0;
            dec = malloc(sizeof(*dec));
            if (!dec || !mg_dec_open(dec, &pp)) {
                ESP_LOGW(PLAY_TAG, "no memory to decode the stream");
                if (dec) {
                    mg_dec_close(dec);
                }
                free(dec);
                dec = NULL;
                mg_lock();
                if (s_play.stream == stream && mg_play_active(&s_play)) {
                    mg_play_stop(&s_play, false);
                    s_play.state = MG_PLAY_DONE;   /* notifies stop_streaming drop */
                    mg_proto_stream_finished(&s_proto);
                }
                mg_unlock();
                continue;
            }
            ESP_LOGI(PLAY_TAG, "stream start: %s %lu Hz%s%s", codec_name(pp.codec), (unsigned long)pp.rate,
                     pp.rate != MG_PLAY_OUT_RATE ? ", resampled to 16 kHz" : "", speaker_muted() ? ", speaker off" : "");
            if (pp.live_depth_ms) {
                mg_lock();
                uint32_t hold = s_play.live_us, allow = s_play.live_allow_us;
                mg_unlock();
                ESP_LOGI(PLAY_TAG, "live depth %u ms asked, holding %lu ms, in-flight allowance %lu ms",
                         (unsigned)pp.live_depth_ms, (unsigned long)(hold / 1000), (unsigned long)(allow / 1000));
            }
            tick_t = esp_timer_get_time();
            tick_underruns = 0;
            continue;
        }
        if (state == MG_PLAY_DONE) {
            /* Played out (keep), or the device gave up on a starved stream (drop). What the touch-down took
             * back from the speaker goes first, unless a flush took the reply. */
            if (nrewind && flush_seq == flush_seen) {
                mg_play_ramp_in(rewind, nrewind, MG_REWIND_RAMP);
                speaker(rewind, nrewind);   /* post-chain already: as it is */
            } else if (nrewind) {
                mg_chain_discard(&chain);   /* a flush in the pause took the reply: the chain's part of it too */
            }
            nrewind = 0;
            if (nfifo) {
                boost(fifo, nfifo);
                size_t padded = (nfifo + PLAY_CHUNK - 1) / PLAY_CHUNK * PLAY_CHUNK;
                memset(fifo + nfifo, 0, (padded - nfifo) * sizeof(int16_t));
                mg_chain_audio(&chain, fifo, padded);
                mg_level_add(&spk, fifo, nfifo, speaker_muted());
                speaker(fifo, padded);
                nfifo = 0;
            }
            for (int i = 0; i < PLAY_TAIL_CHUNKS; i++) {
                speaker(mg_chain_silence(&chain, silence, chain_scratch, PLAY_CHUNK), PLAY_CHUNK);   /* its last 2 ms play */
            }
            mg_lock();
            bool keep = s_play.done_keep;
            mg_proto_stream_finished(&s_proto);
            mg_unlock();
            play_tick(true, &st, 0, 0, &trim, &tick_underruns, &tick_t);
            log_stream(keep ? "keep" : "starved", &pp, &st, dec, &spk);
            log_boost();
            mg_dec_close(dec);
            free(dec);
            dec = NULL;
            continue;
        }
        if (esp_timer_get_time() - tick_t >= 1000000) {
            play_tick(false, &st, buffered, queued, &trim, &tick_underruns, &tick_t);
        }
        if (flush_seq != flush_seen) {
            /* A flush (barge-in): what the FIFO holds is the flushed reply's. Its first 5 ms fade out
             * (no click), the rest goes, and the decoder starts over (nothing concealed); this pop's
             * frames, if any, came after the flush and play. */
            flush_seen = flush_seq;
            if (nrewind) {
                /* The touch-down already silenced the speaker (a hold's claim): none of it plays,
                 * not even a fade-out, which would put 5 ms of the reply back in the user's turn; nor the
                 * chain's look-ahead. (Without the pause the chain's 2 ms go before the FIFO's fade-out.) */
                nrewind = 0;
                nfifo = 0;
                mg_chain_discard(&chain);
            }
            size_t fade = nfifo < PLAY_FLUSH_FADE ? nfifo : PLAY_FLUSH_FADE;
            for (size_t i = 0; i < fade; i++) {
                fifo[i] = (int16_t)((int32_t)fifo[i] * (int32_t)(fade - i) / (int32_t)(fade + 1));
            }
            nfifo = fade;
            if (!mg_dec_flush(dec)) {
                ESP_LOGW(PLAY_TAG, "flush: the decoder didn't restart");
            }
            ESP_LOGI(PLAY_TAG, "flush (%lu ms discarded)", (unsigned long)(flush_us / 1000));
            mg_trim_forgive(&trim);   /* nothing of a flushed reply is owed */
        }
        if (nrewind) {
            /* A tap resumed the reply (or its pause lapsed): first what the touch-down took back from the
             * speaker, ramped in, then the FIFO and the ring, every sample once and in order. */
            mg_play_ramp_in(rewind, nrewind, MG_REWIND_RAMP);
            speaker(rewind, nrewind);   /* post-chain already; the chain goes on from the next sample */
            nrewind = 0;
        }
        if (st.idle_gaps != idle_seen) {
            idle_seen = st.idle_gaps;
            mg_trim_forgive(&trim);   /* a reply boundary: the next reply starts on time */
        }
        if (st.gap_us != gap_seen) {
            /* An underrun, concealed: the reply now runs its length late; play it back up. */
            mg_trim_owe(&trim, st.gap_us - gap_seen);
            gap_seen = st.gap_us;
            char rl[32];
            mg_trim_format_repay(rl, sizeof(rl), &trim, false);
            ESP_LOGI(PLAY_TAG, "%s", rl);
        }
        if (live_grown != live_grown_seen || live_shrunk != live_shrunk_seen) {
            ESP_LOGI(PLAY_TAG, "live depth: in-flight allowance %lu ms (%s)", (unsigned long)(live_allow_us / 1000),
                     live_grown != live_grown_seen ? "underrun" : "clean");
            live_grown_seen = live_grown;
            live_shrunk_seen = live_shrunk;
        }
        /* With a live depth the trim's target follows the allowance and stays within the depth plus it (mg_play.h). */
        trim.live_cap_us = live_us ? (int32_t)(live_us + live_allow_us - (out_us == MG_PLAY_DEPTH_UNKNOWN ? 0 : out_us)) : 0;
        trim.live_allow_us = live_us ? (int32_t)live_allow_us : 0;
        trim.repay_room = held_us >= MG_PLAY_REPAY_ROOM_US;
        mg_trim_event_t tev = mg_trim_update(&trim, esp_timer_get_time(), held_us, trim_playing);
        if (tev != MG_TRIM_NONE) {
            char tl[48];
            mg_trim_format(tl, sizeof(tl), tev, &trim, out_us);
            ESP_LOGI(PLAY_TAG, "%s", tl);
        }
        if (n) {
            size_t got = mg_dec_run(dec, frames, n, fifo + nfifo, MG_PLAY_MAX_OUT + PLAY_CHUNK - nfifo);
            nfifo += mg_trim_run(&trim, fifo + nfifo, got, MG_PLAY_MAX_OUT + PLAY_CHUNK - nfifo);
            if (trim.repaid_now) {
                trim.repaid_now = false;
                ESP_LOGI(PLAY_TAG, "repaid");
            }
        } else if (conceal) {
            /* A gap: LC3's concealment or a faded repeat, after what the FIFO holds (mg_play counts it). */
            nfifo += mg_dec_conceal(dec, fifo + nfifo, MG_PLAY_MAX_OUT + PLAY_CHUNK - nfifo, MG_PLAY_CHUNK_US);
        }
        if (nfifo >= PLAY_CHUNK) {
            size_t whole = nfifo / PLAY_CHUNK * PLAY_CHUNK;
            boost(fifo, whole);
            mg_chain_audio(&chain, fifo, whole);
            mg_level_add(&spk, fifo, whole, speaker_muted());
            speaker(fifo, whole);
            memmove(fifo, fifo + whole, (nfifo - whole) * sizeof(int16_t));
            nfifo -= whole;
            mg_lock();
            /* Not after a flush that came during the write: what's left is the flushed reply's, and
             * the flush already emptied what the task holds (queued ms 0, not audible). */
            uint32_t av_frame = 0;
            int32_t av_pos = 0;
            bool av_ok = false;
            if (s_play.stream == stream && s_play.flush_seq == flush_seq) {
                mg_play_set_pending(&s_play, (uint32_t)(nfifo * 1000000u / MG_PLAY_OUT_RATE));
                av_ok = mg_play_av_pos(&s_play, &av_frame, &av_pos);   /* the picture's audio clock */
            }
            mg_unlock();
            av_publish(av_ok, av_frame, av_pos);
        } else if (!n) {
            /* prebuffering at the start (mg_play counts it), or a stream with nothing to play */
            av_publish(false, 0, 0);   /* no reply audio: no clock for the picture */
            speaker(mg_chain_silence(&chain, silence, chain_scratch, PLAY_CHUNK), PLAY_CHUNK);
        }
    }
}

void mg_ble_setup_changed(void)
{
    uint8_t k[MG_TOKEN_PROOF_KEY_LEN];
    s_set_up = (s_platform.proof_key && s_platform.proof_key(k)) || (s_platform.set_up && s_platform.set_up());
    mg_token_proof_wipe(k, sizeof(k));
    if (s_ready) {
        xSemaphoreTake(s_store_lock, portMAX_DELAY);   /* after an old session's pair in flight */
        mg_lock();
        /* A new setup's commit replaces the key (Gadget setup v2): a proof the
         * client made with the old one stops counting. */
        mg_proto_proof_reset(&s_proto);
        link_check();
        mg_unlock();
        xSemaphoreGive(s_store_lock);
    }
}

/* The wipe's durable mark: how far the last owner's clips reached, written before the first erase and cleared only
 * once every sector is erased, by queue and no-queue images alike, so a boot that finds it (a power cut, a setup
 * reset's own restart) begins again whichever image it runs. */
#define NVS_QUEUE_WIPE "q_wipe"

static bool queue_wipe_mark(uint32_t to)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = to ? nvs_set_u32(h, NVS_QUEUE_WIPE, to) : nvs_erase_key(h, NVS_QUEUE_WIPE);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

static uint32_t queue_wipe_marked(void)
{
    nvs_handle_t h;
    uint32_t to = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_QUEUE_WIPE, &to);
        nvs_close(h);
    }
    return to;
}

#if CONFIG_MUSE_GADGET_BLE_QUEUE
/*
 * The wipe at an ownership change (mgcommands.h, Audio queue): the whole
 * region an earlier owner's clips may have reached is erased, in the
 * background after the commit. The NVS mark (how far) is written before the
 * head's erase and cleared once the last sector is done; a boot that finds it
 * (a power cut, or the setup reset's own restart) begins again (mount_queue).
 * No clips are taken meanwhile (mg_queue_fits).
 */
#define QUEUE_WIPE_RETRY_MS 500
#define QUEUE_WIPE_TRIES 5

static TaskHandle_t s_wipe_task;   /* under mg_lock */

/* One sector at a time: named and counted under the lock, checked and erased
 * without it (each st_erase() pausing after it, so everything else runs, the
 * idle tasks and their watchdog included). The mark goes, and the task ends,
 * under the lock, so a forget meanwhile either finds the task running (and its
 * new wipe is done here) or starts its own. */
static void queue_wipe_task(void *arg)
{
    (void)arg;
    unsigned erased = 0, fails = 0;
    for (;;) {
        uint32_t off;
        mg_lock();
        if (!mg_queue_wipe_next(&s_queue, &off) || fails >= QUEUE_WIPE_TRIES) {
            bool done = !mg_queue_wiping(&s_queue);
            if (done) {
                queue_wipe_mark(0);
            }
            s_wipe_task = NULL;
            mg_unlock();
            if (done) {
                ESP_LOGI(TAG, "offline queue wiped: %u sector(s) erased after the head", erased);
            } else {
                ESP_LOGE(TAG, "offline queue wipe stopped (erase failed): no clips until the next boot resumes it");
            }
            break;
        }
        mg_unlock();
        bool blank = mg_queue_sector_blank(&s_queue, off);
        bool ok = blank || s_queue.store.erase(s_queue.store.ctx, off, s_queue.store.sector);
        erased += ok && !blank;
        fails = ok ? 0 : fails + 1;
        mg_lock();
        mg_queue_wipe_done(&s_queue, off, ok);
        mg_unlock();
        if (!ok) {
            vTaskDelay(pdMS_TO_TICKS(QUEUE_WIPE_RETRY_MS));
        }
    }
    vTaskDelete(NULL);
}

/* Under mg_lock. */
static void queue_wipe_start(void)
{
    if (!s_wipe_task && mg_queue_wiping(&s_queue)
        && xTaskCreate(queue_wipe_task, "mg_qwipe", 3072, NULL, tskIDLE_PRIORITY + 1, &s_wipe_task) != pdPASS) {
        s_wipe_task = NULL;
        ESP_LOGE(TAG, "offline queue wipe couldn't start: no clips until the next boot does it");
    }
}
#endif

#if !CONFIG_MUSE_GADGET_BLE_QUEUE
static void stale_queue_wipe_task(void *arg)
{
    const esp_partition_t *part = arg;
    size_t sector = part->erase_size ? part->erase_size : 4096;
    unsigned failed = 0;
    for (size_t off = 0; off + sector <= part->size; off += sector) {
        if (esp_partition_erase_range(part, off, sector) != ESP_OK) {
            failed++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));   /* the rest of the system runs between sectors */
    }
    if (failed) {
        ESP_LOGE(TAG, "offline clips' partition wipe: %u sector(s) failed: the next boot wipes it again", failed);
    } else {
        queue_wipe_mark(0);   /* every sector erased: only now does the mark go */
        ESP_LOGI(TAG, "offline clips' partition wiped");
    }
    vTaskDelete(NULL);
}

/* The mark first, then the background wipe. false: no mark, or the wipe couldn't start (a marked one waits for the
 * next boot). */
static bool stale_queue_wipe_start(const esp_partition_t *part, const char *why)
{
    bool marked = queue_wipe_mark((uint32_t)part->size);
    if (xTaskCreate(stale_queue_wipe_task, "mg_qwipe", 3072, (void *)part, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "offline clips' partition wipe couldn't start (%s)%s", why,
                 marked ? ": the next boot does it" : "");
        return false;
    }
    ESP_LOGI(TAG, "offline clips' partition: wiping %u KB in the background (%s)%s", (unsigned)(part->size / 1024),
             why, marked ? "" : " (no resume mark)");
    return marked;
}
#endif

bool mg_ble_queue_forget(const char *why)
{
#if CONFIG_MUSE_GADGET_BLE_QUEUE
    if (!s_queue.store.size) {
        return true;   /* no queue mounted: nothing to erase */
    }
    if (!s_ready) {
        return false;
    }
    mg_lock();
    unsigned clips = s_queue.count;
    s_forget_ms = mg_now_ms();   /* a clip recorded before this is the last owner's (save_clip) */
    s_forgot = true;
    /* Also after a failed one this boot: its clips are still in flash. The
     * mark goes before the head's erase: a power cut from here on still wipes
     * it all. Without it, false: the caller's own mark stays and the next boot
     * forgets again. */
    uint32_t to = mg_queue_written_to(&s_queue);
    bool marked = queue_wipe_mark(to);
    bool ok = mg_queue_wipe_begin(&s_queue, to);
    if (ok) {
        s_proto.transfer = false;   /* a download under way ends here, as mg_proto_queue_forget() ends it */
        queue_wipe_start();
    } else {
        s_have_queue = false;   /* unreadable for the rest of this boot rather than left to the next owner */
    }
    mg_unlock();
    if (ok) {
        ESP_LOGI(TAG, "offline queue cleared (%s): %u clip(s); wiping %u KB in the background%s", why, clips,
                 (unsigned)(to / 1024), marked ? "" : " (no resume mark)");
    } else {
        ESP_LOGE(TAG, "offline queue clear failed (%s): queue off until the next boot", why);
    }
    return ok && marked;
#else
    /* A build without the queue: the mg_queue partition may still hold the last owner's clips from an image that
     * had it. Erased in the background, a sector at a time (each erase stops the cache). false if the wipe can't
     * start: the caller's mark stays, and the next boot forgets again. */
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, QUEUE_PART_SUBTYPE, "mg_queue");
    if (!part) {
        return true;
    }
    return stale_queue_wipe_start(part, why);
#endif
}

/* ---- init ---------------------------------------------------------------- */

#if CONFIG_MUSE_GADGET_BLE_QUEUE
static bool st_read(void *ctx, uint32_t off, void *buf, size_t n)
{
    return esp_partition_read(ctx, off, buf, n) == ESP_OK;
}
static bool st_write(void *ctx, uint32_t off, const void *buf, size_t n)
{
    return esp_partition_write(ctx, off, buf, n) == ESP_OK;
}
/* Each sector erase stops the cache, and with it everything running from
 * flash, for tens of ms: pause between sectors so a long clip's save doesn't
 * freeze the screen (and the rest of the worker's work) for a second. */
#define ERASE_PAUSE_MS 20

static bool st_erase(void *ctx, uint32_t off, size_t n)
{
    bool ok = esp_partition_erase_range(ctx, off, n) == ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(ERASE_PAUSE_MS));
    return ok;
}

static void mount_queue(void)
{
    s_queue_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, QUEUE_PART_SUBTYPE, "mg_queue");
    if (!s_queue_part) {
        ESP_LOGI(TAG, "no mg_queue partition: offline clips off");
        return;
    }
    mg_store_t st = {
        .ctx = (void *)s_queue_part,
        .size = s_queue_part->size - s_queue_part->size % s_queue_part->erase_size,
        .sector = s_queue_part->erase_size,
        .read = st_read,
        .write = st_write,
        .erase = st_erase,
    };
    s_have_queue = mg_queue_mount(&s_queue, &st);
    uint32_t wipe = s_have_queue ? queue_wipe_marked() : 0;
    if (wipe) {
        /* An ownership change's wipe that didn't finish: from the head again. */
        s_have_queue = mg_queue_wipe_begin(&s_queue, wipe);
        ESP_LOGW(TAG, "offline queue: an unfinished wipe (%u KB) resumes", (unsigned)(wipe / 1024));
        if (s_have_queue) {
            mg_lock();
            queue_wipe_start();
            mg_unlock();
        }
    }
    if (s_have_queue) {
        ESP_LOGI(TAG, "offline queue: %u clips, %u of %u KB used", s_queue.count,
                 (unsigned)(mg_queue_used(&s_queue) / 1024), (unsigned)(mg_queue_capacity(&s_queue) / 1024));
    }
}
#endif

void mg_ble_init(const mg_ble_platform_t *platform)
{
    if (s_ready) {
        return;
    }
    if (platform) {
        s_platform = *platform;
    }
    mg_unlock_init(&s_unlock);
    /* Listen-during-switch: on by default only where an A/B showed a
     * gain (none on the ESP32 controllers; Kconfig help). link.wake=1
     * turns it on for the boot. */
#if CONFIG_MUSE_GADGET_BLE_LINK_WAKE
    s_linkpol.wake_off = false;
#else
    s_linkpol.wake_off = true;
#endif
    muse_restart_set_pre(pre_restart);
    restart_schedule_boot();
    s_lock = xSemaphoreCreateRecursiveMutex();
    s_store_lock = xSemaphoreCreateMutex();
    s_events = xQueueCreate(16, sizeof(ev_t));
    if (!s_lock || !s_store_lock || !s_events) {
        ESP_LOGE(TAG, "no memory");
        return;
    }
#if CONFIG_MUSE_GADGET_BLE_QUEUE
    mount_queue();
#else
    /* A wipe the last boot left marked (its restart, a power cut): begun again before anything else. */
    if (queue_wipe_marked()) {
        const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, QUEUE_PART_SUBTYPE, "mg_queue");
        if (part) {
            (void)stale_queue_wipe_start(part, "a wipe the last boot left unfinished");
        } else {
            queue_wipe_mark(0);   /* no partition: nothing left to erase */
        }
    }
#endif
    mg_settings_t settings;
    load_settings(&settings);
    mg_play_init(&s_play, CONFIG_MUSE_GADGET_BLE_PLAY_BUFFER);
    s_play.shown32 = s_platform.video_shown32;   /* the A/V anchor's reader of the picture */
    /* The output path's depth, for buffer_update's queued ms (A/V sync). */
    s_play.out_us = s_platform.speaker_depth_us ? s_platform.speaker_depth_us() : MG_PLAY_DEPTH_UNKNOWN;
    bool speaker = s_platform.speaker_write != NULL;
    mg_proto_config_t cfg = {
        .queue = mg_queue_get(),
        .play = speaker ? &s_play : NULL,
        .haptics = s_platform.vibrate != NULL,
        .haptic_actuator = s_platform.haptic_motor ? s_platform.haptic_actuator : NULL,
        .assistant = s_platform.assistant_state != NULL,
#if CONFIG_MUSE_GADGET_AUTOMATION || CONFIG_MUSE_GADGET_TEST_BUILD
        .bench_streams = true,   /* bench, automation and lab tools stream to a set-up gadget unproven, with the bench
                                  * override on */
#endif
        .recovery_mode = s_platform.recovery_mode,
        .recovery_reason = s_platform.recovery_reason,
        .recovery_flags = s_platform.recovery_flags,
    };
    /* UI input: touch events on a touchscreen, and the HID beside them. */
    if (s_platform.touch_w && s_platform.touch_h) {
        cfg.ui_input = 1u << mg_ui_input_feature_touch | 1u << mg_ui_input_feature_controls;
#if CONFIG_MUSE_GADGET_BLE_HID
        s_hid = heap_caps_malloc(sizeof(*s_hid), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_hid) {
            s_hid = malloc(sizeof(*s_hid));
        }
        if (s_hid) {
            mg_hid_init(s_hid, s_platform.touch_w, s_platform.touch_h);
            cfg.ui_input |= (1u << mg_ui_input_feature_hid_digitizer) | (1u << mg_ui_input_feature_hid_mouse_abs) |
                            (1u << mg_ui_input_feature_hid_mouse_rel);
        } else {
            ESP_LOGE(TAG, "no memory for the HID");
        }
#endif
    }
#if CONFIG_MUSE_GADGET_BLE_VIDEO
    if (s_platform.video_caps) {
        /* In PSRAM: the decoder copies each frame out as soon as it's whole.
         * A board without PSRAM keeps a small one internal. */
#if CONFIG_SPIRAM
        uint8_t *ring = heap_caps_malloc(CONFIG_MUSE_GADGET_BLE_VIDEO_BUFFER, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        uint8_t *ring = heap_caps_malloc(CONFIG_MUSE_GADGET_BLE_VIDEO_BUFFER, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
        _Static_assert((CONFIG_MUSE_GADGET_BLE_VIDEO_BUFFER & (CONFIG_MUSE_GADGET_BLE_VIDEO_BUFFER - 1)) == 0,
                       "CONFIG_MUSE_GADGET_BLE_VIDEO_BUFFER: the video ring is a power of two (muse_vstream_init)");
        if (ring) {
            muse_vstream_init(&s_vstream, ring, CONFIG_MUSE_GADGET_BLE_VIDEO_BUFFER, CONFIG_MUSE_GADGET_BLE_VIDEO_BUFFER / 2);
            s_video_caps = *s_platform.video_caps;
#if MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM) > 0
            s_video_caps.l2cap_psm = MG_VIDEO_L2CAP_PSM;
#endif
            /* A UI frame has to fit the ring's largest frame. */
            if (s_video_caps.ui_max_frame > s_vstream.max_frame) {
                s_video_caps.ui_max_frame = s_vstream.max_frame;
            }
            cfg.video = &s_video_caps;
            cfg.vstream = &s_vstream;
        } else {
            ESP_LOGE(TAG, "no memory for the video buffer");
        }
    }
#endif
#if CONFIG_MUSE_GADGET_BLE_SESSION
    if (s_platform.session_credentials && s_platform.session_state && s_platform.session_message
        && s_platform.proof_key && s_platform.proof_clear) {
        size_t max = CONFIG_MUSE_GADGET_BLE_SESSION_MAX_MESSAGE;
        /* The window is the whole buffer: a message is credited back once
         * the session code takes it, so one larger than the window could
         * never finish. The buffer (MG_SESSION_BUF_SIZE(max), 18 KB) is taken
         * only while a session is open and given back when it ends or the
         * link goes (no idle memory: on the C6 it's internal RAM).
         * Outbound: what the session code's largest envelope needs, and as
         * much again. */
        s_session_cfg = (mg_session_config_t){
            .buf = NULL,
            .alloc = session_ring_alloc,
            .release = free,
            .msg_alloc = session_msg_alloc,
            .msg_release = session_msg_release,
            .max_message = max,
            .window = max,
            .out_max = 24 * 1024,
            .store_later = true,
#if CONFIG_MUSE_GADGET_BLE_STANDALONE
            /* No Wi-Fi to refresh its own pair: the client refreshes it
             * and writes it back. */
            .client_refreshes = true,
#endif
            .sealed_clear = MG_SEALED_CLEAR,
        };
        cfg.session = &s_session_cfg;
    }
#endif
    mg_proto_ops_t ops = {
        .reboot = op_reboot,   /* used only in a recovery mode with the restart flag */
        .notify = op_notify,
        .disconnect = op_disconnect,
        .capture = op_capture,
        .mic_gain = op_mic_gain,
        .settings_changed = op_settings_changed,
        .now_ms = op_now,
        .mono_ms = op_mono,
        .play_kick = op_play_kick,
        .stream_starting = op_stream_starting,
        .random = op_random,
    };
    if (speaker && s_platform.speaker_volume && s_platform.set_speaker_volume) {
        ops.speaker_volume = op_speaker_volume;
        ops.set_speaker_volume = op_set_speaker_volume;
    }
    if (cfg.assistant) {
        ops.assistant_state = op_assistant_state;
    }
    if (cfg.haptic_actuator) {
        haptic_init();
        if (s_mgh_on) {
            ops.haptic = op_haptic;
        } else {
            cfg.haptic_actuator = NULL;
        }
    }
    if (s_platform.clock_set && s_platform.clock_get) {
        ops.clock_set = op_clock_set;
        ops.clock_get = op_clock_get;
    }
    if (s_platform.battery_status) {
        ops.battery_status = op_battery_status;
    }
    if (s_platform.power_off) {
        ops.power_off = op_power_off;
        if (s_platform.power_off_ready) {
            ops.power_off_ready = op_power_off_ready;
        }
    }
#if CONFIG_MUSE_GADGET_BLE_HID
    if (s_hid) {
        ops.hid_map = op_hid_map;
        ops.hid_key = op_hid_key;
    }
#endif
    ops.rx_held = op_rx_held;   /* Data discarded after a device end (mgcommands.h) */
    ops.capq_alloc = op_capq_alloc;   /* a capture's audio held for a slow link (mg_capq_t) */
    ops.capq_free = op_capq_free;
    ops.data_room = op_data_room;
#if CONFIG_MUSE_GADGET_BLE_VIDEO
    ops.video_begin = op_video_begin;
    ops.video_end = op_video_end;
#endif
    if (s_platform.proof_key && s_platform.proof_clear) {
        ops.proof_key = op_proof_key;
        ops.set_up = op_set_up;
        ops.proof_clear = op_proof_clear;
        ops.proof_clear_async = op_proof_clear_async;
        mg_ble_setup_changed();
        ESP_LOGI(TAG, "token proof: %s", s_set_up ? "set up" : "not set up");
    }
    /* Mic output only to a central that passed this connection's token
     * proof, on every build; without a proof key nothing can match, so nothing goes. The bench switch
     * (bench.out, automation, off at every boot; over the Nordic UART for its own connection only) is the
     * only way around it. */
    cfg.output_proof = true;
#if !CONFIG_MUSE_GADGET_TEST_BUILD && !CONFIG_MUSE_GADGET_AUTOMATION
    cfg.proof_inputs = true;   /* haptic_play and the owner's content reads wait for the proof on a set-up gadget */
#endif
#if !MG_NO_PERSON
    cfg.press_gate = true;   /* a release build's start_mic needs a press of the device's own lately */
#endif
#if CONFIG_MUSE_GADGET_BLE_SESSION
    if (cfg.session) {
        ops.session_credentials = op_session_credentials;
        ops.session_store_tokens = op_session_store_tokens;
        ops.session_busy = op_session_busy;
        ops.session_state = op_session_state;
        ops.session_message = op_session_message;
        ops.session_data_refused = op_session_data_refused;
    }
#else
    /* A Wi-Fi gadget: the credentials-only device session (portable
     * mg_device_session layer 2): hello, sealed
     * credentials and credentials_update, no VM session; caps sealed only. */
    if (s_platform.session_credentials && s_platform.session_store_tokens && ops.proof_key) {
        cfg.creds_session = true;
        ops.session_credentials = op_session_credentials;
        ops.session_store_tokens = op_session_store_tokens;
        ops.session_busy = op_session_busy;
    }
#endif
    if (s_platform.client_refreshes) {
        ops.client_refreshes = op_client_refreshes;
    }
    ops.link_mode = op_link_mode;   /* link modes: every musegadgets build */
    ops.set_link_mode = op_set_link_mode;
    ops.link_params = op_link_params;
    mg_proto_init(&s_proto, &cfg, &ops, &settings);
#if CONFIG_MUSE_GADGET_BLE_HID
    hid_map_load();
    if (s_hid) {
        s_proto.hid_mode = s_hid->map.mode;   /* what hid_status says until something changes */
        ESP_LOGI(TAG, "HID: mode %u (%s)", s_hid->map.mode, mg_hogp_visible() ? "shown" : "hidden until a hid_map");
    }
#endif

#if CONFIG_MUSE_GADGET_BLE_SMP
    smp_start();   /* firmware updates over BLE */
#endif

    s_nus_ring = heap_caps_malloc(NUS_RING, MALLOC_CAP_8BIT);
    if (s_nus_ring) {
        s_prev_vprintf = esp_log_set_vprintf(nus_vprintf);
    }
    /* Each notification would log a line at info; audio sends dozens a second. */
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    /* Internal stack, never PSRAM (muse_stack.h): SMP maps flash on it
     * (esp_ota_get_state_partition, esp_ota_end's image check, otadata), which
     * freezes the cache. The player may: it only decodes and plays. */
    if (xTaskCreate(worker, "mg_ble", WORKER_STACK, NULL, 5, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the worker");
        return;
    }
    /* Above the voice loop (6), so the speaker isn't starved while it encodes. */
    if (speaker && muse_task_create(player, "mg_play", PLAYER_STACK, NULL, 7, &s_player, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the player");
        /* No speaker after all. Not a second mg_proto_init(): that wiped the state the first one allocated (the
         * credentials session and its blocks) while the worker already ran on it. */
        mg_lock();
        mg_proto_drop_play(&s_proto);
        mg_unlock();
    }
    s_ready = true;
    muse_boot_mark("mg_ready");   /* request_status answered from here (before the first connectable advert) */
    ESP_LOGI(TAG, "musegadgets BLE ready: %s%s, queue %s, playback %s, video %s, session %s", "SBC",
             MG_WITH_LC3 ? "+LC3" : "", s_have_queue ? "on" : "off", s_player ? "on" : "off",
             s_vstream.buf ? "on" : "off", mg_proto_session_offered(&s_proto) ? "on" : "off");
    if (s_platform.refresh_advertising) {
        s_platform.refresh_advertising();
    }
    kick();
}

#if CONFIG_MUSE_GADGET_BLE_VIDEO
void mg_ble_video_suggest_fps(uint8_t fps)
{
    mg_lock();
    s_video_caps.sug_fps = fps;   /* the next capabilities answer: a bench trial (video.fps=, RAM only) */
    mg_unlock();
}

#endif
