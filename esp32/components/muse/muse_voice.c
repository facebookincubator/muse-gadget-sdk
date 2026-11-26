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

#include "muse_voice.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "mg_loud1s.h"   /* the loudest 1 s window in the "recorded" line (header-only) */
#include "muse_adpcm.h"
#include "muse_audio.h"
#include "muse_mic_ring.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_input.h"
#include "muse_mem.h"
#if CONFIG_MUSE_PHOTO_NOTES
#include "muse_photo_note.h"
#endif
#include "muse_settings.h"
#include "muse_motion.h"
#include "muse_state.h"
#include "muse_wakeword_glue.h"
#include "muse_wifi.h"
#include "muse_wdt.h"
#if CONFIG_MUSE_VOICE_SOFTWARE_AEC
#include "muse_aec.h"
#endif
#if CONFIG_MUSE_REALTIME_VOICE
#include "muse_voice_call.h"
#include "muse_voice_call_core.h"
#endif
#if CONFIG_MUSE_RESEND_NOTES
#include "muse_resend.h"
#endif
#if CONFIG_MUSE_GADGET_BLE_AUDIO
#include "mg_voice.h"
#include "mgcommands.h"
#include "muse_link.h"   /* set up for the no-route caption */
#endif


static const char *TAG = "muse_voice";

/* Where the voice task is, for a stuck-task report from the input task
 * (muse_voice_where): a static string and when it got there. Written only by
 * the voice task. */
static const char *volatile s_at = "starting";
static volatile int64_t s_at_us;
static TaskHandle_t s_voice_task;
static volatile int64_t s_alive_us;   /* the voice task's last sign of life: any feed below */

/* The gadget watchdog's voice user (muse_wdt.h), fed only from the voice
 * task itself: a feed from another task (the input task's shutdown wait)
 * would hide a stuck voice task. */
static void voice_fed(void)
{
    if (s_voice_task && xTaskGetCurrentTaskHandle() == s_voice_task) {
        s_alive_us = esp_timer_get_time();
        muse_wdt_feed(MUSE_WDT_MUSE_VOICE);
    }
}
#define AT(where) do { s_at = (where); s_at_us = esp_timer_get_time(); voice_fed(); } while (0)

/*
 * Every blocking call the voice task makes goes through these (the macros
 * below redirect this file's calls): each feeds the watchdog, and a mic read
 * or speaker write that took over 5 s says so once it returns (on mg.voice,
 * the Nordic UART mirror). One that never returns stops the feeds, and the
 * watchdog panics with the task's backtrace (muse_wdt.h).
 */
#define SLOW_AUDIO_US 5000000
static void slow_audio(const char *what, int64_t t0, esp_err_t err)
{
    int64_t dt = esp_timer_get_time() - t0;
    if (dt >= SLOW_AUDIO_US) {
        ESP_LOGW("mg.voice", "a %s blocked %lld ms (%s)", what, (long long)(dt / 1000), esp_err_to_name(err));
    }
}

static esp_err_t voice_read(int16_t *pcm, size_t frames)
{
    voice_fed();
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = (muse_audio_read)(pcm, frames);
    slow_audio("mic read", t0, err);
    voice_fed();
    return err;
}

static esp_err_t voice_write(const int16_t *pcm, size_t frames)
{
    voice_fed();
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = (muse_audio_write)(pcm, frames);
    slow_audio("speaker write", t0, err);
    voice_fed();
    return err;
}

static void voice_delay(TickType_t ticks)
{
    voice_fed();
    (vTaskDelay)(ticks);
    voice_fed();
}

/* Resting: at most 10 s at a time (the watchdog's 30 s, with room), then
 * round the loop again. */
#define REST_SLICE_MS 10000
static void voice_wait_awake(uint32_t ms)
{
    voice_fed();
    (muse_state_wait_awake)(ms < REST_SLICE_MS ? ms : REST_SLICE_MS);
    voice_fed();
}

__attribute__((unused)) static size_t voice_chat_audio_wait(const int16_t *pcm, size_t frames, int wait_ms)
{
    voice_fed();
    size_t n = (muse_hatch_turn_audio_wait)(pcm, frames, wait_ms);
    voice_fed();
    return n;
}

__attribute__((unused)) static size_t voice_chat_read(int16_t *pcm, size_t frames, int wait_ms)
{
    voice_fed();
    size_t n = (muse_hatch_turn_read)(pcm, frames, wait_ms);
    voice_fed();
    return n;
}

#define muse_audio_read(pcm, frames) voice_read((pcm), (frames))
#define muse_audio_write(pcm, frames) voice_write((pcm), (frames))
#define vTaskDelay(ticks) voice_delay(ticks)
#define muse_state_wait_awake(ms) voice_wait_awake(ms)
#define muse_hatch_turn_audio_wait(pcm, frames, ms) voice_chat_audio_wait((pcm), (frames), (ms))
#define muse_hatch_turn_read(pcm, frames, ms) voice_chat_read((pcm), (frames), (ms))

uint32_t muse_voice_quiet_ms(void)
{
    int64_t t = s_alive_us, now = esp_timer_get_time();
    if (!t) {
        return 0;   /* not started */
    }
    int64_t d = (now - t) / 1000;
    return d < 0 ? 0 : d > UINT32_MAX ? UINT32_MAX : (uint32_t)d;
}

char muse_voice_task_state(void)
{
    if (!s_voice_task) {
        return '-';
    }
    switch (eTaskGetState(s_voice_task)) {
    case eRunning: return 'X';
    case eReady: return 'R';
    case eBlocked: return 'B';
    case eSuspended: return 'S';
    case eDeleted: return 'D';
    default: return '?';
    }
}

/* The last mic chunk a talk turn or a client's capture read (mg_turn). */
static volatile int64_t s_chunk_us;

bool muse_voice_capture_stalled(uint32_t stall_ms)
{
    int64_t now = esp_timer_get_time(), entered = s_at_us, chunk = s_chunk_us;
    if (strcmp(s_at, "client capture") != 0 || now - entered < (int64_t)stall_ms * 1000) {
        return false;   /* not in a client capture, or not for long */
    }
    return chunk < entered || now - chunk >= (int64_t)stall_ms * 1000;
}

bool muse_voice_in_client_capture(void)
{
    return strcmp(s_at, "client capture") == 0;
}

const char *muse_voice_where(uint32_t *ms)
{
    const char *at = s_at;
    int64_t since = esp_timer_get_time() - s_at_us;
    if (ms) {
        *ms = since < 0 ? 0 : since > (int64_t)UINT32_MAX * 1000 ? UINT32_MAX : (uint32_t)(since / 1000);
    }
    return at;
}

#define MAX_SECS 15
#define TAIL_FRAMES (MUSE_AUDIO_RATE * 12 / 100)   /* capture lag + poll interval, stops before the release click */
/* A press's capture for a musegadgets client
 * goes on ~200 ms after the release (button_up), then its last frame, then
 * stop_mic; the ring stays up until it actually stops. A turn the
 * device ends (the cap, the client stopping it, a mic error) stops at once. */
#define BLE_POSTROLL_FRAMES (MUSE_AUDIO_RATE * 20 / 100)
#define MAX_FRAMES (MUSE_AUDIO_RATE * MAX_SECS)
/* The hold contract: a press counts only once held this
 * long (button_down goes then, with the pre-roll from the key- or touch-down
 * behind it); a shorter one is a tap and sends nothing. */
#define HOLD_MS 250
#define MIN_HELD_FRAMES (MUSE_AUDIO_RATE * HOLD_MS / 1000)   /* shorter presses are taps, not speech */
#define BUZZ_MS 100     /* ptt_buzz_duration_ms's default (mgcommands.h) */
#define BUZZ_PCT 80     /* ptt_buzz_volume_percent's default */
#if CONFIG_MUSE_BOARD_M5STACK_CARDPUTER_ADV || CONFIG_MUSE_BOARD_AI_PASSPORT
#define PRE_CHUNKS 18                                  /* 360 ms (>= HOLD_MS + 100 ms), ADPCM */
#else
#define PRE_CHUNKS 20                                  /* 400 ms (>= HOLD_MS + 100 ms, <= 1 s) */
#endif
_Static_assert(PRE_CHUNKS * MUSE_AUDIO_CHUNK * 1000 / MUSE_AUDIO_RATE >= HOLD_MS + 100 &&
                   PRE_CHUNKS * MUSE_AUDIO_CHUNK * 1000 / MUSE_AUDIO_RATE <= 1000,
               "the pre-roll keeps the hold threshold + 100 ms, at most 1 s");
/* CONFIG_MUSE_VOICE_NOTE_TOGGLE (#297, off by default, no realtime voice calls): a
 * talk press starts a voice note and the next press sends it; letting go
 * doesn't stop it. Nothing goes to Muse until it holds NOTE_MIN_RECORD_FRAMES
 * of audio (a duration, not detected speech), kept meanwhile in the pre-roll
 * ring, which is that long in a toggle build. */
#if CONFIG_MUSE_VOICE_NOTE_TOGGLE
#define NOTE_TOGGLE 1
#else
#define NOTE_TOGGLE 0
#endif
#define NOTE_MIN_RECORD_FRAMES (MUSE_AUDIO_RATE / 2)
#define NOTE_MIN_CHUNKS (NOTE_MIN_RECORD_FRAMES / MUSE_AUDIO_CHUNK)
#define PRE_CAP_CHUNKS (NOTE_TOGGLE && NOTE_MIN_CHUNKS > PRE_CHUNKS ? NOTE_MIN_CHUNKS : PRE_CHUNKS)
/* Low-memory boards upload a note synchronously: the pre-roll goes up in
 * bursts this long between mic reads, not all at once, so capture doesn't
 * stall behind it (#311; 120 ms bursts don't stall the ADV or the C3). */
#define PRE_BURST_CHUNKS 6
#define SETTLE_CHUNKS 10   /* after Muse makes a sound, 200 ms of capture is its own tail */
#define REST_BACKSTOP_MS 60000

#if CONFIG_MUSE_HATCH
/* A note recorded while Muse is out of reach is saved in PSRAM, and goes once it's back. */
#define HOLD_NOTES 1
#else
#define HOLD_NOTES 0   /* no PSRAM: a press needs Muse */
#endif
#if CONFIG_MUSE_RESEND_NOTES
/* Tried until Muse has it (muse_resend.h); the oldest makes room for a new one. */
#define HELD_MAX CONFIG_MUSE_RESEND_NOTES_MAX
#define HELD_BYTES ((size_t)CONFIG_MUSE_RESEND_NOTES_KB * 1024)
#define MAY_HAVE_IT "MAY HAVE SENT - SEE MUSE APP"
#else
#define HELD_MAX 4
#endif
#define HELD_TRIES 8                            /* then a saved note is dropped */
#define HELD_POLL_MS 5000                       /* resting with notes saved: check for Wi-Fi this often */
#define HELD_KEEP_US (30LL * 60 * 1000000)      /* muse_voice_notes_waiting() */
#define RETRY_MIN_US (15LL * 1000000)
#define RETRY_MAX_US (120LL * 1000000)
#define STALL_US (30LL * 1000000)               /* Muse took none of a note this long: give up */
#define ACK_WAIT_US (30LL * 1000000)            /* asleep, waiting for the VM to have a note */
#define REPLY_COMPLETE_HOLD_MS 300              /* a finished reply's full ring stays up this long */
#define REPLY_COMPLETE_POLL_MS 10
#define REPLY_AUDIO_GAP_US 200000   /* tolerate brief decoder/network gaps and the speaker's DMA tail */

static QueueHandle_t s_queue;
static uint32_t s_press_input_epoch;
static uint32_t s_press_mark;   /* the press's pending-call mark (muse_input_clear_call_pending) */
#if CONFIG_MUSE_WAKEWORD_ENABLE
static bool s_press_synthetic;   /* the turn is the wake word's (cleared if a finger takes it over) */
static uint32_t s_press_disables;   /* its press's disable generation */
static bool s_button_recording;  /* record() is taking a button (not hands-free) note */
#else
static const bool s_press_synthetic = false;   /* folds the hands-free paths away */
#endif

/* Whether an input event was posted by the wake word glue. */
static inline bool ev_synthetic(const muse_input_event_t *ev)
{
#if CONFIG_MUSE_WAKEWORD_ENABLE
    return ev->synthetic;
#else
    (void)ev;
    return false;
#endif
}

/* The press that starts a turn: remember whether it was hands-free. */
static inline void note_press_synthetic(const muse_input_event_t *ev)
{
#if CONFIG_MUSE_WAKEWORD_ENABLE
    s_press_synthetic = ev->synthetic;
    s_press_disables = ev->disables;
#else
    (void)ev;
#endif
}

/* A hands-free turn whose listening was turned off since its wake: it is
 * admitted, opened and sent nothing more. */
static inline bool hands_free_off(void)
{
#if CONFIG_MUSE_WAKEWORD_ENABLE
    return s_press_synthetic && !muse_wakeword_turn_wanted(s_press_disables);
#else
    return false;
#endif
}
static volatile bool s_monitor;
static volatile float s_monitor_db = -100.0f;
static volatile bool s_chirp;
static volatile bool s_loopback;
static volatile bool s_mp3test;
static atomic_int s_call_view;
static atomic_bool s_ptt_call;
static atomic_bool s_shutdown;
static atomic_bool s_reply_waiting;

bool muse_voice_reply_waiting(void) { return atomic_load(&s_reply_waiting); }

muse_voice_view_t muse_voice_call_view(void) { return atomic_load(&s_call_view); }
bool muse_voice_in_call(void) { return muse_voice_call_view() != MUSE_VOICE_VIEW_NONE; }

bool muse_voice_reply_holds_panel(void)
{
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    if (mg_voice_audible()) {
        return true;   /* a client's reply audio reaching the speaker (not an open stream's silence) */
    }
#endif
    return false;
}
bool muse_voice_ptt_call(void) { return muse_voice_in_call() && atomic_load(&s_ptt_call); }
bool muse_voice_ptt_enabled(void)
{
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH_FAMILY && CONFIG_MUSE_REALTIME_VOICE
    return !muse_settings_voice_notes();
#else
    return false;
#endif
}

