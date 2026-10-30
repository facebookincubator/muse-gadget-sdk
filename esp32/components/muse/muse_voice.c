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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "muse_adpcm.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_input.h"
#include "muse_mem.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_REALTIME_VOICE
#include "muse_voice_call.h"
#include "muse_voice_call_core.h"
#endif

static const char *TAG = "muse_voice";

#define MAX_SECS 15
#define TAIL_FRAMES (MUSE_AUDIO_RATE * 12 / 100)   /* capture lag + poll interval, stops before the release click */
#define MAX_FRAMES (MUSE_AUDIO_RATE * MAX_SECS)
#define MIN_HELD_FRAMES (MUSE_AUDIO_RATE * 3 / 10)   /* shorter presses are taps, not speech */
#if CONFIG_MUSE_BOARD_M5STACK_CARDPUTER_ADV || CONFIG_MUSE_BOARD_AI_PASSPORT
#define PRE_CHUNKS 6                                   /* 120 ms; a 320 ms upload burst stalls on the ADV and C3 */
#else
#define PRE_CHUNKS 16                                  /* 320 ms of audio kept from before the press */
#endif
#define SETTLE_CHUNKS 10   /* after Muse makes a sound, 200 ms of capture is its own tail */
#define REST_BACKSTOP_MS 60000

#if CONFIG_MUSE_HATCH
/* A note recorded while Hatch is out of reach is saved in PSRAM, and goes once it's back. */
#define HOLD_NOTES 1
#else
#define HOLD_NOTES 0   /* no PSRAM: a press needs Hatch */
#endif
#define HELD_MAX 4
#define HELD_TRIES 8                            /* then a saved note is dropped */
#define HELD_POLL_MS 5000                       /* resting with notes saved: check for Wi-Fi this often */
#define HELD_KEEP_US (30LL * 60 * 1000000)      /* muse_voice_notes_waiting() */
#define RETRY_MIN_US (15LL * 1000000)
#define RETRY_MAX_US (120LL * 1000000)
#define STALL_US (30LL * 1000000)               /* Hatch took none of a note this long: give up */
#define ACK_WAIT_US (30LL * 1000000)            /* asleep, waiting for the VM to have a note */

static QueueHandle_t s_queue;
static uint32_t s_press_input_epoch;
static uint32_t s_press_mark;   /* the press's pending-call mark (muse_input_clear_call_pending) */
static volatile bool s_monitor;
static volatile float s_monitor_db = -100.0f;
static volatile bool s_chirp;
static volatile bool s_loopback;
static volatile bool s_mp3test;
static atomic_int s_call_view;
static atomic_bool s_ptt_call;
static atomic_bool s_shutdown;

muse_voice_view_t muse_voice_call_view(void) { return atomic_load(&s_call_view); }
bool muse_voice_in_call(void) { return muse_voice_call_view() != MUSE_VOICE_VIEW_NONE; }
bool muse_voice_ptt_call(void) { return muse_voice_in_call() && atomic_load(&s_ptt_call); }
bool muse_voice_ptt_enabled(void)
{
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && CONFIG_MUSE_REALTIME_VOICE
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

const char *muse_voice_call_label(void)
{
    if (!muse_voice_ptt_call()) return NULL;
    switch (muse_voice_call_view()) {
    case MUSE_VOICE_VIEW_CONNECTING: return "CONNECTING";
    case MUSE_VOICE_VIEW_READY: return "HOLD TO TALK";
    case MUSE_VOICE_VIEW_LISTENING: return "LISTENING";
    case MUSE_VOICE_VIEW_THINKING: return "THINKING";
    case MUSE_VOICE_VIEW_SPEAKING: return "SPEAKING";
    case MUSE_VOICE_VIEW_ENDING: return "ENDING CALL";
    default: return NULL;
    }
}

const char *muse_voice_call_hint(void)
{
    if (!muse_voice_ptt_call()) return NULL;
    switch (muse_voice_call_view()) {
    case MUSE_VOICE_VIEW_LISTENING: return "RELEASE";
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

static pre_chunk_t *s_pre;   /* PRE_CHUNKS, a ring */
static size_t s_pre_next, s_pre_fill;
#if MUSE_LOW_MEM
static muse_adpcm_t s_pre_enc;
#endif
static int16_t s_chunk[MUSE_AUDIO_CHUNK];
static int s_settle;

/*
 * The note being recorded (HOLD_NOTES), kept whole in case it can't go now.
 * Streaming, Hatch has had s_sent frames of it.
 */
static int16_t *s_rec;   /* MAX_FRAMES */
static size_t s_rec_n, s_sent;
static bool s_live = true;
static bool s_tried;     /* it went to Hatch */

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
    const pre_chunk_t *c = &s_pre[(s_pre_next + PRE_CHUNKS - s_pre_fill + i) % PRE_CHUNKS];
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
    s_pre_next = (s_pre_next + 1) % PRE_CHUNKS;
    s_pre_fill = s_pre_fill < PRE_CHUNKS ? s_pre_fill + 1 : PRE_CHUNKS;
}

/* One 20 ms idle read: feeds the pre-roll ring and the settings mic meter. */
static void idle_capture(void)
{
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
        return;
    }
    pre_put();
}

/* Consume through the requested edge, leaving later edges for the next
 * phase (notably a quick release after interrupting a saved note's reply). */
static bool got_event(muse_ptt_t type)
{
    muse_input_event_t ev;
    while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
        muse_state_poke();
        if (ev.epoch == muse_input_call_epoch() && ev.type == type) {
            if (type == MUSE_PTT_DOWN) {
                s_press_input_epoch = ev.epoch;
                s_press_mark = ev.mark;
            }
            return true;
        }
        muse_input_clear_call_pending(ev.mark);   /* a press dropped here starts nothing */
    }
    return false;
}

