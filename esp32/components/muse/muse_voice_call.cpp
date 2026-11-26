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
 * A realtime voice call over the Muse session (muse_voice_call.h). The
 * protocol and the call's lifecycle are in muse_voice_call_core.c; this moves
 * bytes between it, the session's streams and the voice task.
 *
 * Ownership: the call's state, streams and the line and audio buffers belong
 * to the session task (muse_chat_session.cpp calls in). The voice task talks
 * to it through a command queue, a stream buffer of mic audio, an event
 * queue and a stream buffer of reply audio, each with one writer and one
 * reader. A generation number tags each call so that a late event or hang-up
 * of an earlier call can't touch the next. A barge-in flush is fenced the
 * same way: the session stops writing reply audio and bumps s_flush_req; the
 * voice task, the buffer's reader, empties it and acks.
 *
 * A call the session couldn't confirm ending (its connection dropped first,
 * or Muse didn't confirm the end-call) may outlive the connection on Muse's
 * side, where another connection can take it over during the disconnect
 * grace. So its end-call is kept, unchanged, as the one pending cleanup, and
 * sent again on the next connection, before a new call starts and within a
 * few bounded tries. It's in RAM only: a restart forgets it.
 *
 * Push-to-talk calls (start_ptt, set_talking): the voice task pushes mic audio
 * only while held, and muse_voice_call_audio refuses it otherwise, so all
 * that reaches s_in is held audio. A byte count fences a new call's audio
 * from anything an earlier call left in s_in. While the call connects, held
 * audio waits in s_in (PTT_IN_BYTES); once live it goes up paced, then, after
 * a release, PTT_TAIL_MS of quiet marked as the turn's tail, then quiet for as
 * long as the call is up. A press is fenced like a flush, in three steps: the
 * voice task empties s_out and bumps s_press_req; the session stops writing
 * s_out and stores s_press_ack; the voice task's next read empties s_out once
 * more and stores s_press_clean. Only then does reply audio go to s_out again;
 * until then (and while held, and while the gate holds a reply waiting for
 * its turn) it waits in s_defer or s_wait, bounded. Status for the screen is one atomic
 * word tagged with the call's generation (s_status).
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "sdkconfig.h"
#if CONFIG_MUSE_APP_VIEW
#include "muse_app_view.h"
#endif

#include "muse_chat_priv.h"
#include "muse_voice_call.h"
#include "muse_voice_call_core.h"
#include "muse_voice_call_priv.h"

static const char *TAG = "muse_call";

#define CALL_IN_BYTES (MUSE_VC_MIC_RATE * 2 * 6)      /* 6 s of held audio while a PTT call connects */
#define HANDS_FREE_IN_BYTES (MUSE_VC_MIC_RATE * 2 * 2) /* 2 s of mic backlog in a hands-free call */
#define DEFER_SAMPLES (MUSE_VC_MIC_RATE * 5)          /* 5 s of reply waiting for its turn or a release */
#define PTT_TAIL_MS 1000                              /* quiet after held audio that's still the turn */

#define CALL_OUT_BYTES (MUSE_VC_MIC_RATE * 2 * 4)     /* 4 s of reply */
#define DEC_BYTES (MUSE_VC_LINE_MAX / 4 * 3)          /* one line's audio at 24 kHz */
#define OUT_SAMPLES (DEC_BYTES / 3 + 2)               /* ... at 16 kHz */
/* Room for everything one inbound frame can complete: its own body plus a line
 * carried over from the frame before. Base64 of 24 kHz PCM16 comes out at half
 * its length as 16 kHz PCM16; 1 KB covers the resampler's rounding. */
#define OUT_ROOM ((MUSE_SESSION_FRAME_MAX + MUSE_VC_LINE_MAX) / 2 + 1024)
#define END_BODY_MAX 160
#define CLEANUP_TRIES 3                        /* end-call sends for a lost call, in all */
#define CLEANUP_ANSWER_US (10 * 1000000LL)     /* each one's wait for an answer */
#define CLEANUP_RETRY_US (5 * 1000000LL)       /* between tries */
#define CLEANUP_IDLE_US (1000000LL)            /* not connected: look again after this */
#define CLEANUP_KEEP_US (10 * 60 * 1000000LL)  /* then it's dropped, tried or not */
#define MIC_PIECE 320
#define QUIET_STEP_US (1000000LL * MIC_PIECE / MUSE_VC_MIC_RATE)   /* one piece of quiet: 20 ms */
#define LINES_PER_POLL 8
#define EV_TEXT 72
#if CONFIG_MUSE_REALTIME_VOICE_GREETING
#define GREETING true
#else
#define GREETING false
#endif

/* A start, written by the voice task before it publishes the generation
 * (s_gen) that names it. Only the newest start matters: the session takes
 * that one and ignores any it skipped. */
#define START_RING 16

struct start_rec_t {
    std::atomic<uint32_t> in_from{0};   /* s_in bytes written before it: the call's audio starts here */
    std::atomic<uint32_t> press{0};     /* push-to-talk: the press that starts it */
    std::atomic<bool> ptt{false};
};

/* s_status bits; the top 16 bits are the call's generation. */
enum : uint32_t {
    ST_CONNECTING = 1u << 0,
    ST_LIVE = 1u << 1,
    ST_HOLD = 1u << 2,
    ST_SENDING = 1u << 3,
    ST_REPLY_OPEN = 1u << 4,
    ST_REPLY_AUDIO = 1u << 5,
    ST_REPLY_DONE = 1u << 6,
    ST_REPLY_LOST = 1u << 7,
    ST_ENDED = 1u << 8,
    ST_HEARD = 1u << 9,
    ST_WORKING = 1u << 10,
};

struct call_ev_t {
    muse_voice_call_ev_t type;
    uint32_t gen;
    char text[EV_TEXT];
};

/* Shared with the voice task. */
static bool s_inited;
static QueueHandle_t s_events;
static StreamBufferHandle_t s_in, s_out;
static std::atomic<uint32_t> s_gen{0};             /* the voice task's current call (its start's number) */
static start_rec_t s_starts[START_RING];           /* by generation */
static std::atomic<uint32_t> s_end_gen{0};         /* the call the voice task last hung up */
/* The voice task's push-to-talk button, as one word so the session never
 * sees half of an edge: generation (16 bits), press number (15), held (1). */
static std::atomic<uint32_t> s_talk_word{0};
static std::atomic<uint32_t> s_flush_req{0}, s_flush_ack{0};
static std::atomic<uint32_t> s_press_req{0}, s_press_ack{0}, s_press_clean{0};
static std::atomic<uint32_t> s_status{0};
static std::atomic<uint32_t> s_in_written{0};      /* bytes the voice task put in s_in */

/* Voice task only. */
static bool v_call, v_ptt, v_talking, v_overflow;   /* v_call: a call, until its ENDED is read */
static bool v_buffering;
static int64_t v_first_sample;
static uint32_t v_press;             /* the last press number */
static uint32_t v_press_told;        /* the press read() last reported as a flush */

/* Session task only. */
/* The large buffers below live in PSRAM: only tasks touch them, and internal
 * RAM is short on boards with the full UI. */
EXT_RAM_BSS_ATTR static muse_vc_call_t s_call;
static muse_vc_turns_t *s_turns;     /* the call's turn table, in PSRAM */
static uint32_t s_call_gen;          /* the generation s_call belongs to */
static uint32_t s_pending_gen;       /* a start waiting for the session (or the last call) */
static int64_t s_media_id, s_end_id;
static uint32_t s_acts;              /* actions waiting for the next poll */
static int64_t s_stop_drain_until;   /* accepted playback, independent of the closed media stream */
static bool s_stop_ended;            /* network ended; notify the voice task after the bounded drain */
static muse_vc_lines_t s_lines;
static char *s_line_buf;
static uint8_t *s_dec;
static int16_t *s_pcm;
static muse_vc_resampler_t s_down;
EXT_RAM_BSS_ATTR static muse_vc_uplink_t s_up;
EXT_RAM_BSS_ATTR static char s_append[MUSE_VC_APPEND_MAX];
EXT_RAM_BSS_ATTR static int16_t s_mic[MIC_PIECE];
static size_t s_mic_len, s_mic_off;
static char s_reply[128];            /* the reply's last words, for the caption */
static char s_error[EV_TEXT];        /* the server's error message, for the end caption */
EXT_RAM_BSS_ATTR static char s_end_body[END_BODY_MAX]; /* the end-call's answer */
static size_t s_end_len;
static bool s_drop;                  /* Muse didn't confirm the end-call: drop the connection */
EXT_RAM_BSS_ATTR static char s_end_sent[MUSE_VC_END_CALL_MAX];  /* the end-call body sent for this call */
static bool s_end_was_sent;
static bool s_end_final;             /* Muse confirmed this call's end-call, or refused it (4xx) */

