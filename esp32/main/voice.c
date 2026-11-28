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

#include "voice.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "button.h"
#include "config_store.h"
#include "led_status.h"
#include "link_pairing.h"
#include "muse_chat.h"
#include "voice_board.h"
#include "voice_muse_chat.h"
#include "voice_player.h"
#if CONFIG_MUSE_REALTIME_VOICE
#include "muse_voice_call.h"
#include "muse_voice_call_core.h"
#endif
#if CONFIG_MUSE_GADGET_BLE_AUDIO
#include "mg_voice.h"
#include "mgcommands.h"
#endif

#include "muse_vol_word.h"   // the bench volume's vol=, header-only

static const char *TAG = "link.voice";

#define KEY_VOLUME          "voice_volume"
#define DEFAULT_VOLUME      60
// Volume change per detent of the dial.
#define VOLUME_STEP         5
// Store a dialled volume once the dial has rested this long.
#define VOLUME_SAVE_MS      2000
#define DIAL_POLL_MS        20

#define CAPTURE_MAX_MS      15000
#define CAPTURE_MIN_MS      300
#define CAPTURE_CHUNK       (VOICE_MIC_RATE / 50)      // 20 ms
#define CAPTURE_MAX         (VOICE_MIC_RATE * CAPTURE_MAX_MS / 1000)
// Keep listening briefly after release so the last word is not clipped.
#define RELEASE_TAIL_MS     250
// The hold contract on the BLE path: a press counts once held this long; before that, nothing reaches the client.
#define HOLD_MS             250
#define HOLD_CHUNKS         ((HOLD_MS + 19) / 20)
#define REPLY_CHUNK         (VOICE_PLAYER_RATE / 50)   // 20 ms

typedef enum { EVT_PRESS, EVT_RELEASE } voice_evt_t;

static QueueHandle_t s_events;
#if CONFIG_MUSE_GADGET_BLE_AUDIO
// Where the press being handled goes (mg_voice.h), decided on the button task.
static volatile mg_route_t s_route = MG_ROUTE_WIFI;
// The press's button_down went to the client (at the hold threshold): whoever takes it sends the button_up, so a
// client never sees an up alone (the button task at the release, or the turn if it sees the release first).
static atomic_bool s_gesture_down;
static void gesture_up(void) {
    if (atomic_exchange(&s_gesture_down, false)) mg_voice_gesture(false);
}
#endif
static atomic_bool s_ready;
static atomic_int s_volume;
// A bench volume holds (vol=N, RAM only, never stored); the dial and
// voice.configure end it.
static atomic_bool s_vol_ram;

static int load_volume(void) {
    char buf[8];
    if (!config_get_str(KEY_VOLUME, buf, sizeof(buf)) || !buf[0]) return DEFAULT_VOLUME;
    int v = atoi(buf);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static bool store_volume(int volume) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", volume);
    return config_set_str(KEY_VOLUME, buf);
}

// Set the speaker volume and show it on the ring.
static void apply_volume(int volume) {
    atomic_store(&s_volume, volume);
    voice_board_set_volume(volume);
    led_status_show_volume(volume);
}

// Turns the volume with the dial. On an internal-RAM stack: storing the volume
// writes NVS.
static void dial_task(void *arg) {
    int64_t save_at = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DIAL_POLL_MS));
        int steps = voice_board_dial_steps();
        if (steps) {
            atomic_store(&s_vol_ram, false);   // the user's own turn: saved below
            int volume = atomic_load(&s_volume) + steps * VOLUME_STEP;
            apply_volume(volume < 0 ? 0 : volume > 100 ? 100 : volume);
            save_at = esp_timer_get_time() + VOLUME_SAVE_MS * 1000LL;
        } else if (save_at && esp_timer_get_time() >= save_at) {
            save_at = 0;
            int volume = atomic_load(&s_volume);
            if (store_volume(volume)) ESP_LOGI(TAG, "volume %d", volume);
            else ESP_LOGW(TAG, "volume could not be stored");
        }
    }
}

// Did a press arrive? Releases are dropped.
static bool pressed_again(TickType_t wait) {
    voice_evt_t evt;
    while (xQueueReceive(s_events, &evt, wait) == pdTRUE) {
        if (evt == EVT_PRESS) return true;
        wait = 0;
    }
    return false;
}

