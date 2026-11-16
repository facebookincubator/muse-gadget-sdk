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
#include "sdkconfig.h"
#include "muse_mirror_session.h"
#include "muse_mirror_link.h"
#include "muse_mirror_core.h"
#include "muse_mirror_audio.h"
#include "muse_console.h"
#include "muse_mem.h"
#include <stdlib.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include "esp_rom_crc.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "miniz.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "muse_mirror";

/* Cable framing is documented in protocols/MIRROR.md. */
#define MSG_MAX     MUSE_MIRROR_MSG_MAX /* at 115200 baud a whole one takes about 2.85 s of SEND_MS */
#define FRAME_MS    40
#define BEAT_MS     1000       /* an empty frame carries the screen state when nothing changes */
#define SEND_MS     3000       /* a viewer that takes longer for one message is dropped */
#define IN_MAX      1024       /* drain unsupported payloads before reading the next length */
#define IN_MS       2000       /* a message, the hello included, arrives whole within this */
#define TAP_MS      150        /* a key tapped from the viewer is held this long */
#define AUD_BURST   16         /* audio messages a pass at most: what built up behind a long screen message */
#define TASK_STACK  12288      /* the ROM's deflate keeps kilobytes of tables on the stack */
#define FAST_MS     150        /* the viewer changes its side's speed after the mark */
#define AUDIO_BAUD  460800     /* a UART console carries sound, as ADPCM, beside the screen from this speed */

static muse_mirror_config_t s_cfg;
static char s_name[48];        /* the board's name, JSON-safe */
static int s_w, s_h;
static atomic_bool s_ready;  /* the task runs once everything is published */

static uint16_t *s_fb;         /* the screen copy, under s_lock */
static uint16_t *s_snap;       /* what the screen copy held when an update began: all its messages come from it */
static bool s_drawing;         /* under s_lock: LVGL is between the first and last flush of a refresh */
static uint8_t *s_msg;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static atomic_uchar s_state;

static muse_mirror_core_t s_core;
enum { CAPTURE_OFF, CAPTURE_REQUEST, CAPTURE_REDRAW, CAPTURE_READY };
static int s_capture;
static unsigned s_generation;

static void capture_set(bool on) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_capture = on ? CAPTURE_REQUEST : CAPTURE_OFF;
    s_generation++;
    xSemaphoreGive(s_lock);
}

bool muse_mirror_capture_begin(unsigned *generation) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool pending = s_capture == CAPTURE_REQUEST;
    *generation = s_generation;
    if (pending) s_capture = CAPTURE_REDRAW;
    xSemaphoreGive(s_lock);
    return pending;
}

void muse_mirror_capture_ready(unsigned generation) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_generation == generation && s_capture == CAPTURE_REDRAW) s_capture = CAPTURE_READY;
    xSemaphoreGive(s_lock);
}
/* Buffers and queues live in PSRAM: a board's own setup tasks need contiguous internal RAM. */
#define BOARD_MAX 2048
#define LOG_LINE 512
#define AUD_MSG (2 + MUSE_MIRROR_AUDIO_CHUNK)
#define AUD_PACKED (2 + MUSE_MIRROR_ADPCM_CHUNK)
static char *s_board, *s_log_line;
static uint8_t *s_aud_msg, *s_aud_packed;
static size_t s_board_len;
static atomic_bool s_serial;
static atomic_bool s_serial_open, s_session_busy;
#define SERIAL_QUIET_MS 6000
#define SERIAL_HELLO 0xffff
#define SERIAL_BYE 0xfffe
#define LINK_HELLO 0xfffd      /* the link rides in b[] */
#define LINK_BYE 0xfffc
#define LOG_WAIT_MS 50
#define SERIAL_QUEUE 32
static const uint8_t s_mark[] = { 0, 'M', 'I', 'R', 'R', 'O', 'R', '1', '\n' };
typedef struct { uint16_t len; uint8_t b[IN_MAX]; } serial_msg_t;
static serial_msg_t *s_serial_msg;   /* the console session's, in PSRAM */
#define SERIAL_INPUT_MAX 64
/* Drain larger messages without allocating their unsupported payloads in the queue. */
#define SERIAL_ITEM_SIZE (offsetof(serial_msg_t, b) + SERIAL_INPUT_MAX)
static QueueHandle_t s_serial_in;
static SemaphoreHandle_t s_serial_tx, s_serial_done;

#define PTR_Q 32
typedef struct { uint16_t x, y; bool down, edge; } ptr_t;
static portMUX_TYPE s_ptr_mux = portMUX_INITIALIZER_UNLOCKED;
static ptr_t s_ptr_q[PTR_Q], s_ptr;
static unsigned s_ptr_head, s_ptr_n;
static int s_ptr_x, s_ptr_y;