void muse_voice_shutdown(int wait_ms)
{
    /* Input-task callers publish only this signal. The voice task owns the
     * core's capture state and calls end from call_input(), never here. */
    atomic_store(&s_shutdown, true);
    int64_t until = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    while (muse_voice_in_call() && esp_timer_get_time() < until) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void muse_voice_cancel_shutdown(void) { atomic_store(&s_shutdown, false); }

/* The caption while a link comes up: a call connecting, or a press while a
 * client's BLE link is up but not ready for it. */
#define CAPTION_CONNECTING "Connecting"
/* A push-to-talk turn the bench guard held that never rose (ptt.minrise): nothing went. */
#define CAPTION_NOTHING_HEARD "Didn't catch that"

const char *muse_voice_call_label(void)
{
    if (!muse_voice_ptt_call()) return NULL;
    switch (muse_voice_call_view()) {
    case MUSE_VOICE_VIEW_CONNECTING: return CAPTION_CONNECTING;
    case MUSE_VOICE_VIEW_READY: return "Hold to talk";
    case MUSE_VOICE_VIEW_LISTENING:
    case MUSE_VOICE_VIEW_THINKING:   /* never a text label for a turn's state */
    case MUSE_VOICE_VIEW_WORKING:    /* Muse still on it (#278): the face shows it */
    case MUSE_VOICE_VIEW_SPEAKING: return "";
    case MUSE_VOICE_VIEW_ENDING: return "Ending call";
    default: return NULL;
    }
}

const char *muse_voice_call_hint(void)
{
    if (!muse_voice_ptt_call()) return NULL;
    switch (muse_voice_call_view()) {
    case MUSE_VOICE_VIEW_LISTENING: return "Release";
    case MUSE_VOICE_VIEW_THINKING:
    case MUSE_VOICE_VIEW_SPEAKING: return "HOLD TO\nINTERRUPT";
    case MUSE_VOICE_VIEW_ENDING: return "";
    default: return "HOLD TO\nTALK";
    }
}

/*
 * Pre-roll: while idle the mic keeps running into this ring, so a recording
 * can start a little before the press. People start talking as they press,
 * and the power key reports the press late.
 */
typedef struct {
#if MUSE_LOW_MEM
    muse_adpcm_t start;   /* encoder state before this chunk, to decode it alone */
    uint8_t adpcm[MUSE_AUDIO_CHUNK / 2];
#else
    int16_t pcm[MUSE_AUDIO_CHUNK];
#endif
} pre_chunk_t;

static pre_chunk_t *s_pre;   /* PRE_CAP_CHUNKS, a ring */
static size_t s_pre_next, s_pre_fill;
#if MUSE_LOW_MEM
static muse_adpcm_t s_pre_enc;
#endif
static int16_t s_chunk[MUSE_AUDIO_CHUNK];
static int s_settle;

#if CONFIG_MUSE_WAKEWORD_ENABLE
/* Wake word pre-roll seeded for the next record(), whole chunks. */
#define SEED_FRAMES (MUSE_AUDIO_RATE)   /* one second, matching the glue */
static int16_t *s_seed;
static volatile size_t s_seed_frames;
#endif

/*
 * The note being recorded (HOLD_NOTES), kept whole in case it can't go now.
 * Streaming, Muse has had s_sent frames of it.
 */
static int16_t *s_rec;   /* MAX_FRAMES */
static size_t s_rec_n, s_sent;
static bool s_live = true;
static bool s_fail_caption;   /* the idle caption is a failed turn's */
static bool s_fail_away;      /* and Muse has been out of reach since */
static bool s_tried;     /* it went to Muse */

typedef struct {
    int16_t *pcm;
    size_t frames;
    int64_t at_us;   /* when it was recorded */
    int tries;
} held_note_t;

static int s_held_count;
static volatile bool s_waiting;        /* muse_voice_notes_waiting() */
#if HOLD_NOTES
static held_note_t s_held[HELD_MAX];   /* oldest first */
static int64_t s_next_send_us;
static int64_t s_send_backoff_us = RETRY_MIN_US;
#endif

/* The i-th oldest pre-roll chunk, into out. */
static void pre_get(size_t i, int16_t *out)
{
    const pre_chunk_t *c = &s_pre[(s_pre_next + PRE_CAP_CHUNKS - s_pre_fill + i) % PRE_CAP_CHUNKS];
#if MUSE_LOW_MEM
    muse_adpcm_t dec = c->start;
    muse_adpcm_decode_block(&dec, c->adpcm, MUSE_AUDIO_CHUNK, out);
#else
    memcpy(out, c->pcm, sizeof(c->pcm));
#endif
}

/* Call after Muse plays anything: its own sound must not become pre-roll. */
static void pre_reset(void)
{
    s_pre_fill = 0;
    s_settle = SETTLE_CHUNKS;
}

/* Stores one successful read in the pre-roll (or the isolated waking hold). */
static void pre_put(void)
{
    pre_chunk_t *c = &s_pre[s_pre_next];
#if MUSE_LOW_MEM
    c->start = s_pre_enc;
    muse_adpcm_encode_block(&s_pre_enc, s_chunk, MUSE_AUDIO_CHUNK, c->adpcm);
#else
    memcpy(c->pcm, s_chunk, sizeof(c->pcm));
#endif
    s_pre_next = (s_pre_next + 1) % PRE_CAP_CHUNKS;
    s_pre_fill = s_pre_fill < PRE_CAP_CHUNKS ? s_pre_fill + 1 : PRE_CAP_CHUNKS;
}

/* One 20 ms read: the settings mic meter, and with keep (a press is held,
 * the mic ring on) the pre-roll. Idle reads keep nothing: the gadget never
 * holds audio from before a press, when its ring was off (the ring
 * shows whenever the mic captures). */
static void idle_capture(bool keep)
{
    AT("idle mic read");
    if (muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    if (s_monitor) {
        float db = muse_audio_dbfs(s_chunk, MUSE_AUDIO_CHUNK);
        s_monitor_db = db > s_monitor_db ? db : s_monitor_db * 0.9f + db * 0.1f;
    }
    if (s_settle > 0) {
        s_settle--;
        return;   /* Muse's own tail: not pre-roll, and not wake word input either */
    }
    muse_wakeword_on_audio(s_chunk, MUSE_AUDIO_CHUNK);   /* wake tap: copy and return */
    if (keep) {
        pre_put();
    }
}

#if CONFIG_MUSE_WAKEWORD_ENABLE
/* Whether a queued edge counts for a phase looking for `type`. Real edges
 * count in their own input epoch. Wake word edges never shape a button
 * note: a synthetic release only ends a hands-free turn, and a synthetic
 * press doesn't count while a button note records. */
static bool edge_counts(const muse_input_event_t *ev, muse_ptt_t type)
{
    if (!ev->synthetic) return ev->epoch == muse_input_call_epoch();
    if (type == MUSE_PTT_UP) return s_press_synthetic;
    return !s_button_recording;
}

/* A real edge in the current input epoch: one the input queue's overflow
 * made stale is ignored here, so it owns nothing for the wake word either. */
static bool live_real_edge(const muse_input_event_t *ev)
{
    return !ev->synthetic && ev->epoch == muse_input_call_epoch();
}
/* Wake word preemption needs real presses even when a drain is only
 * looking for the other edge. */
#define NOTE_REAL_EDGE(ev) \
    do { if (live_real_edge(&(ev))) muse_wakeword_note_ptt((ev).type == MUSE_PTT_DOWN); } while (0)
#else
#define NOTE_REAL_EDGE(ev) ((void)0)
#endif

/* Consume through the requested edge, leaving later edges for the next
 * phase (notably a quick release after interrupting a saved note's reply). */
/* When the press being decided was seen (muse_input_event_t.at_us): the hold
 * threshold counts from there, not from when this task got to it (a dark
 * panel's resting codecs take a while to open). */
static int64_t s_press_at_us;
static void note_press(const muse_input_event_t *ev)
{
    s_press_at_us = ev->at_us ? ev->at_us : esp_timer_get_time();
}

static bool got_event(muse_ptt_t type)
{
    muse_input_event_t ev;
    while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
        muse_state_poke();
        NOTE_REAL_EDGE(ev);
#if CONFIG_MUSE_WAKEWORD_ENABLE
        if (live_real_edge(&ev) && ev.type == MUSE_PTT_DOWN && type == MUSE_PTT_UP && s_press_synthetic) {
            /* A finger on a hands-free note takes it over: from now on it's
             * the button's, ended by the finger's release, and a synthetic
             * release (endpointing that raced the press) no longer counts. */
            s_press_synthetic = false;
        }
        if (ev.type == type && edge_counts(&ev, type)) {
#else
        if (ev.epoch == muse_input_call_epoch() && ev.type == type) {
#endif
            if (type == MUSE_PTT_DOWN) {
                s_press_input_epoch = ev.epoch;
                s_press_mark = ev.mark;
                note_press(&ev);
                note_press_synthetic(&ev);
            }
            return true;
        }
        if (ev_synthetic(&ev) && ev.type == MUSE_PTT_DOWN) {
            muse_wakeword_turn_ended();   /* a hands-free press dropped: its capture closes */
        }
        muse_input_clear_call_pending(ev.mark);   /* a press dropped here starts nothing */
    }
    return false;
}

/*
 * The press's button_down to a musegadgets client goes once the hold
 * threshold passes, and button_up only after a button_down: whoever
 * takes s_gesture_down (the input task at the release, or the turn when it
 * sees the release first) sends the up, so a client never sees an up alone.
 */
__attribute__((unused)) static atomic_bool s_gesture_down;   /* unused without musegadgets BLE audio */

static void gesture_down(void)
{
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    mg_voice_gesture(true);
    atomic_store(&s_gesture_down, true);
#endif
}

static void gesture_up(void)
{
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    if (atomic_exchange(&s_gesture_down, false)) {
        mg_voice_gesture(false);
    }
#endif
}

void muse_voice_talk_released(void)
{
    gesture_up();
}

/*
 * The barge-in echo rule: the reply is
 * muted at touch-down (keeping its audio), not at the hold threshold, so
 * the ~250 ms of pre-roll recorded before the turn is claimed doesn't carry
 * the Muse's own words. A tap resumes it where it paused; at the threshold
 * claim_turn() takes the speaker as before (stop_streaming drop, or the
 * call's flush). s_barge_owed: the voice task decided a press and hasn't
 * claimed the turn: the pause is its to end (the loop's top ends it for
 * every path that didn't; one never decided lapses, mg_proto.h).
 */
static bool s_barge_owed;

void muse_voice_talk_pressed(void)
{
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    mg_voice_barge_pause(true);
#endif
}

static void barge_resume(void)
{
    if (s_barge_owed) {
        s_barge_owed = false;
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        mg_voice_barge_pause(false);
#endif
    }
}

/* The motor's buzz at the talk threshold, where the board has one. */
static void threshold_buzz(void)
{
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    if (mg_ble_haptic_talk_cue(BUZZ_MS)) {
        return;   /* through the haptics core: its rules, and it takes the motor from a client's pattern */
    }
#endif
    if (muse_board->buzz && muse_settings_haptics()) {
        muse_board->buzz(BUZZ_MS, BUZZ_PCT);
    }
}

/*
 * The hold contract: from the key- or touch-down the mic captures into
 * the pre-roll with the ring on at once; the press is a talk only if it's
 * still held HOLD_MS later. A tap (or a swipe, which the screen reports as a
 * release) sends nothing to anyone and says "Hold to talk".
 */
/*
 * The press's release, if it's queued, against the threshold `until`: -1 not
 * yet, 0 let go before it (a tap: the release is taken), 1 let go after it
 * (held through it: the release stays queued for the turn, which ends at once).
 * Anything else ahead of it goes as got_event drops it.
 */
static int release_vs(int64_t until)
{
    muse_input_event_t ev;
    while (xQueuePeek(s_queue, &ev, 0) == pdTRUE) {
        if (ev_synthetic(&ev)) {
            xQueueReceive(s_queue, &ev, 0);   /* wake word edges never decide a button press */
            if (ev.type == MUSE_PTT_DOWN) muse_wakeword_turn_ended();
            continue;
        }
        if (ev.epoch == muse_input_call_epoch() && ev.type == MUSE_PTT_UP) {
            if (ev.at_us && ev.at_us >= until) {
                return 1;
            }
            xQueueReceive(s_queue, &ev, 0);
            muse_state_poke();
            NOTE_REAL_EDGE(ev);
            return 0;
        }
        xQueueReceive(s_queue, &ev, 0);
        muse_state_poke();
        NOTE_REAL_EDGE(ev);
        muse_input_clear_call_pending(ev.mark);   /* a press dropped here starts nothing */
    }
    return -1;
}

/*
 * The press talks if it's still held HOLD_MS after it was seen,
 * on a lit screen or a dark one, however long the mic took to open; listening
 * into the pre-roll meanwhile. False: let go sooner (a tap).
 */
static bool press_held(void)
{
    /* The SenseCAP Watcher's wheel posts its press once held MUSE_WHEEL_HOLD_MS
     * (a click sleeps), with the press's own time: the threshold has passed
     * when it arrives, so no second wait (#156). */
    int64_t until = s_press_at_us + HOLD_MS * 1000LL;
    for (;;) {
        int r = release_vs(until);
        if (r >= 0) {
            return r == 1;
        }
        if (esp_timer_get_time() >= until) {
            return true;
        }
        idle_capture(true);
    }
}

static bool hold_gate(bool buzz)
{
    s_settle = 0;   /* the press is the user's: nothing of Muse's own to skip */
    s_pre_fill = 0;
    muse_mic_ring_set(true);
    if (!press_held()) {
        s_pre_fill = 0;
        muse_mic_ring_set(false);
        barge_resume();   /* a tap: the reply plays on from where it paused */
        ESP_LOGI(TAG, "tap: under %d ms, nothing sent", HOLD_MS);
        return false;
    }
    if (buzz) {
        threshold_buzz();
    }
    return true;   /* the turn keeps the ring on and sends the pre-roll first */
}

/* Level stats of a recording, gathered chunk by chunk for the log. */
typedef struct {
    double acc;
    size_t frames;
    int peak, clipped;
    float floor_db;
    float tail_db[25];   /* the last 0.5 s, to see where speech ends and the release click lands */
    size_t chunks;
    mg_loud1s_t loud;    /* the loudest 1 s window, from each 10 ms half-chunk (mg_loud1s.h) */
} rec_stats_t;

/* The loudest second's ring (MG_LOUD1S_RING_BYTES, PSRAM), kept for good; NULL: not measured. */
static uint32_t *s_loud_ring;

static void stats_start(rec_stats_t *st)
{
    if (!s_loud_ring) {
        s_loud_ring = heap_caps_malloc(MG_LOUD1S_RING_BYTES, MUSE_BIG_CAPS);
    }
    mg_loud1s_reset(&st->loud, s_loud_ring);
}

/* Adds one captured chunk (up to MUSE_AUDIO_CHUNK frames, 20 ms) to the
 * stats, as two halves: each half's energy is one 10 ms frame of the loudest
 * second, from the sum already kept (nothing more per sample). */
static void keep(rec_stats_t *st, const int16_t *pcm, size_t n)
{
    size_t half = n / 2;
    for (size_t h = 0, i = 0; h < 2; h++) {
        double before = st->acc;
        size_t end = h ? n : half;
        for (; i < end; i++) {
            int v = abs(pcm[i]);
            st->peak = v > st->peak ? v : st->peak;
            st->clipped += v >= 32000;
            st->acc += (double)pcm[i] * pcm[i];
        }
        size_t len = h ? n - half : half;
        if (len) {
            mg_loud1s_push(&st->loud, (uint32_t)((st->acc - before) / (double)len));
        }
    }
    st->frames += n;
    if (n == MUSE_AUDIO_CHUNK) {
        float db = muse_audio_dbfs(pcm, n);
        st->floor_db = db < st->floor_db ? db : st->floor_db;
        st->tail_db[st->chunks++ % 25] = db;
    }
}

/* Gives Muse what it has room for of the note being recorded. */
static void feed_live(void)
{
#if HOLD_NOTES
    if (s_live && s_sent < s_rec_n) {
        s_sent += muse_hatch_turn_audio_wait(s_rec + s_sent, s_rec_n - s_sent, 0);
    }
#endif
}

/* Streams the note being recorded from its start, then the rest as it comes. */
static void go_live(void)
{
    if (hands_free_off()) return;
    muse_hatch_turn_begin();
    s_live = s_tried = true;
    s_sent = 0;
    feed_live();
}

/* One chunk of speech: into the stats, and on to Muse, by way of the kept note if there is one. */
static void take(rec_stats_t *st, const int16_t *pcm)
{
    if (hands_free_off()) return;
    keep(st, pcm, MUSE_AUDIO_CHUNK);
    if (!s_rec) {
        if (s_live) {
            muse_hatch_turn_audio(pcm, MUSE_AUDIO_CHUNK);
        }
        return;
    }
    memcpy(s_rec + s_rec_n, pcm, MUSE_AUDIO_CHUNK * sizeof(int16_t));
    s_rec_n += MUSE_AUDIO_CHUNK;
    feed_live();
}

/* Up to `chunks` of the pre-roll, oldest first, taken out of the ring (#311):
 * room for the reads that queue behind it on a low-memory board. */
static void take_pre(rec_stats_t *st, size_t chunks)
{
    while (chunks-- && s_pre_fill) {
        pre_get(0, s_chunk);
        s_pre_fill--;
        take(st, s_chunk);
    }
}

/* record(): a read failed before the release (or the toggled note's stop
 * press): a press that came out short then is a mic error, never a tap (#311). */
static bool s_capture_failed;

/*
 * Records speech until release (the next press for a toggled note) or
 * MAX_SECS, starting with the pre-roll; *held gets the part after the press
 * up to the release (the capture-lag tail after it never counts, #311). It
 * streams to Muse as it goes if it can.
 * If not, the note is kept in s_rec to send later, and streams from partway
 * if Muse comes within reach. Returns false if Muse failed the turn with no
 * kept note to fall back on (why says what failed). There is no start chirp:
 * anything played now would land on top of the first words.
 */
static bool record(bool seeded, bool barge_in, size_t *held, char *why, size_t cap)
{
#if CONFIG_MUSE_WAKEWORD_ENABLE
    /* A hands-free turn takes the chunks this task already read past its
     * seed (the wake task trails it) before anything goes. If overload
     * overwrote some, the note can't be continuous: it ends here, unsent. */
    const int16_t *later[MUSE_WAKEWORD_BACKLOG_CHUNKS];
    size_t backlog = 0;
    if (seeded && s_seed_frames &&
        !muse_wakeword_take_backlog(later, MUSE_WAKEWORD_BACKLOG_CHUNKS, &backlog)) {
        s_seed_frames = 0;
        free(s_rec);   /* can_record() allocated it for this note */
        s_rec = NULL;
        muse_wakeword_turn_ended();
        if (cap) why[0] = '\0';
        *held = 0;
        return false;
    }
#endif
    muse_state_set_own_turn(true);   /* a press's turn: the panel stays lit until done */
    muse_state_set_mode(MUSE_MODE_LISTENING);
    muse_state_set_progress(0);
#if CONFIG_MUSE_WAKEWORD_ENABLE
    s_button_recording = !seeded;
#endif
    s_rec_n = s_sent = 0;
    s_live = s_tried = false;
    if (!s_rec || (muse_hatch_ready() && !s_held_count)) {
        go_live();
    }
#if CONFIG_MUSE_PHOTO_NOTES
    /* A photo at the press, for a note the person's press started that goes now. */
    muse_photo_note_begin(s_live && !seeded);
    const char *photo = muse_photo_note_label();
    if (photo) {
        muse_state_set_caption("%s", photo);
    } else
#endif
    muse_state_set_caption(s_live ? "" : "Recording...");   /* no "Listening" text: the ring says it */
    bool heard = false, ok = true;
    bool gave_up = false;   /* Muse failed this note: it's kept, and goes later */
    char text[96];
    rec_stats_t st = { 0 };
    stats_start(&st);

    size_t n = 0;
    s_capture_failed = false;
    muse_mic_ring_set(true);   /* capturing for Muse, from here to the end of the loop */
    if (barge_in) {
        /* Pressed during playback: the queued tail of the reply is still sounding. */
        s_pre_fill = 0;   /* none of the ring goes with it */
        for (int i = 0; i < SETTLE_CHUNKS; i++) {
            muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK);
        }
    } else {
        /* The pre-roll first, in order; on a low-memory board a burst now and
         * the rest between the mic reads below. */
        n = s_pre_fill * MUSE_AUDIO_CHUNK;
        take_pre(&st, MUSE_LOW_MEM ? PRE_BURST_CHUNKS : s_pre_fill);
    }
#if CONFIG_MUSE_WAKEWORD_ENABLE
    if (seeded && s_seed_frames) {
        /* A hands-free turn: the seeded audio already includes the wake
         * word. Then what this task had already read past the seed's end
         * (taken above), so the command has no gap. */
        for (size_t off = 0; off < s_seed_frames; off += MUSE_AUDIO_CHUNK) {
            take(&st, s_seed + off);
            n += MUSE_AUDIO_CHUNK;
        }
        for (size_t i = 0; i < backlog; i++) {
            take(&st, later[i]);
            n += MUSE_AUDIO_CHUNK;
        }
    }
    s_seed_frames = 0;   /* used once, never by a button turn */
#endif
    size_t pre = n;
    bool released = false;
    size_t at_release = 0;
    size_t stop_at = MAX_FRAMES;
    while (n + MUSE_AUDIO_CHUNK <= stop_at && !hands_free_off()) {
        if (muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) != ESP_OK) {
            s_capture_failed = !released;
            break;
        }
        muse_state_set_level(muse_audio_level(s_chunk, MUSE_AUDIO_CHUNK));
        muse_wakeword_on_audio(s_chunk, MUSE_AUDIO_CHUNK);   /* live endpointing feed */
#if MUSE_LOW_MEM
        if (s_pre_fill) {
            pre_put();   /* behind the pre-roll still to go, in order */
            take_pre(&st, PRE_BURST_CHUNKS);
        } else
#endif
        {
            take(&st, s_chunk);
        }
        /* Live transcript as the caption. A failure stops the streaming; the
         * kept note goes later, or without one the failure is the caption. */
        muse_hatch_ev_t ev;
        while (s_live && (ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            if (ev == MUSE_HATCH_EV_HEARD && text[0]) {
                heard = true;
                muse_state_set_caption("%s", text);
            } else if (ev == MUSE_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "muse: %s", text);
                muse_hatch_turn_cancel();
                s_live = false;
                gave_up = true;
                if (!s_rec) {
                    ok = false;
                    strlcpy(why, text, cap);
                    muse_state_set_caption("%s", text);
                }
            }
        }
        n += MUSE_AUDIO_CHUNK;
        bool tick = n % (MUSE_AUDIO_CHUNK * 5) == 0;
        if (tick && s_rec && !s_live && !gave_up && !s_held_count && muse_hatch_ready()) {
            ESP_LOGI(TAG, "Muse in reach: streaming the note so far");
            go_live();
        }
        muse_state_set_progress((float)n / MAX_FRAMES);
        if (!heard && ok && tick) {
#if CONFIG_MUSE_PHOTO_NOTES
            if ((photo = muse_photo_note_label())) {
                muse_state_set_caption("%s • %.1fs", photo, (double)n / MUSE_AUDIO_RATE);
            } else
#endif
            /* The format is the caption's own text, for its translation (muse_lang.c). */
            muse_state_set_caption(s_live ? " %.1fs" : "Recording %.1fs", (double)n / MUSE_AUDIO_RATE);
        }
        /*
         * Capture runs 60-80 ms behind real time and people let go on their
         * last syllable, so keep going briefly after release.
         */
        /* A wake word note ends at its synthetic release (endpointing or
         * disable) in toggle builds too: no second press is coming. */
        if (!released && got_event(NOTE_TOGGLE && !seeded ? MUSE_PTT_DOWN : MUSE_PTT_UP)) {
            released = true;
            at_release = n;
            stop_at = n + TAIL_FRAMES < MAX_FRAMES ? n + TAIL_FRAMES : MAX_FRAMES;
        }
    }
    if (hands_free_off()) {
        /* Listening was turned off: drop the note, abandon a streamed start. */
        if (s_live) muse_hatch_turn_cancel();
        s_live = false;
        free(s_rec);
        s_rec = NULL;
        muse_state_set_level(0);
        muse_mic_ring_set(false);
        if (!released) muse_wakeword_turn_ended();
        if (cap) why[0] = '\0';
        *held = 0;
        return false;
    }
    take_pre(&st, s_pre_fill);   /* a low-memory board's pre-roll not yet sent (a read failed) */
    muse_mic_ring_set(false);
    muse_state_set_level(0);
#if CONFIG_MUSE_WAKEWORD_ENABLE
    s_button_recording = false;
#endif
    if (seeded && !released) {
        muse_wakeword_turn_ended();   /* the cap or a failure ended it, not the glue */
    }
    *held = (released ? at_release : n) - pre;

    char tail[160];
    int tl = 0;
    size_t shown = st.chunks < 25 ? st.chunks : 25;
    for (size_t i = st.chunks - shown; i < st.chunks; i++) {
        tl += snprintf(tail + tl, sizeof(tail) - tl, " %.0f", st.tail_db[i % 25]);
    }
    ESP_LOGI(TAG, "end levels:%s", tail);
    float rms_db = st.frames ? 10.0f * log10f((float)(st.acc / st.frames / (32768.0 * 32768.0)) + 1e-10f) : -100.0f;
    char loud[28] = "";
    float loud_db;
    if (mg_loud1s_db(&st.loud, &loud_db)) {
        snprintf(loud, sizeof(loud), ", loud1s %.1f dBFS", (double)loud_db);   /* the loudest 1 s window */
    }
    ESP_LOGI(TAG, "recorded %.2fs%s: rms %.1f dBFS, peak %.1f dBFS, floor %.1f dBFS%s, %d clipped, gain %d dB",
             (double)n / MUSE_AUDIO_RATE, released ? "" : " (max)", rms_db < -100.0f ? -100.0f : rms_db,
             20.0 * log10((st.peak + 1) / 32768.0), st.floor_db, loud, st.clipped, muse_audio_mic_gain());
    return ok;
}