/* Level stats of a recording, gathered chunk by chunk for the log. */
typedef struct {
    double acc;
    size_t frames;
    int peak, clipped;
    float floor_db;
    float tail_db[25];   /* the last 0.5 s, to see where speech ends and the release click lands */
    size_t chunks;
} rec_stats_t;

/* Adds one captured chunk (up to MUSE_AUDIO_CHUNK frames) to the stats. */
static void keep(rec_stats_t *st, const int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int v = abs(pcm[i]);
        st->peak = v > st->peak ? v : st->peak;
        st->clipped += v >= 32000;
        st->acc += (double)pcm[i] * pcm[i];
    }
    st->frames += n;
    if (n == MUSE_AUDIO_CHUNK) {
        float db = muse_audio_dbfs(pcm, n);
        st->floor_db = db < st->floor_db ? db : st->floor_db;
        st->tail_db[st->chunks++ % 25] = db;
    }
}

/* Gives Hatch what it has room for of the note being recorded. */
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
    muse_hatch_turn_begin();
    s_live = s_tried = true;
    s_sent = 0;
    feed_live();
}

/* One chunk of speech: into the stats, and on to Hatch, by way of the kept note if there is one. */
static void take(rec_stats_t *st, const int16_t *pcm)
{
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

/*
 * Records speech until release or MAX_SECS, starting with the pre-roll; *held
 * gets the part after the press. It streams to Hatch as it goes if it can.
 * If not, the note is kept in s_rec to send later, and streams from partway
 * if Hatch comes within reach. Returns false if Hatch failed the turn with no
 * kept note to fall back on (why says what failed). There is no start chirp:
 * anything played now would land on top of the first words.
 */
static bool record(bool barge_in, size_t *held, char *why, size_t cap)
{
    muse_state_set_mode(MUSE_MODE_LISTENING);
    muse_state_set_progress(0);
    s_rec_n = s_sent = 0;
    s_live = s_tried = false;
    if (!s_rec || (muse_hatch_ready() && !s_held_count)) {
        go_live();
    }
    muse_state_set_caption(s_live ? "LISTENING..." : "RECORDING...");
    bool heard = false, ok = true;
    bool gave_up = false;   /* Hatch failed this note: it's kept, and goes later */
    char text[96];
    rec_stats_t st = { 0 };

    size_t n = 0;
    if (barge_in) {
        /* Pressed during playback: the queued tail of the reply is still sounding. */
        for (int i = 0; i < SETTLE_CHUNKS; i++) {
            muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK);
        }
    } else {
        for (size_t i = 0; i < s_pre_fill; i++) {
            pre_get(i, s_chunk);
            take(&st, s_chunk);
            n += MUSE_AUDIO_CHUNK;
        }
    }
    size_t pre = n;
    bool released = false;
    size_t stop_at = MAX_FRAMES;
    while (n + MUSE_AUDIO_CHUNK <= stop_at) {
        if (muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) != ESP_OK) {
            break;
        }
        muse_state_set_level(muse_audio_level(s_chunk, MUSE_AUDIO_CHUNK));
        take(&st, s_chunk);
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
            muse_state_set_caption("%s %.1fs", s_live ? "LISTENING" : "RECORDING", (double)n / MUSE_AUDIO_RATE);
        }
        /*
         * Capture runs 60-80 ms behind real time and people let go on their
         * last syllable, so keep going briefly after release.
         */
        if (!released && got_event(MUSE_PTT_UP)) {
            released = true;
            stop_at = n + TAIL_FRAMES < MAX_FRAMES ? n + TAIL_FRAMES : MAX_FRAMES;
        }
    }
    muse_state_set_level(0);
    *held = n - pre;

    char tail[160];
    int tl = 0;
    size_t shown = st.chunks < 25 ? st.chunks : 25;
    for (size_t i = st.chunks - shown; i < st.chunks; i++) {
        tl += snprintf(tail + tl, sizeof(tail) - tl, " %.0f", st.tail_db[i % 25]);
    }
    ESP_LOGI(TAG, "end levels:%s", tail);
    float rms_db = st.frames ? 10.0f * log10f((float)(st.acc / st.frames / (32768.0 * 32768.0)) + 1e-10f) : -100.0f;
    ESP_LOGI(TAG, "recorded %.2fs%s: rms %.1f dBFS, peak %.1f dBFS, floor %.1f dBFS, %d clipped, gain %d dB",
             (double)n / MUSE_AUDIO_RATE, released ? "" : " (max)", rms_db < -100.0f ? -100.0f : rms_db,
             20.0 * log10((st.peak + 1) / 32768.0), st.floor_db, st.clipped, muse_settings_mic_gain());
    return ok;
}