/* The one pending cleanup: an earlier call's end-call, exactly as first built. */
struct cleanup_t {
    bool active;
    char call_id[MUSE_VC_CALL_ID_MAX];
    char body[MUSE_VC_END_CALL_MAX];
    uint8_t tries;
    int64_t since_us, next_us;
};
EXT_RAM_BSS_ATTR static cleanup_t s_cleanup;
static int64_t s_cleanup_id, s_cleanup_at;   /* its end-call stream in flight, and when it went */
EXT_RAM_BSS_ATTR static char s_cleanup_ans[END_BODY_MAX];
static size_t s_cleanup_len;
static uint32_t s_gate_started, s_gate_done;  /* the start that waited for a cleanup try */
static uint32_t s_in_read;           /* bytes taken from s_in */
static uint32_t s_in_from;           /* the call's audio starts at this byte */
static uint32_t s_taken_gen;         /* the newest start the session took */
static bool s_ptt;                   /* the newest call (started or waiting) is push-to-talk */
static bool s_talk;                  /* held, as the session last heard */
static uint32_t s_press;             /* the press the session last processed */
static uint32_t s_tail_left;         /* 16 kHz samples of the turn's quiet tail still to send */
static int64_t s_quiet_next_us;      /* the next quiet append is due (its own clock: no catching up) */
static muse_vc_reply_gate_t *s_gate; /* in PSRAM */
/* Rings of reply audio, DEFER_SAMPLES each: s_defer holds audio that may
 * play but can't yet (held, the press fence, s_out full); s_wait holds the
 * audio of the id waiting for its turn, apart, so it never delays or
 * reorders audio already playing. */
struct ring_t {
    int16_t *buf;
    size_t head, len;
};
static ring_t s_defer, s_wait;
/* A press during a reply: response.cancel for its response.created id goes
 * up before the press's first mic audio. Best effort: it stops the agent
 * work bound to that response, if any; the server's barge-in on the new
 * speech is what stops the provider's own speech. */
static char s_cancel[MUSE_VC_TURN_ID_MAX];
static uint32_t s_st;                /* status bits (session's copy) */
static int64_t s_wall_offset_ms;
static bool s_wall_known;
enum class audio_format { unknown, continuous, legacy };
static audio_format s_format;
static unsigned s_duplicate_audio, s_retired_audio;
static uint32_t s_pcm_written;
static std::atomic<uint32_t> s_spoken_frames{0};
static uint32_t s_bad_lines, s_bad_audio, s_dropped_audio, s_frames_out;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void *big_alloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

/* ---- To the voice task ---- */

static void publish(void);

static bool emit(muse_voice_call_ev_t type, const char *text)
{
    if (type == MUSE_VOICE_CALL_EV_ENDED) {
#if CONFIG_LV_USE_SNAPSHOT
        if (!s_ptt) ESP_LOGI(TAG, "playback filtered: alternate=%u retired=%u", s_duplicate_audio, s_retired_audio);
#endif
        s_st = ST_ENDED;
        s_status.store((s_call_gen << 16) | s_st);
    }
    call_ev_t ev = {};
    ev.type = type;
    ev.gen = s_call_gen;
    if (text) {
        strlcpy(ev.text, text, sizeof(ev.text));
    }
    /* Live reply pages retry on a full queue. Call boundaries can wait for room. */
    return xQueueSend(s_events, &ev, type == MUSE_VOICE_CALL_EV_ENDED || type == MUSE_VOICE_CALL_EV_READY ? pdMS_TO_TICKS(200) : 0) == pdTRUE;
}

/* The audio decoder and transcript decoder run serially on the session task.
 * A final transcript can occupy almost the entire NDJSON line. */
static const char *decode_caption(muse_vc_span_t raw)
{
    char *text = reinterpret_cast<char *>(s_dec);
    return muse_vc_json_unescape(raw, text, MUSE_VC_LINE_MAX) ? text : "";
}

/* Hands-free transcripts can arrive in one burst, well ahead of the audio.
 * Keep unread text on the session task and send one readable part at a time.
 * 'response' separates queued earlier words from the response whose final
 * transcript may replace its deltas. 'skipped' counts reclaimed bytes of it. */
static char *s_caption_text;   /* one wire line of display backlog, in PSRAM */
static size_t s_caption_size, s_caption_page, s_caption_read, s_caption_response, s_caption_skipped;
static int64_t s_caption_due;
static size_t s_caption_visible;
static uint32_t s_word_at, s_word_last_spoken;
static int64_t s_word_changed_us, s_word_due_us, s_last_pcm_us, s_last_text_us;
static bool s_caption_final, s_caption_started;
/* Each group of eight transcript bytes carries a position in the 16 kHz
 * playback FIFO. An extra group covers an unaligned start after compaction. */
static constexpr size_t CUE_BYTES = 8;
static constexpr size_t CUE_COUNT = MUSE_VC_LINE_MAX / CUE_BYTES + 1;
static uint32_t *s_caption_cues;
static size_t s_cue_origin;

static size_t cue_index(size_t offset)
{
    return ((s_cue_origin + offset) / CUE_BYTES) % CUE_COUNT;
}

static void clear_captions()
{
    s_caption_size = s_caption_page = s_caption_read = s_caption_response = s_caption_skipped = 0;
    s_caption_due = s_word_due_us = 0;
    s_caption_started = s_caption_final = false;
    s_caption_visible = s_cue_origin = 0;
    s_word_last_spoken = s_word_at = s_spoken_frames.load();
    s_word_changed_us = now_us();
}

static void drop_caption_prefix(size_t n)
{
    memmove(s_caption_text, s_caption_text + n, s_caption_size - n);
    s_caption_size -= n;
    s_cue_origin = (s_cue_origin + n) % (CUE_BYTES * CUE_COUNT);
    s_caption_visible = s_caption_visible > n ? s_caption_visible - n : 0;
    s_caption_page = s_caption_page > n ? s_caption_page - n : 0;
    s_caption_read = s_caption_read > n ? s_caption_read - n : 0;
    if (n > s_caption_response) s_caption_skipped += n - s_caption_response;
    s_caption_response = s_caption_response > n ? s_caption_response - n : 0;
}

static void queue_caption(const char *text, bool final)
{
    size_t n = strlen(text);
    if (!n) return;
    s_last_text_us = now_us();
    if (final) {
        /* Deltas already shown stay shown when the full transcript arrives. */
        size_t skip = s_caption_skipped < n ? s_caption_skipped : n;
        text += skip;
        n -= skip;
        s_caption_final = true;
        size_t received = s_caption_size - s_caption_response;
        if (n >= received && !memcmp(text, s_caption_text + s_caption_response, received)) {
            /* Preserve audio markers for all the original streaming deltas. */
            text += received;
            n -= received;
        } else {
            s_caption_size = s_caption_response;
            if (s_caption_visible > s_caption_size) s_caption_visible = s_caption_size;
            if (s_caption_read > s_caption_size) s_caption_page = s_caption_read = s_caption_size;
            s_caption_due = 0;
        }
    }
    if (s_caption_size + n > MUSE_VC_LINE_MAX) {
        size_t drop = s_caption_size + n - MUSE_VC_LINE_MAX;
        if (s_caption_read > drop && s_caption_read <= s_caption_size) drop = s_caption_read;
        while (drop < s_caption_size && (s_caption_text[drop] & 0xc0) == 0x80) ++drop;
        drop_caption_prefix(drop);
    }
    for (size_t pos = s_caption_size; pos < s_caption_size + n;) {
        s_caption_cues[cue_index(pos)] = s_pcm_written;
        pos += CUE_BYTES - (s_cue_origin + pos) % CUE_BYTES;
    }
    memcpy(s_caption_text + s_caption_size, text, n);
    s_caption_size += n;
}

/* Reveal words against samples actually taken for the speaker. The service
 * supplies transcript deltas, not word timestamps. A short estimated word
 * duration handles multi-word deltas, and silence cannot advance that clock.
 * A completed text-only response can still be read when playout is idle. */
static void reveal_caption(int64_t now)
{
    if (s_caption_visible >= s_caption_size) return;
    uint32_t spoken = s_spoken_frames.load(std::memory_order_relaxed);
    if (spoken != s_word_last_spoken) {
        s_word_changed_us = now;
        s_word_last_spoken = spoken;
    }
    size_t queued = xStreamBufferBytesAvailable(s_out) / sizeof(int16_t);
    uint32_t read = s_pcm_written - queued;
    size_t from = s_caption_visible, end = from;
    while (end < s_caption_size && (s_caption_text[end] == ' ' || s_caption_text[end] == '\n')) ++end;
    size_t word = end;
    while (end < s_caption_size && end - word < 32 && s_caption_text[end] != ' ' && s_caption_text[end] != '\n') ++end;
    while (end < s_caption_size && (s_caption_text[end] & 0xc0) == 0x80) ++end;
    if (end == word) return;
    uint32_t marker = s_caption_cues[cue_index(end - 1)];
    const uint32_t lag = MUSE_VC_MIC_RATE * 120 / 1000;
    bool at_speaker = (int32_t)(read - marker) >= (int32_t)lag ||
                      (!queued && now - s_last_pcm_us >= 120000);
    bool fallback = (s_caption_final || now - s_last_text_us >= 2000000) &&
                    now - s_word_changed_us >= 1200000 &&
                    (int32_t)(read - marker) >= 0;
    if (!at_speaker || (int32_t)(spoken - s_word_at) < 0) {
        if (!fallback || now < s_word_due_us) return;
    }
    /* Count letters rather than UTF-8 bytes; split long unspaced runs above. */
    unsigned letters = 0;
    for (size_t i = word; i < end; ++i) letters += (s_caption_text[i] & 0xc0) != 0x80;
    uint32_t duration_ms = 100 + 35 * letters;
    s_word_at = spoken + duration_ms * (MUSE_VC_MIC_RATE / 1000);
    s_word_due_us = now + duration_ms * 1000;
    s_caption_visible = end;
}

