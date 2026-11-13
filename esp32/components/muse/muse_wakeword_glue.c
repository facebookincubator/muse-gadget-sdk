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

#include "muse_wakeword_glue.h"

#include "sdkconfig.h"

#if CONFIG_MUSE_WAKEWORD_ENABLE

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_input.h"
#include "muse_mem.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_voice.h"
#include "muse_wakeword.h"
#include "ww_runner.h"
#if CONFIG_MUSE_WAKEWORD_RUNNER_BENCH
#include "ww_runner_bench.h"
#endif

static const char *TAG = "muse_wake";

#define TAP_CHUNKS 25                                  /* 500 ms of scheduling slack */
#define CHUNK_BYTES (MUSE_AUDIO_CHUNK * sizeof(int16_t))
#define PRE_FRAMES (MUSE_AUDIO_RATE)                   /* 1 s of upload pre-roll */
#define WAKE_TASK_STACK 4096                           /* the bench runner is tiny */
#define WAKE_TASK_PRIO 5                               /* below muse_voice (6) */

static QueueHandle_t s_queue;
static ww_runner_t *s_runner;
static muse_wakeword_core_t s_core;
/* voice_task -> wake task. A stream buffer is FreeRTOS's single-writer,
 * single-reader primitive: the writer owns the head and the reader the
 * tail, so neither needs a lock. Only whole chunks are ever written. */
static StreamBufferHandle_t s_tap;
static volatile uint32_t s_tap_drops;   /* frames; voice_task only writes */
static muse_ww_ring_t s_pre;    /* wake task only: upload pre-roll */
static int16_t *s_snap;         /* PRE_FRAMES, for muse_voice_seed_preroll */
static bool s_started;
/* Cross-task control surface: other tasks only publish the desired
 * flags below (and the PTT edge flags). The wake task is the single
 * reader of the tap and the single writer of the pre-roll ring, the
 * session core, and the runner, applying flag transitions at its loop
 * boundary in apply_requests(). That keeps the runner single-threaded
 * (see ww_runner.h). */
static volatile bool s_enabled;      /* desired listening opt-in */
static volatile bool s_playback;     /* desired: own-voice playback active */
static volatile bool s_ptt_turn;     /* desired: physical PTT turn recording */
static atomic_bool s_pending_down, s_pending_up;   /* physical, from muse_voice */
#if CONFIG_MUSE_WAKEWORD_RUNNER_BENCH
static volatile bool s_bench_wake_req, s_bench_test_req;
#endif
static volatile bool s_applied_enabled;   /* wake task applies; taps/status read */
static bool s_applied_playback;           /* wake task only */
static bool s_applied_ptt_turn;           /* wake task only */
static muse_wakeword_stats_t s_stats_snap;   /* wake task writes, status reads */
static int s_state_snap;
static uint32_t s_capture_chunks;                    /* wake task only, for the log */
static size_t s_seeded_frames;
static bool s_up_owed;   /* wake task only: a synthetic release the full queue refused */

const char *muse_wakeword_runner_kind(void)
{
    return "bench";
}

bool muse_wakeword_enabled(void)
{
    return s_started && s_enabled;
}

bool muse_wakeword_listening(void)
{
    return s_started && s_applied_enabled && !s_applied_playback && !s_applied_ptt_turn &&
           !muse_voice_resting();
}

void muse_wakeword_set_ptt_turn(bool active)
{
    if (!s_started) {
        return;
    }
    s_ptt_turn = active;   /* applied by the wake task */
}

void muse_wakeword_on_audio(const int16_t *pcm, size_t frames)
{
    if (!s_started || !s_applied_enabled) {
        return;   /* one branch while off: keep the tap cheap */
    }
    /* Whole chunks or nothing: a send into too little space would be
     * partial, so check first. Space only grows between the check and the
     * send (this is the only writer). On overload the newest chunk drops. */
    size_t bytes = frames * sizeof(int16_t);
    if (xStreamBufferSpacesAvailable(s_tap) >= bytes) {
        xStreamBufferSend(s_tap, pcm, bytes, 0);
    } else {
        s_tap_drops += frames;
    }
}

