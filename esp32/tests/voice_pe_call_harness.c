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
 * The Voice PE's call loop (run_call in main/voice.c) against fakes of the
 * board, the player and the call, for test_muse_voice_call.py: the mute switch
 * is the privacy gate, so no captured audio may be handed on once it's on,
 * and no caller text may reach the log.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_compat.h"

/* Logged lines, to check none carries caller text. */
static char s_log[8192];
static void fake_log(const char *fmt, ...)
{
    size_t n = strlen(s_log);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_log + n, sizeof(s_log) - n, fmt, ap);
    va_end(ap);
    strlcat(s_log, "\n", sizeof(s_log));
}
#define ESP_LOGE(tag, ...) fake_log(__VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_log(__VA_ARGS__)
#define ESP_LOGI(tag, ...) fake_log(__VA_ARGS__)
#define MUSE_FAKE_LOG_DEFINED

#include "../main/voice.c"

/* ---- FreeRTOS ---- */

struct fake_queue {
    int items[16];
    int n;
};

QueueHandle_t xQueueCreate(size_t length, size_t item)
{
    (void)length;
    assert(item == sizeof(int));
    return calloc(1, sizeof(struct fake_queue));
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)wait;
    if (q->n == 16) return pdFALSE;
    memcpy(&q->items[q->n++], item, sizeof(int));
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    (void)wait;
    if (!q->n) return pdFALSE;
    memcpy(item, &q->items[0], sizeof(int));
    memmove(q->items, q->items + 1, (size_t)--q->n * sizeof(int));
    return pdTRUE;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, unsigned prio, TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)out;
    return pdPASS;
}

BaseType_t xTaskCreateWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, unsigned prio,
                               TaskHandle_t *out, uint32_t caps)
{
    (void)caps;
    return xTaskCreate(fn, name, stack, arg, prio, out);
}

static int64_t s_now = 1000000;
void vTaskDelay(TickType_t ticks) { s_now += (int64_t)ticks * 1000; }
void vTaskSuspend(TaskHandle_t task) { (void)task; abort(); }
int64_t esp_timer_get_time(void) { return s_now; }
void *heap_caps_malloc_prefer(size_t size, size_t num, ...) { (void)num; return malloc(size); }

/* ---- The board, LEDs, settings ---- */

static bool s_muted;
static bool s_read_muted;   /* the switch was on when the last capture finished */
static int s_reads, s_mute_on_read = -1, s_mute_after_read = -1;
static int s_release_on_read = -1;   /* the button is let go while this chunk is captured (the button task's call) */
static bool on_press(bool pressed);
static char s_seq[512];               /* D down, U up, B begin, A audio, E end: what the client side saw, in order */
static void seq(char c) { size_t n = strlen(s_seq); if (n + 1 < sizeof(s_seq)) { s_seq[n] = c; s_seq[n + 1] = 0; } }

static int s_mic_starts;
esp_err_t voice_board_mic_start(void) { s_mic_starts++; return ESP_OK; }
void voice_board_mic_stop(void) {}
size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak)
{
    s_now += 20000;   /* the read blocks for the chunk */
    s_reads++;
    if (s_reads == s_mute_on_read) {
        s_muted = true;   /* the switch goes on while this chunk is captured */
    }
    if (s_reads == s_release_on_read) {
        on_press(false);
    }
    for (size_t i = 0; i < frames; i++) pcm[i] = (int16_t)(i % 2 ? 3000 : -3000);
    *peak = 3000;
    s_read_muted = s_muted;
    return frames;
}
bool voice_board_muted(void)
{
    bool m = s_muted;
    if (s_reads == s_mute_after_read) {
        s_muted = true;   /* goes on right after this check */
    }
    return m;
}
bool voice_board_echo_cancelled(void) { return false; }
void voice_board_set_volume(int percent) { (void)percent; }
int voice_board_dial_steps(void) { return 0; }
esp_err_t voice_board_init(void) { return ESP_OK; }