/* Find the next word boundary that fits the round speaker-on caption box. */
static size_t caption_page_end(size_t from)
{
    size_t end = s_caption_visible;
    if (from < s_caption_response && s_caption_response < end) end = s_caption_response;
    if (end - from <= 32) return end;
    size_t cut = from + 32;
    while (cut > from && (s_caption_text[cut] & 0xc0) == 0x80) --cut;
    if (cut == from) cut = from + 32;   /* not UTF-8: cut at 32 bytes */
    size_t word = cut;
    while (word > from && s_caption_text[word] != ' ' && s_caption_text[word] != '\n') --word;
    return word > from ? word : cut;
}

static void pump_caption(int64_t now)
{
    if (s_ptt || s_call.state != MUSE_VC_LIVE || s_call.held || s_caption_read >= s_caption_size) return;
    reveal_caption(now);
    if (s_caption_read >= s_caption_visible) return;
    size_t from = s_caption_page;
    size_t end = caption_page_end(from);
    if (end <= s_caption_read) {
        if (now < s_caption_due) return;
        from = end;
        while (from < s_caption_visible && (s_caption_text[from] == ' ' || s_caption_text[from] == '\n')) ++from;
        if (from == s_caption_visible) return;
        end = caption_page_end(from);
    }
    char phrase[33];
    size_t n = end - from;
    memcpy(phrase, s_caption_text + from, n);
    phrase[n] = 0;
    if (!emit(MUSE_VOICE_CALL_EV_REPLY, phrase)) return;  /* retry without advancing */
    muse_vc_call_activity(&s_call, now);   /* a text-only reply may span the idle deadline */
    s_caption_read = end;
    s_caption_page = from;
    s_caption_due = now + 160000;  /* keep the last word legible at a page break */
}

static void reply_caption(muse_vc_span_t raw, bool final)
{
    const char *text = decode_caption(raw);
    if (!s_ptt) {
        if (*text && !s_caption_started) {
            s_caption_started = emit(MUSE_VOICE_CALL_EV_REPLY_START, nullptr);
            s_word_changed_us = now_us();
        }
        queue_caption(text, final);
        return;
    }
    size_t n = strlen(text);
    if (!n) return;
    if (final || n >= sizeof(s_reply) - 1) s_reply[0] = 0;
    if (n >= sizeof(s_reply)) text += n - (sizeof(s_reply) - 1);
    n = strlen(text);
    size_t have = strlen(s_reply);
    if (have + n >= sizeof(s_reply)) {
        size_t drop = have + n + 1 - sizeof(s_reply);
        memmove(s_reply, s_reply + drop, have - drop + 1);
    }
    strlcat(s_reply, text, sizeof(s_reply));
    char line[EV_TEXT];
    muse_hatch_tail_words(s_reply, line, sizeof(line));
    if (line[0]) emit(MUSE_VOICE_CALL_EV_REPLY, line);
}

/* ---- Wall clock, for the end-call's timestamps ---- */

/* Nothing on the gadget sets the clock (no SNTP), so it's taken from the
 * realtime response's Date header when it isn't set. Unset and without one,
 * uptime still gives an ordered pair. */
static bool clock_set(int64_t *ms)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    *ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return *ms > 1704067200000LL;   /* 2024-01-01 */
}

static int64_t wall_ms(int64_t now)
{
    int64_t ms;
    if (clock_set(&ms)) {
        return ms;
    }
    return now / 1000 + (s_wall_known ? s_wall_offset_ms : 0);
}

/* ---- Actions ---- */

static bool take(uint32_t acts);

/* ---- Cleanup of a call whose end wasn't confirmed ---- */

static void cleanup_drop(const char *why)
{
    if (s_cleanup.active) {
        ESP_LOGI(TAG, "earlier call %s: %s", s_cleanup.call_id, why);
    }
    s_cleanup = cleanup_t{};
}

/* Keeps this call's end-call (the one sent, or one built now) for the next connection. */
static void cleanup_keep(int64_t now)
{
    if (!s_call.end_call_needed || s_end_final) {
        return;
    }
    cleanup_t c = {};
    c.active = true;
    strlcpy(c.call_id, s_call.call_id, sizeof(c.call_id));
    if (s_end_was_sent) {
        strlcpy(c.body, s_end_sent, sizeof(c.body));
    } else if (!muse_vc_end_call_body(&s_call, wall_ms(now), now, c.body, sizeof(c.body))) {
        return;
    }
    if (s_cleanup.active && strcmp(s_cleanup.call_id, c.call_id) != 0) {
        cleanup_drop("replaced by a later call's");
    }
    if (!s_cleanup.active) {
        c.since_us = c.next_us = now;
        s_cleanup = c;
        ESP_LOGW(TAG, "call %s may still be open on Muse: ending it on the next connection", c.call_id);
    }
}

static void cleanup_failed(int64_t now, const char *why)
{
    if (s_cleanup.tries >= CLEANUP_TRIES) {
        cleanup_drop("not confirmed ended; giving up");
        return;
    }
    ESP_LOGW(TAG, "earlier call %s: end-call %s", s_cleanup.call_id, why);
    s_cleanup.next_us = now + CLEANUP_RETRY_US;
}

/* Sends the pending end-call if the session is connected. */
static void cleanup_send(int64_t now)
{
    if (!s_cleanup.active || s_cleanup_id) {
        return;
    }
    if (s_cleanup.tries >= CLEANUP_TRIES) {
        cleanup_drop("not confirmed ended; giving up");
        return;
    }
    s_cleanup_len = 0;
    int64_t id = muse_session_call_open(false, "/voice/end-call", "application/json", s_cleanup.body, true);
    if (!id) {
        s_cleanup.next_us = now + CLEANUP_IDLE_US;   /* not connected yet */
        return;
    }
    s_cleanup.tries++;
    s_cleanup_id = id;
    s_cleanup_at = now;
    ESP_LOGI(TAG, "ending earlier call %s (try %d)", s_cleanup.call_id, s_cleanup.tries);
}

/* Each pass: the answer deadline, expiry, and a try while nothing else runs. */
static void cleanup_poll(int64_t now)
{
    if (s_cleanup_id && now - s_cleanup_at > CLEANUP_ANSWER_US) {
        muse_session_call_reset(s_cleanup_id);
        s_cleanup_id = 0;
        cleanup_failed(now, "got no answer");
    }
    if (!s_cleanup.active || s_cleanup_id) {
        return;
    }
    if (now - s_cleanup.since_us > CLEANUP_KEEP_US) {
        cleanup_drop("kept too long; dropped");
    } else if (s_call.state == MUSE_VC_IDLE && !s_pending_gen && now >= s_cleanup.next_us) {
        cleanup_send(now);
    }
}

static void end_call_settled(const char *why)
{
    ESP_LOGI(TAG, "end-call %s", why);
    take(muse_vc_call_end_call_done(&s_call, now_us()));
}

static bool ptt_fenced(void);

/* Carries out what the call core asked for. False if a send failed. */
static bool run(uint32_t acts, bool allow_stop_drain = true)
{
    int64_t now = now_us();
    if (allow_stop_drain && s_ptt && s_call.reason == MUSE_VC_END_STOP && ptt_fenced() &&
        (acts & (MUSE_VC_ACT_HALF_CLOSE | MUSE_VC_ACT_ENDED))) {
        if (!s_stop_drain_until) s_stop_drain_until = now + s_call.limits.drain_us;
        if ((acts & MUSE_VC_ACT_ENDED) && now < s_stop_drain_until &&
            (s_defer.len || xStreamBufferBytesAvailable(s_out))) {
            s_stop_ended = true;
            acts &= ~MUSE_VC_ACT_ENDED;
        }
    }
    if (acts & MUSE_VC_ACT_RESET_MEDIA) {
        muse_session_call_reset(s_media_id);
        s_media_id = 0;
    }
    if (acts & MUSE_VC_ACT_RESET_END_CALL) {
        ESP_LOGW(TAG, "end-call: no answer");
        muse_session_call_reset(s_end_id);
        s_end_id = 0;
        s_drop = true;
        cleanup_keep(now);
    }
    if (acts & MUSE_VC_ACT_END_CALL) {
        char body[MUSE_VC_END_CALL_MAX];
        if (!muse_vc_end_call_body(&s_call, wall_ms(now), now, body, sizeof(body))) {
            muse_vc_call_end_call_sent(&s_call, now);
            end_call_settled("body too long");
        } else {
            s_end_len = 0;
            s_end_id = muse_session_call_open(false, "/voice/end-call", "application/json", body, true);
            if (!s_end_id) {
                return false;   /* the connection is gone: the call's kept for the next one */
            }
            strlcpy(s_end_sent, body, sizeof(s_end_sent));
            s_end_was_sent = true;
            ESP_LOGI(TAG, "end-call %s (%s)", s_call.call_id, muse_vc_end_caption(s_call.reason));
            muse_vc_call_end_call_sent(&s_call, now);
        }
    }
    if ((acts & MUSE_VC_ACT_HALF_CLOSE) && s_media_id && !muse_session_call_send(s_media_id, nullptr, 0, true)) {
        return false;
    }
    if (acts & MUSE_VC_ACT_READY) {
        ESP_LOGI(TAG, "call %s live after %d ms", s_call.call_id, (int)((now - s_call.start_us) / 1000));
        /* Admitting this call ended any earlier one of the account's on Muse's side. */
        if (s_cleanup.active && !s_cleanup_id) {
            cleanup_drop("replaced by this call");
        }
        muse_vc_uplink_init(&s_up);
        s_mic_len = s_mic_off = 0;
        emit(MUSE_VOICE_CALL_EV_READY, nullptr);
    }
    if (acts & MUSE_VC_ACT_ENDED) {
        s_stop_ended = false;
        s_stop_drain_until = 0;
        char why[EV_TEXT];
        if (s_call.reason == MUSE_VC_END_REJECTED && s_call.http_status) {
            snprintf(why, sizeof(why), "%s (%d)", muse_vc_end_caption(s_call.reason), s_call.http_status);
        } else if (s_call.reason == MUSE_VC_END_FAILED && s_error[0]) {
            strlcpy(why, s_error, sizeof(why));
        } else {
            strlcpy(why, muse_vc_end_caption(s_call.reason), sizeof(why));
        }
        ESP_LOGI(TAG, "call %s ended: %s after %.1fs; %u audio frames played, %u dropped, %u bad audio, "
                      "%u bad lines, %u long lines skipped",
                 s_call.call_id, muse_vc_end_caption(s_call.reason), (now - s_call.start_us) / 1e6,
                 (unsigned)s_frames_out, (unsigned)s_dropped_audio, (unsigned)s_bad_audio, (unsigned)s_bad_lines,
                 (unsigned)s_lines.skipped);
        s_media_id = s_end_id = 0;
        emit(MUSE_VOICE_CALL_EV_ENDED, why);
    }
    return true;
}