static int s_tap_key;          /* a key tapped from the viewer, let go at s_tap_until */
static int64_t s_tap_until;
static unsigned s_held;        /* keys the viewer holds down, a bit per code */
#define TEXT_Q 32
static QueueHandle_t s_text_in;      /* the viewer's typing, for the UI task */
static atomic_bool s_text_field;   /* a text field is open, as the UI task last said */
static bool s_text_viewer;           /* mirror task: this viewer types (it sent 'O') */
static int s_text_sent = -1;         /* mirror task: what this viewer was last told */
static bool s_audio_rings;           /* the board has sound and its rings exist */
static bool s_audio;                 /* mirror task: this session's cable carries sound */
static char *s_audio_flag;           /* "audio" in s_board, five characters */
static atomic_int s_baud;            /* the speed a UART console was raised to for this session */
static bool s_zip_viewer;            /* mirror task: this viewer inflates 'Z' messages */
static muse_mirror_adpcm_t s_adpcm;  /* mirror task: carried from chunk to chunk on a UART */
static const muse_mirror_link_t *_Atomic s_link;        /* mirror task: the viewer's link; NULL on the cable */
static const muse_mirror_link_t *_Atomic s_link_owner;  /* the link that has the viewer or asked for it */
static bool s_link_down;             /* the link's task: its viewer's finger is down */

static bool viewer_send(const void *buf, size_t len, int ms);
static void serial_end(void);
static void viewer_drop(void);

bool muse_mirror_connected(void) {
    return s_serial || s_link;
}

/* Mirror task: what the audio path gathered for a listening viewer. */
static void audio_pump(void) {
    for (int i = 0; i < AUD_BURST && muse_mirror_connected(); i++) {
        if (!muse_mirror_audio_take(s_aud_msg + 2, &s_aud_msg[1])) return;
        bool sent;
        if (s_cfg.uart && !s_link) {
            s_aud_packed[0] = 'a';
            s_aud_packed[1] = s_aud_msg[1];
            muse_mirror_adpcm(&s_adpcm, s_aud_msg + 2, s_aud_packed + 2);
            sent = viewer_send(s_aud_packed, AUD_PACKED, SEND_MS);
        } else {
            s_aud_msg[0] = 'A';
            sent = viewer_send(s_aud_msg, AUD_MSG, SEND_MS);
        }
        if (!sent) viewer_drop();
    }
}

/* The speed a UART session runs at: the one the viewer asked for, else the console's own. */
static size_t uart_baud(void) {
    size_t baud = (size_t)atomic_load(&s_baud);
#ifdef CONFIG_ESP_CONSOLE_UART_BAUDRATE
    if (!baud) baud = CONFIG_ESP_CONSOLE_UART_BAUDRATE;
#endif
    return baud;
}

/* Mirror task: a new viewer has asked for nothing yet, and is told whether its cable carries sound. */
static void viewer_options(void) {
    s_zip_viewer = false;
    s_adpcm = (muse_mirror_adpcm_t){ 0 };
    s_audio = s_audio_rings && (s_link ? s_link->audio : !s_cfg.uart || uart_baud() >= AUDIO_BAUD);
    if (s_audio_flag) memcpy(s_audio_flag, s_audio ? "true " : "false", 5);
}

/* Mirror task: what the viewer can take, what it listens to and where the sound goes. */
static bool option_input(const uint8_t *buf, size_t len) {
    if (len == 1 && buf[0] == 'Z') {
        s_zip_viewer = true;
        return true;
    }
    if (len != 2 || (buf[0] != 7 && buf[0] != 9 && buf[0] != 10)) return false;
    if (!s_audio) return true;
    if (buf[0] == 7) muse_mirror_audio_listen(buf[1]);
    else if (buf[0] == 9) muse_mirror_audio_speaker(buf[1] == 1);
    else muse_mirror_audio_mic(buf[1]);
    return true;
}

void muse_mirror_text_open(bool open) {
    atomic_store(&s_text_field, open);
}

bool muse_mirror_text_read(muse_mirror_text_t *out) {
    memset(out, 0, sizeof(*out));
    return s_text_in && xQueueReceive(s_text_in, out, 0) == pdTRUE;
}

/* Mirror task: a new viewer, or none, starts without typing pending. */
static void text_reset(void) {
    s_text_viewer = false;
    s_text_sent = -1;
    if (s_text_in) xQueueReset(s_text_in);
}

/* Mirror task: whether a field is open, to a viewer that types, when it changed. */
static void text_pump(void) {
    if (!muse_mirror_connected() || !s_text_viewer) return;
    bool open = atomic_load(&s_text_field);
    if (open == s_text_sent) return;
    uint8_t msg[2] = {'O', open};
    if (viewer_send(msg, 2, SEND_MS)) {
        s_text_sent = open;
    } else {
        viewer_drop();
    }
}

static void *alloc(size_t size, bool zero) {
    return zero ? heap_caps_calloc(1, size, MUSE_BIG_CAPS) : heap_caps_malloc(size, MUSE_BIG_CAPS);
}