static void go_idle(const char *caption);

/*
 * Plays Hatch's reply as it arrives, with its text as the caption. Returns
 * true if interrupted by a new press. *delivered: the VM has the note.
 */
static bool hatch_reply(bool *delivered)
{
    muse_state_set_mode(MUSE_MODE_THINKING);
    muse_state_set_caption("SENDING VOICE NOTE");   /* until there's a transcript or reply */
    static int16_t buf[MUSE_AUDIO_CHUNK];
    static const int16_t silence[MUSE_AUDIO_CHUNK];
    char text[96];
    static char page[MUSE_CAPTION_MAX];
    char incomplete[sizeof(text)] = "";
    bool done = false, speaking = false, replied = false;
    size_t played = 0;
    int64_t t0 = esp_timer_get_time();
    *delivered = false;
    for (;;) {
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
                    muse_state_set_caption("NOTE SENT - WAITING FOR MUSE");
                }
                break;
            case MUSE_HATCH_EV_REPLY:
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
                muse_state_set_level(0);
                go_idle(text);
                return false;
            default:
                break;
            }
        }
        if (got_event(MUSE_PTT_DOWN)) {
            ESP_LOGI(TAG, "reply interrupted");
            muse_hatch_turn_cancel();
            muse_state_set_level(0);
            return true;
        }
        size_t n = muse_hatch_turn_read(buf, MUSE_AUDIO_CHUNK, speaking || done ? 0 : 20);
        if (n) {
            if (!speaking) {
                speaking = true;
                muse_state_set_mode(MUSE_MODE_SPEAKING);
                ESP_LOGI(TAG, "reply audio after %.2fs", (esp_timer_get_time() - t0) / 1e6);
            }
            muse_state_set_level(muse_audio_level(buf, n));
            muse_audio_write(buf, n);
            played += n;
        } else if (done) {
            break;
        } else if (speaking) {
            /* Between messages: keep the speaker fed so it doesn't replay stale DMA. */
            muse_state_set_level(0);
            muse_audio_write(silence, MUSE_AUDIO_CHUNK);
        }
        /* The page being said, or before the speech the reply's opening page. */
        if ((speaking || replied) && muse_hatch_turn_caption(played, page, sizeof(page))) {
            muse_state_set_caption("%s", page);
        }
    }
    muse_state_set_level(0);
    ESP_LOGI(TAG, "muse reply: %.2fs of audio, %.2fs total", (double)played / MUSE_AUDIO_RATE,
             (esp_timer_get_time() - t0) / 1e6);
    if (incomplete[0]) {
        ESP_LOGW(TAG, "muse: %s", incomplete);
        go_idle(incomplete);
        return false;
    }
    if (!played) {
        /* No speech (TTS unavailable): leave the reply text up for a moment. */
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    return false;
}