static void go_idle(const char *caption);

/*
 * Plays Muse's reply as it arrives, with its text as the caption. Returns
 * true if interrupted by a new press. *delivered: the VM has the note.
 * opening: the caption until there's a transcript or reply.
 */
static bool hatch_reply(bool *delivered, const char *opening)
{
    muse_wakeword_set_playback(true);   /* never trigger on Muse's own voice */
    atomic_store(&s_reply_waiting, false);
    muse_state_set_progress(0);   /* the recording's progress must not carry into the reply (#334) */
    muse_state_set_mode(MUSE_MODE_THINKING);
    muse_state_set_caption("%s", opening);
    static int16_t buf[MUSE_AUDIO_CHUNK];
    static const int16_t silence[MUSE_AUDIO_CHUNK];
    char text[96];
    static char page[MUSE_CAPTION_MAX];
    char incomplete[sizeof(text)] = "";   /* DONE's reason a reply was cut short (#115) */
    bool done = false, speaking = false, replied = false;
    size_t played = 0;
    int progress_message = -1;
    int progress_logged = -1;
    float progress = 0;
    int64_t t0 = esp_timer_get_time();
    int64_t last_audio_us = t0;
    *delivered = false;
    for (;;) {
        muse_state_poke();   /* an active turn is activity: no auto-sleep mid-reply (#286) */
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            switch (ev) {
            case MUSE_HATCH_EV_HEARD:
                if (!speaking && !replied) {
                    muse_state_set_caption("\"%s\"", text);
                }
                break;
            case MUSE_HATCH_EV_SENT:
                *delivered = true;
                if (!speaking && !replied) {
                    muse_state_set_caption("Note sent, waiting for Muse");
                }
                break;
            case MUSE_HATCH_EV_REPLY:
#if CONFIG_MUSE_REPLY_NOTIFY
                if (!replied) {
                    /* In place of a vibration motor: flash the screen and,
                     * with the speaker on and the volume up, ping. */
                    muse_state_flash();
                    if (muse_settings_speaker_on() && muse_settings_volume() > 0) {
                        muse_audio_notify();
                    }
                }
#endif
                replied = *delivered = true;
                /* Once speech starts, the caption follows it. The event only
                 * has room for the page's start; the page itself comes below. */
                if (!speaking && !muse_hatch_turn_caption(played, page, sizeof(page))) {
                    muse_state_set_caption("%s", text);
                }
                break;
            case MUSE_HATCH_EV_DONE:
                done = *delivered = true;
                strlcpy(incomplete, text, sizeof(incomplete));
                break;
            case MUSE_HATCH_EV_ERROR:
                ESP_LOGW(TAG, "muse: %s", text);
                atomic_store(&s_reply_waiting, false);
                muse_state_set_level(0);
                muse_wakeword_set_playback(false);
                go_idle(text);
                s_fail_caption = true;   /* cleared once Muse is back (voice_task) */
                s_fail_away = false;
                return false;
            default:
                break;
            }
        }
        if (got_event(MUSE_PTT_DOWN)) {
            ESP_LOGI(TAG, "reply interrupted");
            atomic_store(&s_reply_waiting, false);
            muse_hatch_turn_cancel();
            muse_state_set_progress(0);
            muse_state_set_level(0);
            muse_wakeword_set_playback(false);
            return true;
        }
        size_t n = muse_hatch_turn_read(buf, MUSE_AUDIO_CHUNK, speaking || done ? 0 : 20);
        if (n) {
            atomic_store(&s_reply_waiting, false);
            if (!speaking) {
                speaking = true;
                muse_state_set_mode(MUSE_MODE_SPEAKING);
                ESP_LOGI(TAG, "reply audio after %.2fs", (esp_timer_get_time() - t0) / 1e6);
            }
            muse_state_set_level(muse_audio_level(buf, n));
            muse_audio_write(buf, n);
            last_audio_us = esp_timer_get_time();
            played += n;
        } else if (done) {
            atomic_store(&s_reply_waiting, true);
            muse_state_set_progress(!incomplete[0] && played ? 1 : 0);
            ESP_LOGI(TAG, "reply progress: %s", incomplete[0] ? "incomplete" : "complete");
            break;
        } else if (speaking) {
            /* Between messages: keep the speaker fed so it doesn't replay stale DMA. */
            muse_state_set_level(0);
            muse_audio_write(silence, MUSE_AUDIO_CHUNK);
            if (esp_timer_get_time() - last_audio_us >= REPLY_AUDIO_GAP_US) {
                atomic_store(&s_reply_waiting, true);
            }
        }
        if (speaking) {
            /* The reply's progress ring (#334). */
            int message;
            float next = muse_turn_progress(played, &message);
            if (message != progress_message) {
                progress_message = message;
                progress_logged = -1;
            }
            /* Keep the ring forward-only across messages. Their count is unknown,
             * so hold at 99% until the entire turn has completed successfully. */
            if (next > 0.99f) next = 0.99f;
            if (next > progress) progress = next;
            muse_state_set_progress(incomplete[0] ? 0 : progress);
            int percent = (int)(progress * 100);
            int bucket = percent == 99 ? 99 : percent / 10 * 10;
            if (!incomplete[0] && message >= 0 && bucket != progress_logged) {
                progress_logged = bucket;
                ESP_LOGI(TAG, "reply progress: message %d, %d%% (PCM cursor %.2fs)",
                         message, percent, (double)played / MUSE_AUDIO_RATE);
            }
        }
        /* The page being said, or before the speech the reply's opening page. */
        if ((speaking || replied) && muse_hatch_turn_caption(played, page, sizeof(page))) {
            muse_state_set_caption("%s", page);
        }
    }
    if (played) {
        muse_audio_write_end();   /* the speech chain's last 2 ms (a board with one; muse_audio.h) */
    }
    muse_state_set_level(0);
    muse_wakeword_set_playback(false);
    ESP_LOGI(TAG, "muse reply: %.2fs of audio, %.2fs total", (double)played / MUSE_AUDIO_RATE,
             (esp_timer_get_time() - t0) / 1e6);
    if (incomplete[0]) {
        /* A capped reply says so (#115), and the caption stays up. */
        ESP_LOGW(TAG, "muse: %s", incomplete);
        atomic_store(&s_reply_waiting, false);
        go_idle(incomplete);
        return false;
    }
    if (!played) {
        /* No speech (TTS unavailable): leave the reply text up for a moment. */
        vTaskDelay(pdMS_TO_TICKS(2500));
    } else {
        /* Keep the final caption and full ring visible long enough to register.
         * A new press still interrupts immediately and starts the next note. */
        for (int elapsed = 0; elapsed < REPLY_COMPLETE_HOLD_MS; elapsed += REPLY_COMPLETE_POLL_MS) {
            if (got_event(MUSE_PTT_DOWN)) {
                ESP_LOGI(TAG, "reply interrupted after completion");
                atomic_store(&s_reply_waiting, false);
                muse_hatch_turn_cancel();
                muse_state_set_progress(0);
                muse_state_set_level(0);
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(REPLY_COMPLETE_POLL_MS));
        }
    }
    atomic_store(&s_reply_waiting, false);
    return false;
}