// Stream the microphone into the turn until release (plus a tail) or the
// limit. Returns the number of samples, 0 if the capture failed.
static size_t record(void) {
    static int16_t chunk[CAPTURE_CHUNK];
    if (voice_board_mic_start() != ESP_OK) return 0;
    size_t samples = 0;
    int64_t stop_at = 0;
    while (samples < CAPTURE_MAX) {
        voice_evt_t evt;
        if (!stop_at && xQueueReceive(s_events, &evt, 0) == pdTRUE && evt == EVT_RELEASE) {
            stop_at = esp_timer_get_time() + RELEASE_TAIL_MS * 1000LL;
        }
        if (stop_at && esp_timer_get_time() >= stop_at) break;
        int peak = 0;
        size_t got = voice_board_mic_read(chunk, CAPTURE_CHUNK, &peak);
        // The mute switch wins: read after each chunk, before it's handed on (as in a call).
        if (!got || voice_board_muted()) break;
        muse_hatch_turn_audio(chunk, got);
        samples += got;
        led_status_set_level(peak / 12000.0f);
    }
    voice_board_mic_stop();
    if (samples >= CAPTURE_MAX) ESP_LOGI(TAG, "capture limit reached");
    return samples;
}

static bool fail(const char *why) {
    ESP_LOGW(TAG, "turn failed: %s", why);
    led_status_set_voice(LED_VOICE_ERROR);
    return false;
}

// Play the reply as it arrives. Returns true if a new press interrupted it.
static bool reply(void) {
    static int16_t pcm[REPLY_CHUNK];
    char text[96];
    char incomplete[sizeof(text)] = "";
    bool done = false;
    size_t played = 0;
    int64_t t0 = esp_timer_get_time();
    voice_player_begin();
    for (;;) {
        if (pressed_again(0)) {
            muse_hatch_turn_cancel();
            voice_player_stop();
            return true;
        }
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            switch (ev) {
            case MUSE_HATCH_EV_HEARD:
                ESP_LOGI(TAG, "heard: %s", text);
                led_status_set_voice(LED_VOICE_THINKING);
                break;
            case MUSE_HATCH_EV_REPLY:
                if (!voice_player_started()) led_status_set_voice(LED_VOICE_BUFFERING);
                break;
            case MUSE_HATCH_EV_DONE:
                done = true;
                strlcpy(incomplete, text, sizeof(incomplete));
                break;
            case MUSE_HATCH_EV_ERROR:
                voice_player_stop();
                return fail(text);
            default:
                break;
            }
        }
        // After DONE the reply's audio is all decoded; drain what's left.
        size_t n = muse_hatch_turn_read(pcm, REPLY_CHUNK, done ? 0 : 20);
        if (n) {
            voice_player_write(pcm, n);
            played += n;
        } else if (done) {
            break;
        }
        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);
    }
    voice_player_end();
    while (!voice_player_wait(0)) {
        if (pressed_again(pdMS_TO_TICKS(40))) {
            voice_player_stop();
            return true;
        }
        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);
    }
    ESP_LOGI(TAG, "reply: %.1fs of speech, %.1fs total", (double)played / VOICE_PLAYER_RATE,
             (esp_timer_get_time() - t0) / 1e6);
    if (incomplete[0]) return fail(incomplete);
    led_status_set_voice(LED_VOICE_IDLE);
    return false;
}

#if CONFIG_MUSE_REALTIME_VOICE
// How far the speaker runs behind the player's buffer (DMA), and how long the
// mic stays shut after Muse's voice stops when the call is half duplex.
#define CALL_SPEAKER_LAG_US 80000
#define CALL_TAIL_US        300000
#define CALL_END_WAIT_US    (20LL * 1000000)

// Full duplex only where the XMOS confirms echo cancellation; otherwise the
// mic is shut while Muse is audibly talking.
static bool call_full_duplex(void) {
#if CONFIG_MUSE_REALTIME_VOICE_AEC_DUPLEX
    return voice_board_echo_cancelled();
#else
    return false;
#endif
}