static void dirty_add(int x1, int y1, int x2, int y2) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    muse_mirror_core_dirty(&s_core, x1, y1, x2, y2);
    xSemaphoreGive(s_lock);
}

static void pointer_put(int x, int y, bool down) {
    portENTER_CRITICAL(&s_ptr_mux);
    bool was = s_ptr_n ? s_ptr_q[(s_ptr_head + s_ptr_n - 1) % PTR_Q].down : s_ptr.down;
    ptr_t value = { x, y, down, down != was };
    unsigned last = (s_ptr_head + s_ptr_n - 1) % PTR_Q;
    if (s_ptr_n && !value.edge && !s_ptr_q[last].edge) {
        s_ptr_q[last] = value;
    } else {
        if (s_ptr_n == PTR_Q) {
            /* Overflow cancels the gesture before accepting a new press. */
            s_ptr_head = 0;
            s_ptr_n = 1;
            s_ptr_q[0] = (ptr_t){ x, y, false, true };
        }
        s_ptr_q[(s_ptr_head + s_ptr_n++) % PTR_Q] = value;
    }
    portEXIT_CRITICAL(&s_ptr_mux);
    s_ptr_x = x; s_ptr_y = y;
}

bool muse_mirror_pointer_read(int *x, int *y, bool *down) {
    portENTER_CRITICAL(&s_ptr_mux);
    if (s_ptr_n) {
        s_ptr = s_ptr_q[s_ptr_head];
        s_ptr_head = (s_ptr_head + 1) % PTR_Q;
        s_ptr_n--;
    }
    *x = s_ptr.x; *y = s_ptr.y; *down = s_ptr.down;
    bool more = s_ptr_n > 0;
    portEXIT_CRITICAL(&s_ptr_mux);
    return more;
}

uint16_t *muse_mirror_frame_begin(void) {
    uint16_t *fb = __atomic_load_n(&s_fb, __ATOMIC_ACQUIRE);
    if (!fb) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_capture == CAPTURE_OFF) {
        xSemaphoreGive(s_lock);
        return NULL;
    }
    return fb;
}

/* The rectangle clipped to the screen, as x1, y1, x2, y2; false when nothing is left. */
static bool clip(int x, int y, int width, int height, int *x1, int *y1, int *x2, int *y2) {
    if (width <= 0 || height <= 0 || x >= s_w || y >= s_h) return false;
    int64_t xe = (int64_t)x + width - 1, ye = (int64_t)y + height - 1;
    *x1 = x < 0 ? 0 : x;
    *y1 = y < 0 ? 0 : y;
    *x2 = xe >= s_w ? s_w - 1 : (int)xe;
    *y2 = ye >= s_h ? s_h - 1 : (int)ye;
    return *x2 >= *x1 && *y2 >= *y1;
}

void muse_mirror_frame_end(int x, int y, int width, int height, bool last) {
    if (!__atomic_load_n(&s_fb, __ATOMIC_ACQUIRE)) return;
    int x1, y1, x2, y2;
    if (clip(x, y, width, height, &x1, &y1, &x2, &y2)) muse_mirror_core_dirty(&s_core, x1, y1, x2, y2);
    s_drawing = !last;
    xSemaphoreGive(s_lock);
}

void muse_mirror_set_screen_state(uint8_t state) {
    state = state > 2 ? 2 : state;
    if (atomic_exchange(&s_state, state) == state) return;
    if (s_w) dirty_add(0, 0, 0, 0);       /* a frame goes out with it; one pixel's worth of rectangle */
}

/* On a UART a message takes wire time: with sound at most 250 ms of it, inside the viewer's 300 ms cushion;
 * without, 2 s, inside SEND_MS. A raw row always fits (start() checks it against MSG_MAX). */
static size_t msg_cap(void) {
    if (!s_cfg.uart || s_link) return MSG_MAX;
    size_t row = (size_t)s_w * 2 + MUSE_MIRROR_RECT_HDR + 8, wire = uart_baud() / 10 * (s_audio ? 1 : 8) / 4;
    return wire < row ? row : wire < MSG_MAX ? wire : MSG_MAX;
}

/* A new update starts from a snapshot taken between LVGL's refreshes where it can (20 ms at most), so the
 * messages of one update show one moment however long they take; 'F' state bit 7 says more follows. */
static size_t build_msg(void) {
    for (int tries = 0;; tries++) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_capture != CAPTURE_READY || s_core.tail_on || !s_drawing || tries >= 10) break;
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    size_t n = 0;
    if (s_capture == CAPTURE_READY) {
        for (int i = 0; !s_core.tail_on && i < s_core.ndirty; i++) {
            const muse_mirror_rect_t *r = &s_core.dirty[i];
            for (int y = r->y1; y <= r->y2; y++) {
                memcpy(s_snap + y * s_w + r->x1, s_fb + y * s_w + r->x1, (r->x2 - r->x1 + 1) * sizeof(uint16_t));
            }
        }
        n = muse_mirror_core_build(&s_core, s_snap, s_msg, msg_cap(), s_state);
        if (n && s_core.tail_on) s_msg[1] |= 0x80;
    }
    xSemaphoreGive(s_lock);
    return n;
}