void muse_wakeword_note_ptt(bool down)
{
    if (down) {
        atomic_store(&s_pending_down, true);
    } else {
        atomic_store(&s_pending_up, true);
    }
}

void muse_wakeword_set_playback(bool on)
{
    if (!s_started) {
        return;
    }
    s_playback = on;   /* applied by the wake task */
}

void muse_wakeword_set_enabled(bool on)
{
    if (!s_started) {
        /* Init failed, so nothing listens. Saving "on" would start
         * listening at a later boot nobody asked for; "off" still saves. */
        if (!on) {
            muse_settings_set_wakeword_on(false);
        }
        return;
    }
    muse_settings_set_wakeword_on(on);
    /* Publish only. The wake task applies the transition, so a
     * redundant enable cannot reset a live session out from under
     * muse_voice (which would orphan the synthetic hold). */
    s_enabled = on;
    /* A voice task resting asleep on battery re-checks now rather than at
     * its 60 s backstop: listening keeps the codecs on. */
    muse_state_nudge();
}

void muse_wakeword_bench_trigger(bool with_speech)
{
#if CONFIG_MUSE_WAKEWORD_RUNNER_BENCH
    if (!s_started) {
        return;
    }
    if (with_speech) {
        s_bench_test_req = true;
    } else {
        s_bench_wake_req = true;
    }
    ESP_LOGI(TAG, "bench trigger%s queued", with_speech ? " (with speech)" : "");
#else
    (void)with_speech;
    ESP_LOGW(TAG, "bench trigger ignored: runner=%s has no triggers", muse_wakeword_runner_kind());
#endif
}

static bool post_ptt(muse_ptt_t type)
{
    /* The current input epoch, like a real edge: muse_voice cancels a press
     * whose epoch is stale, so a zero here would end every hands-free turn
     * once any real edge had been dropped. */
    muse_input_event_t ev = { .type = type, .synthetic = true, .epoch = muse_input_call_epoch(),
                              .at_us = esp_timer_get_time() };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "input queue full, synthetic %s %s", type == MUSE_PTT_DOWN ? "down" : "up",
                 type == MUSE_PTT_DOWN ? "dropped" : "retried next chunk");
        return false;
    }
    return true;
}