/*
 * Mode changes of a musegadgets turn are logged on "mg.face", which the
 * Nordic UART mirror carries, so a client can follow the face over BLE.
 */
#if CONFIG_MUSE_GADGET_BLE_AUDIO
static const char *const FACE_NAMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = "boot", [MUSE_MODE_IDLE] = "idle", [MUSE_MODE_LISTENING] = "listening",
    [MUSE_MODE_THINKING] = "thinking", [MUSE_MODE_SPEAKING] = "speaking", [MUSE_MODE_ERROR] = "error",
    [MUSE_MODE_OFF] = "off",
};
#endif

static void set_mode(muse_mode_t mode)
{
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    if (muse_state_mode(NULL) != mode) {
        char what[24];
        snprintf(what, sizeof(what), "face %s", FACE_NAMES[mode]);
        mg_voice_face(what);
    }
#endif
    muse_state_set_mode(mode);
}

__attribute__((unused)) static void make_happy(void)   /* unused without musegadgets BLE audio */
{
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    mg_voice_face("face happy");
#endif
    muse_state_make_happy();
}

static void go_idle(const char *caption)
{
    muse_state_poke();   /* the reading clock starts when the turn ends, not at the press (#286) */
    muse_state_set_progress(0);
    set_mode(MUSE_MODE_IDLE);
    muse_state_set_caption("%s", caption);
    s_fail_caption = false;   /* a newer caption replaces a failed turn's */
}

/* Errors and status stay up; a press too short to count only needs a hint,
 * which clears itself (#283). */
static void too_short(const char *hint)
{
    muse_state_set_progress(0);
    set_mode(MUSE_MODE_IDLE);
    muse_state_set_hint(hint);
}

/* Why a press can't go to Muse; voice notes only go there. */
static const char *not_ready_reason(void)
{
    muse_hatch_status_t st;
    muse_hatch_status(&st);
    switch (st.state) {
    case MUSE_HATCH_NOT_SET: return "Set up Muse first";
    case MUSE_HATCH_OFFLINE: return "No Wi-Fi";
    default: return "Can't reach Muse";
    }
}

static volatile bool s_resting;

/*
 * Asleep on battery with nothing to do: power the codecs down (the loop rests
 * Wi-Fi alongside). They come back within one wait on waking, and a talk press
 * that woke Muse is kept for the loop (a note may follow). On USB power
 * nothing changes asleep.
 */
static void set_resting(bool rest)
{
    if (rest == s_resting) {
        return;
    }
    s_resting = rest;
    AT(rest ? "codecs off (rest)" : "codecs on (awake)");
    muse_audio_power(!rest);
    AT("voice session rest");
    muse_hatch_set_resting(rest);
    if (rest) {
        /* Presses from before sleeping are stale, but not one that just woke Muse. */
        muse_input_event_t ev;
        while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
            if (ev.wake) {
                xQueueSendToFront(s_queue, &ev, 0);
                break;
            }
            muse_input_clear_call_pending(ev.mark);   /* this press won't start a call */
        }
    } else {
        pre_reset();
    }
    ESP_LOGI(TAG, "%s", rest ? "resting: codecs off, Wi-Fi modem sleep" : "awake: codecs on");
}

#if NOTE_TOGGLE
typedef enum {
    TOGGLE_RECORD,      /* NOTE_MIN_RECORD_FRAMES in the ring: record() takes it from here */
    TOGGLE_TOO_SHORT,   /* the stop press came first: nothing went */
    TOGGLE_CANCELLED,   /* END_CALL, a dropped edge (a new input epoch), shutting down */
    TOGGLE_MIC_ERROR,
} toggle_t;

/*
 * A toggled note's start (NOTE_TOGGLE, #297): from the press the mic captures
 * into the pre-roll ring with the ring on, and nothing goes to Muse until it
 * holds NOTE_MIN_RECORD_FRAMES; letting go doesn't stop it. barge_in: pressed
 * during a reply, whose queued tail is still sounding: none of that counts.
 * A stop press that comes later stays queued for record().
 */
static toggle_t toggle_gate(bool barge_in)
{
    s_settle = 0;
    s_pre_fill = 0;
    muse_mic_ring_set(true);
    muse_state_set_own_turn(true);   /* a press's turn: the panel stays lit until done */
    set_mode(MUSE_MODE_LISTENING);
    muse_state_set_progress(0);
    muse_state_set_caption("%s", "");   /* no "Listening" text: the ring says it */
    int settle = barge_in ? SETTLE_CHUNKS : 0;
    toggle_t result = TOGGLE_RECORD;
    for (size_t captured = 0; captured < NOTE_MIN_RECORD_FRAMES;) {
        if (atomic_load(&s_shutdown) || s_press_input_epoch != muse_input_call_epoch()) {
            result = TOGGLE_CANCELLED;
            break;
        }
        muse_input_event_t ev;
        if (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
            muse_state_poke();
            muse_input_clear_call_pending(ev.mark);
            if (ev_synthetic(&ev)) {
                if (ev.type == MUSE_PTT_DOWN) muse_wakeword_turn_ended();   /* never shapes a button note */
                continue;
            }
            NOTE_REAL_EDGE(ev);
            if (ev.type == MUSE_PTT_END_CALL) {
                result = TOGGLE_CANCELLED;
                break;
            }
            if (ev.type == MUSE_PTT_DOWN && ev.epoch == muse_input_call_epoch()) {
                result = TOGGLE_TOO_SHORT;
                break;
            }
            continue;   /* letting go doesn't stop a toggled note */
        }
        AT("toggled note's opening");
        if (muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) != ESP_OK) {
            result = atomic_load(&s_shutdown) || s_press_input_epoch != muse_input_call_epoch()
                ? TOGGLE_CANCELLED : TOGGLE_MIC_ERROR;
            break;
        }
        if (settle) {
            settle--;
            continue;
        }
        pre_put();
        muse_state_set_level(muse_audio_level(s_chunk, MUSE_AUDIO_CHUNK));
        captured += MUSE_AUDIO_CHUNK;
    }
    if (result != TOGGLE_RECORD) {
        s_pre_fill = 0;
        muse_mic_ring_set(false);
        muse_state_set_level(0);
        ESP_LOGI(TAG, "toggled note: %s, nothing sent",
                 result == TOGGLE_TOO_SHORT ? "stopped under 500 ms" : result == TOGGLE_MIC_ERROR ? "mic error" : "cancelled");
    }
    return result;
}
#endif

/*
 * A press that woke the screen: listen from now, into the pre-roll, and
 * record only if it's still held after a tap's length. False if it was let
 * go sooner: it only woke Muse.
 */
static bool held_on_waking(void)
{
    s_settle = 0;   /* nothing played: no tail of Muse's own to skip */
    s_pre_fill = 0;
    muse_mic_ring_set(true);   /* listening for this press from here */
    bool held = press_held();
    if (!held) {
        s_pre_fill = 0;
        muse_mic_ring_set(false);
    } else {
        threshold_buzz();
    }
    return held;   /* held: the turn takes over the ring */
}

/* A waking PTT tap only wakes. Start a new, held-only ring at DOWN: none of
 * the idle pre-roll belongs to this press. UP leaves a later DOWN queued. */
static bool ptt_held_on_waking(void)
{
    s_pre_fill = 0;
    s_settle = 0;
    int64_t until = s_press_at_us + HOLD_MS * 1000LL;   /* from the press, as every other press */
    muse_mic_ring_set(true);   /* listening for this press from here */
    for (;;) {
        muse_input_event_t ev;
        if (s_press_input_epoch != muse_input_call_epoch()) {
            s_pre_fill = 0;
            muse_mic_ring_set(false);
            return false;
        }
        if (xQueuePeek(s_queue, &ev, 0) == pdTRUE && ev.type == MUSE_PTT_UP && ev.at_us && ev.at_us >= until) {
            return true;   /* let go after the threshold: held; the caller sees the release */
        }
        if (xQueuePeek(s_queue, &ev, 0) == pdTRUE) {
            if (ev_synthetic(&ev)) {
                xQueueReceive(s_queue, &ev, 0);   /* not this finger's release */
                if (ev.type == MUSE_PTT_DOWN) muse_wakeword_turn_ended();
                continue;
            }
            s_pre_fill = 0;
            muse_mic_ring_set(false);
            if (ev.type != MUSE_PTT_DOWN) xQueueReceive(s_queue, &ev, 0);
            return false;
        }
        if (esp_timer_get_time() >= until) return true;   /* held: the caller takes over the ring */
        if (muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (xQueuePeek(s_queue, &ev, 0) != pdTRUE) {
            pre_put();   /* still held after the blocking read */
        }
    }
}

static void drop_rec(void)
{
    free(s_rec);
    s_rec = NULL;
}

#if HOLD_NOTES
/* True if a press is queued, left there for the loop; releases ahead of it are dropped. */
static bool press_waiting(void)
{
    muse_input_event_t ev;
    while (xQueuePeek(s_queue, &ev, 0) == pdTRUE) {
        if (ev.type == MUSE_PTT_DOWN) {
            return true;
        }
        xQueueReceive(s_queue, &ev, 0);
    }
    return false;
}

static void back_off(void)
{
#if CONFIG_MUSE_RESEND_NOTES
    s_next_send_us = esp_timer_get_time() + muse_resend_delay_us(s_held[0].tries);
#else
    s_next_send_us = esp_timer_get_time() + s_send_backoff_us;
    s_send_backoff_us = s_send_backoff_us * 2 > RETRY_MAX_US ? RETRY_MAX_US : s_send_backoff_us * 2;
#endif
}

static void drop_oldest(void)
{
    free(s_held[0].pcm);
    s_held_count--;
    memmove(s_held, s_held + 1, s_held_count * sizeof(s_held[0]));
    if (!s_held_count) {
        s_next_send_us = 0;
        s_send_backoff_us = RETRY_MIN_US;
    }
}

#if CONFIG_MUSE_RESEND_NOTES
/* Drops the oldest saved notes until one more of `frames` fits; false if it can't. */
static bool make_room(size_t frames)
{
    size_t sizes[HELD_MAX];
    for (int i = 0; i < s_held_count; i++) {
        sizes[i] = s_held[i].frames * sizeof(int16_t);
    }
    int drop = muse_resend_make_room(sizes, s_held_count, HELD_MAX, HELD_BYTES, frames * sizeof(int16_t));
    if (drop < 0) {
        ESP_LOGW(TAG, "a %.1fs note is more than the %u KB kept to send again", (double)frames / MUSE_AUDIO_RATE,
                 (unsigned)(HELD_BYTES / 1024));
        return false;
    }
    while (drop--) {
        ESP_LOGW(TAG, "dropped the oldest saved note (%.1fs, %d tries) to keep a new one",
                 (double)s_held[0].frames / MUSE_AUDIO_RATE, s_held[0].tries);
        drop_oldest();
    }
    return true;
}

/* A note that isn't kept to send again: Muse refused it, or may have it. */
static void drop_unsendable(muse_chat_fail_t why, bool quiet)
{
    bool refused = why == MUSE_CHAT_FAIL_REFUSED;
    ESP_LOGW(TAG, "not sending a note again: %s", refused ? "Muse refused it" : "it went up whole, unacknowledged");
    const char *caption = refused ? "MUSE DIDN'T TAKE IT" : MAY_HAVE_IT;
    if (quiet) {
        muse_state_set_caption("%s", caption);
    } else {
        go_idle(caption);
    }
}
#endif

/* Saves the note just recorded to send later. `tried`: Muse failed it, so it waits out the backoff. */
static void hold_rec(bool tried)
{
#if CONFIG_MUSE_RESEND_NOTES
    if (s_rec_n && !make_room(s_rec_n)) {
        drop_rec();
        go_idle("Couldn't save the note");
        return;
    }
#endif
    if (s_held_count >= HELD_MAX) {   /* can_record() leaves room: not expected */
        drop_rec();
        go_idle("Couldn't save the note");
        return;
    }
    if (!s_rec_n) {   /* nothing captured: realloc to 0 would free s_rec and return NULL */
        drop_rec();
        too_short("Hold to talk");
        return;
    }
    int16_t *pcm = heap_caps_realloc(s_rec, s_rec_n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_held[s_held_count++] = (held_note_t){
        .pcm = pcm ? pcm : s_rec,
        .frames = s_rec_n,
        .at_us = esp_timer_get_time(),
        .tries = tried,
    };
    s_rec = NULL;
    if (tried) {
        back_off();
    }
    ESP_LOGI(TAG, "saved a %.1fs note to send later (%d waiting)", (double)s_rec_n / MUSE_AUDIO_RATE, s_held_count);
    go_idle(tried ? "Saved, will try again" : "Saved, sends when online");
}

typedef enum {
    FED,
    FEED_FAILED,    /* the turn failed, or Muse stopped taking audio */
    FEED_PRESSED,   /* a press is queued: it goes first */
} feed_t;

/*
 * Gives Muse a note from *sent on, waiting for room as it goes, then ends
 * the turn. Stops for a press only if `yield`.
 */
static feed_t feed_rest(const int16_t *pcm, size_t frames, size_t *sent, bool yield, bool wake_owned)
{
    char text[96];
    int64_t moved = esp_timer_get_time();
    for (;;) {
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            if (ev == MUSE_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "muse: %s", text);
                return FEED_FAILED;
            }
        }
        if (wake_owned && hands_free_off()) {
            return FEED_FAILED;   /* listening turned off: nothing more goes, no commit */
        }
        if (*sent == frames) {
            break;
        }
        if (yield && press_waiting()) {
            return FEED_PRESSED;
        }
        size_t n = muse_hatch_turn_audio_wait(pcm + *sent, frames - *sent, 100);
        *sent += n;
        int64_t now = esp_timer_get_time();
        if (n) {
            moved = now;
        } else if (now - moved > STALL_US) {
            ESP_LOGW(TAG, "Muse stopped taking the note");
            return FEED_FAILED;
        }
    }
    muse_hatch_turn_end();
    return FED;
}