/* A key the viewer holds (bit in s_held) or taps (s_tap_key until
 * s_tap_until); the board sees it down while either says so. */
static void key_set(int code, bool down) {
    if (!s_cfg.on_key || code < 1 || code > 10) return;
    bool was = (s_held & 1u << code) || (s_tap_until && s_tap_key == code);
    if (down) s_held |= 1u << code;
    else s_held &= ~(1u << code);
    bool now = (s_held & 1u << code) || (s_tap_until && s_tap_key == code);
    if (now != was) s_cfg.on_key(code, now);
}

static void tap_end(void) {
    if (!s_tap_until) return;
    int code = s_tap_key;
    s_tap_until = 0;
    if (s_cfg.on_key && !(s_held & 1u << code)) s_cfg.on_key(code, false);
}

/* Every key the viewer had down is let go: on its way out, nothing may stay pressed. */
static void keys_release(void) {
    tap_end();
    for (int code = 1; code <= 10; code++) {
        if (s_held & 1u << code) key_set(code, false);
    }
}

/* A message from the viewer. */
static void input(const uint8_t *buf, size_t len) {
    if (len == 1 && buf[0] == 'O') {   /* this viewer types, and hears when a field opens */
        s_text_viewer = true;
        s_text_sent = -1;
        return;
    }
    muse_mirror_text_t text;
    if (muse_mirror_core_text(buf, len, &text.edit, text.text)) {
        /* With no field open the typing has nowhere to go; a full queue means the UI task stalled. */
        bool ok = !atomic_load(&s_text_field)
                  || xQueueSend(s_text_in, &text, pdMS_TO_TICKS(SEND_MS)) == pdTRUE;
        memset(&text, 0, sizeof(text));
        if (!ok) viewer_drop();
        return;
    }
    if (len == 1 && buf[0] == 'R') {
        dirty_add(0, 0, s_w - 1, s_h - 1);
        if (!viewer_send(s_board, s_board_len, SEND_MS)) viewer_drop();
        return;
    }
    if (option_input(buf, len)) return;
    if (len == 2 && buf[0] == 3 && buf[1] >= 1 && buf[1] <= 10) {
        int code = buf[1];
        tap_end();
        bool was = (s_held & 1u << code) != 0;
        s_tap_key = code;
        s_tap_until = esp_timer_get_time() + TAP_MS * 1000LL;
        if (s_cfg.on_key && !was) s_cfg.on_key(code, true);
    } else if (len == 3 && buf[0] == 6 && buf[2] <= 1) {
        key_set(buf[1], buf[2] != 0);
    } else if (len == 5 && buf[0] <= 1 && s_cfg.touch) {
        int x = buf[1] | buf[2] << 8, y = buf[3] | buf[4] << 8;
        if (x < s_w && y < s_h) pointer_put(x, y, buf[0] != 0);
    }
}


#define SERIAL_CHUNK 128

static bool serial_write(const void *buf, size_t len, int64_t until)
{
    for (const uint8_t *p = buf; len;) {
        int64_t left = until - esp_timer_get_time();
        if (left <= 0) return false;
        size_t n = len < SERIAL_CHUNK ? len : SERIAL_CHUNK;
        int ms = left / 1000;
        int written = muse_console_write_bytes(p, n, ms < 200 ? ms : 200);
        if (written <= 0) continue;
        p += written;
        len -= written;
    }
    return true;
}

static bool serial_send(const void *buf, size_t len, int ms)
{
    uint8_t hdr[6] = { 0xA5, 0x5A, len, len >> 8, len >> 16, len >> 24 };
    uint32_t crc = esp_rom_crc32_le(0, hdr + 2, 4);
    crc = esp_rom_crc32_le(crc, buf, len);
    uint8_t tail[4] = { crc, crc >> 8, crc >> 16, crc >> 24 };
    int64_t until = esp_timer_get_time() + ms * 1000LL;
    if (xSemaphoreTake(s_serial_tx, pdMS_TO_TICKS(ms)) != pdTRUE) return false;
    bool ok = serial_write(hdr, sizeof(hdr), until) && serial_write(buf, len, until)
              && serial_write(tail, sizeof(tail), until);
    xSemaphoreGive(s_serial_tx);
    return ok;
}

static bool viewer_send(const void *buf, size_t len, int ms)
{
    if (s_link) return s_link->send(buf, len, ms);
    return s_serial && serial_send(buf, len, ms);
}

/* Nothing a leaving viewer held stays held, whatever carried it. */
static void viewer_release(void)
{
    text_reset();
    muse_mirror_audio_reset();
    keys_release();
    pointer_put(s_ptr_x, s_ptr_y, false);
    capture_set(false);
}