esp_err_t voice_player_init(void) { return ESP_OK; }
void voice_player_begin(void) {}
esp_err_t voice_player_write(const int16_t *pcm, size_t samples) { (void)pcm; (void)samples; return ESP_OK; }
void voice_player_end(void) {}
bool voice_player_wait(int timeout_ms) { (void)timeout_ms; return true; }
void voice_player_stop(void) {}
bool voice_player_started(void) { return false; }
size_t voice_player_queued(void) { return 0; }

static led_voice_t s_led_voice;
void led_status_set_voice(led_voice_t voice) { s_led_voice = voice; }
void led_status_set_level(float level) { (void)level; }
void led_status_show_volume(int percent) { (void)percent; }
bool config_get_str(const char *key, char *out, size_t cap) { (void)key; (void)out; (void)cap; return false; }
bool config_set_str(const char *key, const char *value) { (void)key; (void)value; return true; }
void button_set_press_cb(button_press_cb cb) { (void)cb; }
void voice_hatch_refresh(void) {}
bool link_pairing_confirmation_required(void) { return false; }

/* ---- Muse's session and the call ---- */

static int s_audio_calls, s_audio_while_muted, s_end_calls, s_polls;
static bool s_ended;

void muse_hatch_start(void) {}
bool muse_hatch_ready(void) { return true; }
void muse_hatch_turn_begin(void) {}
static int s_turn_audio, s_turn_audio_muted, s_turn_ends, s_turn_cancels;
void muse_hatch_turn_audio(const int16_t *pcm, size_t frames)
{
    (void)pcm; (void)frames;
    s_turn_audio++;
    if (s_read_muted) s_turn_audio_muted++;
}
void muse_hatch_turn_end(void) { s_turn_ends++; }
void muse_hatch_turn_cancel(void) { s_turn_cancels++; }
muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap) { (void)text; (void)cap; return MUSE_HATCH_EV_NONE; }
size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms) { (void)pcm; (void)frames; (void)wait_ms; return 0; }

bool muse_voice_call_ready(void) { return true; }
static int s_call_starts;
void muse_voice_call_start(void) { s_call_starts++; }
void muse_voice_call_end(void) { s_end_calls++; }
bool muse_voice_call_audio(const int16_t *pcm, size_t frames)
{
    (void)pcm; (void)frames;
    s_audio_calls++;
    if (s_read_muted) s_audio_while_muted++;   /* captured with the switch on */
    return true;
}
size_t muse_voice_call_read(int16_t *pcm, size_t frames, bool *flushed)
{
    (void)pcm; (void)frames;
    *flushed = false;
    return 0;
}
muse_voice_call_ev_t muse_voice_call_event(char *text, size_t cap)
{
    s_polls++;
    if (s_polls == 1) {
        text[0] = '\0';
        return MUSE_VOICE_CALL_EV_READY;
    }
    if (s_polls == 3) {
        strlcpy(text, "SECRET CALLER WORDS", cap);
        return MUSE_VOICE_CALL_EV_HEARD;
    }
    if (s_end_calls && !s_ended) {
        s_ended = true;
        strlcpy(text, "CALL ENDED", cap);
        return MUSE_VOICE_CALL_EV_ENDED;
    }
    return MUSE_VOICE_CALL_EV_NONE;
}