/* One core tick and everything its actions mean for the device. */
static void tick(const muse_wakeword_input_t *in)
{
    /* A release the full queue refused goes first: without it the turn
     * it ends would record to its 15 s cap. */
    if (s_up_owed && post_ptt(MUSE_PTT_UP)) {
        s_up_owed = false;
    }
    muse_ww_state_t before = muse_wakeword_core_state(&s_core);
    uint32_t act = muse_wakeword_core_tick(&s_core, in);
    muse_ww_state_t now = muse_wakeword_core_state(&s_core);
    if (now == MUSE_WW_CAPTURING || now == MUSE_WW_CAPTURING_PTT) {
        s_capture_chunks++;
    }
    if (before == MUSE_WW_IDLE && now == MUSE_WW_ARMED) {
        ESP_LOGI(TAG, "wake heard (runner=%s), listening for speech", muse_wakeword_runner_kind());
        muse_state_set_asleep(false);   /* like a talk press: the prompt must be seen */
        muse_state_set_caption("WAKE HEARD - SPEAK NOW");
    }
    if (act & MUSE_WW_ACTION_POST_PTT_DOWN) {
        /* Snapshot point: audio muse_voice reads between here and
         * record()'s first live read stays in the tap and is
         * not in the note (input-queue + can_record latency, normally
         * a few ms). Endpointing likewise trails live audio by the
         * current tap backlog (up to TAP_CHUNKS under sustained load).
         * Accepted budget for now; closing it means seeding when
         * record() accepts the turn instead of here. */
        s_seeded_frames = muse_ww_ring_snapshot(&s_pre, s_snap, PRE_FRAMES);
        muse_voice_seed_preroll(s_snap, s_seeded_frames);
        s_capture_chunks = 0;
        /* Show the capture state here, before posting: record() sets the
         * same state when it accepts the turn, and replaces it with the
         * reason when it cannot (for example, Muse is not set up yet). */
        muse_state_set_mode(MUSE_MODE_LISTENING);
        muse_state_set_caption("LISTENING...");
        /* Never a press ahead of an owed release. A press the queue
         * refused opens no turn: drop the session and its seed, so no
         * stray release or stale pre-roll follows it. */
        if (!s_up_owed && post_ptt(MUSE_PTT_DOWN)) {
            ESP_LOGI(TAG, "speech started, pre-roll %.2fs", (double)s_seeded_frames / MUSE_AUDIO_RATE);
        } else {
            muse_wakeword_core_abort(&s_core);
            muse_voice_seed_preroll(NULL, 0);
        }
    }
    if ((act & MUSE_WW_ACTION_POST_PTT_UP) && !post_ptt(MUSE_PTT_UP)) {
        s_up_owed = true;
    }
    if (act & MUSE_WW_ACTION_CONVERTED_TO_PTT) {
        ESP_LOGI(TAG, "the talk button took over");
    }
    if (act & MUSE_WW_ACTION_SUBMITTED) {
        ESP_LOGI(TAG, "wake: captured %.1fs, preroll %.1fs",
                 s_capture_chunks * (double)MUSE_AUDIO_CHUNK / MUSE_AUDIO_RATE,
                 (double)s_seeded_frames / MUSE_AUDIO_RATE);
    }
    if (act & MUSE_WW_ACTION_CANCELLED) {
        if (before == MUSE_WW_ARMED) {
            muse_state_set_caption("%s", "");   /* ours to clear; no churn beyond that */
        }
        ESP_LOGI(TAG, "wake ignored (no speech followed)");
    }
}

/* Wake task only: turn the published desire flags into transitions
 * on the pre-roll ring, core, and runner. Outside init this is the one place
 * those objects are mutated, so no other task can race them. */
static void apply_requests(void)
{
    if (s_enabled != s_applied_enabled) {
        bool on = s_enabled;
        if (on) {
            /* Re-enabled while a disable is still closing a capture:
             * let the core post its up/submit first, then reset. */
            if (muse_wakeword_core_state(&s_core) != MUSE_WW_IDLE) {
                muse_wakeword_input_t in = { 0 };
                tick(&in);
            }
            xStreamBufferReset(s_tap);   /* no task blocks on it here: we are its reader */
            muse_ww_ring_clear(&s_pre);
            muse_wakeword_core_reset(&s_core);   /* an owed release still goes */
            atomic_store(&s_pending_down, false);
            atomic_store(&s_pending_up, false);
            ww_runner_reset(s_runner);
        } else {
            muse_ww_ring_clear(&s_pre);
            /* The core closes an open capture on its next ticks with
             * enabled=false; the task keeps ticking until it is idle. */
        }
        s_applied_enabled = on;
        ww_runner_set_listening(s_runner, on);
        ESP_LOGI(TAG, "listening %s (runner=%s)", on ? "on" : "off", muse_wakeword_runner_kind());
    }
    if (s_playback != s_applied_playback) {
        s_applied_playback = s_playback;
        ww_runner_set_suppressed(s_runner, s_applied_playback || s_applied_ptt_turn);
        muse_ww_ring_clear(&s_pre);   /* Muse's own output is never pre-roll */
        if (!s_applied_playback) {
            ww_runner_reset(s_runner);   /* no stale model state across turns */
        }
    }
    if (s_ptt_turn != s_applied_ptt_turn) {
        bool turned_off = s_applied_ptt_turn && !s_ptt_turn;
        s_applied_ptt_turn = s_ptt_turn;
        ww_runner_set_suppressed(s_runner, s_applied_playback || s_applied_ptt_turn);
        if (turned_off) {
            muse_ww_ring_clear(&s_pre);
            ww_runner_reset(s_runner);
        }
    }
#if CONFIG_MUSE_WAKEWORD_RUNNER_BENCH
    if (s_bench_test_req) {
        s_bench_test_req = s_bench_wake_req = false;
        ww_runner_bench_trigger_test(s_runner);
    } else if (s_bench_wake_req) {
        s_bench_wake_req = false;
        ww_runner_bench_trigger(s_runner);
    }
#endif
}

