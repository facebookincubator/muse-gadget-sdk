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

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>


#define IN_MAX 1024
#define IN_MS 2000
#define SERIAL_QUIET_MS 6000
#define SERIAL_HELLO 0xffff
#define SERIAL_BYE 0xfffe
#define SEND_MS 3000
#define KEYS 11
#define MUSE_MIRROR_ADPCM_CHUNK 164
#define MUSE_MIRROR_AUDIO_CHUNK 640
#define FAST_MS 150
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0

#include "session_types.inc"
static serial_msg_t queue[32];
static int head, count, inputs, acknowledgements, boards, dirty, waited;
static bool queue_stalled;
static int s_serial_in, s_serial_done;
static int s_w = 10, s_h = 10, s_ptr_x, s_ptr_y;
static bool s_serial, send_ok = true, pointer_down;
static atomic_bool s_serial_open;
static atomic_bool s_session_busy;
static bool s_ready = true, pump_on_send;
static int s_serial_tx;
static const uint8_t s_mark[] = "MIRROR1\n";
static void serial_pump(void);
static unsigned held;
static const char s_board[] = "B{}";
static size_t s_board_len = sizeof(s_board) - 1;
static int xQueueReceive(int q, serial_msg_t *m, int ticks)
{
    (void)q; (void)ticks;
    if (!count) return 0;
    memset(m, 0xcc, sizeof(*m));
    memcpy(m, &queue[head++], SERIAL_ITEM_SIZE);
    count--;
    return pdTRUE;
}
static int xQueueSend(int q, const serial_msg_t *m, int ticks)
{
    (void)q;
    waited = ticks;
    if (queue_stalled && !ticks) return 0;
    queue_stalled = false;
    if (!count) head = 0;
    memcpy(&queue[head + count++], m, SERIAL_ITEM_SIZE);
    if (pump_on_send) serial_pump();
    return pdTRUE;
}
static void xSemaphoreGive(int sem) { if (sem == s_serial_done) acknowledgements++; }
static void xSemaphoreTake(int sem, int ticks) { (void)sem; (void)ticks; }
static int audio_resets, mic_packed, offers, baud_now, baud_sets, settled;
static bool uart = true;
static atomic_int s_baud;
static void viewer_options(void) { offers++; assert(atomic_load(&s_baud) == baud_now); }
static bool muse_console_set_baud(int baud) { if (!uart) return false; baud_now = baud; baud_sets++; return true; }
static void muse_mirror_audio_reset(void) { audio_resets++; }
static void muse_mirror_audio_mic_adpcm(const uint8_t *chunk)
{
    for (size_t i = 0; i < MUSE_MIRROR_ADPCM_CHUNK; i++) assert(chunk[i] == (uint8_t)(i + 2));
    mic_packed++;
}
static void muse_mirror_audio_mic_put(const uint8_t *pcm, size_t samples)
{
    assert(samples == MUSE_MIRROR_AUDIO_CHUNK / 2 && pcm[0] == 2);
    mic_packed++;
}
static int text_resets;
static void text_reset(void) { text_resets++; }
static void keys_release(void) { held = 0; }
static void pointer_put(int x, int y, bool down) { (void)x; (void)y; pointer_down = down; }
static bool viewer_send(const void *p, size_t n, int ms)
{
    (void)n; (void)ms;
    if (*(const char *)p == 'B') boards++;
    return send_ok;
}
static int s_task;
static void xTaskNotifyGive(int task) { (void)task; }
static void capture_set(bool on) { (void)on; }
static size_t last_input;
static void input(const uint8_t *p, size_t n)
{
    assert(n <= SERIAL_INPUT_MAX);
    if (pump_on_send) {
        assert(n && p[0] == 'R');
        for (size_t i = 1; i < n; i++) assert(p[i] == (uint8_t)i);
    }
    last_input = n;
    inputs++;
}
static void dirty_add(int x1, int y1, int x2, int y2)
{
    assert(!x1 && !y1 && x2 == s_w - 1 && y2 == s_h - 1);
    dirty++;
}
typedef int esp_err_t;
typedef int (*vprintf_like_t)(const char *, va_list);
#define ESP_OK 0
#define ESP_ERR_NOT_SUPPORTED 1
#define ESP_ERR_INVALID_STATE 2
#define TAG "muse_mirror"
#define ESP_LOGW(tag, ...) ((void)(tag))
static int serial_log(const char *fmt, va_list ap) { (void)fmt; (void)ap; return 0; }
static vprintf_like_t esp_log_set_vprintf(vprintf_like_t f) { (void)f; return serial_log; }
static void vTaskDelay(int ticks) { if (ticks == FAST_MS) settled++; }
static int64_t now;
static int64_t esp_timer_get_time(void) { return now; }
static bool serial_write(const void *p, size_t n, int64_t until)
{
    (void)p; (void)n; (void)until; return true;
}
static uint8_t wire[IN_MAX + 9];
static size_t wire_size, wire_at, payload_size;
static bool payload_timeout;
static int payload_reads;
static bool muse_console_read(void *p, size_t n, int ms)
{
    if (wire_at == 4) {
        assert(n == payload_size && ms == IN_MS);
        payload_reads++;
        if (payload_timeout) {
            now += ms * 1000LL;
            wire_at += n / 2;
            return false;
        }
    }
    if (wire_size - wire_at < n) {
        now += ms * 1000LL;
        return false;
    }
    memcpy(p, wire + wire_at, n);
    wire_at += n;
    return true;
}
#include "session_impl.inc"
static void put(int kind)
{
    if (!count) head = 0;
    queue[head + count++].len = kind;
}
int main(void)
{
    s_serial_in = 1; s_serial_done = 2;
    s_serial_msg = calloc(1, sizeof(*s_serial_msg));
    s_serial_open = true;
    put(SERIAL_HELLO);
    serial_pump();
    assert(s_serial && boards == 1 && dirty == 1 && text_resets == 1);
    held = 6; pointer_down = true;
    s_serial_open = false;
    put(3); put(SERIAL_BYE);
    serial_pump();
    assert(!s_serial && !held && !pointer_down && !inputs && acknowledgements == 1 && text_resets == 2);
    put(SERIAL_HELLO); put(SERIAL_BYE);
    serial_pump();
    assert(boards == 1 && acknowledgements == 2);
    s_serial_open = true; send_ok = false;
    put(SERIAL_HELLO); put(SERIAL_BYE);
    serial_pump();
    assert(!s_serial && !s_serial_open && acknowledgements == 3);
    s_serial = s_serial_open = true;
    for (int i = 0; i < 16; i++) put(3);
    serial_pump();
    assert(inputs == 8 && count == 8);
    serial_pump();
    assert(inputs == 16 && !count);
    bool down = false;
    serial_msg_t m = { .len = 5, .b = { 1, 10, 0, 20, 0 } };
    assert(serial_enqueue(&m, &down) && down && count == 1);
    queue_stalled = true;
    m.b[1] = 122;
    assert(serial_enqueue(&m, &down) && down && count == 1 && waited == 0);
    m.b[0] = 0;
    assert(serial_enqueue(&m, &down) && !down && count == 2 && waited == SEND_MS);
    assert(queue[1].b[1] == 122 && queue[1].b[0] == 0);
    queue_stalled = true;
    m.len = 3; m.b[0] = 6; m.b[1] = 1; m.b[2] = 0;
    assert(serial_enqueue(&m, &down) && count == 3 && waited == SEND_MS);
    count = head = 0;
    send_ok = true;
    pump_on_send = true;
    for (size_t n = 1; n <= IN_MAX; n++) {
        memset(wire, 0, sizeof(wire));
        wire[0] = n; wire[1] = n >> 8;
        wire[4] = 'R';
        for (size_t i = 1; i < n; i++) wire[4 + i] = i;
        wire[4 + n] = 1;
        wire[8 + n] = 'R';
        wire_size = n + 9; wire_at = 0; payload_size = n;
        payload_reads = 0;
        int before = inputs;
        assert(muse_mirror_serial_session(0) == ESP_OK);
        assert(payload_reads == 1 && wire_at == wire_size && !count);
        assert(inputs == before + 1 + (n <= SERIAL_INPUT_MAX));
        assert(last_input == 1 && !s_session_busy && !s_serial_open && !s_serial);
    }
    /* The viewer's microphone goes to the ring from the console's task, never through the queue. */
    static const struct { size_t n; uint8_t format; int packed; } sounds[] = {
        { 166, 3, 1 }, { 165, 3, 0 }, { 168, 3, 0 }, { 166, 4, 0 }, { 166, 2, 0 }, { 642, 2, 1 }, { 3, 3, 0 },
    };
    for (size_t k = 0; k < sizeof(sounds) / sizeof(sounds[0]); k++) {
        size_t n = sounds[k].n;
        memset(wire, 0, sizeof(wire));
        wire[0] = n; wire[1] = n >> 8;
        wire[4] = 'S'; wire[5] = sounds[k].format;
        for (size_t i = 2; i < n; i++) wire[4 + i] = i;
        wire_size = n + 4; wire_at = 0; payload_size = n;
        payload_reads = 0; mic_packed = 0;
        int before = inputs, resets = audio_resets;
        assert(muse_mirror_serial_session(0) == ESP_OK);
        assert(payload_reads == 1 && wire_at == wire_size && !count && inputs == before);
        assert(mic_packed == sounds[k].packed);
        assert(audio_resets == resets + 1 && !s_serial);
    }
    memset(wire, 0, sizeof(wire));
    wire[0] = IN_MAX & 0xff; wire[1] = IN_MAX >> 8; wire[4] = 'R';
    wire[4 + IN_MAX] = 1; wire[8 + IN_MAX] = 'R';
    wire_size = IN_MAX + 9; payload_size = IN_MAX;
    /* A UART console runs faster for the session and returns to its own speed; USB and odd speeds stay. */
    static const struct { int baud; bool uart, fast; } speeds[] = {
        { 921600, true, true }, { 230400, true, true }, { 115200, true, false }, { 3000000, true, false },
        { 921600, false, false },
    };
    for (size_t k = 0; k < sizeof(speeds) / sizeof(speeds[0]); k++) {
        uart = speeds[k].uart;
        wire_at = 0; baud_sets = settled = offers = 0;
        assert(muse_mirror_serial_session(speeds[k].baud) == ESP_OK);
        assert(offers == 1 && settled == speeds[k].fast && baud_sets == 2 * speeds[k].fast);
        assert(!baud_now && !atomic_load(&s_baud));
    }
    uart = true;
    wire_at = 0; payload_timeout = true; payload_reads = 0;
    int before = inputs;
    assert(muse_mirror_serial_session(0) == ESP_OK);
    assert(payload_reads == 1 && inputs == before && !count && !s_session_busy);
    assert(wire_at == wire_size);
    return 0;
}