#if CONFIG_MUSE_GADGET_BLE_AUDIO
/* musegadgets BLE around the real route decision (mg_route.c). */
#include "mg_route.h"
static mg_policy_t s_policy = MG_POLICY_AUTO;
static bool s_client;
static int s_mg_begins, s_mg_ends;
static mg_route_t s_mg_route = MG_ROUTE_NONE;
mg_route_t mg_voice_route(bool wifi_ready)
{
    mg_route_inputs_t in = { .wifi_ready = wifi_ready, .client_connected = s_client, .client_ready = s_client,
                             .ptt_enabled = s_client };
    return mg_route_decide(s_policy, &in);
}
bool mg_voice_gesture(bool down) { seq(down ? 'D' : 'U'); return s_client; }
bool mg_voice_held_begin(void) { return false; }   /* the held live turn: test_mg_ble's */
bool mg_voice_begin(mg_route_t route, bool from_client) { (void)from_client; s_mg_begins++; s_mg_route = route; seq('B'); return true; }
static int s_mg_audio, s_mg_audio_muted, s_mg_kept = -1;
static int s_link_drop_at = -1;   /* the client's link drops at this mg_voice_audio (the capture ends there) */
static bool s_linked, s_linked_stays;
bool mg_voice_audio(const int16_t *pcm, size_t frames)
{
    (void)pcm; (void)frames;
    s_mg_audio++;
    seq('A');
    if (s_read_muted) s_mg_audio_muted++;
    if (s_mg_audio == s_link_drop_at) {
        s_linked = s_linked_stays;
        return false;
    }
    return true;
}
bool mg_voice_client_linked(void) { return s_linked; }
bool mg_ble_haptic_talk_cue(uint16_t ms) { (void)ms; return false; }
void mg_voice_end(bool keep) { s_mg_kept = keep; s_mg_ends++; seq('E'); }
bool mg_voice_capture_requested(void) { return false; }
void mg_voice_claim(bool claim) { (void)claim; }
void mg_voice_claim_turn(bool on) { (void)on; }
bool mg_voice_playing(void) { return false; }
bool mg_voice_audible(void) { return false; }
bool mg_voice_client_reports_state(void) { return true; }
void mg_voice_face(const char *what) { (void)what; }
#endif