/* The session is the mic buffer's reader, and takes only the bytes of the
 * call it has: once the voice task has started a newer one, never past
 * where that one's audio begins. The written count is read before the
 * generation, so a start after that read began past every byte below it. */
/* The voice task hung up the call the session has (it may not have seen
 * that yet): none of its mic goes anywhere from now on. */
static bool end_requested(void)
{
    return s_taken_gen && s_end_gen.load() == s_taken_gen;
}

static uint32_t in_limit(void)
{
    uint32_t written = s_in_written.load();
    uint32_t gen = s_gen.load();
    if (end_requested()) {
        /* Read after the written count: every byte below it came before the
         * hang-up; from here on none is read (a newer call's start record
         * drops the rest, so none reaches it either). */
        return s_in_read;
    }
    if (gen == s_taken_gen) {
        return written;
    }
    if (gen - s_taken_gen >= START_RING) {
        return s_in_read;   /* the next start's record is gone: take nothing */
    }
    uint32_t cut = s_starts[(s_taken_gen + 1) % START_RING].in_from.load();
    if (s_gen.load() - s_taken_gen >= START_RING) {
        return s_in_read;
    }
    return cut;
}

static size_t in_receive(void *buf, size_t cap)
{
    int32_t room = (int32_t)(in_limit() - s_in_read);
    if (room <= 0) {
        return 0;
    }
    size_t got = xStreamBufferReceive(s_in, buf, cap < (size_t)room ? cap : (size_t)room, 0);
    s_in_read += (uint32_t)got;
    return got;
}

static void drain_in(void)
{
    while (in_receive(s_mic, sizeof(s_mic))) {
    }
}

/* ---- Push-to-talk ---- */

/* The newest call's generation: a start waiting for the session, or the call. */
static uint32_t newest_gen(void)
{
    return s_pending_gen ? s_pending_gen : s_call_gen;
}

/* A push-to-talk call is waiting to start or running: its held audio stays in s_in. */
static bool ptt_keeps_audio(void)
{
    return s_ptt && (s_pending_gen || s_call.state == MUSE_VC_OPENING || s_call.state == MUSE_VC_CONNECTING ||
                     s_call.state == MUSE_VC_LIVE);
}

/* Drops what an earlier call left in s_in, ahead of this call's first byte. */
static void drop_before_call(void)
{
    while ((int32_t)(s_in_from - s_in_read) > 0) {
        size_t want = s_in_from - s_in_read;
        if (!in_receive(s_mic, want < sizeof(s_mic) ? want : sizeof(s_mic))) {
            return;
        }
    }
}

/* Next mic samples into s_mic (none if s_in is empty). */
static size_t mic_take(void)
{
    drop_before_call();
    if ((int32_t)(s_in_from - s_in_read) > 0) {
        return 0;
    }
    return in_receive(s_mic, sizeof(s_mic)) / sizeof(int16_t);
}

static void ring_clear(ring_t *r)
{
    r->head = r->len = 0;
}

static bool ring_push(ring_t *r, const int16_t *pcm, size_t n)
{
    if (r->len + n > DEFER_SAMPLES) {
        return false;
    }
    for (size_t i = 0; i < n;) {
        size_t at = (r->head + r->len) % DEFER_SAMPLES;
        size_t k = DEFER_SAMPLES - at < n - i ? DEFER_SAMPLES - at : n - i;
        memcpy(r->buf + at, pcm + i, k * sizeof(int16_t));
        r->len += k;
        i += k;
    }
    return true;
}

/* Up to the ring's end: the next contiguous piece. */
static size_t ring_peek(const ring_t *r, const int16_t **p)
{
    size_t n = DEFER_SAMPLES - r->head;
    *p = r->buf + r->head;
    return n < r->len ? n : r->len;
}

static void ring_pop(ring_t *r, size_t n)
{
    r->head = (r->head + n) % DEFER_SAMPLES;
    r->len -= n;
}

static void defer_clear(void)
{
    ring_clear(&s_defer);
    ring_clear(&s_wait);
}

static bool taking_audio(void);

/* Released input, current call generation, and the press/flush fence complete. */
static bool ptt_fenced(void)
{
    return !end_requested() && !s_talk && s_gen.load() == s_call_gen && s_press == s_press_req.load() &&
           s_press_clean.load() == s_press && s_flush_req.load() == s_flush_ack.load();
}

static bool ptt_playable(void)
{
    bool draining = s_stop_drain_until && now_us() < s_stop_drain_until;
    return (taking_audio() || draining) && ptt_fenced();
}

static void out_write(const int16_t *pcm, size_t n)
{
    xStreamBufferSend(s_out, pcm, n * sizeof(int16_t), 0);
    if (muse_vc_peak(pcm, n) >= MUSE_VC_AUDIBLE_PEAK) {
        s_st |= ST_REPLY_AUDIO;
    }
}

/* Moves playable reply audio to s_out as far as it fits. */
static void defer_flush(void)
{
    while (s_defer.len && ptt_playable()) {
        const int16_t *p;
        size_t n = ring_peek(&s_defer, &p);
        size_t room = xStreamBufferSpacesAvailable(s_out) / sizeof(int16_t);
        n = n < room ? n : room;
        if (!n) {
            return;
        }
        out_write(p, n);
        ring_pop(&s_defer, n);
    }
}

/* Too much of a reply came before it could play: drop it whole rather than
 * play it cut, and say so. */
static void reply_lost(void)
{
    ESP_LOGW(TAG, "reply dropped: too much audio before it could play");
    muse_vc_reply_gate_press(s_gate);
    defer_clear();
    s_st |= ST_REPLY_LOST;
}

/* The gate dropped something: queued playback (s_out via the reader, and
 * s_defer), or the waiting id's held audio. */
static void gate_flush(void)
{
    uint8_t f = muse_vc_reply_gate_take_flush(s_gate);
    if (f & MUSE_VC_GATE_FLUSH_PLAYING) {
        s_flush_req.fetch_add(1);   /* the reader empties s_out; nothing new is written until it has */
        ring_clear(&s_defer);
        /* The next reply resamples as on a fresh call (no edge or phase of the flushed one). */
        muse_vc_resampler_init(&s_down, MUSE_VC_WIRE_RATE, MUSE_VC_MIC_RATE);
    }
    if (f & MUSE_VC_GATE_FLUSH_WAITING) {
        ring_clear(&s_wait);
    }
}

/* The waiting id's turn started: its audio joins what plays, in order. */
static void wait_to_defer(void)
{
    while (s_wait.len) {
        const int16_t *p;
        size_t n = ring_peek(&s_wait, &p);
        if (!ring_push(&s_defer, p, n)) {
            reply_lost();
            return;
        }
        ring_pop(&s_wait, n);
    }
}

/* A press: the old reply stops for good (muse_vc_reply_gate_t), the mic's
 * turn starts, and the voice task learns the session has stopped writing. */
static void ptt_pressed(uint32_t press)
{
    s_cancel[0] = '\0';
    const char *open = muse_vc_reply_gate_open_response(s_gate);
    if (s_call.state == MUSE_VC_LIVE && open) {
        strlcpy(s_cancel, open, sizeof(s_cancel));
    }
    muse_vc_reply_gate_press(s_gate);
    s_stop_drain_until = 0;
    defer_clear();
    s_reply[0] = '\0';   /* the old reply's caption is done too */
    muse_vc_resampler_init(&s_down, MUSE_VC_WIRE_RATE, MUSE_VC_MIC_RATE);   /* no edge carried from the old reply */
    s_st &= ~(ST_REPLY_AUDIO | ST_REPLY_DONE | ST_REPLY_LOST | ST_HEARD);
    s_talk = true;
    s_tail_left = 0;
    s_press = press;
    s_press_ack.store(press);
}