/* Waits for the VM to have the note just sent; returns the event that says so, or NONE. */
static muse_hatch_ev_t wait_landed(void)
{
    char text[96];
    int64_t give_up = esp_timer_get_time() + ACK_WAIT_US;
    while (esp_timer_get_time() < give_up) {
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            if (ev == MUSE_HATCH_EV_SENT || ev == MUSE_HATCH_EV_REPLY || ev == MUSE_HATCH_EV_DONE) {
                return ev;
            }
            if (ev == MUSE_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "muse: %s", text);
                return MUSE_HATCH_EV_NONE;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "no word that the note arrived");
    return MUSE_HATCH_EV_NONE;
}

#if CONFIG_MUSE_RESEND_NOTES
/*
 * Sends the oldest saved note again, quietly: the screen changes only once
 * it lands or won't be sent again. Once the VM has it, awake, its reply
 * plays like any other; asleep, the reply waits in the app. A try that
 * failed in a way worth another waits out muse_resend_delay_us(). A press
 * while it goes up comes first, and the note stays saved. Returns true if a
 * press interrupted the reply.
 */
static bool send_held(bool asleep)
{
    held_note_t *h = &s_held[0];
    ESP_LOGI(TAG, "sending a saved note%s: %.1fs, from %llds ago, try %d", asleep ? " (asleep)" : "",
             (double)h->frames / MUSE_AUDIO_RATE, (long long)((esp_timer_get_time() - h->at_us) / 1000000),
             h->tries + 1);
    muse_hatch_turn_begin();
    size_t sent = 0;
    feed_t fed = feed_rest(h->pcm, h->frames, &sent, true, false);
    muse_hatch_ev_t landed = fed == FED ? wait_landed() : MUSE_HATCH_EV_NONE;
    if (landed == MUSE_HATCH_EV_NONE) {
        muse_hatch_turn_cancel();
        if (fed == FEED_PRESSED) {
            return false;
        }
        muse_chat_fail_t why = muse_resend_outcome(muse_chat_turn_failure(), fed == FED);
        if (why == MUSE_CHAT_FAIL_RETRY) {
            h->tries++;
            back_off();
            ESP_LOGI(TAG, "saved note not sent, next try in %llds",
                     (long long)((s_next_send_us - esp_timer_get_time()) / 1000000));
            return false;
        }
        drop_oldest();
        drop_unsendable(why, asleep);
        return false;
    }
    ESP_LOGI(TAG, "saved note sent");
    drop_oldest();
    s_next_send_us = 0;   /* the next one right away */
    if (asleep || landed == MUSE_HATCH_EV_DONE) {
        muse_hatch_turn_cancel();   /* asleep, the reply is left for the app */
        if (asleep) {
            muse_state_set_caption("Saved note sent");
        } else {
            go_idle("Saved note sent");
        }
        return false;
    }
    bool delivered;
    return hatch_reply(&delivered, "Saved note sent");
}
#else
/*
 * Sends the oldest saved note. Awake, it's a turn like any other, reply and
 * all; asleep (quiet), it's done once the VM has the note, and the reply
 * waits in the app. A press while it goes up comes first, and the note stays
 * saved. Returns true if a press interrupted the reply.
 */
static bool send_held(bool quiet)
{
    held_note_t *h = &s_held[0];
    ESP_LOGI(TAG, "sending a saved note%s: %.1fs, from %llds ago, try %d", quiet ? " (asleep)" : "",
             (double)h->frames / MUSE_AUDIO_RATE, (long long)((esp_timer_get_time() - h->at_us) / 1000000),
             h->tries + 1);
    if (!quiet) {
        muse_state_set_mode(MUSE_MODE_THINKING);
        muse_state_set_caption("Sending saved note");
    }
    muse_hatch_turn_begin();
    size_t sent = 0;
    feed_t fed = feed_rest(h->pcm, h->frames, &sent, true, false);
    bool delivered = false, interrupted = false;
    if (fed == FED && quiet) {
        delivered = wait_landed() != MUSE_HATCH_EV_NONE;
    } else if (fed == FED) {
        interrupted = hatch_reply(&delivered, "Sending voice note");
    }
    if (fed != FED || quiet) {
        muse_hatch_turn_cancel();   /* asleep, the reply is left for the app */
    }
    if (fed == FEED_PRESSED) {
        if (!quiet) {
            go_idle("");
        }
        return false;
    }
    const char *caption;
    if (delivered || interrupted) {
        drop_oldest();
        s_next_send_us = 0;   /* the next one right away */
        s_send_backoff_us = RETRY_MIN_US;
        if (quiet) {
            muse_state_set_caption("Saved note sent");
        }
        return interrupted;
    } else if (++h->tries >= HELD_TRIES) {
        ESP_LOGW(TAG, "giving up on a saved note after %d tries", h->tries);
        drop_oldest();
        caption = "COULDN'T SEND A SAVED NOTE";
    } else {
        back_off();
        caption = "Saved note: will try again";
    }
    if (quiet) {
        muse_state_set_caption("%s", caption);
    } else {
        go_idle(caption);
    }
    return false;
}
#endif

/*
 * Whether to send a saved note now: Muse within reach and the backoff over.
 * Coming within reach starts the backoff over. Keeps s_waiting too.
 */
static bool held_due(void)
{
    static bool was_ready = true;
    if (!s_held_count) {
        was_ready = true;
        s_waiting = false;
        return false;
    }
    int64_t now = esp_timer_get_time();
    s_waiting = now - s_held[0].at_us < HELD_KEEP_US;
    bool ready = muse_hatch_ready();
    if (ready && !was_ready) {
        s_next_send_us = 0;
        s_send_backoff_us = RETRY_MIN_US;
    }
    was_ready = ready;
    return ready && now >= s_next_send_us;
}
#endif

/*
 * After the release: the rest of the note goes to Muse and the reply plays,
 * or if it can't go now it's saved to send later. Returns true if a press
 * interrupted the reply.
 */
static bool finish_note(void)
{
    bool delivered;
#if CONFIG_MUSE_PHOTO_NOTES
    if (s_live) {
        muse_photo_note_wait();   /* the press's photo goes up with the note's end */
    }
#endif
#if HOLD_NOTES
    if (s_rec) {
        bool fed = false, interrupted = false;
        delivered = false;
        if (s_live && s_sent == s_rec_n && !hands_free_off()) {
            muse_hatch_turn_end();   /* before the chirp, which takes ~90 ms */
            fed = true;
        }
        muse_audio_chirp(0);
        if (s_live && !fed) {
            /* Muse is behind (still connecting, say): the rest from the kept note. */
            muse_state_set_mode(MUSE_MODE_THINKING);
            muse_state_set_caption("Sending voice note");
            fed = feed_rest(s_rec, s_rec_n, &s_sent, false, true) == FED;
            if (!fed) {
                muse_hatch_turn_cancel();
            }
        }
        if (fed) {
            interrupted = hatch_reply(&delivered, "Sending voice note");
        }
        if (delivered || interrupted || hands_free_off()) {
            drop_rec();   /* a hands-free note whose listening was turned off is never kept */
            return interrupted;
        }
#if CONFIG_MUSE_RESEND_NOTES
        /* Saved only if Muse can't have it: not refused, and not all of it went up. */
        muse_chat_fail_t why = s_tried ? muse_resend_outcome(muse_chat_turn_failure(), fed) : MUSE_CHAT_FAIL_RETRY;
        if (why != MUSE_CHAT_FAIL_RETRY) {
            drop_rec();
            drop_unsendable(why, false);
            return false;
        }
#endif
        hold_rec(s_tried);
        return interrupted;
    }
#endif
    muse_hatch_turn_end();   /* before the chirp, which takes ~90 ms */
    muse_audio_chirp(0);
    return hatch_reply(&delivered, "Sending voice note");
}

/*
 * Whether a press can record: Muse within reach, or (HOLD_NOTES) set up
 * with room to save the note. If not, the caption says why.
 */
static bool can_record(void)
{
    if (!muse_wifi_connected()) {
        muse_wifi_apply();   /* retry now, not after the backoff */
    }
    bool ready = muse_hatch_ready();
#if HOLD_NOTES
    muse_hatch_status_t st;
    muse_hatch_status(&st);
    if (st.state == MUSE_HATCH_NOT_SET) {
        go_idle("Set up Muse first");
        return false;
    }
#if !CONFIG_MUSE_RESEND_NOTES   /* with it, the oldest saved note makes room */
    if ((!ready || s_held_count) && s_held_count >= HELD_MAX) {
        go_idle("Notes still waiting to send");
        return false;
    }
#endif
    if (ready && s_held_count) {
        s_next_send_us = 0;   /* the saved ones go first, right after */
    }
    s_rec = heap_caps_malloc(MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rec) {
        ESP_LOGE(TAG, "no memory to keep a note");
    }
#endif
    if (!ready && !s_rec) {
        go_idle(not_ready_reason());
        return false;
    }
    return true;
}

#if CONFIG_MUSE_REALTIME_VOICE
/* Calls: how far the speaker runs behind a write, and how long the mic stays
 * shut after Muse's voice stops (these boards have no echo cancellation). */
#define CALL_SPEAKER_LAG_US 120000
#define CALL_TAIL_US 300000
#define CALL_END_WAIT_US (20LL * 1000000)   /* hung up: wait this long for the call to wind down */
#define CALL_RELEASE_US 120000            /* bounded capture lag, as for voice notes */

typedef struct {
    bool ptt, held, live, hung_up, server_hold, awaiting;
    unsigned press;
    uint32_t input_epoch;
    int64_t give_up, release_until;
    size_t release_frames;
    muse_vc_gate_t gate;
} call_ui_t;

static void call_view(muse_voice_view_t view);

static void finish_release(call_ui_t *c)
{
    if (!c->release_until) return;
    c->release_until = 0;
    c->release_frames = 0;
    muse_voice_call_set_talking(false);
}

/* Every edge matters: UP begins a bounded capture tail, whereas DOWN starts
 * a new press epoch. A read crossing DOWN must never enter that new press. */
static void call_input(call_ui_t *c)
{
    muse_input_event_t ev;
    if (c->input_epoch != muse_input_call_epoch()) c->hung_up = true;
    while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
        muse_state_poke();
        muse_input_clear_call_pending(ev.mark);   /* in the call now, or dropped */
        if (ev_synthetic(&ev)) {
            /* A call is never driven by the wake word: drop its edges. */
            if (ev.type == MUSE_PTT_DOWN) muse_wakeword_turn_ended();
            continue;
        }
        if (ev.epoch != c->input_epoch) continue;
        if (c->hung_up) continue;
        if (ev.type == MUSE_PTT_END_CALL || (!c->ptt && ev.type == MUSE_PTT_DOWN)) {
            c->hung_up = true;
        } else if (c->ptt && ev.type == MUSE_PTT_DOWN) {
            finish_release(c);   /* old press closes before the new one opens */
            ++c->press;
            c->held = true;
            muse_voice_call_set_talking(true);
            muse_vc_gate_init(&c->gate, false, 0);
            c->awaiting = false;
        } else if (c->ptt && ev.type == MUSE_PTT_UP && c->held) {
            c->held = false;
            muse_state_set_level(0);
            call_view(c->live ? MUSE_VOICE_VIEW_THINKING : MUSE_VOICE_VIEW_CONNECTING);
            if (c->server_hold) {
                muse_voice_call_set_talking(false);
            } else {
                c->release_until = esp_timer_get_time() + CALL_RELEASE_US;
                c->release_frames = TAIL_FRAMES;
            }
        }
    }
    if (atomic_load(&s_shutdown) || c->input_epoch != muse_input_call_epoch()) c->hung_up = true;
    if (c->release_until && (c->hung_up || c->server_hold ||
        esp_timer_get_time() >= c->release_until)) finish_release(c);
    if (c->hung_up && !c->give_up) {
        c->held = false;
        c->give_up = esp_timer_get_time() + CALL_END_WAIT_US;
        muse_voice_call_end();
        muse_state_set_level(0);
        call_view(MUSE_VOICE_VIEW_ENDING);
        muse_state_set_caption("Hanging up...");
    }
}