static void go_idle(const char *caption)
{
    muse_state_set_progress(0);
    muse_state_set_mode(MUSE_MODE_IDLE);
    muse_state_set_caption("%s", caption);
}

/* Why a press can't go to Hatch; voice notes only go there. */
static const char *not_ready_reason(void)
{
    muse_hatch_status_t st;
    muse_hatch_status(&st);
    switch (st.state) {
    case MUSE_HATCH_NOT_SET: return "SET UP MUSE FIRST";
    case MUSE_HATCH_OFFLINE: return "NO WI-FI";
    default: return "CAN'T REACH MUSE";
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
    muse_audio_power(!rest);
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

/*
 * A press that woke the screen: listen from now, into the pre-roll, and
 * record only if it's still held after a tap's length. False if it was let
 * go sooner: it only woke Muse.
 */
static bool held_on_waking(void)
{
    s_settle = 0;   /* nothing played: no tail of Muse's own to skip */
    for (size_t n = 0; n < MIN_HELD_FRAMES; n += MUSE_AUDIO_CHUNK) {
        if (got_event(MUSE_PTT_UP)) {
            return false;
        }
        idle_capture();
    }
    return !got_event(MUSE_PTT_UP);
}

/* A waking PTT tap only wakes. Start a new, held-only ring at DOWN: none of
 * the idle pre-roll belongs to this press. UP leaves a later DOWN queued. */
static bool ptt_held_on_waking(void)
{
    s_pre_fill = 0;
    s_settle = 0;
    int64_t until = esp_timer_get_time() + 300000;
    for (;;) {
        muse_input_event_t ev;
        if (s_press_input_epoch != muse_input_call_epoch()) {
            s_pre_fill = 0;
            return false;
        }
        if (xQueuePeek(s_queue, &ev, 0) == pdTRUE) {
            s_pre_fill = 0;
            if (ev.type != MUSE_PTT_DOWN) xQueueReceive(s_queue, &ev, 0);
            return false;
        }
        if (esp_timer_get_time() >= until) return true;
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
    s_next_send_us = esp_timer_get_time() + s_send_backoff_us;
    s_send_backoff_us = s_send_backoff_us * 2 > RETRY_MAX_US ? RETRY_MAX_US : s_send_backoff_us * 2;
}

/* Saves the note just recorded to send later. `tried`: Hatch failed it, so it waits out the backoff. */
static void hold_rec(bool tried)
{
    if (s_held_count >= HELD_MAX) {   /* can_record() leaves room: not expected */
        drop_rec();
        go_idle("COULDN'T SAVE THE NOTE");
        return;
    }
    if (!s_rec_n) {   /* nothing captured: realloc to 0 would free s_rec and return NULL */
        drop_rec();
        go_idle("HOLD LONGER TO TALK");
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
    go_idle(tried ? "SAVED, WILL TRY AGAIN" : "SAVED, SENDS WHEN ONLINE");
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

typedef enum {
    FED,
    FEED_FAILED,    /* the turn failed, or Hatch stopped taking audio */
    FEED_PRESSED,   /* a press is queued: it goes first */
} feed_t;

/*
 * Gives Hatch a note from *sent on, waiting for room as it goes, then ends
 * the turn. Stops for a press only if `yield`.
 */
static feed_t feed_rest(const int16_t *pcm, size_t frames, size_t *sent, bool yield)
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

/* Asleep: waits for the VM to have the note just sent. */
static bool wait_delivered(void)
{
    char text[96];
    int64_t give_up = esp_timer_get_time() + ACK_WAIT_US;
    while (esp_timer_get_time() < give_up) {
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            if (ev == MUSE_HATCH_EV_SENT || ev == MUSE_HATCH_EV_REPLY || ev == MUSE_HATCH_EV_DONE) {
                return true;
            }
            if (ev == MUSE_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "muse: %s", text);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "no word that the note arrived");
    return false;
}

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
        muse_state_set_caption("SENDING SAVED NOTE");
    }
    muse_hatch_turn_begin();
    size_t sent = 0;
    feed_t fed = feed_rest(h->pcm, h->frames, &sent, true);
    bool delivered = false, interrupted = false;
    if (fed == FED && quiet) {
        delivered = wait_delivered();
    } else if (fed == FED) {
        interrupted = hatch_reply(&delivered);
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
            muse_state_set_caption("SAVED NOTE SENT");
        }
        return interrupted;
    } else if (++h->tries >= HELD_TRIES) {
        ESP_LOGW(TAG, "giving up on a saved note after %d tries", h->tries);
        drop_oldest();
        caption = "COULDN'T SEND A SAVED NOTE";
    } else {
        back_off();
        caption = "SAVED NOTE: WILL TRY AGAIN";
    }
    if (quiet) {
        muse_state_set_caption("%s", caption);
    } else {
        go_idle(caption);
    }
    return false;
}

/*
 * Whether to send a saved note now: Hatch within reach and the backoff over.
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
 * After the release: the rest of the note goes to Hatch and the reply plays,
 * or if it can't go now it's saved to send later. Returns true if a press
 * interrupted the reply.
 */
static bool finish_note(void)
{
    bool delivered;
#if HOLD_NOTES
    if (s_rec) {
        bool fed = false, interrupted = false;
        delivered = false;
        if (s_live && s_sent == s_rec_n) {
            muse_hatch_turn_end();   /* before the chirp, which takes ~90 ms */
            fed = true;
        }
        muse_audio_chirp(0);
        if (s_live && !fed) {
            /* Hatch is behind (still connecting, say): the rest from the kept note. */
            muse_state_set_mode(MUSE_MODE_THINKING);
            muse_state_set_caption("SENDING VOICE NOTE");
            fed = feed_rest(s_rec, s_rec_n, &s_sent, false) == FED;
            if (!fed) {
                muse_hatch_turn_cancel();
            }
        }
        if (fed) {
            interrupted = hatch_reply(&delivered);
        }
        if (delivered || interrupted) {
            drop_rec();
        } else {
            hold_rec(s_tried);
        }
        return interrupted;
    }
#endif
    muse_hatch_turn_end();   /* before the chirp, which takes ~90 ms */
    muse_audio_chirp(0);
    return hatch_reply(&delivered);
}

/*
 * Whether a press can record: Hatch within reach, or (HOLD_NOTES) set up
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
        go_idle("SET UP MUSE FIRST");
        return false;
    }
    if ((!ready || s_held_count) && s_held_count >= HELD_MAX) {
        go_idle("NOTES STILL WAITING TO SEND");
        return false;
    }
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
        muse_state_set_caption("HANGING UP...");
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
    atomic_store(&s_ptt_call, ptt);
    call_view(MUSE_VOICE_VIEW_CONNECTING);
    muse_input_clear_call_pending(s_press_mark);
    if (atomic_load(&s_shutdown) || c.input_epoch != muse_input_call_epoch()) {
        atomic_store(&s_call_view, MUSE_VOICE_VIEW_NONE);
        atomic_store(&s_ptt_call, false);
        if (!atomic_load(&s_shutdown)) go_idle("INPUT LOST - PRESS AGAIN");
        return;
    }
    muse_state_set_level(0);
    muse_state_set_progress(0);
    muse_state_set_caption("CONNECTING TO MUSE...");
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
                muse_state_set_caption(ptt ? "HOLD YELLOW TO TALK" : "IN A CALL - PRESS TO HANG UP");
                break;
            case MUSE_VOICE_CALL_EV_HEARD:
                muse_state_set_caption("\"%s\"", text);
                break;
            case MUSE_VOICE_CALL_EV_REPLY:
                muse_state_set_caption("%s", text);
                break;
            case MUSE_VOICE_CALL_EV_HOLD:
                c.server_hold = true;
                finish_release(&c);
                muse_state_set_caption("ONE MOMENT...");
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
        else mic = mic && c.live && muse_vc_gate_mic_open(&c.gate, now);
        if (mic) mic = muse_voice_call_audio(s_chunk, MUSE_AUDIO_CHUNK);
        if (ptt && mic) {
            c.awaiting = true;
            if (c.release_until) {
                c.release_frames = c.release_frames > MUSE_AUDIO_CHUNK ? c.release_frames - MUSE_AUDIO_CHUNK : 0;
                if (!c.release_frames) finish_release(&c);
            }
        }
        bool show_mic = mic && c.live && (!ptt || c.held);
        float mic_level = show_mic ? muse_audio_level(s_chunk, MUSE_AUDIO_CHUNK) : 0;
        if (s_monitor) s_monitor_db = show_mic ? muse_audio_dbfs(s_chunk, MUSE_AUDIO_CHUNK) : -100.0f;
        bool flushed = false;
        size_t n = c.hung_up || (ptt && (c.held || c.release_until)) ? 0 :
            muse_voice_call_read(out, MUSE_AUDIO_CHUNK, &flushed);
        if (flushed) muse_vc_gate_flush(&c.gate, now);
        if (n) {
            muse_vc_gate_playout(&c.gate, out, n, now, CALL_SPEAKER_LAG_US);
            muse_audio_write(out, n);
        } else {
            muse_audio_write(silence, MUSE_AUDIO_CHUNK);
        }
        bool speaking = !muse_vc_gate_mic_open(&c.gate, now) && !(ptt && (c.held || c.release_until));
        if (ptt && !c.held && !status.sending &&
            ((!status.heard && !status.reply_open) || (status.reply_done && !speaking))) c.awaiting = false;
        if (ptt && (status.heard || status.reply_open) && !status.reply_done) c.awaiting = true;
        muse_voice_view_t view = c.hung_up ? MUSE_VOICE_VIEW_ENDING :
            !c.live ? MUSE_VOICE_VIEW_CONNECTING :
            c.server_hold ? MUSE_VOICE_VIEW_THINKING :
            ptt && c.held ? (mic ? MUSE_VOICE_VIEW_LISTENING : MUSE_VOICE_VIEW_THINKING) :
            speaking ? MUSE_VOICE_VIEW_SPEAKING :
            !ptt ? MUSE_VOICE_VIEW_LISTENING :
            c.release_until || c.awaiting || status.sending ? MUSE_VOICE_VIEW_THINKING : MUSE_VOICE_VIEW_READY;
        if (ptt && view == MUSE_VOICE_VIEW_READY && muse_voice_call_view() != view) {
            muse_state_set_caption("HOLD YELLOW TO TALK");
        }
        /* level also animates the mouth; only LISTENING displays the meter.
         * Clear before changing modes so speaker level cannot cross into it. */
        if (muse_voice_call_view() != view) muse_state_set_level(0);
        call_view(view);
        muse_state_set_level(view == MUSE_VOICE_VIEW_SPEAKING ? (n ? muse_audio_level(out, n) : 0) :
                             view == MUSE_VOICE_VIEW_LISTENING ? mic_level : 0);
        if (ptt && status.overflow) muse_state_set_caption("MIC BUFFER FULL - RELEASE AND RETRY");
        else if (ptt && status.reply_lost) muse_state_set_caption("REPLY MISSED - HOLD TO TRY AGAIN");
        if (c.hung_up && now > c.give_up) break;
    }
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
    muse_state_set_level(0);
    s_monitor_db = -100.0f;
    if (!atomic_load(&s_shutdown)) go_idle(ended && text[0] ? text : "CALL ENDED");
    atomic_store(&s_call_view, MUSE_VOICE_VIEW_NONE);
    atomic_store(&s_ptt_call, false);
}
#endif