static void publish(void)
{
    uint32_t gen = newest_gen();
    if (s_st & ST_ENDED && !s_pending_gen) {
        return;   /* emit() stored it */
    }
    uint32_t st = 0;
    if (s_ptt) {
        st = s_st & (ST_REPLY_AUDIO | ST_REPLY_DONE | ST_REPLY_LOST | ST_HEARD);
        bool connecting = s_pending_gen || s_call.state == MUSE_VC_OPENING || s_call.state == MUSE_VC_CONNECTING;
        if (connecting) {
            st |= ST_CONNECTING;
        } else if (s_call.state == MUSE_VC_LIVE) {
            st |= ST_LIVE;
            if (s_call.held) {
                st |= ST_HOLD;
            }
            if (muse_vc_reply_gate_playing(s_gate)) {
                st |= ST_REPLY_OPEN;
            }
        }
        if ((connecting || s_call.state == MUSE_VC_LIVE) &&
            (s_talk || s_tail_left || xStreamBufferBytesAvailable(s_in) || s_mic_off < s_mic_len)) {
            st |= ST_SENDING;
        }
    }
    /* Muse has a caller turn open. Hands-free, only once its first reply is
     * done and while the server doesn't hear the caller; push-to-talk gates
     * those on the voice task (holding, awaiting). */
    if (!s_pending_gen && muse_vc_call_working(&s_call) &&
        (s_ptt || (!s_call.speaking && !s_call.awaiting_reply))) {
        st |= ST_WORKING;
    }
    s_status.store((gen << 16) | st);
}

/* From a stream handler: what can't wait (flush, hold) now, the rest at the next poll. */
static bool take(uint32_t acts)
{
    if ((acts & MUSE_VC_ACT_FLUSH) || ((acts & MUSE_VC_ACT_HOLD) && s_call.held)) clear_captions();
    if (acts & MUSE_VC_ACT_FLUSH) {
        if (!s_ptt) muse_vc_reply_gate_press(s_gate);
        s_flush_req.fetch_add(1);
        /* A barge-in resyncs the downlink; the next reply resamples as on a fresh call. */
        muse_vc_resampler_init(&s_down, MUSE_VC_WIRE_RATE, MUSE_VC_MIC_RATE);
    }
    if (acts & MUSE_VC_ACT_HOLD) {
        ESP_LOGI(TAG, "input %s", s_call.held ? "held" : "resumed");
        /* Whatever the mic queued until now was captured before or during the
         * hold, even if it's released before the next poll: none of it goes up. */
        drain_in();
        muse_vc_uplink_discard(&s_up);
        s_mic_len = s_mic_off = 0;
        emit(s_call.held ? MUSE_VOICE_CALL_EV_HOLD : MUSE_VOICE_CALL_EV_RESUME, nullptr);
    }
    s_acts |= acts & ~(MUSE_VC_ACT_FLUSH | MUSE_VC_ACT_HOLD);
    return true;
}

/* ---- Downlink ---- */

/* Reply audio is wanted once the call is live (its identity confirmed), and
 * after a stop from Muse (its goodbye), whose last words still play while the
 * call winds down. Nothing before readiness reaches the speaker. */
static bool taking_audio(void)
{
    return s_call.state == MUSE_VC_LIVE ||
           (s_call.state == MUSE_VC_ENDING && s_call.reason == MUSE_VC_END_STOP && !s_call.half_closed);
}

static void play(const muse_vc_event_t &ev, int64_t now)
{
    if (!taking_audio()) {
        return;
    }
    if (!ev.audio_rate_ok) {
        s_bad_audio++;
        return;
    }
    if (!s_ptt && s_flush_req.load() != s_flush_ack.load()) {
        return;   /* barged in: the reader hasn't emptied the buffer yet (PTT: it waits in s_defer) */
    }
    muse_vc_gate_verdict_t verdict = MUSE_VC_GATE_PLAY;
    if (s_ptt) {
        verdict = muse_vc_reply_gate_audio(s_gate, &ev);
        gate_flush();
        if (muse_vc_reply_gate_full(s_gate) && s_call.state == MUSE_VC_LIVE) {
            /* Too many replies to tell old from new: end the call, visibly,
             * rather than risk playing an old one or go quiet for good. */
            ESP_LOGW(TAG, "too many replies in one call: ending it");
            strlcpy(s_error, "CALL FULL, CALL AGAIN", sizeof(s_error));
            take(muse_vc_call_hang_up(&s_call, MUSE_VC_END_FAILED, now));
            return;
        }
        if (verdict == MUSE_VC_GATE_DROP) {
            return;   /* stale, unattributed, or no new turn to answer */
        }
    }
    audio_format format = ev.audio_cavd ? audio_format::continuous : audio_format::legacy;
    if (!s_ptt && s_format != audio_format::unknown && s_format != format) {
        ++s_duplicate_audio;
        return;
    }
    muse_vc_resampler_t before = s_down;
    bool audible;
    long n = muse_vc_decode_audio(&s_down, ev.audio, s_dec, DEC_BYTES, s_pcm, &audible);
    if (n < 0) {
        s_bad_audio++;
        return;
    }
    if (!s_ptt && !muse_vc_hands_free_audio(s_gate, ev.response_id, audible)) {
        s_down = before;   /* no interpolation from a discarded packet */
        ++s_retired_audio;
        return;
    }
    if (audible) {
        if (!s_ptt) s_format = format;
        muse_vc_call_activity(&s_call, now);
    }
    size_t bytes = (size_t)n * sizeof(int16_t);
    if (!bytes) {
        return;
    }
    if (s_ptt) {
        if (verdict == MUSE_VC_GATE_HOLD) {
            if (!ring_push(&s_wait, s_pcm, (size_t)n)) {
                reply_lost();
            }
            return;
        }
        defer_flush();
        if (ptt_playable() && !s_defer.len && xStreamBufferSpacesAvailable(s_out) >= bytes) {
            out_write(s_pcm, (size_t)n);
            s_frames_out++;
        } else if (!ring_push(&s_defer, s_pcm, (size_t)n)) {
            if (ptt_playable()) {
                s_dropped_audio++;   /* behind on playback: rx_full should have held the socket */
            } else {
                reply_lost();
            }
        }
        return;
    }
    if (xStreamBufferSpacesAvailable(s_out) < bytes) {
        s_dropped_audio++;
        return;
    }
    s_last_pcm_us = now;
    s_pcm_written += xStreamBufferSend(s_out, s_pcm, bytes, 0) / sizeof(int16_t);
    s_frames_out++;
}

static void on_line(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    muse_vc_event_t ev;
    if (!muse_vc_parse_event(line, len, &ev)) {
        s_bad_lines++;
        return;
    }
    int64_t now = now_us();
    if (ev.kind == MUSE_VC_EV_AUDIO) {
        play(ev, now);
        return;
    }
    bool before = s_call.state == MUSE_VC_LIVE;
    /* In a push-to-talk call, reply captions only while Muse answers the last hold. */
    bool captions = !s_ptt || muse_vc_reply_gate_captions(s_gate);
    switch (ev.kind) {
    case MUSE_VC_EV_RESPONSE_CREATED:
        drop_caption_prefix(s_caption_read);
        s_caption_response = s_caption_size;
        s_caption_skipped = 0;
        s_caption_started = s_caption_final = false;
        if (!s_caption_size) {
            s_caption_due = 0;
            s_word_last_spoken = s_spoken_frames.load();
            s_word_at = s_word_last_spoken + MUSE_VC_MIC_RATE / 10;
            s_word_changed_us = now;
        }
        s_reply[0] = '\0';
        break;
    case MUSE_VC_EV_REPLY_TEXT:
        if (before && captions) {
            reply_caption(ev.text, false);
        }
        break;
    case MUSE_VC_EV_REPLY_DONE:
        if (before && captions) {
            reply_caption(ev.text, true);
        }
        break;
    case MUSE_VC_EV_USER_TEXT:
        if (before) {
            char heard[EV_TEXT];
            muse_hatch_tail_words(decode_caption(ev.text), heard, sizeof(heard));
            if (heard[0]) emit(MUSE_VOICE_CALL_EV_HEARD, heard);
        }
        break;
    case MUSE_VC_EV_RESPONSE_DONE: {
        s_caption_final = true;
        char status[24] = "";
        if (ev.status.p) {
            muse_vc_json_unescape(ev.status, status, sizeof(status));
        }
        ESP_LOGI(TAG, "response done%s%s", status[0] ? ": " : "", status);
        break;
    }
    case MUSE_VC_EV_ERROR: {
        /* The code and type only: the message can carry what the caller said. */
        char code[48] = "", etype[48] = "";
        if (ev.code.p) {
            muse_vc_json_unescape(ev.code, code, sizeof(code));
        }
        if (ev.etype.p) {
            muse_vc_json_unescape(ev.etype, etype, sizeof(etype));
        }
        ESP_LOGW(TAG, "server error: code %s, type %s", code[0] ? code : "-", etype[0] ? etype : "-");
        if (!before || !strcmp(code, "session_dead")) {
            strlcpy(s_error, code[0] ? code : etype[0] ? etype : "CALL FAILED", sizeof(s_error));
        }
        break;
    }
    case MUSE_VC_EV_STOP:
        ESP_LOGI(TAG, "session_control stop");
        break;
    default:
        break;
    }
    take(muse_vc_call_event(&s_call, &ev, now));
    /* The lifecycle validates first (a turn with a bad or conflicting
     * identity fails the call): the gate only hears what it accepted. */
    if (s_ptt && before && s_call.state == MUSE_VC_LIVE) {
        bool was_waiting = muse_vc_reply_gate_waiting(s_gate);
        muse_vc_reply_gate_event(s_gate, &ev);
        gate_flush();
        if (!muse_vc_reply_gate_playing(s_gate)) {
            ring_clear(&s_defer);   /* what played while held is stale now */
        }
        if (was_waiting && !muse_vc_reply_gate_waiting(s_gate)) {
            if (muse_vc_reply_gate_captions(s_gate) && ev.kind == MUSE_VC_EV_TURN_STARTED) {
                wait_to_defer();   /* confirmed: what waited plays, in order */
            } else {
                ring_clear(&s_wait);
            }
        }
        if (ev.kind == MUSE_VC_EV_SPEECH_STARTED) {
            s_st |= ST_HEARD;
        }
        if (muse_vc_reply_gate_done(s_gate, &ev)) {
            s_st |= ST_REPLY_DONE;   /* this reply's own turn or response, never an older one's */
        }
    } else if (s_ptt && s_call.state != MUSE_VC_LIVE) {
        if (ptt_playable()) {
            ring_clear(&s_wait);   /* Muse's goodbye drains only confirmed, playable audio. */
        } else {
            defer_clear();
        }
    }
}