static void call_view(muse_voice_view_t view)
{
    atomic_store(&s_call_view, view);
    muse_mode_t mode = MUSE_MODE_THINKING;
    if (view == MUSE_VOICE_VIEW_LISTENING) mode = MUSE_MODE_LISTENING;
    else if (view == MUSE_VOICE_VIEW_SPEAKING) mode = MUSE_MODE_SPEAKING;
    else if (view == MUSE_VOICE_VIEW_READY) mode = MUSE_MODE_IDLE;
    muse_state_set_mode(mode);
}

/* One read/write per pass paces the shared duplex bus. PTT admits the held
 * interval plus a bounded capture-lag tail; never idle pre-roll or a block
 * crossing into a different press. */
static void run_call(bool ptt, size_t waking_chunks)
{
    static int16_t out[MUSE_AUDIO_CHUNK];
    static const int16_t silence[MUSE_AUDIO_CHUNK];
    char text[96] = "";
    call_ui_t c = { .ptt = ptt, .held = ptt, .input_epoch = s_press_input_epoch };
    muse_vc_gate_init(&c.gate, false, ptt ? 0 : CALL_TAIL_US);
    bool ended = false;
    int64_t caption_until = 0;
#if CONFIG_LV_USE_SNAPSHOT
    int64_t last_write = 0, longest_write_gap = 0;
    unsigned empty_during_speech = 0, write_errors = 0;
#endif
    atomic_store(&s_ptt_call, ptt);
    call_view(MUSE_VOICE_VIEW_CONNECTING);
    muse_input_clear_call_pending(s_press_mark);
    if (atomic_load(&s_shutdown) || c.input_epoch != muse_input_call_epoch()) {
        atomic_store(&s_call_view, MUSE_VOICE_VIEW_NONE);
        atomic_store(&s_ptt_call, false);
        if (!atomic_load(&s_shutdown)) go_idle("Input lost, press again");
        return;
    }
    muse_wakeword_set_ptt_turn(true);   /* a call pauses the runner like a recorded turn */
    muse_state_set_level(0);
    muse_state_set_progress(0);
    muse_state_set_caption("Connecting to Muse...");
#if CONFIG_MUSE_VOICE_SOFTWARE_AEC
    /* Hands-free only (#382): the mic stays open while Muse talks; NULL (no
     * AEC, or it couldn't start): half-duplex, as without it. */
    muse_aec_t *aec = ptt ? NULL : muse_aec_start();
#endif
    if (ptt) {
        muse_voice_call_start_ptt();
        /* These were captured while this waking press was held. Enqueue
         * them before UP, so release drains them like other captured input. */
        for (size_t i = 0; i < waking_chunks && !atomic_load(&s_shutdown) &&
             c.input_epoch == muse_input_call_epoch(); i++) {
            pre_get(i, s_chunk);
            c.awaiting |= muse_voice_call_audio(s_chunk, MUSE_AUDIO_CHUNK);
        }
    } else muse_voice_call_start();
    while (!ended) {
        call_input(&c);
        unsigned press = c.press;
        bool capture_window = c.held || c.release_until;
        bool captured = muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) == ESP_OK;
        call_input(&c);   /* UP permits the last syllable; DOWN invalidates this read */
        int64_t now = esp_timer_get_time();
        muse_voice_call_ev_t ev;
        while ((ev = muse_voice_call_event(text, sizeof(text))) != MUSE_VOICE_CALL_EV_NONE) {
            switch (ev) {
            case MUSE_VOICE_CALL_EV_READY:
                c.live = true;
                muse_state_set_caption(ptt ? "Hold yellow to talk" : "In a call, press to hang up");
                break;
            case MUSE_VOICE_CALL_EV_HEARD:
                caption_until = 0;
                muse_state_set_caption("\"%s\"", text);
                break;
            case MUSE_VOICE_CALL_EV_REPLY_START:
            case MUSE_VOICE_CALL_EV_REPLY:
                if (!ptt && c.live && !c.hung_up && !c.server_hold) {
                    /* Text can lead the first audible chunk. Give even a
                     * text-only phrase time to appear, independently of AEC. */
                    caption_until = now + 600000;
                    call_view(MUSE_VOICE_VIEW_SPEAKING);
                }
                if (ev == MUSE_VOICE_CALL_EV_REPLY) muse_state_set_caption("%s", text);
                break;
            case MUSE_VOICE_CALL_EV_HOLD:
                caption_until = 0;
                c.server_hold = true;
                finish_release(&c);
                muse_state_set_caption("One moment...");
                break;
            case MUSE_VOICE_CALL_EV_RESUME:
                c.server_hold = false;
                break;
            case MUSE_VOICE_CALL_EV_ENDED:
                ended = true;
                break;
            default:
                break;
            }
            if (ended) break;
        }
        if (ended) break;
        muse_voice_call_ptt_status_t status = {0};
        if (ptt) {
            muse_voice_call_ptt_status(&status);
            c.live = status.live;
            c.server_hold = status.hold;
            if (c.server_hold || (c.release_until && esp_timer_get_time() >= c.release_until)) finish_release(&c);
        }
        bool mic = captured && !c.hung_up && !c.server_hold && c.input_epoch == muse_input_call_epoch();
        if (ptt) mic = mic && capture_window && (c.held || c.release_until) && press == c.press && status.capture;
        else mic = mic && c.live &&
#if CONFIG_MUSE_VOICE_SOFTWARE_AEC
                   !aec &&
#endif
                   muse_vc_gate_mic_open(&c.gate, now);
        if (mic) mic = muse_voice_call_audio(s_chunk, MUSE_AUDIO_CHUNK);
#if CONFIG_MUSE_VOICE_SOFTWARE_AEC
        /* AEC sends its cleaned frames below, so `mic` is false in this mode.
         * Audio the call may not take (a server hold, say) reaches AEC as
         * silence, so none of it is sent in a later frame. */
        bool aec_mic = aec && captured && c.live && !c.hung_up && !c.server_hold &&
                       c.input_epoch == muse_input_call_epoch();
        muse_mic_ring_set(mic || aec_mic);   /* AEC's open mic captures for Muse too: the ring */
#else
        muse_mic_ring_set(mic);   /* the call's mic open: what goes to Muse this chunk */
#endif
        if (ptt && mic) {
            c.awaiting = true;
            if (c.release_until) {
                c.release_frames = c.release_frames > MUSE_AUDIO_CHUNK ? c.release_frames - MUSE_AUDIO_CHUNK : 0;
                if (!c.release_frames) finish_release(&c);
            }
        }
        bool show_mic = mic && c.live && (!ptt || c.held);
#if CONFIG_MUSE_VOICE_SOFTWARE_AEC
        if (aec_mic) show_mic = true;   /* the listening meter stays responsive */
#endif
        float mic_level = show_mic ? muse_audio_level(s_chunk, MUSE_AUDIO_CHUNK) : 0;
        if (s_monitor) s_monitor_db = show_mic ? muse_audio_dbfs(s_chunk, MUSE_AUDIO_CHUNK) : -100.0f;
        bool flushed = false;
        size_t n = 0;
        if (!c.hung_up && !(ptt && (c.held || c.release_until))) {
            n = ptt ? muse_voice_call_read(out, MUSE_AUDIO_CHUNK, &flushed)
                    : muse_voice_call_read_paced(out, MUSE_AUDIO_CHUNK, &flushed);
        }
#if CONFIG_MUSE_VOICE_SOFTWARE_AEC
        /* AEC's reference is one whole chunk per pass, so play exactly that:
         * a short read must not slide the echo away from its reference. */
        if (aec && n && n < MUSE_AUDIO_CHUNK) {
            memset(out + n, 0, (MUSE_AUDIO_CHUNK - n) * sizeof(int16_t));
            n = MUSE_AUDIO_CHUNK;
        }
#endif
        if (flushed) {
            caption_until = 0;
            muse_vc_gate_flush(&c.gate, now);
        }
        if (n) muse_vc_gate_playout(&c.gate, out, n, now, CALL_SPEAKER_LAG_US);
        esp_err_t written = muse_audio_write(n ? out : silence, n ? n : MUSE_AUDIO_CHUNK);
#if CONFIG_LV_USE_SNAPSHOT
        int64_t write_at = esp_timer_get_time();
        if (last_write && n && write_at - last_write > longest_write_gap) longest_write_gap = write_at - last_write;
        if (!n && !c.hung_up && !muse_vc_gate_mic_open(&c.gate, now)) ++empty_during_speech;
        if (written != ESP_OK) ++write_errors;
        last_write = write_at;
#else
        (void)written;
#endif
#if CONFIG_MUSE_VOICE_SOFTWARE_AEC
        if (aec) {
            /* Advance the reference even when a mic read fails; otherwise
             * each missed chunk shifts speaker echo out of alignment. On a
             * board with a speaker speech chain (muse_audio_write) the
             * reference is this PCM, before the chain. */
            size_t clean_n = muse_aec_feed(aec, aec_mic ? s_chunk : silence,
                          n && muse_settings_speaker_on() && !muse_motion_quiet() ? out : silence,
                          muse_settings_speaker_on() && !muse_motion_quiet() ? n : 0);
            if (clean_n && aec_mic) {
                int16_t *clean = muse_aec_output(aec);
                /* Below the near-end speech floor, a residual during
                 * playback should be silence to the server's voice detector. */
                if (!muse_vc_gate_mic_open(&c.gate, now) &&
                    muse_audio_dbfs(clean, clean_n) < -55.0f) {
                    memset(clean, 0, clean_n * sizeof(int16_t));
                }
                muse_voice_call_audio(clean, clean_n);
            }
        }
#endif
        bool speaking = !muse_vc_gate_mic_open(&c.gate, now) && !(ptt && (c.held || c.release_until));
        if (ptt && !c.held && !status.sending &&
            ((!status.heard && !status.reply_open) || (status.reply_done && !speaking))) c.awaiting = false;
        if (ptt && (status.heard || status.reply_open) && !status.reply_done) c.awaiting = true;
        muse_voice_view_t view = c.hung_up ? MUSE_VOICE_VIEW_ENDING :
            !c.live ? MUSE_VOICE_VIEW_CONNECTING :
            c.server_hold ? MUSE_VOICE_VIEW_THINKING :
            ptt && c.held ? (mic ? MUSE_VOICE_VIEW_LISTENING : MUSE_VOICE_VIEW_THINKING) :
            (speaking || now < caption_until) ? MUSE_VOICE_VIEW_SPEAKING :
            !ptt ? (muse_voice_call_working() ? MUSE_VOICE_VIEW_WORKING : MUSE_VOICE_VIEW_LISTENING) :
            c.release_until || c.awaiting || status.sending ? MUSE_VOICE_VIEW_THINKING :
            status.working ? MUSE_VOICE_VIEW_WORKING : MUSE_VOICE_VIEW_READY;
        if (ptt && view == MUSE_VOICE_VIEW_READY && muse_voice_call_view() != view) {
            muse_state_set_caption("Hold yellow to talk");
        }
        /* level also animates the mouth; only LISTENING displays the meter.
         * Clear before changing modes so speaker level cannot cross into it. */
        if (muse_voice_call_view() != view) muse_state_set_level(0);
        call_view(view);
        muse_state_set_level(view == MUSE_VOICE_VIEW_SPEAKING ? (n ? muse_audio_level(out, n) : 0) :
                             view == MUSE_VOICE_VIEW_LISTENING ? mic_level : 0);
        if (ptt && status.overflow) muse_state_set_caption("Mic buffer full, release and retry");
        else if (ptt && status.reply_lost) muse_state_set_caption("Reply missed, hold to try again");
        if (c.hung_up && now > c.give_up) break;
    }
#if CONFIG_LV_USE_SNAPSHOT
    ESP_LOGI(TAG, "playback timing: longest_write_gap_ms=%u empty_in_tail=%u write_errors=%u",
             (unsigned)(longest_write_gap / 1000), empty_during_speech, write_errors);
#endif
    muse_mic_ring_set(false);
    finish_release(&c);
    /* ENDED closes the network, not the speaker queue. Drain its bounded
     * tail without reopening the mic; a new press only interrupts this tail. */
    if (ended && !c.hung_up && !(ptt && c.held)) {
        int64_t until = esp_timer_get_time() + 2000000;
        while (esp_timer_get_time() < until) {
            muse_input_event_t input;
            bool stop = false;
            while (xQueueReceive(s_queue, &input, 0) == pdTRUE) {
                muse_state_poke();
                muse_input_clear_call_pending(input.mark);   /* dropped: it only interrupts the tail */
                stop |= input.type == MUSE_PTT_DOWN || input.type == MUSE_PTT_END_CALL;
            }
            if (stop || atomic_load(&s_shutdown)) break;
            bool flushed;
            size_t n = muse_voice_call_read(out, MUSE_AUDIO_CHUNK, &flushed);
            if (!n) break;
            muse_state_set_level(0);
            call_view(MUSE_VOICE_VIEW_SPEAKING);
            muse_state_set_level(muse_audio_level(out, n));
            muse_audio_write(out, n);
        }
    }
    muse_audio_write(silence, MUSE_AUDIO_CHUNK);
    muse_wakeword_set_ptt_turn(false);
#if CONFIG_MUSE_VOICE_SOFTWARE_AEC
    muse_aec_stop(aec);
#endif
    muse_state_set_level(0);
    s_monitor_db = -100.0f;
    if (!atomic_load(&s_shutdown)) go_idle(ended && text[0] ? text : "Call ended");
    atomic_store(&s_call_view, MUSE_VOICE_VIEW_NONE);
    atomic_store(&s_ptt_call, false);
}
#endif

#if CONFIG_MUSE_GADGET_BLE_AUDIO
/*
 * The face after a musegadgets utterance, as on the Wi-Fi path: thinking
 * once push-to-talk ends, then what the client's assistant_state says
 * (responding shows speaking, done the happy animation then idle, error the
 * error face then idle), speaking while the client's audio plays whatever
 * the last state was, and idle after 120 s without an update (tool calls
 * can keep a turn quiet that long) or when the
 * client disconnects. A client that has never sent assistant_state (an older
 * app) gets the happy animation and idle when its playback ends, instead of
 * thinking again. The voice task applies it between presses.
 */
#define FACE_TIMEOUT_US (120LL * 1000000)
#define FACE_ERROR_US (3LL * 1000000)

typedef enum {
    FACE_NONE,        /* no BLE turn: the voice loop's own face */
    FACE_THINKING,
    FACE_RESPONDING,
    FACE_ERROR,
} face_t;