static void serial_end(void)
{
    if (!s_serial) return;
    viewer_release();
    s_serial = false;
    s_serial_open = false;             /* the console's session stops within a second */
}

/* With s_serial or s_link set: the board, then the whole screen. */
static bool viewer_admit(void)
{
    text_reset();
    viewer_options();
    capture_set(true);
    if (!viewer_send(s_board, s_board_len, SEND_MS)) return false;
    dirty_add(0, 0, s_w - 1, s_h - 1);
    return true;
}

/* The link's viewer is gone. A drop also frees the link to ask again. */
static void link_end(bool dropped)
{
    const muse_mirror_link_t *link = s_link;
    if (!link) return;
    viewer_release();
    s_link = NULL;
    if (dropped) atomic_compare_exchange_strong(&s_link_owner, &link, NULL);
}

static void serial_admit(void)
{
    if (!s_serial_open) return;
    link_end(false);                   /* a link whose BYE found the queue full */
    s_serial = true;
    if (!viewer_admit()) serial_end();
}

static void link_admit(const muse_mirror_link_t *link)
{
    if (atomic_load(&s_link_owner) != link) return;   /* it left, or the session dropped it, since */
    if (s_link) viewer_release();                     /* a new viewer on the same link */
    s_link = link;
    if (!viewer_admit()) link_end(true);
}

static void serial_pump(void)
{
    serial_msg_t m;
    for (int i = 0; i < 8 && s_serial_in && xQueueReceive(s_serial_in, &m, 0) == pdTRUE; i++) {
        const muse_mirror_link_t *link = NULL;
        if (m.len == LINK_HELLO || m.len == LINK_BYE) memcpy(&link, m.b, sizeof(link));
        if (m.len == SERIAL_HELLO) {
            serial_admit();
        } else if (m.len == SERIAL_BYE) {
            if (s_serial) viewer_send("E", 1, 500);   /* the viewer stops sending and asks again */
            serial_end();
            xSemaphoreGive(s_serial_done);
        } else if (m.len == LINK_HELLO) {
            link_admit(link);
        } else if (m.len == LINK_BYE) {
            if (s_link == link) link_end(false);
        } else if ((s_link || (s_serial && s_serial_open)) && m.len <= SERIAL_INPUT_MAX) {
            input(m.b, m.len);
        }
    }
}

static atomic_uint s_log_dropped;

static int serial_log(const char *fmt, va_list ap)
{
    if (xSemaphoreTake(s_serial_tx, pdMS_TO_TICKS(LOG_WAIT_MS)) != pdTRUE) {
        atomic_fetch_add(&s_log_dropped, 1);
        return 0;
    }
    int64_t until = esp_timer_get_time() + LOG_WAIT_MS * 1000LL;
    unsigned dropped = atomic_exchange(&s_log_dropped, 0);
    if (dropped) {
        int n = snprintf(s_log_line, LOG_LINE, "\nmirror: %u log records dropped (TX busy)\n", dropped);
        if (!serial_write(s_log_line, n, until)) atomic_fetch_add(&s_log_dropped, dropped);
    }
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(s_log_line, LOG_LINE, fmt, copy);
    va_end(copy);
    char *line = s_log_line;
    bool ok = n <= 0;
    if (n > 0) {
        if ((size_t)n >= LOG_LINE) {
            line = malloc((size_t)n + 1);
            if (line) vsnprintf(line, (size_t)n + 1, fmt, ap);
        }
        ok = line && serial_write(line, n, until);
        if (!ok) atomic_fetch_add(&s_log_dropped, 1);
    }
    if (line != s_log_line) free(line);
    xSemaphoreGive(s_serial_tx);
    return ok ? n : 0;
}

static bool serial_enqueue(const serial_msg_t *m, bool *pointer_down, int wait)
{
    bool pointer = m->len == 5 && m->b[0] <= 1;
    bool move = pointer && m->b[0] && *pointer_down;
    if (xQueueSend(s_serial_in, m, move ? 0 : wait) != pdTRUE) return move;
    if (pointer) *pointer_down = m->b[0] != 0;
    xTaskNotifyGive(s_task);
    return true;
}

/* The viewer's microphone, 'S': too long for the queue, and the ring takes it from any task. */
static bool mic_input(const uint8_t *b, size_t n)
{
    if (n <= 2 || b[0] != 'S') return false;
    if (b[1] == 3 && n == 2 + MUSE_MIRROR_ADPCM_CHUNK) muse_mirror_audio_mic_adpcm(b + 2);
    else if (b[1] == 2 && n == 2 + MUSE_MIRROR_AUDIO_CHUNK) muse_mirror_audio_mic_put(b + 2, MUSE_MIRROR_AUDIO_CHUNK / 2);
    return true;
}

static bool fast_baud(int baud)
{
    return baud == 230400 || baud == 460800 || baud == 921600;
}