static void voice_task(void *arg)
{
    bool pending_down = false;
    muse_audio_selftest();
    for (;;) {
        bool wake = false;
        if (!pending_down) {
            muse_input_event_t ev;
            bool asleep = muse_state_asleep();
            bool battery = muse_state_on_battery();
            bool rest = asleep && battery && !s_chirp && !s_mp3test && !s_loopback;
#if HOLD_NOTES
            /* A press goes first: send_held() leaves it queued and returns
             * without backing off, so retrying before it's read would spin. */
            if (held_due() && !press_waiting()) {
                set_resting(false);   /* full power while it goes, even asleep */
                muse_wifi_power(MUSE_WIFI_FULL);
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
            muse_wifi_power(rest ? MUSE_WIFI_REST : battery ? MUSE_WIFI_DOZE : MUSE_WIFI_FULL);
            if (rest) {
                /* Wait for waking, USB power or a bench request without
                 * polling (the timeout is only a backstop, or a look for
                 * Wi-Fi to send saved notes), so the CPU can stay asleep. */
                muse_state_wait_awake(s_waiting ? HELD_POLL_MS : REST_BACKSTOP_MS);
                continue;
            }
            if (s_chirp) {
                s_chirp = false;
                muse_audio_chirp(1);
                pre_reset();
            }
            if (s_mp3test) {
                s_mp3test = false;
                int16_t *pcm = NULL;
                size_t n = muse_hatch_mp3_selftest(&pcm);
                for (size_t i = 0; i < n; i += MUSE_AUDIO_CHUNK) {
                    muse_audio_write(pcm + i, n - i < MUSE_AUDIO_CHUNK ? n - i : MUSE_AUDIO_CHUNK);
                }
                free(pcm);
                pre_reset();
            }
            if (s_loopback) {
                s_loopback = false;
                muse_audio_loopback_test(muse_settings_volume());
                pre_reset();
            }
            /* The 20 ms read paces this loop. */
            idle_capture();
            if (xQueueReceive(s_queue, &ev, 0) != pdTRUE) {
                continue;
            }
            muse_state_poke();
            if (ev.type != MUSE_PTT_DOWN || ev.epoch != muse_input_call_epoch()) {
                muse_input_clear_call_pending(ev.mark);
                continue;
            }
            s_press_input_epoch = ev.epoch;
            s_press_mark = ev.mark;
            wake = ev.wake;
        }
        bool ptt = muse_voice_ptt_enabled();
        if (wake && !(ptt ? ptt_held_on_waking() : held_on_waking())) {
            /* A tap: it only woke Muse. A press posted since keeps its own mark. */
            muse_input_clear_call_pending(s_press_mark);
            continue;
        }
        if (atomic_load(&s_shutdown)) {
            pending_down = false;
            muse_input_clear_call_pending(s_press_mark);
            continue;
        }
        muse_wifi_power(MUSE_WIFI_FULL);
#if CONFIG_MUSE_REALTIME_VOICE
        if (ptt) {
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
            continue;
        }
        size_t held;
        char why[96];
        bool ok = record(pending_down, &held, why, sizeof(why));
        pending_down = false;
        if (held < MIN_HELD_FRAMES && !wake) {
            muse_hatch_turn_cancel();
            drop_rec();
            pre_reset();
#if CONFIG_MUSE_REALTIME_VOICE && !CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
            /* Other boards retain tap-to-call beside holding for a note. */
            if (muse_voice_call_ready()) {
                run_call(false, 0);
                pre_reset();
                continue;
            }
#endif
            go_idle("HOLD LONGER TO TALK");
            continue;
        }
        if (!ok) {
            pre_reset();
            go_idle(why);   /* Hatch failed it while recording */
            continue;
        }
        pending_down = finish_note();
        pre_reset();
        if (!pending_down && muse_state_mode(NULL) != MUSE_MODE_IDLE) {
            muse_state_make_happy();
            go_idle("");
        }
    }
}

esp_err_t muse_voice_start(QueueHandle_t queue)
{
    s_queue = queue;
    s_pre = heap_caps_malloc(PRE_CHUNKS * sizeof(pre_chunk_t), MUSE_BIG_CAPS);
    if (!s_pre || muse_audio_init(muse_settings_volume(), muse_settings_mic_gain()) != ESP_OK) {
        muse_state_set_mode(MUSE_MODE_ERROR);
        muse_state_set_caption("AUDIO INIT FAILED");
        return ESP_FAIL;
    }
    /* Stack in PSRAM if there is any (this task never writes flash) to spare internal RAM for Wi-Fi/BLE. */
    if (xTaskCreatePinnedToCoreWithCaps(voice_task, "muse_voice", 6144, NULL, 6, NULL, MUSE_AUDIO_CORE,
                                        MUSE_BIG_CAPS) != pdPASS) {
        /* Without the task nothing would ever rest the codecs, which
         * muse_audio_init() left open: on the Watcher that's ~33 mA. */
        muse_audio_power(false);
        muse_state_set_mode(MUSE_MODE_ERROR);
        muse_state_set_caption("AUDIO INIT FAILED");
        return ESP_ERR_NO_MEM;
    }
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