// A realtime call, from a tap until it ends; a press (or the mute switch)
// hangs up. The mic read paces the loop; the player's own task drains the
// reply to the speaker.
static void run_call(void) {
    static int16_t chunk[CAPTURE_CHUNK];
    static int16_t pcm[REPLY_CHUNK];
    char text[96] = "";
    muse_vc_gate_t gate;
    bool duplex = call_full_duplex();
    muse_vc_gate_init(&gate, duplex, CALL_TAIL_US);
    bool live = false, hung_up = false, ended = false;
    int64_t give_up = 0;
    voice_player_stop();
    led_status_set_level(0);
    led_status_set_voice(LED_VOICE_THINKING);
    if (voice_board_mic_start() != ESP_OK) {
        fail("microphone unavailable");
        return;
    }
    voice_player_begin();
    muse_voice_call_start();
    ESP_LOGI(TAG, "call: started (%s duplex)", duplex ? "full" : "half");
    while (!ended) {
        int peak = 0;
        size_t got = voice_board_mic_read(chunk, CAPTURE_CHUNK, &peak);
        int64_t now = esp_timer_get_time();
        // The mute switch is read after the capture and before anything is
        // handed on: a chunk captured while the switch went on is dropped.
        bool muted = voice_board_muted();
        if (got && !muted && live && !hung_up && muse_vc_gate_mic_open(&gate, now)) {
            muse_voice_call_audio(chunk, got);
            led_status_set_level(peak / 12000.0f);
        }
        muse_voice_call_ev_t ev;
        while (!ended && (ev = muse_voice_call_event(text, sizeof(text))) != MUSE_VOICE_CALL_EV_NONE) {
            switch (ev) {
            case MUSE_VOICE_CALL_EV_READY:
                live = true;
                led_status_set_voice(LED_VOICE_LISTENING);
                break;
            case MUSE_VOICE_CALL_EV_ENDED:
                ESP_LOGI(TAG, "call: %s", text);
                ended = true;
                break;
            default:
                break;
            }
        }
        if (ended) break;
        bool press = false;
        voice_evt_t evt;
        while (xQueueReceive(s_events, &evt, 0) == pdTRUE) press |= evt == EVT_PRESS;
        if (!hung_up && (press || muted || !got)) {
            ESP_LOGI(TAG, "call: hanging up (%s)", press ? "press" : muted ? "muted" : "microphone failed");
            hung_up = true;
            give_up = now + CALL_END_WAIT_US;
            muse_voice_call_end();
            voice_player_stop();
            led_status_set_voice(LED_VOICE_THINKING);
        }
        if (hung_up) {
            if (now > give_up) {
                ESP_LOGW(TAG, "call: no word that it ended");
                break;
            }
            if (!got) vTaskDelay(pdMS_TO_TICKS(20));   // no mic to pace the loop
            continue;
        }
        // Move what has arrived to the player (up to 200 ms per pass).
        for (int i = 0; i < 10; i++) {
            bool flushed = false;
            size_t n = muse_voice_call_read(pcm, REPLY_CHUNK, &flushed);
            if (flushed) {
                // The caller barged in: drop what the player still holds.
                voice_player_begin();
                muse_vc_gate_flush(&gate, now);
            }
            if (!n) break;
            int64_t queued = (int64_t)voice_player_queued() * 1000000 / VOICE_PLAYER_RATE;
            muse_vc_gate_playout(&gate, pcm, n, now, queued + CALL_SPEAKER_LAG_US);
            voice_player_write(pcm, n);
        }
        if (live) {
            led_status_set_voice(muse_vc_gate_mic_open(&gate, now) ? LED_VOICE_LISTENING : LED_VOICE_SPEAKING);
        }
    }
    voice_board_mic_stop();
    led_status_set_level(0);
    if (ended && !hung_up) {
        // Muse ended it (a goodbye, say): let what it already said finish.
        bool flushed;
        size_t n;
        while ((n = muse_voice_call_read(pcm, REPLY_CHUNK, &flushed)) > 0) voice_player_write(pcm, n);
        voice_player_end();
        while (!voice_player_wait(0)) {
            if (pressed_again(pdMS_TO_TICKS(40))) break;
        }
    }
    voice_player_stop();
    led_status_set_voice(LED_VOICE_IDLE);
}
#endif

#if CONFIG_MUSE_GADGET_BLE_AUDIO
// A press (or the client's start_mic) streamed to a musegadgets BLE client, or
// recorded to the offline queue for one. No reply to play: the client has it.
// The ring after a musegadgets utterance, as on the Wi-Fi path: thinking once
// push-to-talk ends, then the client's assistant_state (responding: speaking,
// done or idle: idle, error: the error colour for 3 s), idle after 60 s
// without an update or when the client disconnects.
#define RING_TIMEOUT_US (60LL * 1000000)
#define RING_ERROR_US (3LL * 1000000)
static volatile int s_ring_pending = -1;
static bool s_ring_turn;
static int64_t s_ring_until;