static volatile int s_face_pending = -1;   /* the client's latest mg_assistant_state_t, -1 none */
static face_t s_face;
static int64_t s_face_until;
static bool s_face_playing;

void muse_voice_assistant_state(uint8_t state)
{
    s_face_pending = state;
    muse_state_nudge();   /* wakes a resting voice task */
}

/* A client's turn or playback over: idle, without go_idle()'s poke. The
 * phone's assistant_state or playback alone never holds a lit panel
 * (check_sleep restarts the timer where a reply's hold ends). */
static void face_idle(void)
{
    muse_state_set_progress(0);
    set_mode(MUSE_MODE_IDLE);
    muse_state_set_caption("%s", "");
}

static void face_show(face_t face)
{
    s_face = face;
    int64_t now = esp_timer_get_time();
    switch (face) {
    case FACE_THINKING:
        muse_state_set_caption("%s", "");
        set_mode(MUSE_MODE_THINKING);
        s_face_until = now + FACE_TIMEOUT_US;
        break;
    case FACE_RESPONDING:
        muse_state_set_caption("%s", "");
        set_mode(MUSE_MODE_SPEAKING);
        s_face_until = now + FACE_TIMEOUT_US;
        break;
    case FACE_ERROR:
        set_mode(MUSE_MODE_ERROR);
        muse_state_set_caption("%s", "Something went wrong");
        s_face_until = now + FACE_ERROR_US;
        break;
    default:
        face_idle();
        break;
    }
}

/* Applies the client's state, playback and timeouts. */
static void face_update(void)
{
    bool playing = mg_voice_audible();   /* audio playing, not the stream open (a stream can stay open between replies) */
    if (playing != s_face_playing) {
        s_face_playing = playing;
        if (playing) {
            set_mode(MUSE_MODE_SPEAKING);   /* whatever the last state was */
        } else if (s_face_pending < 0 && !mg_voice_client_reports_state()) {
            muse_state_set_level(0);
            mg_voice_face("playback over, client sends no assistant_state");
            s_face = FACE_NONE;
            make_happy();
            face_idle();
        } else {
            muse_state_set_level(0);
            if (s_face_pending < 0) {
                face_show(s_face);          /* back to it */
            }                               /* else the state sent during playback, below */
        }
    }
    int pending = s_face_pending;
    if (pending >= 0 && !playing) {   /* while playing, it waits for the end */
        s_face_pending = -1;
        switch (pending) {
        case mg_assistant_state_thinking:
            face_show(FACE_THINKING);
            break;
        case mg_assistant_state_responding:
            face_show(FACE_RESPONDING);
            break;
        case mg_assistant_state_done:
            /* The Wi-Fi path's end of a turn. */
            s_face = FACE_NONE;
            make_happy();
            face_idle();
            break;
        case mg_assistant_state_error:
            face_show(FACE_ERROR);
            break;
        default:   /* idle: the turn is cancelled */
            face_show(FACE_NONE);
            break;
        }
    }
    if (s_face != FACE_NONE && !playing && esp_timer_get_time() >= s_face_until) {
        if (s_face != FACE_ERROR) {
            mg_voice_face("no assistant_state for 120 s");
        }
        face_show(FACE_NONE);
    }
}

/*
 * One utterance to a musegadgets client over BLE, or into the offline queue
 * for one (mg_voice.h picked the route). Push-to-talk starts with the
 * pre-roll and runs to the release; capture the client started with
 * start_mic runs until it sends stop_mic. There's no reply to play: the
 * client has the audio.
 */

#define MIC_OPEN_GRACE_READS 75   /* 1.5 s of closed reads: past muse_audio's 1 s retry */

/* In a post-roll: a talk press (of this input epoch) waiting? It goes back to
 * the queue's front for the main loop, which takes it the moment this turn
 * ends; anything else waiting is stale (a press's late edges). */
static bool postroll_cut_by_press(void)
{
    muse_input_event_t ev;
    while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
        if (ev.type == MUSE_PTT_DOWN && ev.epoch == muse_input_call_epoch()) {
            xQueueSendToFront(s_queue, &ev, 0);
            return true;
        }
        muse_input_clear_call_pending(ev.mark);
    }
    return false;
}

static void mg_turn(mg_route_t route, bool from_client, bool barge_in, bool wake)
{
    (void)wake;   /* a waking press passed the hold threshold too */
    bool ble = route == MG_ROUTE_BLE || route == MG_ROUTE_HELD;   /* a held live turn is a BLE turn */
    /* A press's turn keeps the panel lit until it's done; a client's
     * start_mic is the phone's turn, but its capture wakes a dark panel:
     * the ring shows whenever the mic captures for the app. */
    muse_state_set_own_turn(!from_client);
    if (from_client) {
        muse_input_wake("start_mic");
    }
    set_mode(MUSE_MODE_LISTENING);
    muse_state_set_progress(0);
    muse_state_set_caption(ble ? "" : "Recording for later");
    if (!mg_voice_begin(route, from_client)) {
        go_idle(ble ? "Open Muse app" : "Couldn't record");
        return;
    }
    size_t n = 0;
    /* Capturing for the client (push-to-talk or its start_mic): the ring,
     * always (a released app's own pre-live start_mic too). */
    muse_mic_ring_set(true);
    if (!from_client && !barge_in) {
        for (size_t i = 0; i < s_pre_fill; i++) {
            pre_get(i, s_chunk);
            mg_voice_audio(s_chunk, MUSE_AUDIO_CHUNK);
            n += MUSE_AUDIO_CHUNK;
        }
    }
    size_t held = 0;
    size_t stop_at = from_client ? SIZE_MAX : MAX_FRAMES;
    bool released = false;
    int misses = 0;   /* reads in a row with the mic not open yet */
    while (held + MUSE_AUDIO_CHUNK <= stop_at) {
        if (muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) != ESP_OK) {
            /* The codecs may still be opening (muse_audio retries a refused
             * open once a second, and a closed read waits out its 20 ms): a
             * client's capture gives them MIC_OPEN_GRACE_READS before it ends. */
            if (from_client && held == 0 && ++misses < MIC_OPEN_GRACE_READS) {
                continue;
            }
            if (from_client && held == 0) {
                ESP_LOGW("mg.voice", "client capture: the mic didn't open in %d ms", MIC_OPEN_GRACE_READS * 20);
            }
            break;
        }
        s_chunk_us = esp_timer_get_time();   /* muse_voice_capture_stalled */
        muse_state_set_level(muse_audio_level(s_chunk, MUSE_AUDIO_CHUNK));
        held += MUSE_AUDIO_CHUNK;
        if (!mg_voice_audio(s_chunk, MUSE_AUDIO_CHUNK)) {
            break;   /* the client went, stopped it, or the queue's full */
        }
        if (!from_client) {
            muse_state_set_progress((float)held / MAX_FRAMES);
            if (!released && got_event(MUSE_PTT_UP)) {
                released = true;
                gesture_up();   /* unless the input task sent it at the release */
                /* The post-roll: ~200 ms more, never past the cap. */
                stop_at = held + BLE_POSTROLL_FRAMES < MAX_FRAMES ? held + BLE_POSTROLL_FRAMES : MAX_FRAMES;
            } else if (released && postroll_cut_by_press()) {
                /* A new press in the post-roll: this turn ends now, with
                 * its last frame and stop_mic, and the press goes straight on to
                 * its own turn (the mic keeps running into its pre-roll). */
                ESP_LOGI(TAG, "a new press %u ms into the post-roll: this turn ends, the next starts",
                         (unsigned)((held + BLE_POSTROLL_FRAMES - stop_at) * 1000 / MUSE_AUDIO_RATE));
                break;
            }
        } else {
            got_event(MUSE_PTT_UP);   /* presses during client capture only go as gestures */
        }
    }
    muse_mic_ring_set(false);
    muse_state_set_level(0);
    /* A turn the device ended with the key still down (the 15 s cap, the
     * client stopping it, a mic error): button_up first, then [stop_mic]
     * (mgcommands: button up -> gesture, stop capture, stop_mic; Android
     * waits 2 s for a stop_mic after a button_up). The release later sends
     * nothing more: gesture_up() sends the one up. */
    gesture_up();
    /* Push-to-talk passed the hold threshold before it got here: every
     * turn that reached the client is kept. */
    bool keep = true;
    mg_voice_end(keep);
    /* The bench guard (ptt.minrise): a held turn that never rose went nowhere. */
    bool unsent = ble && !from_client && mg_voice_turn_dropped();
    if (ble && mg_voice_turn_queued()) {
        ble = false;   /* a held live turn whose client wasn't ready in time: saved as a clip */
    }
    ESP_LOGI(TAG, "%s: %.2fs",
             unsent ? "not sent (nothing heard)" : ble ? "sent to the BLE client" : "queued for the BLE client",
             (double)n / MUSE_AUDIO_RATE + (double)held / MUSE_AUDIO_RATE);
    muse_state_set_progress(0);
    if (unsent) {
        /* No turn reached the client: no thinking face, the idle one with
         * why (a reply paused at the touch-down plays on, as after a tap). */
        s_face = FACE_NONE;
        go_idle(CAPTION_NOTHING_HEARD);
    } else if (!ble) {
        /* Saved to the queue with nobody to answer: that's the end of it. */
        s_face = FACE_NONE;
        make_happy();
        go_idle("Saved for your phone");
    } else if (!from_client) {
        /* The utterance is with the assistant now; assistant_state takes it from here. */
        s_face_pending = -1;
        face_show(FACE_THINKING);
    } else {
        /* A capture the client drove: it sends thinking itself if a turn follows. */
        face_show(s_face);
    }
    s_face_playing = false;   /* face_update picks up playback again */
}
#endif

/*
 * The voice loop's own use of the speaker and mic (a turn, a chirp, a bench
 * test) ends a musegadgets client's playback and holds new playback off
 * until the loop is idle again.
 */
#if CONFIG_MUSE_GADGET_BLE_AUDIO
/* claim: the device's own sound (a client's start gets busy). claim_turn: a
 * push-to-talk turn (a client's reply that starts meanwhile is kept and plays
 * after the turn and its tail). */
static int s_claim;   /* 0 none, 1 own sound, 2 a turn */
static void set_claim(int c)
{
    if (c == s_claim) {
        return;
    }
    if (s_claim == 1) {
        mg_voice_claim(false);
    } else if (s_claim == 2) {
        mg_voice_claim_turn(false);
    }
    s_claim = c;
    if (c == 1) {
        mg_voice_claim(true);
    } else if (c == 2) {
        mg_voice_claim_turn(true);
    }
}
static void claim_audio(bool claim)
{
    set_claim(claim ? 1 : 0);
}
static void claim_turn(void)
{
    s_barge_owed = false;   /* the turn ends the press's pause (mg_proto_claim_turn) */
    set_claim(2);
}
#define client_playing() mg_voice_playing()
#else
#define claim_audio(claim) ((void)0)
#define claim_turn() (s_barge_owed = false)
#define client_playing() false
#endif

/* While a toggled note records, the buttons treat talk as held: no menu or sleep (#297). */
static void note_toggled(bool on)
{
#if NOTE_TOGGLE
    muse_input_note_toggled(on);
#else
    (void)on;
#endif
}

