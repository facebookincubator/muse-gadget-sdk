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

esp_err_t voice_board_mic_start(void) { return ESP_OK; }
void voice_board_mic_stop(void) {}
size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak)
{
    s_now += 20000;   /* the read blocks for the chunk */
    s_reads++;
    if (s_reads == s_mute_on_read) {
        s_muted = true;   /* the switch goes on while this chunk is captured */
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

void led_status_set_voice(led_voice_t voice) { (void)voice; }
void led_status_set_level(float level) { (void)level; }
void led_status_show_volume(int percent) { (void)percent; }
bool config_get_str(const char *key, char *out, size_t cap) { (void)key; (void)out; (void)cap; return false; }
bool config_set_str(const char *key, const char *value) { (void)key; (void)value; return true; }
void button_set_press_cb(button_press_cb cb) { (void)cb; }
void voice_hatch_refresh(void) {}

/* ---- Muse's session and the call ---- */

static int s_audio_calls, s_audio_while_muted, s_end_calls, s_polls;
static bool s_ended;

void muse_hatch_start(void) {}
bool muse_hatch_ready(void) { return true; }
void muse_hatch_turn_begin(void) {}
void muse_hatch_turn_audio(const int16_t *pcm, size_t frames) { (void)pcm; (void)frames; }
void muse_hatch_turn_end(void) {}
void muse_hatch_turn_cancel(void) {}
muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap) { (void)text; (void)cap; return MUSE_HATCH_EV_NONE; }
size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms) { (void)pcm; (void)frames; (void)wait_ms; return 0; }

bool muse_voice_call_ready(void) { return true; }
void muse_voice_call_start(void) {}
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

static void reset(void)
{
    s_muted = s_read_muted = false;
    s_reads = s_audio_calls = s_audio_while_muted = s_end_calls = s_polls = 0;
    s_mute_on_read = s_mute_after_read = -1;
    s_ended = false;
    s_log[0] = '\0';
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