void voice_assistant_state(uint8_t state) {
    s_ring_pending = state;
}

static void ring(led_voice_t v, int64_t for_us, const char *name) {
    led_status_set_voice(v);
    s_ring_turn = v != LED_VOICE_IDLE;
    s_ring_until = esp_timer_get_time() + for_us;
    char what[24];
    snprintf(what, sizeof(what), "ring %s", name);
    mg_voice_face(what);
}

static void ring_update(void) {
    int st = s_ring_pending;
    if (st >= 0) {
        s_ring_pending = -1;
        switch (st) {
        case mg_assistant_state_thinking: ring(LED_VOICE_THINKING, RING_TIMEOUT_US, "thinking"); break;
        case mg_assistant_state_responding: ring(LED_VOICE_SPEAKING, RING_TIMEOUT_US, "responding"); break;
        case mg_assistant_state_error: ring(LED_VOICE_ERROR, RING_ERROR_US, "error"); break;
        default: ring(LED_VOICE_IDLE, 0, st == mg_assistant_state_done ? "done" : "idle"); break;
        }
    }
    if (s_ring_turn && esp_timer_get_time() >= s_ring_until) ring(LED_VOICE_IDLE, 0, "idle");
}

static void mg_turn(mg_route_t route, bool from_client) {
    static int16_t chunk[CAPTURE_CHUNK];
    voice_player_stop();
    led_status_set_level(0);
    led_status_set_voice(LED_VOICE_LISTENING);
    // The mute switch wins over a press and a client's start_mic alike, also while a held turn waits for its
    // proof: the mic never opens with it on (the end still tells the client, and clears its request).
    bool muted = voice_board_muted();
    // The hold contract (a press, not a client's start_mic): the capture starts at the press into a pre-roll;
    // a release before HOLD_MS is a tap (nothing reaches the client: no gesture, audio or stop_mic); held past it,
    // the turn opens with its button_down and the pre-roll first.
    static int16_t pre[HOLD_CHUNKS][CAPTURE_CHUNK];
    size_t pre_got[HOLD_CHUNKS] = { 0 };
    int npre = 0;
    if (!from_client && muted) {
        ESP_LOGI(TAG, "capture refused: muted");   // a press: nothing reached the client, nothing to end
        ring(LED_VOICE_IDLE, 0, "idle");
        return;
    }
    if (!from_client) {
        if (voice_board_mic_start() != ESP_OK) {
            fail("mic");
            return;
        }
        for (; npre < HOLD_CHUNKS; npre++) {
            voice_evt_t evt;
            if (xQueueReceive(s_events, &evt, 0) == pdTRUE && evt == EVT_RELEASE) break;   // a tap
            int peak = 0;
            pre_got[npre] = voice_board_mic_read(pre[npre], CAPTURE_CHUNK, &peak);
            if (!pre_got[npre] || (muted = voice_board_muted())) break;
            led_status_set_level(peak / 12000.0f);
        }
        if (npre < HOLD_CHUNKS) {
            voice_board_mic_stop();
            led_status_set_level(0);
            if (muted) ESP_LOGI(TAG, "capture refused: muted");
            else ESP_LOGI(TAG, "a tap (%d ms): nothing sent", npre * 20);
            ring(LED_VOICE_IDLE, 0, "idle");
            return;
        }
        if (route == MG_ROUTE_HELD) {
            mg_voice_held_begin();   // the connect burst: held until the client is proven and ready
        }
        if (mg_voice_gesture(true)) atomic_store(&s_gesture_down, true);   // held past the threshold: the press now
    }
    if (!mg_voice_begin(route, from_client) || muted || (from_client && voice_board_mic_start() != ESP_OK)) {
        if (!from_client) voice_board_mic_stop();
        gesture_up();
        mg_voice_end(false);
        if (muted) {
            ESP_LOGI(TAG, "capture refused: muted");
            ring(LED_VOICE_IDLE, 0, "idle");
        } else {
            fail("BLE client not ready");
        }
        return;
    }
    size_t samples = 0, held = 0;   // held: up to the release (the tail after it doesn't count toward the minimum)
    for (int i = 0; i < npre; i++) {
        samples += pre_got[i];
        held += pre_got[i];
        if (!mg_voice_audio(pre[i], pre_got[i])) break;   // the pre-roll first, in order
    }
    int64_t stop_at = 0;
    for (;;) {
        voice_evt_t evt;
        if (!from_client && !stop_at && xQueueReceive(s_events, &evt, 0) == pdTRUE && evt == EVT_RELEASE) {
            stop_at = esp_timer_get_time() + RELEASE_TAIL_MS * 1000LL;
            gesture_up();   // (the button task's, if it hasn't: an up only after its down)
        }
        if ((stop_at && esp_timer_get_time() >= stop_at) || (!from_client && samples >= CAPTURE_MAX)) break;
        int peak = 0;
        size_t got = voice_board_mic_read(chunk, CAPTURE_CHUNK, &peak);
        // The mute switch wins: read after each chunk, before it's handed on; the capture ends unsent.
        if (!got || (muted = voice_board_muted())) break;
        samples += got;
        if (!stop_at) held += got;
        led_status_set_level(peak / 12000.0f);
        if (!mg_voice_audio(chunk, got)) break;
    }
    voice_board_mic_stop();
    gesture_up();
    bool keep = !muted && (from_client || held >= VOICE_MIC_RATE * CAPTURE_MIN_MS / 1000);
    mg_voice_end(keep);
    led_status_set_level(0);
    if (keep && route == MG_ROUTE_BLE && !from_client && mg_voice_client_linked()) {
        s_ring_pending = -1;
        ring(LED_VOICE_THINKING, RING_TIMEOUT_US, "thinking");   // with the assistant now
    } else {   // (a client gone mid-turn has nothing to think about: idle, not 60 s of thinking)
        ring(LED_VOICE_IDLE, 0, "idle");
    }
}
#endif