/* ---- Uplink ---- */

static const int16_t s_quiet[MIC_PIECE] = {};

/* A push-to-talk call's uplink: held audio, then quiet. */
static bool pump_ptt(int64_t now)
{
    if (s_cancel[0]) {
        char esc[MUSE_VC_TURN_ID_MAX * 6 + 1], cancel[sizeof(esc) + 64];
        if (muse_vc_json_escape(s_cancel, esc, sizeof(esc))) {
            int n = snprintf(cancel, sizeof(cancel), "{\"type\":\"response.cancel\",\"response_id\":\"%s\"}\n", esc);
            if (n > 0 && (size_t)n < sizeof(cancel) &&
                !muse_session_call_send(s_media_id, reinterpret_cast<const uint8_t *>(cancel), (size_t)n, false)) {
                return false;
            }
        }
        s_cancel[0] = '\0';
    }
    if (s_talk) {
        muse_vc_call_activity(&s_call, now);   /* holding the button isn't idle */
    }
    for (int lines = 0; lines < LINES_PER_POLL;) {
        if (end_requested()) {
            /* Hung up mid-pump: nothing more goes, a partial append included. */
            muse_vc_uplink_discard(&s_up);
            s_mic_len = s_mic_off = 0;
            break;
        }
        if (!muse_vc_uplink_may_send(&s_up, now, s_call.live_us)) {
            break;
        }
        if (s_mic_off == s_mic_len) {
            s_mic_len = mic_take();
            s_mic_off = 0;
        }
        size_t len, used;
        if (s_mic_off < s_mic_len) {
            used = muse_vc_uplink_push(&s_up, s_mic + s_mic_off, s_mic_len - s_mic_off, s_append, &len);
            s_mic_off += used;
        } else if (!s_talk) {
            /* Released and the held audio is all up: quiet, the first
             * PTT_TAIL_MS of it the turn's tail for the server's endpointing,
             * paced to real time on its own clock, from the moment the held
             * audio ran out (however far ahead of real time that went). */
            if (now < s_quiet_next_us) {
                break;
            }
            /* One piece per 20 ms of real time (an append per 40 ms); after a
             * gap (an input hold, a slow loop) it starts again from now rather
             * than catching up. */
            s_quiet_next_us = s_quiet_next_us + 2 * QUIET_STEP_US < now ? now + QUIET_STEP_US
                                                                         : s_quiet_next_us + QUIET_STEP_US;
            used = muse_vc_uplink_push(&s_up, s_quiet, MIC_PIECE, s_append, &len);
            s_tail_left = s_tail_left > used ? s_tail_left - (uint32_t)used : 0;
        } else {
            break;   /* held: wait for the mic */
        }
        if (len) {
            if (!muse_session_call_send(s_media_id, reinterpret_cast<const uint8_t *>(s_append), len, false)) {
                return false;
            }
            lines++;
        }
    }
    return true;
}

static bool pump_mic(int64_t now)
{
    if (ptt_keeps_audio() && s_call.state != MUSE_VC_LIVE) {
        drop_before_call();   /* held audio waits for the call */
        return true;
    }
    if (s_ptt && s_call.state == MUSE_VC_LIVE && !s_call.held && s_media_id) {
        return pump_ptt(now);
    }
    if (s_call.state != MUSE_VC_LIVE || s_call.held || !s_media_id) {
        /* Not live, held, or hung up (a mute, say): the mic goes nowhere, and
         * a partly filled append is dropped, never sent. */
        drain_in();
        muse_vc_uplink_discard(&s_up);
        s_mic_len = s_mic_off = 0;
        return true;
    }
    for (int lines = 0; lines < LINES_PER_POLL;) {
        if (end_requested()) {
            /* Hung up mid-pump: nothing more goes, a partial append included. */
            muse_vc_uplink_discard(&s_up);
            s_mic_len = s_mic_off = 0;
            break;
        }
        if (s_mic_off == s_mic_len) {
            s_mic_len = mic_take();
            s_mic_off = 0;
            if (!s_mic_len) {
                break;
            }
        }
        if (!muse_vc_uplink_may_send(&s_up, now, s_call.live_us)) {
            break;
        }
        size_t len;
        s_mic_off += muse_vc_uplink_push(&s_up, s_mic + s_mic_off, s_mic_len - s_mic_off, s_append, &len);
        if (len) {
            if (!muse_session_call_send(s_media_id, reinterpret_cast<const uint8_t *>(s_append), len, false)) {
                return false;
            }
            lines++;
        }
    }
    return true;
}

/* ---- Starting ---- */

static bool begin_call(uint32_t gen)
{
    s_pending_gen = 0;
    s_call_gen = gen;
    s_stop_drain_until = 0;
    s_stop_ended = false;
    if (!muse_session_call_connect()) {
        emit(MUSE_VOICE_CALL_EV_ENDED, muse_hatch_configured() ? "CAN'T REACH MUSE" : "MUSE NOT SET UP");
        return true;
    }
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    char id[37];
    muse_vc_uuid(rnd, id);
    char admission[MUSE_VC_ADMISSION_MAX];
    const char *device = muse_voice_call_device_id();
    if (!muse_vc_admission(id, device ? device : "", GREETING, admission, sizeof(admission)) ||
        strlcat(admission, MUSE_VC_CAPABILITY_OFFER, sizeof(admission)) >= sizeof(admission)) {
        emit(MUSE_VOICE_CALL_EV_ENDED, "CALL FAILED");
        return true;
    }
    int64_t now = now_us();
    muse_vc_call_begin(&s_call, id, now);
    muse_vc_lines_init(&s_lines, s_line_buf, MUSE_VC_LINE_MAX);
    muse_vc_resampler_init(&s_down, MUSE_VC_WIRE_RATE, MUSE_VC_MIC_RATE);
    muse_vc_uplink_init(&s_up);
    s_mic_len = s_mic_off = 0;
    s_format = audio_format::unknown;
    s_duplicate_audio = s_retired_audio = s_pcm_written = 0;
    muse_vc_reply_gate_init(s_gate);
    if (s_ptt) {
        defer_clear();
        s_cancel[0] = '\0';
        s_st = 0;
        s_quiet_next_us = 0;
    }
    clear_captions();
    s_reply[0] = s_error[0] = '\0';
    s_acts = 0;
    s_drop = false;
    s_end_was_sent = s_end_final = false;
    s_bad_lines = s_bad_audio = s_dropped_audio = s_frames_out = 0;
    char path[96];
    snprintf(path, sizeof(path), "/voice/realtime?call_id=%s", id);
    s_media_id = muse_session_call_open(true, path, "application/x-ndjson; charset=utf-8", admission, false);
    if (!s_media_id) {
        take(muse_vc_call_lost(&s_call, now));
        run(s_acts);
        s_acts = 0;
        return false;
    }
    ESP_LOGI(TAG, "calling: %s", id);
    return true;
}

/* ---- Session side ---- */