esp_err_t muse_mirror_serial_session(int baud)
{
    if (!s_ready || !s_serial_in) {
        ESP_LOGW(TAG, "mirror unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_exchange(&s_session_busy, true)) return ESP_ERR_INVALID_STATE;
    if (atomic_load(&s_link_owner)) {
        s_session_busy = false;
        ESP_LOGW(TAG, "mirror busy: another viewer has it");
        return ESP_ERR_INVALID_STATE;
    }
    vprintf_like_t log = esp_log_set_vprintf(serial_log);
    vTaskDelay(pdMS_TO_TICKS(50));     /* a log line already on its way finishes first */
    serial_msg_t *m = s_serial_msg;
    m->len = SERIAL_HELLO;
    s_serial_open = true;
    xSemaphoreTake(s_serial_tx, portMAX_DELAY);
    bool marked = serial_write(s_mark, sizeof(s_mark), esp_timer_get_time() + SEND_MS * 1000LL);
    bool fast = marked && fast_baud(baud) && muse_console_set_baud(baud);
    atomic_store(&s_baud, fast ? baud : 0);
    if (fast) vTaskDelay(pdMS_TO_TICKS(FAST_MS));   /* nothing, a log line included, while the viewer switches */
    xSemaphoreGive(s_serial_tx);
    if (marked && xQueueSend(s_serial_in, m, pdMS_TO_TICKS(500)) == pdTRUE) {
        xTaskNotifyGive(s_task);
        int64_t heard = esp_timer_get_time();
        bool pointer_down = false;
        while (s_serial_open) {
            uint8_t hdr[4];
            if (!muse_console_read(hdr, 1, 1000)) {
                if (esp_timer_get_time() - heard > SERIAL_QUIET_MS * 1000LL) break;
                continue;
            }
            if (!s_serial_open || !muse_console_read(hdr + 1, 3, IN_MS)) break;
            size_t n = hdr[0] | hdr[1] << 8;
            if (hdr[2] || hdr[3] || n > IN_MAX) break;   /* not a viewer's message: the console is the console again */
            if (n && !muse_console_read(m->b, n, IN_MS)) break;
            m->len = n;
            heard = esp_timer_get_time();
            if (mic_input(m->b, n)) continue;
            if (!serial_enqueue(m, &pointer_down, pdMS_TO_TICKS(SEND_MS))) break;
        }
    }
    s_serial_open = false;
    m->len = SERIAL_BYE;
    xQueueSend(s_serial_in, m, portMAX_DELAY);
    xTaskNotifyGive(s_task);
    /* No console reuse or hook restoration until the mirror task has consumed the final BYE. */
    xSemaphoreTake(s_serial_done, portMAX_DELAY);
    if (fast) {
        xSemaphoreTake(s_serial_tx, portMAX_DELAY);    /* a log line goes out whole at one speed */
        muse_console_set_baud(0);
        xSemaphoreGive(s_serial_tx);
        atomic_store(&s_baud, 0);
    }
    /* What the viewer sent meanwhile is no console command: a finger's x of 122 is 'z', sleep. */
    uint8_t c;
    for (int64_t until = esp_timer_get_time() + 2000000; esp_timer_get_time() < until && muse_console_read(&c, 1, 150); ) {
    }
    esp_log_set_vprintf(log);
    s_session_busy = false;
    return ESP_OK;
}

static void viewer_drop(void) {
    if (s_link) link_end(true);
    else serial_end();
}

static serial_msg_t *s_link_msg;   /* PSRAM, from the first link: its RPC or socket task's stack stays small */

bool muse_mirror_link_open(const muse_mirror_link_t *link)
{
    if (!link || !link->send || !s_ready || !s_serial_in) return false;
    const muse_mirror_link_t *was = NULL, *mine = link;
    if (!atomic_compare_exchange_strong(&s_link_owner, &was, link) && was != link) return false;
    if (!s_link_msg) s_link_msg = alloc(sizeof(*s_link_msg), false);
    if (atomic_load(&s_session_busy) || !s_link_msg) {   /* the cable has the viewer, or no memory */
        if (!was) atomic_compare_exchange_strong(&s_link_owner, &mine, NULL);
        return false;
    }
    s_link_down = false;
    s_link_msg->len = LINK_HELLO;
    memcpy(s_link_msg->b, &link, sizeof(link));
    if (xQueueSend(s_serial_in, s_link_msg, 0) != pdTRUE) {
        if (!was) atomic_compare_exchange_strong(&s_link_owner, &mine, NULL);
        return false;
    }
    xTaskNotifyGive(s_task);
    return true;
}

bool muse_mirror_link_input(const muse_mirror_link_t *link, const uint8_t *msg, size_t len)
{
    if (!link || atomic_load(&s_link_owner) != link || !msg || !len) return false;
    if (mic_input(msg, len)) return true;
    if (len > SERIAL_INPUT_MAX) return false;
    s_link_msg->len = len;
    memcpy(s_link_msg->b, msg, len);
    /* Never waits: the mirror task's send may be waiting on this task to make room. */
    return serial_enqueue(s_link_msg, &s_link_down, 0);
}

void muse_mirror_link_close(const muse_mirror_link_t *link)
{
    const muse_mirror_link_t *mine = link;
    if (!link || !atomic_compare_exchange_strong(&s_link_owner, &mine, NULL)) return;
    s_link_msg->len = LINK_BYE;
    memcpy(s_link_msg->b, &link, sizeof(link));
    /* A BYE that finds no room is not needed: the link's next send fails and drops the viewer. */
    if (xQueueSend(s_serial_in, s_link_msg, pdMS_TO_TICKS(500)) == pdTRUE) xTaskNotifyGive(s_task);
}

const char *muse_mirror_link_board(size_t *len)
{
    if (!s_ready) return NULL;
    if (len) *len = s_board_len;
    return s_board;
}

#define ZIP_MIN   256                              /* shorter messages go as they are */
#define ZIP_FLAGS (TDEFL_GREEDY_PARSING_FLAG | 4)  /* raw deflate, four probes a match */
static tdefl_compressor *s_zip;                    /* with s_zmsg, from the first viewer that asks */
static uint8_t *s_zmsg;

/* Mirror task: s_msg deflated behind a 'Z' in s_zmsg, when that is shorter; else 0. */
static size_t zip(size_t len) {
    if (!s_zip_viewer || len < ZIP_MIN) return 0;
    if (!s_zip) {
        tdefl_compressor *state = alloc(sizeof(*state), false);
        uint8_t *out = alloc(MSG_MAX, false);
        if (!state || !out) {
            heap_caps_free(state);
            heap_caps_free(out);
            s_zip_viewer = false;
            return 0;
        }
        s_zmsg = out;
        s_zip = state;
    }
    size_t in = len, out = len - 2;
    if (tdefl_init(s_zip, NULL, NULL, ZIP_FLAGS) != TDEFL_STATUS_OKAY
        || tdefl_compress(s_zip, s_msg, &in, s_zmsg + 1, &out, TDEFL_FINISH) != TDEFL_STATUS_DONE) {
        return 0;
    }
    s_zmsg[0] = 'Z';
    return out + 1;
}

static void mirror_task(void *arg) {
    (void)arg;
    int64_t last_beat = 0, last_frame = 0;
    bool more = false;    /* the rest of an update waits, on USB: it goes at once, not a frame later */
    while (!s_ready) vTaskDelay(1);
    for (;;) {
        serial_pump();
        bool queued = s_serial_in && uxQueueMessagesWaiting(s_serial_in);   /* the pump takes 8 a pass */
        ulTaskNotifyTake(pdTRUE, more || queued ? 0 : muse_mirror_connected() ? pdMS_TO_TICKS(10) : portMAX_DELAY);
        serial_pump();
        if (s_tap_until && esp_timer_get_time() > s_tap_until) tap_end();
        text_pump();
        audio_pump();
        bool hurry = more;
        more = false;
        if (!muse_mirror_connected()) continue;
        int64_t now = esp_timer_get_time() / 1000;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool dirty = muse_mirror_core_pending(&s_core);
        xSemaphoreGive(s_lock);
        if ((!hurry && now - last_frame < FRAME_MS) || (!dirty && now - last_beat < BEAT_MS)) continue;
        last_beat = last_frame = now;
        size_t len = build_msg(), packed = zip(len);
        if (len && !viewer_send(packed ? s_zmsg : s_msg, packed ? packed : len, SEND_MS)) {
            viewer_drop();
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        more = !s_cfg.uart && s_core.tail_on;
        xSemaphoreGive(s_lock);
    }
}

/* text as the inside of a JSON string: a bounded copy of whole UTF-8 characters, quotes and backslashes
 * escaped, control characters and broken sequences out. */
static void json_text(char *out, size_t size, const char *text) {
    size_t at = 0;
    for (const char *p = text ? text : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || (c >= 0x80 && c < 0xC2) || c > 0xF4) continue;
        size_t need = c == '"' || c == '\\' ? 2 : c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;   /* whole characters */
        if (c >= 0x80) {
            size_t k = 1;
            while (k < need && ((unsigned char)p[k] & 0xC0) == 0x80) k++;
            if (k < need) continue;       /* a broken sequence: its lead byte goes, the rest is read afresh */
        }
        if (at + need >= size) break;
        if (need == 2 && c < 0x80) out[at++] = '\\';
        out[at++] = (char)c;
        for (size_t k = 1; k < need && c >= 0x80; k++) out[at++] = *++p;
    }
    out[at] = 0;
}

esp_err_t muse_mirror_session_start(const muse_mirror_config_t *config) {
    if ((size_t)config->width * 2 + MUSE_MIRROR_RECT_HDR + 8 > MSG_MAX) {
        ESP_LOGE(TAG, "a raw row of %d pixels does not fit a %d KB message", config->width, MSG_MAX / 1024);
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *config;
    s_cfg.buttons = NULL;  /* Metadata is serialized below; callers may use a temporary array. */
    json_text(s_name, sizeof(s_name), config->name);
    int w = config->width, h = config->height;
    char *big = alloc(BOARD_MAX + LOG_LINE + AUD_MSG + AUD_PACKED, false);
    if (!big) return ESP_ERR_NO_MEM;
    s_board = big;
    s_log_line = big + BOARD_MAX;
    s_aud_msg = (uint8_t *)s_log_line + LOG_LINE;
    s_aud_packed = s_aud_msg + AUD_MSG;
    s_audio_rings = config->audio && muse_mirror_audio_init();
    s_board_len = snprintf(s_board, BOARD_MAX,
        "B{\"name\":\"%s\",\"w\":%d,\"h\":%d,\"round\":%s,\"diag\":%.3f,\"touch\":%s,\"keyboard\":%s,"
        "\"audio\":false,\"buttons\":[",
        s_name, w, h, config->round ? "true" : "false",
        (double)config->diagonal, config->touch ? "true" : "false",
        config->keyboard ? "true" : "false");
    s_audio_flag = strstr(s_board, "\"audio\":false") + 8;
    for (size_t i = 0; i < config->button_count; i++) {
        int role = config->buttons[i].role;
        int x = config->buttons[i].x;
        int y = config->buttons[i].y;
        char label[96];
        json_text(label, sizeof(label), config->buttons[i].name);
        int added = snprintf(s_board + s_board_len, BOARD_MAX - s_board_len,
            "%s{\"role\":%d,\"name\":\"%s\",\"x\":%d,\"y\":%d}", i ? "," : "", role, label, x, y);
        if (added < 0 || (size_t)added >= BOARD_MAX - s_board_len) return ESP_ERR_INVALID_ARG;
        s_board_len += added;
    }
    if (s_board_len + 3 > BOARD_MAX) return ESP_ERR_INVALID_ARG;
    memcpy(s_board + s_board_len, "]}", 3); s_board_len += 2;
    if (!muse_mirror_core_init(&s_core, w, h)) return ESP_ERR_INVALID_ARG;
    s_text_in = xQueueCreateWithCaps(TEXT_Q, sizeof(muse_mirror_text_t), MUSE_BIG_CAPS);
    s_serial_in = xQueueCreateWithCaps(SERIAL_QUEUE, SERIAL_ITEM_SIZE, MUSE_BIG_CAPS);
    s_serial_tx = xSemaphoreCreateMutex();
    s_serial_done = xSemaphoreCreateBinary();
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    uint16_t *fb = alloc((size_t)w * h * sizeof(uint16_t), true);
    uint16_t *snap = alloc((size_t)w * h * sizeof(uint16_t), false);
    uint8_t *msg = alloc(MSG_MAX, false);
    s_serial_msg = alloc(sizeof(*s_serial_msg), false);
    esp_err_t err = ESP_OK;
    const char *fail = NULL;
    if (!lock || !fb || !snap || !msg || !s_serial_msg || !s_serial_in || !s_serial_tx || !s_serial_done || !s_text_in) {
        fail = "no memory";
        err = ESP_ERR_NO_MEM;
    }
    if (!fail) {
        s_lock = lock;
        s_msg = msg;
        s_snap = snap;
        /* In PSRAM: the ROM's deflate keeps kilobytes of tables on the stack, and the task writes no flash. */
        BaseType_t ok = xTaskCreateWithCaps(mirror_task, "muse_mirror", TASK_STACK, NULL, 3, &s_task, MUSE_BIG_CAPS);
        if (ok != pdPASS) {
            fail = "no task";
            err = ESP_ERR_NO_MEM;
            s_task = NULL;
            s_msg = NULL;
            s_lock = NULL;
            s_snap = NULL;
        }
    }
    if (fail) {
        ESP_LOGE(TAG, "%s: no live screen", fail);
        heap_caps_free(fb);
        heap_caps_free(snap);
        heap_caps_free(msg);
        heap_caps_free(s_serial_msg);
        s_serial_msg = NULL;
        if (lock) vSemaphoreDelete(lock);
        if (s_serial_in) vQueueDeleteWithCaps(s_serial_in);
        if (s_serial_tx) vSemaphoreDelete(s_serial_tx);
        if (s_serial_done) vSemaphoreDelete(s_serial_done);
        if (s_text_in) vQueueDeleteWithCaps(s_text_in);
        s_serial_in = NULL; s_serial_tx = NULL; s_serial_done = NULL; s_text_in = NULL;
        return err;
    }
    s_w = w;                                          /* set only here: the screen calls check s_w, then lock */
    s_h = h;
    __atomic_store_n(&s_fb, fb, __ATOMIC_RELEASE);   /* the drawing side may call in from now on */
    atomic_store(&s_ready, true);   /* and the task may serve */
    ESP_LOGI(TAG, "%s, %d x %d, console cable", s_name, s_w, s_h);
    return ESP_OK;
}