// Returns true if a new press interrupted the turn.
static bool run_turn(void) {
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    mg_route_t route = s_route;
    if (route == MG_ROUTE_BLE || route == MG_ROUTE_QUEUE || route == MG_ROUTE_HELD) {
        mg_turn(route, false);
        return false;
    }
#endif
    voice_player_stop();
    led_status_set_level(0);
    led_status_set_voice(LED_VOICE_LISTENING);
    muse_hatch_turn_begin();
    size_t samples = record();
    if (voice_board_muted()) {
        // Muted during the press: the turn ends unsent, no call.
        muse_hatch_turn_cancel();
        ESP_LOGI(TAG, "turn cancelled: muted");
        led_status_set_voice(LED_VOICE_IDLE);
        return false;
    }
    if (samples < VOICE_MIC_RATE * CAPTURE_MIN_MS / 1000) {
        muse_hatch_turn_cancel();
        if (!samples) return fail("microphone unavailable");
#if CONFIG_MUSE_REALTIME_VOICE
        // A tap: a call, beside holding for a note.
        if (muse_voice_call_ready()) {
            run_call();
            return false;
        }
#endif
        ESP_LOGI(TAG, "press too short");
        led_status_set_voice(LED_VOICE_IDLE);
        return false;
    }
    ESP_LOGI(TAG, "recorded %.1fs", (double)samples / VOICE_MIC_RATE);
    led_status_set_voice(LED_VOICE_TRANSCRIBING);
    muse_hatch_turn_end();
    return reply();
}

// Runs on the button task. Claims the press only when a turn can run, so the
// button keeps its setup role otherwise.
static bool on_press(bool pressed) {
    if (pressed) {
        if (!atomic_load(&s_ready) || voice_board_muted()) return false;
        // A pairing waiting for its confirmation takes the button (a press,
        // or a re-setup's hold): never push to talk meanwhile.
        if (link_pairing_confirmation_required()) return false;
        voice_hatch_refresh();
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        // The voice transport: this press goes to a BLE client, the offline
        // queue, or Muse over Wi-Fi; if nowhere, the button keeps its setup role.
        s_route = mg_voice_route(muse_hatch_ready());
        if (s_route == MG_ROUTE_NONE || (s_route == MG_ROUTE_WIFI && !muse_hatch_ready())) return false;
#else
        if (!muse_hatch_ready()) return false;
#endif
    }
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    if (s_route == MG_ROUTE_BLE || s_route == MG_ROUTE_HELD || s_route == MG_ROUTE_QUEUE) {
        // A turn's gestures follow the hold threshold (mg_turn sends the down there); the up only after it.
        if (!pressed) gesture_up();
    } else {
        mg_voice_gesture(pressed);
    }
#endif
    voice_evt_t evt = pressed ? EVT_PRESS : EVT_RELEASE;
    xQueueSend(s_events, &evt, 0);
    return true;
}