static void snapshot_status(void)
{
    s_stats_snap = muse_wakeword_core_stats(&s_core);
    s_state_snap = (int)muse_wakeword_core_state(&s_core);
}

static muse_wakeword_input_t next_input(const int16_t *pcm)
{
    /* One suppression gate for the core: own-voice playback and a
     * physical turn both mean "discard wakes". Runner listening and
     * suppression were applied with the transitions above. */
    muse_wakeword_input_t in = {
        .voiced = muse_wakeword_energy_voiced(pcm, MUSE_AUDIO_CHUNK, MUSE_WAKEWORD_ENERGY_RMS_DEFAULT),
        /* Read and clear at once: a press noted in between must not be lost. */
        .ptt_down = atomic_exchange(&s_pending_down, false),
        .ptt_up = atomic_exchange(&s_pending_up, false),
        .playback = s_applied_playback || s_applied_ptt_turn,
        .enabled = s_applied_enabled,
    };
    in.event = ww_runner_feed(s_runner, pcm, MUSE_AUDIO_CHUNK);
    return in;
}

static void wake_task(void *arg)
{
    (void)arg;
    static int16_t chunk[MUSE_AUDIO_CHUNK];
    for (;;) {
        apply_requests();
        /* The timeout keeps requests applied while the tap is dry. */
        if (xStreamBufferReceive(s_tap, chunk, CHUNK_BYTES, pdMS_TO_TICKS(10)) != CHUNK_BYTES) {
            /* Disabled mid-capture with the tap now dry: one silent
             * tick is what closes the synthetic hold. */
            if (!s_applied_enabled && muse_wakeword_core_state(&s_core) != MUSE_WW_IDLE) {
                muse_wakeword_input_t in = { 0 };
                tick(&in);
            } else if (s_up_owed && post_ptt(MUSE_PTT_UP)) {
                s_up_owed = false;   /* the tap is dry, but the release still goes */
            }
            snapshot_status();
            continue;
        }
        if (!s_applied_playback && !s_applied_ptt_turn) {
            muse_ww_ring_push(&s_pre, chunk, MUSE_AUDIO_CHUNK);
        }
        muse_wakeword_input_t in = next_input(chunk);
        tick(&in);
        snapshot_status();
    }
}