static void voice_task(void *arg)
{
    bool pending_down = false;
    AT("self-test");
    muse_audio_selftest();
    for (;;) {
        bool wake = false;
        AT("loop top");
        barge_resume();   /* a press decided without a turn (a tap, no route, a waking tap): the reply resumes */
        /* This press came from the wake word glue. One that interrupted a
         * reply (pending_down) keeps that: it is a voice note, not a call. */
        bool synthetic = pending_down && s_press_synthetic;
        if (!pending_down) {
            muse_input_event_t ev;
            claim_audio(false);
#if CONFIG_MUSE_GADGET_BLE_AUDIO
            face_update();
#endif
            bool asleep = muse_state_asleep();
#if CONFIG_MUSE_GADGET_BLE_AUDIO
            if (mg_voice_capture_requested()) {
                ESP_LOGI(TAG, "client capture: starting");
                set_resting(false);
                AT("client capture");
                mg_turn(MG_ROUTE_BLE, true, false, false);
                pre_reset();
                continue;
            }
#endif
            bool battery = muse_state_on_battery();
            /* A client's playback keeps the codecs on, asleep or not, and so
             * does the wake word opt-in. Not muse_wakeword_listening(): it
             * reads false while resting, and briefly after a reply until the
             * wake task applies playback off. */
            bool rest = asleep && battery && !s_chirp && !s_mp3test && !s_loopback && !client_playing() &&
                        !muse_wakeword_enabled();
#if HOLD_NOTES
            /* A press goes first: send_held() leaves it queued and returns
             * without backing off, so retrying before it's read would spin. */
            if (held_due() && !press_waiting()) {
                set_resting(false);   /* full power while it goes, even asleep */
                muse_wifi_power(MUSE_WIFI_FULL);
                claim_audio(true);
                AT("sending a saved note");
                pending_down = send_held(asleep);
                pre_reset();
                if (!pending_down && !asleep && muse_state_mode(NULL) != MUSE_MODE_IDLE) {
                    muse_state_make_happy();
                    go_idle("");
                }
                continue;
            }
#endif
            set_resting(rest);
            /* On battery Wi-Fi rests with the codecs, and between turns dozes
             * between beacons as xiaozhi's idle does; a press wakes it fully. */
            AT("wifi power");
            muse_wifi_power(rest ? MUSE_WIFI_REST : battery ? MUSE_WIFI_DOZE : MUSE_WIFI_FULL);
            if (rest) {
                AT("resting (waits for a wake or a request)");
                /* Wait for waking, USB power or a bench request without
                 * polling (the timeout is only a backstop, or a look for
                 * Wi-Fi to send saved notes), so the CPU can stay asleep. */
                muse_state_wait_awake(s_waiting ? HELD_POLL_MS : REST_BACKSTOP_MS);
                continue;
            }
            if (s_chirp || s_mp3test || s_loopback) {
                claim_audio(true);
            }
            if (s_chirp) {
                s_chirp = false;
                muse_wakeword_set_playback(true);
                muse_audio_chirp(1);
                muse_wakeword_set_playback(false);
                pre_reset();
            }
            if (s_mp3test) {
                s_mp3test = false;
                muse_wakeword_set_playback(true);
                int16_t *pcm = NULL;
                size_t n = muse_hatch_mp3_selftest(&pcm);
                for (size_t i = 0; i < n; i += MUSE_AUDIO_CHUNK) {
                    muse_audio_write(pcm + i, n - i < MUSE_AUDIO_CHUNK ? n - i : MUSE_AUDIO_CHUNK);
                }
                free(pcm);
                muse_wakeword_set_playback(false);
                pre_reset();
            }
            if (s_loopback) {
                s_loopback = false;
                muse_wakeword_set_playback(true);
                muse_audio_loopback_test(muse_settings_volume());
                muse_wakeword_set_playback(false);
                pre_reset();
            }
            if (s_fail_caption && muse_state_mode(NULL) == MUSE_MODE_IDLE) {
                /* A turn that failed as Muse went out of reach keeps its
                 * caption until Muse is back. One that failed with Muse
                 * still connected (no reply, say) keeps it. */
                muse_hatch_status_t hst;
                muse_hatch_status(&hst);
                if (hst.state != MUSE_HATCH_REACHABLE) {
                    s_fail_away = true;
                } else if (s_fail_away) {
                    s_fail_caption = false;
                    muse_state_set_caption("%s", "");
                }
            }
            if (client_playing()) {
                s_settle = SETTLE_CHUNKS;   /* the client's audio on the speaker isn't pre-roll */
            }
            /* A press goes first (a waking one too: it may only wake Muse). */
            muse_input_event_t next;
            if (xQueuePeek(s_queue, &next, 0) != pdTRUE && muse_hatch_incoming()) {
                /* Muse took long and has the answer now (#268): light the screen, say it. */
                bool delivered;
                muse_state_set_asleep(false);
                muse_wifi_power(MUSE_WIFI_FULL);
                claim_audio(true);
                AT("a late reply");
                pending_down = hatch_reply(&delivered, "Muse has an answer");   /* a late reply: no note to send */
                pre_reset();
                if (!pending_down && muse_state_mode(NULL) != MUSE_MODE_IDLE) {
                    muse_state_make_happy();
                    go_idle("");
                }
                continue;
            }
            /* The 20 ms read paces this loop. */
            idle_capture(false);
            if (xQueueReceive(s_queue, &ev, 0) != pdTRUE) {
                continue;
            }
            muse_state_poke();
            NOTE_REAL_EDGE(ev);
            /* Synthetic (wake word) edges carry no call epoch/mark. */
            if (ev.type != MUSE_PTT_DOWN ||
                (!ev_synthetic(&ev) && ev.epoch != muse_input_call_epoch())) {
                muse_input_clear_call_pending(ev.mark);
                continue;
            }
            s_press_input_epoch = ev.epoch;
            s_press_mark = ev.mark;
            note_press(&ev);
            note_press_synthetic(&ev);
            wake = ev.wake;
            synthetic = ev_synthetic(&ev);
        }
        s_barge_owed = true;   /* this press is being decided: its touch-down's pause is ours to end */
        bool ptt = muse_voice_ptt_enabled();
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        /* The voice transport, picked at the press: the BLE client, the
         * offline queue, or Muse over Wi-Fi. With none the mic never opens. */
        /* A wake word turn is a voice note to Muse over Wi-Fi, never a client's. */
        mg_route_t route = synthetic ? MG_ROUTE_WIFI : mg_voice_route(muse_hatch_ready());
        if (route == MG_ROUTE_NONE) {
            pending_down = false;
            muse_input_clear_call_pending(s_press_mark);
            /* Set up (a device token, the "Not set up" check) and a
             * client's link is up but the app isn't ready (or push-to-talk is
             * off): it's coming, say so. No link, or not set up (setup
             * happens in the app): "Open Muse app". */
            go_idle(muse_link_token_stored() && mg_voice_client_linked() ? CAPTION_CONNECTING : "Open Muse app");
            continue;
        }
#endif
        /* A toggled note (NOTE_TOGGLE, a note over Wi-Fi): the press itself
         * starts it, with no hold; a waking press only wakes (#297). */
        bool toggle = NOTE_TOGGLE && !ptt && !synthetic;   /* a wake word note ends at its own release */
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        toggle = toggle && route == MG_ROUTE_WIFI;
#endif
        if (wake && toggle) {
            muse_input_clear_call_pending(s_press_mark);
            continue;   /* the next press starts the note */
        }
        if (wake && !synthetic && !(ptt ? ptt_held_on_waking() : held_on_waking())) {
            /* A tap: it only woke Muse. A press posted since keeps its own mark. */
            muse_input_clear_call_pending(s_press_mark);
            continue;
        }
        /* The hold contract for a press on an awake screen: a tap only
         * says how. A waking press passed the same threshold just above. The
         * StopWatch's Wi-Fi realtime call keeps its own start. */
        /* A wake detection already qualified a hands-free turn, and its seed
         * holds the start: no hold to measure. */
        bool gated = wake || toggle || synthetic;
        if (synthetic) {
            s_settle = 0;
            s_pre_fill = 0;   /* the seed is its pre-roll */
        }
        bool realtime_call = ptt;   /* the StopWatch's Wi-Fi realtime call starts at the press */
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        realtime_call = ptt && route == MG_ROUTE_WIFI;
#endif
        if (!wake && !pending_down && !realtime_call && !toggle && !synthetic) {
            bool buzz = true;
            __attribute__((unused)) bool wifi_route = true;
#if CONFIG_MUSE_GADGET_BLE_AUDIO
            buzz = route != MG_ROUTE_BLE;   /* mg_voice_begin buzzes there, with the client's ptt_buzz_* */
            wifi_route = route == MG_ROUTE_WIFI;
#endif
            if (!hold_gate(buzz)) {
                muse_input_clear_call_pending(s_press_mark);
                if (muse_input_take_touch_tap()) {
                    /* A tap on the home's hold region (Screen or Both): the
                     * happy bounce, no hint. */
                    muse_state_make_happy();
                    go_idle("");
#if CONFIG_MUSE_REALTIME_VOICE && !CONFIG_MUSE_BOARD_M5STACK_STOPWATCH_FAMILY
                /* Other boards retain tap-to-call beside holding for a note,
                 * for a press still of this input epoch (#311), over Wi-Fi. */
                } else if (wifi_route && s_press_input_epoch == muse_input_call_epoch() &&
                           muse_voice_call_ready()) {
                    muse_wifi_power(MUSE_WIFI_FULL);
                    run_call(false, 0);
                    pre_reset();
#endif
                } else {
                    too_short("Hold to talk");   /* a tap of the talk key: a hint (#283) */
                }
                continue;
            }
            gated = true;
        }
        if (wake) {
            /* The waking hold's capture is over; the turn (if any) turns the
             * ring on again as it starts capturing. */
            muse_mic_ring_set(false);
        }
        if (atomic_load(&s_shutdown)) {
            pending_down = false;
            muse_mic_ring_set(false);
            muse_input_clear_call_pending(s_press_mark);
            if (synthetic) muse_wakeword_turn_ended();
            continue;
        }
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        if (route == MG_ROUTE_BLE) {
            /* The bench guard (ptt.minrise, off unless a test tool set it): the
             * turn is held on the gadget, button_down and claim included, until
             * its capture rises over its own floor (mg_voice_hold). */
            mg_voice_hold();
        } else if (route == MG_ROUTE_HELD) {
            /* The client's link is up but it isn't proven and ready yet
             * (the connect burst): a held live turn, button_down included, live
             * once it is, else an offline clip after 3 s (mg_voice_held_begin). */
            mg_voice_held_begin();
        }
#endif
        /* Held past the threshold: now a client hears the press, ahead of its
         * audio (a wake word turn has no press). */
        if (!synthetic) {
            gesture_down();
        }
        /* Push-to-talk holds the speaker: a client's playback stops
         * (stop_streaming drop), and a reply that starts during the turn
         * waits and plays after it (mgcommands.h, Audio playback). */
        claim_turn();
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        if (route != MG_ROUTE_WIFI) {
            /* Not a realtime call: the press's call mark (muse_input.c) goes,
             * so the speaker button doesn't take its next press for a hang-up. */
            muse_input_clear_call_pending(s_press_mark);
        }
        if (route == MG_ROUTE_BLE || route == MG_ROUTE_QUEUE || route == MG_ROUTE_HELD) {
            AT("talk turn (BLE, held or queue)");
            mg_turn(route, false, pending_down, wake);
            pending_down = false;
            pre_reset();
            continue;
        }
        /* A Wi-Fi turn: it runs its own face from here. */
        s_face = FACE_NONE;
        s_face_pending = -1;
#endif
        muse_wifi_power(MUSE_WIFI_FULL);
#if CONFIG_MUSE_REALTIME_VOICE
        if (ptt && !synthetic) {   /* a wake word turn is a voice note, not a call */
            pending_down = false;
            if (muse_voice_call_ready()) run_call(true, wake ? s_pre_fill : 0);
            else {
                muse_input_clear_call_pending(s_press_mark);
                if (!muse_wifi_connected()) muse_wifi_apply();
                go_idle(not_ready_reason());
            }
            pre_reset();
            continue;
        }
#endif
        muse_input_clear_call_pending(s_press_mark);
        if (!can_record()) {
            pending_down = false;
            if (synthetic) muse_wakeword_turn_ended();
            pre_reset();
            muse_mic_ring_set(false);   /* the hold's capture is over: nothing goes */
            continue;
        }
#if NOTE_TOGGLE
        if (toggle) {
            note_toggled(true);
            AT("toggled note");
            toggle_t t = toggle_gate(pending_down);
            if (t != TOGGLE_RECORD) {
                note_toggled(false);
                pending_down = false;
                drop_rec();
                pre_reset();
                if (atomic_load(&s_shutdown) || muse_state_mode(NULL) == MUSE_MODE_OFF) continue;
                if (t == TOGGLE_MIC_ERROR) go_idle("Microphone error, try again");
                else if (t == TOGGLE_TOO_SHORT) too_short("Record a longer note");
                else go_idle("");
                continue;
            }
        }
#endif
        size_t held;
        char why[96];
        AT("talk turn (Wi-Fi)");
        /* A toggled note's opening is in the ring already (a reply's tail skipped). */
        muse_wakeword_set_ptt_turn(!synthetic);   /* a real turn pauses the runner */
        bool ok = record(synthetic, pending_down && !toggle, &held, why, sizeof(why));
        muse_wakeword_set_ptt_turn(false);
        note_toggled(false);
        pending_down = false;
        if (held < MIN_HELD_FRAMES && !gated) {
            /* Only a press that interrupted a reply gets here undecided: it's
             * judged by what it held before its release. */
            muse_hatch_turn_cancel();
            drop_rec();
            pre_reset();
            if (atomic_load(&s_shutdown) || muse_state_mode(NULL) == MUSE_MODE_OFF) continue;
            if (s_capture_failed) {
                go_idle("Microphone error, try again");   /* never a tap (#311) */
                continue;
            }
#if CONFIG_MUSE_REALTIME_VOICE && !CONFIG_MUSE_BOARD_M5STACK_STOPWATCH_FAMILY
            /* Other boards retain tap-to-call beside holding for a note, for
             * a press still of this input epoch (#311). */
            if (s_press_input_epoch == muse_input_call_epoch() && muse_voice_call_ready()) {
                run_call(false, 0);
                pre_reset();
                continue;
            }
#endif
            too_short("Hold to talk");
            continue;
        }
        if (!ok) {
            pre_reset();
            go_idle(why);   /* Muse failed it while recording */
            continue;
        }
        AT("note's reply (Wi-Fi)");
        pending_down = finish_note();
        pre_reset();
        if (!pending_down && muse_state_mode(NULL) != MUSE_MODE_IDLE) {
            muse_state_make_happy();
            go_idle("");
        }
    }
}

#if CONFIG_MUSE_WAKEWORD_ENABLE
void muse_voice_seed_preroll(const int16_t *pcm, size_t frames)
{
    if (!s_seed || !pcm) {
        s_seed_frames = 0;   /* NULL clears a seed whose press never went */
        return;
    }
    size_t n = frames < SEED_FRAMES ? frames : SEED_FRAMES;
    n -= n % MUSE_AUDIO_CHUNK;   /* whole chunks only: take() works in chunks */
    memcpy(s_seed, pcm, n * sizeof(int16_t));
    s_seed_frames = n;   /* publish after the copy */
}
#endif

esp_err_t muse_voice_start(QueueHandle_t queue)
{
    s_queue = queue;
    s_pre = heap_caps_malloc(PRE_CAP_CHUNKS * sizeof(pre_chunk_t), MUSE_BIG_CAPS);
    if (!s_pre || muse_audio_init(muse_settings_volume(), muse_settings_mic_gain()) != ESP_OK) {
        muse_state_set_mode(MUSE_MODE_ERROR);
        muse_state_set_caption("Audio didn't start");
        return ESP_FAIL;
    }
    /* Stack in PSRAM if there is any (this task never writes flash) to spare internal RAM for Wi-Fi/BLE. */
    if (xTaskCreatePinnedToCoreWithCaps(voice_task, "muse_voice", 6144, NULL, 6, &s_voice_task, MUSE_AUDIO_CORE,
                                        MUSE_BIG_CAPS) != pdPASS) {
        /* Without the task nothing would ever rest the codecs, which
         * muse_audio_init() left open: on the Watcher that's ~33 mA. */
        muse_audio_power(false);
        muse_state_set_mode(MUSE_MODE_ERROR);
        muse_state_set_caption("Audio didn't start");
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_MUSE_WAKEWORD_ENABLE
    /* The wake word rides the voice pipeline (its tap is this task's reads,
     * its presses go to the same queue), and needs the seed buffer. */
    s_seed = heap_caps_malloc(SEED_FRAMES * sizeof(int16_t), MUSE_BIG_CAPS);
    if (!s_seed || muse_wakeword_init(queue) != ESP_OK) {
        ESP_LOGW(TAG, "wake word unavailable; push-to-talk unaffected");
        free(s_seed);   /* heap_caps_malloc memory: free() returns it */
        s_seed = NULL;
    }
#endif
    return ESP_OK;
}

void muse_voice_set_monitor(bool on)
{
    s_monitor_db = -100.0f;
    s_monitor = on;
}

float muse_voice_monitor_db(void)
{
    return s_monitor_db;
}

void muse_voice_request_chirp(void)
{
    s_chirp = true;
    muse_state_nudge();
}

void muse_voice_request_loopback(void)
{
    s_loopback = true;
    muse_state_nudge();
}

void muse_voice_request_mp3test(void)
{
    s_mp3test = true;
    muse_state_nudge();
}

bool muse_voice_resting(void)
{
    return s_resting;
}

bool muse_voice_notes_waiting(void)
{
    return s_waiting;
}

/* The redirects above are this file's own: nothing after it (a host test that
 * includes this file and defines the real calls) sees them. */
#undef muse_audio_read
#undef muse_audio_write
#undef vTaskDelay
#undef muse_state_wait_awake
#undef muse_hatch_turn_audio_wait
#undef muse_hatch_turn_read