bool muse_voice_call_init(void)
{
    if (s_inited) {
        return true;
    }
    static const muse_vc_limits_t limits = {
        .open_us = 30 * 1000000LL,      /* the ingress waits up to 120 s for a head; we don't */
        .ready_us = 40 * 1000000LL,     /* the server gives up on its backend after 25 s */
        .idle_us = CONFIG_MUSE_REALTIME_VOICE_IDLE_S * 1000000LL,
        .max_us = CONFIG_MUSE_REALTIME_VOICE_MAX_S * 1000000LL,
        .end_call_us = 10 * 1000000LL,
        .drain_us = 5 * 1000000LL,
    };
    s_turns = static_cast<muse_vc_turns_t *>(big_alloc(sizeof(muse_vc_turns_t)));
    if (!s_turns) {
        ESP_LOGE(TAG, "no memory for calls");
        return false;
    }
    muse_vc_call_init(&s_call, &limits, s_turns);
    s_events = xQueueCreateWithCaps(16, sizeof(call_ev_t), MALLOC_CAP_SPIRAM);
    s_in = xStreamBufferCreateWithCaps(CALL_IN_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_out = xStreamBufferCreateWithCaps(CALL_OUT_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_line_buf = static_cast<char *>(big_alloc(MUSE_VC_LINE_MAX));
    s_caption_text = static_cast<char *>(big_alloc(MUSE_VC_LINE_MAX));
    s_caption_cues = static_cast<uint32_t *>(big_alloc(CUE_COUNT * sizeof(*s_caption_cues)));
    s_dec = static_cast<uint8_t *>(big_alloc(MUSE_VC_LINE_MAX));
    s_pcm = static_cast<int16_t *>(big_alloc(OUT_SAMPLES * sizeof(int16_t)));
    s_gate = static_cast<muse_vc_reply_gate_t *>(big_alloc(sizeof(muse_vc_reply_gate_t)));
    s_defer.buf = static_cast<int16_t *>(big_alloc(DEFER_SAMPLES * sizeof(int16_t)));
    s_wait.buf = static_cast<int16_t *>(big_alloc(DEFER_SAMPLES * sizeof(int16_t)));
    if (!s_events || !s_in || !s_out || !s_line_buf || !s_caption_text || !s_caption_cues || !s_dec || !s_pcm || !s_gate || !s_defer.buf || !s_wait.buf) {
        ESP_LOGE(TAG, "no memory for calls");
        return false;   /* muse_voice_call_ready() stays false */
    }
    muse_vc_reply_gate_init(s_gate);   /* a press reads it before the first call begins */
    s_inited = true;
    return true;
}

/* The voice task's newest start, its hang-up and its button, all state
 * rather than queued commands: none can be lost, and edges coalesce. */
static void take_commands(int64_t now)
{
    uint32_t gen = s_gen.load();
    if (gen != s_taken_gen) {
        start_rec_t &r = s_starts[gen % START_RING];
        uint32_t in_from = r.in_from.load(), press = r.press.load();
        bool ptt = r.ptt.load();
        if (s_gen.load() == gen) {   /* else it's being rewritten: next pass */
            if (s_call.state != MUSE_VC_IDLE) {
                take(muse_vc_call_hang_up(&s_call, MUSE_VC_END_LOCAL, now));   /* a new call replaces it */
            }
            s_taken_gen = gen;
            s_pending_gen = gen;
            s_ptt = ptt;
            s_st = 0;
            s_talk = false;
            s_tail_left = 0;
            s_in_from = in_from;
            if (ptt) {
                ptt_pressed(press);
            }
        }
    }
    uint32_t end = s_end_gen.load();
    if (end && end == s_pending_gen) {
        s_pending_gen = 0;
        s_call_gen = end;
        emit(MUSE_VOICE_CALL_EV_ENDED, muse_vc_end_caption(MUSE_VC_END_LOCAL));
    } else if (end && end == s_call_gen) {
        take(muse_vc_call_hang_up(&s_call, MUSE_VC_END_LOCAL, now));
    }
    uint32_t w = s_talk_word.load();
    if (s_ptt && !(s_st & ST_ENDED) && (w >> 16) == (newest_gen() & 0xffff)) {
        uint32_t press = (w >> 1) & 0x7fff;
        if (press != (s_press & 0x7fff)) {
            ptt_pressed(s_press + ((press - s_press) & 0x7fff));
        }
        if (!(w & 1) && s_talk) {
            s_talk = false;
            s_tail_left = PTT_TAIL_MS * (MUSE_VC_MIC_RATE / 1000);
        }
    }
}

bool muse_voice_call_poll(void)
{
    if (!s_inited) {
        return true;
    }
    int64_t now = now_us();
    take_commands(now);
    take(muse_vc_call_poll(&s_call, now));
    uint32_t acts = s_acts;
    s_acts = 0;
    if (acts && !run(acts)) {
        return false;
    }
    if (s_drop && s_call.state == MUSE_VC_IDLE) {
        s_drop = false;
        ESP_LOGW(TAG, "Muse didn't confirm the end-call: dropping the connection");
        return false;
    }
    cleanup_poll(now);
    if (s_pending_gen && s_call.state == MUSE_VC_IDLE) {
        /* An earlier call still pending cleanup gets one try first: its
         * end-call names its own id, so it can't end the new one. */
        if (s_cleanup.active && s_gate_done != s_pending_gen) {
            if (s_gate_started != s_pending_gen) {
                /* This start's one try: the one already in flight, or a new one. */
                s_gate_started = s_pending_gen;
                if (!s_cleanup_id && muse_session_call_connect()) {
                    cleanup_send(now);
                }
            }
            if (!s_cleanup_id) {
                s_gate_done = s_pending_gen;   /* tried (or couldn't): go on */
            } else {
                publish();
                return pump_mic(now_us());
            }
        }
        if (!begin_call(s_pending_gen)) {
            return false;
        }
    }
    if (s_ptt) {
        if (s_stop_drain_until && now >= s_stop_drain_until) defer_clear();
        defer_flush();
        if (s_stop_ended && (!ptt_playable() || (!s_defer.len && !xStreamBufferBytesAvailable(s_out)))) {
            defer_clear();
            run(MUSE_VC_ACT_ENDED, false);
        }
    }
    pump_caption(now_us());
    bool ok = pump_mic(now_us());
    publish();
    return ok;
}

bool muse_voice_call_busy(void)
{
    return s_inited && (s_call.state != MUSE_VC_IDLE || s_stop_ended || s_pending_gen || s_cleanup_id ||
                        s_gen.load() != s_taken_gen);
}

bool muse_voice_call_rx_full(void)
{
    if (!s_inited || !taking_audio()) {
        return false;
    }
    if (s_ptt) {
        /* Reply audio that may play overflows into s_defer when s_out is full.
         * Audio that's waiting never holds the socket: the event that frees it
         * (turn_started, a release) comes the same way. */
        return ptt_playable() && (DEFER_SAMPLES - s_defer.len) * sizeof(int16_t) < OUT_ROOM;
    }
    return xStreamBufferSpacesAvailable(s_out) < OUT_ROOM;
}

void muse_voice_call_head(int64_t id, int status, const char *date, size_t date_len)
{
    if (!id || id != s_media_id) {
        return;
    }
    int64_t now = now_us(), ms;
    if (date && !clock_set(&ms)) {
        int64_t at = muse_vc_http_date(date, date_len);
        if (at > 0) {
            s_wall_offset_ms = at - now / 1000;
            s_wall_known = true;
        }
    }
    ESP_LOGI(TAG, "realtime: HTTP %d after %d ms", status, (int)((now - s_call.start_us) / 1000));
    take(muse_vc_call_head(&s_call, status, now));
}

void muse_voice_call_data(int64_t id, const uint8_t *data, size_t len)
{
    if (!id) {
        return;
    }
    if (id == s_media_id) {
        muse_vc_lines_feed(&s_lines, data, len, on_line, nullptr, nullptr);
    } else if (id == s_end_id) {
        size_t take = len < sizeof(s_end_body) - 1 - s_end_len ? len : sizeof(s_end_body) - 1 - s_end_len;
        memcpy(s_end_body + s_end_len, data, take);
        s_end_len += take;
    } else if (id == s_cleanup_id) {
        size_t take = len < sizeof(s_cleanup_ans) - 1 - s_cleanup_len ? len : sizeof(s_cleanup_ans) - 1 - s_cleanup_len;
        memcpy(s_cleanup_ans + s_cleanup_len, data, take);
        s_cleanup_len += take;
    }
}

/* {"ok":true,"result":{"ok":true}}, the answer to an end-call Muse acted on
 * (or found stale, which is as good). */
static bool end_call_confirmed(const char *body, size_t len)
{
    muse_vc_span_t v;
    muse_vc_jkind_t ok = muse_vc_json_get(body, len, "ok", &v);
    muse_vc_jkind_t inner = muse_vc_json_get(body, len, "result.ok", &v);
    return (ok == MUSE_VC_J_TRUE || inner == MUSE_VC_J_TRUE) && ok != MUSE_VC_J_FALSE && inner != MUSE_VC_J_FALSE;
}

void muse_voice_call_stream_end(int64_t id, bool ok, int status)
{
    if (!id) {
        return;
    }
    if (id == s_media_id) {
        s_media_id = 0;
        ESP_LOGI(TAG, "realtime stream %s (HTTP %d)", ok ? "ended" : "failed", status);
        take(muse_vc_call_media_end(&s_call, ok, ok ? 0 : status, now_us()));
    } else if (id == s_end_id) {
        s_end_id = 0;
        bool confirmed = ok && status >= 200 && status < 300 && end_call_confirmed(s_end_body, s_end_len);
        /* A 4xx is Muse's final word on this id (refused, unknown): sending it
         * again wouldn't change it, now or after a reconnect. */
        s_end_final = confirmed || (status >= 400 && status < 500);
        if (!confirmed) {
            ESP_LOGW(TAG, "end-call not confirmed (HTTP %d)", status);
            s_drop = true;
            cleanup_keep(now_us());
        }
        end_call_settled(confirmed ? "confirmed" : "failed");
    } else if (id == s_cleanup_id) {
        s_cleanup_id = 0;
        if (ok && status >= 200 && status < 300 && end_call_confirmed(s_cleanup_ans, s_cleanup_len)) {
            cleanup_drop("confirmed ended");
        } else if (status >= 400 && status < 500) {
            cleanup_drop("end-call refused; not retried");
        } else {
            cleanup_failed(now_us(), "not confirmed");
        }
    }
}

void muse_voice_call_disconnected(void)
{
    if (!s_inited) {
        return;
    }
    int64_t now = now_us();
    /* A call in progress, or ending without a confirmed end-call, may live on at Muse. */
    if (s_call.state != MUSE_VC_IDLE) {
        cleanup_keep(now);
    }
    if (s_cleanup_id) {
        s_cleanup_id = 0;
        cleanup_failed(now, "lost with the connection");
    }
    s_media_id = s_end_id = 0;
    s_drop = false;
    s_stop_drain_until = 0;
    defer_clear();
    take(muse_vc_call_lost(&s_call, now));
    uint32_t acts = s_acts | (s_stop_ended ? MUSE_VC_ACT_ENDED : 0);
    s_acts = 0;
    run(acts & MUSE_VC_ACT_ENDED, false);   /* nothing to send or keep playing on a dead connection */
}

/* ---- Voice task side ---- */

static void drain_out(void)
{
    int16_t junk[128];
    while (xStreamBufferReceive(s_out, junk, sizeof(junk), 0)) {
    }
}

static void talk_word(uint32_t gen, bool held)
{
    s_talk_word.store(((gen & 0xffff) << 16) | ((v_press & 0x7fff) << 1) | (held ? 1u : 0u));
}

/* Publishes a start: its record first, then the generation that names it. */
static uint32_t publish_start(bool ptt)
{
    uint32_t gen = s_gen.load() + 1;   /* the voice task is s_gen's only writer */
    start_rec_t &r = s_starts[gen % START_RING];
    r.in_from.store(s_in_written.load());   /* bytes before this are an earlier call's */
    r.press.store(v_press);
    r.ptt.store(ptt);
    if (ptt) {
        talk_word(gen, true);
    }
    s_gen.store(gen);
    muse_session_call_wake();
    return gen;
}

extern "C" bool muse_voice_call_ready(void)
{
    return s_inited && muse_hatch_ready();
}

extern "C" void muse_voice_call_start(void)
{
#if CONFIG_MUSE_APP_VIEW
    muse_app_view_preempt(MUSE_APP_STOP_CALL);
#endif
    if (!s_inited) {
        return;
    }
    v_call = true;
    v_buffering = true;
    v_first_sample = 0;
    v_ptt = v_talking = v_overflow = false;
    drain_out();
    s_flush_ack.store(s_flush_req.load());
    publish_start(false);
}

extern "C" void muse_voice_call_start_ptt(void)
{
#if CONFIG_MUSE_APP_VIEW
    muse_app_view_preempt(MUSE_APP_STOP_CALL);
#endif
    if (!s_inited) {
        return;
    }
    v_call = v_ptt = v_talking = true;
    v_overflow = false;
    v_press = s_press_req.fetch_add(1) + 1;
    drain_out();
    s_flush_ack.store(s_flush_req.load());
    publish_start(true);
}

extern "C" void muse_voice_call_set_talking(bool held)
{
    if (!s_inited || !v_ptt || held == v_talking) {
        return;
    }
    v_talking = held;
    if (held) {
        v_overflow = false;
        v_press = s_press_req.fetch_add(1) + 1;
        drain_out();   /* the old reply stops now; read() finishes the fence */
    }
    talk_word(s_gen.load(), held);
    muse_session_call_wake();
}

extern "C" void muse_voice_call_end(void)
{
    if (s_inited) {
        if (v_ptt) {
            talk_word(s_gen.load(), false);
        }
        v_talking = false;
        s_end_gen.store(s_gen.load());
        muse_session_call_wake();
    }
}

/* The session's status word, if it's about the voice task's current call. */
static bool status_mine(uint32_t *st)
{
    uint32_t w = s_status.load();
    if ((w >> 16) != (s_gen.load() & 0xffff)) {
        return false;
    }
    *st = w & 0xffff;
    return true;
}

extern "C" bool muse_voice_call_audio(const int16_t *pcm, size_t frames)
{
    size_t bytes = frames * sizeof(int16_t);
    if (!s_in || !v_call) {
        return false;
    }
    if (v_ptt) {
        uint32_t st;
        if (!v_talking || v_overflow || (status_mine(&st) && (st & (ST_HOLD | ST_ENDED)))) {
            return false;   /* only held audio goes up; after a gap, none of the rest of the hold */
        }
        if (xStreamBufferSpacesAvailable(s_in) < bytes) {
            v_overflow = true;
            return false;
        }
    } else if (xStreamBufferBytesAvailable(s_in) + bytes > HANDS_FREE_IN_BYTES ||
               xStreamBufferSpacesAvailable(s_in) < bytes) {
        return false;   /* the session is behind (or not live): this audio is lost either way */
    }
    xStreamBufferSend(s_in, pcm, bytes, 0);
    s_in_written.fetch_add((uint32_t)bytes);
    return true;
}

extern "C" size_t muse_voice_call_read(int16_t *pcm, size_t frames, bool *flushed)
{
    *flushed = false;
    if (!s_out) {
        return 0;
    }
    uint32_t req = s_flush_req.load();
    if (req != s_flush_ack.load()) {
        drain_out();
        s_flush_ack.store(req);
        *flushed = true;
        return 0;
    }
    uint32_t press = s_press_req.load();
    if (v_ptt && s_press_clean.load() != press) {
        /* Only the current push-to-talk call's presses fence the reader: a
         * start the session never took (replaced first) leaves its press
         * unacked, and a hands-free call after it mustn't wait on that. */
        drain_out();   /* empty again whatever the session wrote before it stopped */
        if (s_press_ack.load() == press) {
            s_press_clean.store(press);
        }
        *flushed = v_press_told != press;   /* once per press */
        v_press_told = press;
        return 0;
    }
    size_t n = xStreamBufferReceive(s_out, pcm, frames * sizeof(int16_t), 0) / sizeof(int16_t);
    if (!v_ptt && n && muse_vc_peak(pcm, n) >= MUSE_VC_AUDIBLE_PEAK)
        s_spoken_frames.fetch_add(n, std::memory_order_relaxed);
    return n;
}

extern "C" size_t muse_voice_call_read_paced(int16_t *pcm, size_t frames, bool *flushed)
{
    if (!s_out || s_flush_req.load() != s_flush_ack.load()) {
        v_buffering = true;
        v_first_sample = 0;
        return muse_voice_call_read(pcm, frames, flushed);
    }
    size_t available = xStreamBufferBytesAvailable(s_out) / sizeof(int16_t);
    if (!available) {
        v_buffering = true;
        v_first_sample = 0;
    } else if (v_buffering) {
        int64_t now = now_us();
        if (!v_first_sample) v_first_sample = now;
        /* Three 20 ms blocks absorb a short delivery gap. A final short
         * packet is released at the deadline even if nothing follows it. */
        v_buffering = available < MUSE_VC_MIC_RATE * 60 / 1000 && now - v_first_sample < 60000;
    }
    if (v_buffering) {
        *flushed = false;
        return 0;
    }
    return muse_voice_call_read(pcm, frames, flushed);
}

extern "C" muse_voice_call_ev_t muse_voice_call_event(char *text, size_t cap)
{
    call_ev_t ev;
    while (s_events && xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        if (ev.gen == s_gen.load()) {
            if (ev.type == MUSE_VOICE_CALL_EV_ENDED) {
                v_call = v_ptt = v_talking = false;
            }
            strlcpy(text, ev.text, cap);
            return ev.type;
        }
    }
    return MUSE_VOICE_CALL_EV_NONE;
}

extern "C" bool muse_voice_call_working(void)
{
    uint32_t st;
    return s_inited && status_mine(&st) && !(st & ST_ENDED) && (st & ST_WORKING);
}

extern "C" void muse_voice_call_ptt_status(muse_voice_call_ptt_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_inited || !v_ptt) {
        return;
    }
    uint32_t st;
    if (!status_mine(&st)) {
        st = ST_CONNECTING | ST_SENDING;   /* the session hasn't taken the start yet */
    }
    if (st & ST_ENDED) {
        return;
    }
    out->connecting = st & ST_CONNECTING;
    out->live = st & ST_LIVE;
    out->talking = v_talking;
    out->hold = st & ST_HOLD;
    out->heard = st & ST_HEARD;
    out->capture = v_talking && !out->hold && !v_overflow;
    out->sending = (st & ST_SENDING) || v_talking;
    out->reply_open = st & ST_REPLY_OPEN;
    out->reply_audio = st & ST_REPLY_AUDIO;
    out->reply_done = st & ST_REPLY_DONE;
    out->overflow = v_overflow;
    out->reply_lost = st & ST_REPLY_LOST;
    out->working = st & ST_WORKING;
}