static void voice_task(void *arg) {
    if (voice_board_init() != ESP_OK || voice_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio hardware unavailable; voice chat disabled");
        vTaskSuspend(NULL);
        return;
    }
    voice_board_set_volume(atomic_load(&s_volume));
    atomic_store(&s_ready, true);
    if (xTaskCreate(dial_task, "dial", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the dial");
    }
    ESP_LOGI(TAG, "ready");

    for (;;) {
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        // A musegadgets client may start capture itself (start_mic).
        if (mg_voice_capture_requested()) {
            mg_turn(MG_ROUTE_BLE, true);
            continue;
        }
        ring_update();
        if (!pressed_again(pdMS_TO_TICKS(100))) continue;
#else
        if (!pressed_again(portMAX_DELAY)) continue;
#endif
        while (run_turn()) {
        }
    }
}

void voice_init(void) {
    s_events = xQueueCreate(8, sizeof(voice_evt_t));
    if (!s_events) {
        ESP_LOGE(TAG, "no memory for voice chat");
        return;
    }
    atomic_store(&s_volume, load_volume());
    voice_hatch_refresh();
    muse_hatch_start();
    // The stack is in PSRAM, so the task must not touch flash (NVS): pairing
    // needs an 8 KB internal block for its TLS task.
    if (xTaskCreateWithCaps(voice_task, "voice", 4096, NULL, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "failed to start voice chat");
        return;
    }
    button_set_press_cb(on_press);
}

// The bench volume (bench and test mutes are RAM-only), on the USB console (sdk_token_console.c): vol=N sets the speaker
// in RAM only, never stored, and the ring shows it; vol=saved goes back to
// the stored volume; vol? reads both. Answered as on the Muse boards
// (muse_vol_word.h). A reboot comes back at the stored volume.
bool voice_volume_word(const char *line) {
    int pct = 0;
    muse_vol_word_t w = muse_vol_word(line, &pct);
    if (w == MUSE_VOL_WORD_NONE) return false;
    if (w == MUSE_VOL_WORD_BAD) {
        printf("@vol error: " MUSE_VOL_USAGE "\n");
        fflush(stdout);
        return true;
    }
    int saved = load_volume();
    int now = -1;
    if (w == MUSE_VOL_WORD_SET) {
        atomic_store(&s_vol_ram, true);
        now = pct;
    } else if (w == MUSE_VOL_WORD_SAVED && atomic_exchange(&s_vol_ram, false)) {
        now = saved;
    }
    if (now >= 0) {
        if (atomic_load(&s_ready)) apply_volume(now);
        else {
            atomic_store(&s_volume, now);
            led_status_show_volume(now);
        }
    }
    char out[48];
    muse_vol_text(out, sizeof(out), w == MUSE_VOL_WORD_SET, atomic_load(&s_volume), saved);
    printf("@%s\n", out);
    fflush(stdout);
    ESP_LOGI(TAG, "%s", out);
    return true;
}

cJSON *voice_configure_command(cJSON *params) {
    cJSON *volume = cJSON_GetObjectItem(params, "volume");
    const char *why = NULL;
    if (volume) {
        if (!cJSON_IsNumber(volume) || volume->valueint < 0 || volume->valueint > 100) {
            why = "volume must be 0-100";
        } else if (volume->valueint != atomic_load(&s_volume) || atomic_load(&s_vol_ram)) {
            // A change from outside (Muse): saved, and the ring shows it at once
            // (a screenless gadget plays its volume
            // cue), before voice is up too. The level it already plays at
            // changes nothing.
            if (!store_volume(volume->valueint)) why = "volume could not be stored";
            else {
                atomic_store(&s_vol_ram, false);
                if (atomic_load(&s_ready)) apply_volume(volume->valueint);
                else {
                    atomic_store(&s_volume, volume->valueint);
                    led_status_show_volume(volume->valueint);
                }
            }
        }
    }

    cJSON *result = cJSON_CreateObject();
    if (why) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "code", "invalid_params");
        cJSON_AddStringToObject(error, "message", why);
        cJSON_AddItemToObject(result, "error", error);
        return result;
    }
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "volume", atomic_load(&s_volume));
    return result;
}