esp_err_t muse_wakeword_init(QueueHandle_t queue)
{
    if (s_started) {
        return ESP_OK;
    }
    if (MUSE_AUDIO_RATE != 16000) {   /* runner front-end is 16 kHz only */
        ESP_LOGE(TAG, "audio rate %d is not the runner's 16000", MUSE_AUDIO_RATE);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_queue = queue;
    s_tap = xStreamBufferCreateWithCaps(TAP_CHUNKS * CHUNK_BYTES, CHUNK_BYTES, MUSE_BIG_CAPS);
    int16_t *pre_storage = heap_caps_malloc(PRE_FRAMES * sizeof(int16_t), MUSE_BIG_CAPS);
    s_snap = heap_caps_malloc(PRE_FRAMES * sizeof(int16_t), MUSE_BIG_CAPS);
    ww_config_t cfg = {
        .sample_rate_hz = MUSE_AUDIO_RATE,
#if CONFIG_MUSE_WAKEWORD_SENSITIVITY_LOW
        .sensitivity = WW_SENSITIVITY_LOW,
#elif CONFIG_MUSE_WAKEWORD_SENSITIVITY_HIGH
        .sensitivity = WW_SENSITIVITY_HIGH,
#else
        .sensitivity = WW_SENSITIVITY_DEFAULT,
#endif
        .vad = WW_VAD_ENERGY,
    };
    s_runner = (s_tap && pre_storage && s_snap) ? ww_runner_create(&cfg) : NULL;
    if (!s_runner) {
        if (s_tap) {
            vStreamBufferDeleteWithCaps(s_tap);
            s_tap = NULL;
        }
        free(pre_storage);
        free(s_snap);
        s_snap = NULL;
        ESP_LOGE(TAG, "no memory for wake word");
        return ESP_ERR_NO_MEM;
    }
    muse_ww_ring_init(&s_pre, pre_storage, PRE_FRAMES);
    muse_wakeword_config_t core_cfg = {
        .start_timeout_chunks = muse_wakeword_ms_to_chunks(CONFIG_MUSE_WAKEWORD_START_TIMEOUT_MS),
        .end_silence_chunks = muse_wakeword_ms_to_chunks(CONFIG_MUSE_WAKEWORD_END_SILENCE_MS),
        .max_capture_chunks = muse_wakeword_ms_to_chunks(CONFIG_MUSE_WAKEWORD_MAX_CAPTURE_MS),
        .energy_endpointing = true,
    };
    muse_wakeword_core_init(&s_core, &core_cfg);
    s_enabled = s_applied_enabled = muse_settings_wakeword_on();
    ww_runner_set_listening(s_runner, s_applied_enabled);
    if (xTaskCreatePinnedToCoreWithCaps(wake_task, "muse_wake", WAKE_TASK_STACK, NULL,
                                        WAKE_TASK_PRIO, NULL, MUSE_AUDIO_CORE,
                                        MUSE_BIG_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the wake task");
        ww_runner_destroy(s_runner);
        s_runner = NULL;
        vStreamBufferDeleteWithCaps(s_tap);
        s_tap = NULL;
        free(pre_storage);
        muse_ww_ring_init(&s_pre, NULL, 0);
        free(s_snap);
        s_snap = NULL;
        return ESP_ERR_NO_MEM;   /* s_started stays false: every hook no-ops */
    }
    s_started = true;
    ESP_LOGI(TAG, "wake word ready: runner=%s, listening %s", muse_wakeword_runner_kind(),
             s_enabled ? "on (saved opt-in)" : "off (enable_wakeword turns it on)");
    return ESP_OK;
}

void muse_wakeword_status(muse_wakeword_status_t *out)
{
    if (!out) {
        return;
    }
    /* Diagnostic snapshot refreshed by the wake task; individual
     * fields are word-sized and may tear across one refresh. */
    *out = (muse_wakeword_status_t){
        .enabled = muse_wakeword_enabled(),
        .listening = muse_wakeword_listening(),
        .runner = muse_wakeword_runner_kind(),
        .state = s_state_snap,
        .wakes = s_stats_snap.wakes,
        .cancels = s_stats_snap.cancels,
        .submits = s_stats_snap.submits,
        .conversions = s_stats_snap.conversions,
        .suppressed_wakes = s_stats_snap.suppressed_wakes,
        .dropped_frames = s_tap_drops,
    };
}

#else   /* !CONFIG_MUSE_WAKEWORD_ENABLE: stubs, see the header */

esp_err_t muse_wakeword_init(QueueHandle_t queue)
{
    (void)queue;
    return ESP_OK;
}
void muse_wakeword_on_audio(const int16_t *pcm, size_t frames)
{
    (void)pcm;
    (void)frames;
}
void muse_wakeword_note_ptt(bool down) { (void)down; }
void muse_wakeword_set_enabled(bool on) { (void)on; }
bool muse_wakeword_enabled(void) { return false; }
bool muse_wakeword_listening(void) { return false; }
void muse_wakeword_set_playback(bool on) { (void)on; }
void muse_wakeword_set_ptt_turn(bool active) { (void)active; }
const char *muse_wakeword_runner_kind(void) { return "off"; }
void muse_wakeword_bench_trigger(bool with_speech) { (void)with_speech; }
void muse_wakeword_status(muse_wakeword_status_t *out)
{
    if (out) {
        *out = (muse_wakeword_status_t){ .runner = "off" };
    }
}

#endif