static void reset(void)
{
    s_muted = s_read_muted = false;
    s_reads = s_audio_calls = s_audio_while_muted = s_end_calls = s_polls = 0;
    s_mute_on_read = s_mute_after_read = -1;
    s_ended = false;
    s_log[0] = '\0';
    s_seq[0] = '\0';
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    s_events = xQueueCreate(8, sizeof(voice_evt_t));
    int which = atoi(argv[1]);
    reset();
    if (which == 0) {
        /* The switch goes on during the 5th capture: that chunk and every one after stay put. */
        s_mute_on_read = 5;
    } else if (which == 1) {
        /* ...or right after a check: the chunk captured before it may go, the next may not. */
        s_mute_after_read = 5;
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    } else if (which >= 2 && which <= 4) {
        /* A press, released at once (a tap), routed as the policy says: to the
         * phone over BLE, or on Wi-Fi to Muse, where a tap is a realtime call
         * (the mute switch then hangs it up, at the 20th capture: past the tap's 250 ms tail, ~13 captures,
         * which the switch would otherwise cancel before any call). */
        s_policy = which == 4 ? MG_POLICY_WIFI_ONLY : MG_POLICY_AUTO;
        s_client = which != 3;
        s_mute_on_read = 20;
        atomic_store(&s_ready, true);
        assert(on_press(true));
        on_press(false);
        assert(!run_turn());
        if (which == 2) {
            /* A tap to the phone sends nothing at all (no gesture, no turn, no stop_mic). */
            assert(s_route == MG_ROUTE_BLE && s_mg_begins == 0 && s_mg_ends == 0 && !s_seq[0]);
            assert(!s_call_starts && !s_audio_calls);
        } else {
            assert(s_route == MG_ROUTE_WIFI && !s_mg_begins && s_call_starts == 1 && s_end_calls == 1);
        }
        printf("ok route %d\n", (int)s_route);
        return 0;
    } else if (which == 5) {
        /* A client's start_mic with the mute switch on: the mic never opens, nothing goes, and the end tells the
         * client (and clears its request). */
        s_muted = true;
        s_client = true;
        mg_turn(MG_ROUTE_BLE, true);
        assert(s_mic_starts == 0 && s_reads == 0 && s_mg_audio == 0 && s_mg_begins == 1 && s_mg_ends == 1 && !s_mg_kept);
        printf("ok start_mic refused while muted\n");
        return 0;
    } else if (which == 6 || which == 7) {
        /* A press to the phone (6: held until the release; 7: a client's start_mic), the switch going on during
         * the 4th capture: chunks 1-3 went, the 4th and none after; the turn ends unsent. */
        s_client = true;
        s_mute_on_read = which == 6 ? 17 : 4;   /* a press: after its threshold (13 chunks of pre-roll) */
        mg_turn(MG_ROUTE_BLE, which == 7);
        assert(s_mic_starts == 1 && s_reads == s_mute_on_read && s_mg_audio == s_mute_on_read - 1 &&
               s_mg_audio_muted == 0 && !s_mg_kept);
        printf("ok capture stopped by mute after %d chunks\n", s_mg_audio);
        return 0;
    } else if (which == 9 || which == 10) {
        /* A press to the phone past the minimum (25 chunks, 500 ms) ending with the capture: 9, the client's
         * link drops there: idle, not thinking for 60 s; 10, still linked: thinking (the assistant has it). */
        s_client = true;
        s_linked = true;
        s_link_drop_at = 25;
        if (which == 10) {
            s_linked_stays = true;
        }
        mg_turn(MG_ROUTE_BLE, false);
        assert(s_mg_audio == 25 && s_mg_kept == 1);
        assert(s_led_voice == (which == 9 ? LED_VOICE_IDLE : LED_VOICE_THINKING));
        printf("ok link %s mid-turn: ring %s\n", which == 9 ? "dropped" : "kept", which == 9 ? "idle" : "thinking");
        return 0;
    } else if (which >= 11 && which <= 13) {
        /* The hold contract on the phone route: 11, a tap (let go at the 5th chunk): nothing reaches the client; 12, held to the
         * 20th chunk: button_down at the threshold, the turn opens with its 13 chunks of pre-roll first, button_up at
         * the release, kept; 13, let go at the 14th chunk (280 ms held): the tail after it (250 ms) doesn't count
         * toward the 300 ms minimum, so the turn isn't kept. */
        s_client = true;
        atomic_store(&s_ready, true);
        s_release_on_read = which == 11 ? 5 : which == 12 ? 20 : 14;
        assert(on_press(true));
        assert(!run_turn());
        if (which == 11) {
            assert(!s_seq[0] && s_mg_begins == 0 && s_mg_ends == 0);
            printf("ok tap: nothing sent\n");
        } else {
            assert(!strncmp(s_seq, "DBAAAAAAAAAAAAA", 15));   /* the down, the turn, the pre-roll in order */
            char *u = strchr(s_seq, 'U'), *e = strchr(s_seq, 'E');
            assert(u && e && u < e && !strchr(u + 1, 'U') && e[1] == 0);
            assert(s_mg_kept == (which == 12));
            printf("ok %s: %s\n", which == 12 ? "held 400 ms, kept" : "held 280 ms + tail, not kept", s_seq);
        }
        return 0;
    } else if (which == 8) {
        /* A Wi-Fi press (record), the switch going on during the 4th capture: chunks 1-3 went to Muse, the
         * 4th and none after; the turn is cancelled, no reply, no call. */
        s_policy = MG_POLICY_WIFI_ONLY;
        s_client = false;
        s_mute_on_read = 4;
        atomic_store(&s_ready, true);
        assert(on_press(true));
        assert(!run_turn());
        assert(s_reads == 4 && s_turn_audio == 3 && s_turn_audio_muted == 0 && s_turn_cancels == 1 && !s_turn_ends);
        assert(!s_call_starts);
        printf("ok Wi-Fi turn cancelled by mute after %d chunks\n", s_turn_audio);
        return 0;
#endif
    } else {
        return 2;
    }
    run_call();
    assert(s_end_calls == 1);
    assert(s_audio_while_muted == 0);
    assert(s_audio_calls == (which == 0 ? 3 : 4));   /* reads 2-4 (and 5) after READY on read 1 */
    assert(!strstr(s_log, "SECRET"));
    printf("ok %d chunks before mute\n", s_audio_calls);
    return 0;
}
