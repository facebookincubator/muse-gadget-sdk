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

/* Waveshare ESP32-S3-ePaper-1.54G voice: one ES8311 codec with an analog
 * microphone and an NS4150B speaker amplifier, and the green LED on GPIO3 for
 * the voice states (the e-paper is too slow for them). The reply's first page
 * goes on the e-paper with the page count, in GNU Unifont's 8x16 letters, and
 * a PWR tap shows the next. PWR taps and holds beep.
 * Pins: Waveshare's schematic and Example/ESP-IDF_5.5.1/07_Audio_Test
 * (github.com/waveshareteam/ESP32-S3-ePaper-1.54G).
 */
#include "voice_board.h"
#include "led_status.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "link_pairing.h"
#include "muse_chat.h"
#include "muse_text.h"
#include "fonts/epaper_154g_caption_font.h"
#include "pixel_font.h"

#define AUDIO_POWER 42  // active low
#define AUDIO_PA    46  // active high; controlled here, not by the codec
#define I2C_SDA     47
#define I2C_SCL     48
#define VOICE_LED   3   // active low
#define AUDIO_CHUNK 320
// Above the network tasks, so a TLS handshake cannot starve the microphone.
#define CAPTURE_PRIORITY 6
// voice.c drops presses shorter than its CAPTURE_MIN_MS (300 ms): no end cue for those.
#define END_CUE_FRAMES   (VOICE_MIC_RATE * 300 / 1000)
#define ERROR_BLINK_MS   1200

static const char *TAG = "link.voice_board";
static SemaphoreHandle_t s_lock;
static i2s_chan_handle_t s_tx, s_rx;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_data_if_t *s_data;
static const audio_codec_if_t *s_codec;
static esp_codec_dev_handle_t s_dev;
static bool s_recording, s_playing, s_cue;
static UBaseType_t s_priority;
static size_t s_frames; // read since voice_board_mic_start(), under s_lock
static int s_volume = 60;
// Every use and conversion is under s_lock, including microphone unpacking.
static int16_t s_stereo[AUDIO_CHUNK * 2];

// ---- Codec and amplifier ----------------------------------------------------

static void power_down(void) {
    gpio_set_level(AUDIO_PA, 0);
    if (s_dev) {
        // Also disables the I2S data after an open() that failed partway.
        esp_codec_dev_close(s_dev);
        esp_codec_dev_delete(s_dev);
        s_dev = NULL;
    }
    if (s_codec) {
        audio_codec_delete_codec_if(s_codec);
        s_codec = NULL;
    }
    gpio_set_level(AUDIO_POWER, 1);
}

static esp_err_t power_up(void) {
    if (s_dev) return ESP_OK;
    gpio_set_level(AUDIO_PA, 0);
    gpio_set_level(AUDIO_POWER, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    // Recreate the codec after each rail power cycle: its register contents
    // were lost. A cached, already-open codec interface would skip its init.
    es8311_codec_cfg_t cfg = {
        .ctrl_if = s_ctrl,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 3.3, .codec_dac_voltage = 3.3 },
    };
    s_codec = es8311_codec_new(&cfg);
    if (!s_codec) goto fail;
    esp_codec_dev_cfg_t dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_codec, .data_if = s_data,
    };
    s_dev = esp_codec_dev_new(&dev);
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = VOICE_MIC_RATE, .channel = 2, .bits_per_sample = 16,
    };
    if (!s_dev) goto fail;
    // open() disables both channels before it sets the format; that logs an
    // error for a channel that is not running yet.
    i2s_channel_enable(s_tx);
    i2s_channel_enable(s_rx);
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) goto fail;
    if (esp_codec_dev_set_in_gain(s_dev, 30.0f) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_vol(s_dev, s_volume) != ESP_CODEC_DEV_OK) goto fail;
    return ESP_OK;
fail:
    power_down();
    return ESP_FAIL;
}

// Under s_lock.
static bool speaker_on(void) {
    if (power_up() != ESP_OK) return false;
    if (!gpio_get_level(AUDIO_PA)) {
        gpio_set_level(AUDIO_PA, 1);
        // NS4150B startup is typically 120 ms. Queue no tone before it wakes.
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return true;
}

// Under s_lock: switch off whatever nothing uses any more.
static void release(void) {
    if (!s_playing && !s_cue) gpio_set_level(AUDIO_PA, 0);
    if (!s_playing && !s_cue && !s_recording) power_down();
}

// Under s_lock.
static esp_err_t write_stereo(const int16_t *stereo, size_t frames) {
    const uint8_t *p = (const uint8_t *)stereo;
    size_t left = frames * sizeof(int16_t) * 2;
    esp_err_t err = ESP_OK;
    while (left && err == ESP_OK) {
        size_t sent = 0;
        err = i2s_channel_write(s_tx, p, left, &sent, 200);
        if (err == ESP_OK && !sent) err = ESP_ERR_TIMEOUT;
        p += sent;
        left -= sent;
    }
    return err;
}

// A short tone when the microphone opens (1 kHz) and closes (500 Hz). Local
// only: no network turn and no microphone data is sent by this.
static void cue(bool end) {
    static const int16_t wave[16] = {
        0, 383, 707, 924, 1000, 924, 707, 383,
        0, -383, -707, -924, -1000, -924, -707, -383,
    };
    static int16_t tone[160 * 2];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cue = true;
    esp_err_t err = speaker_on() ? ESP_OK : ESP_FAIL;
    xSemaphoreGive(s_lock);
    for (size_t chunk = 0; chunk < 5 && err == ESP_OK; chunk++) {
        for (size_t i = 0; i < 160; i++) {
            size_t sample = chunk * 160 + i;
            // 5 ms fades reduce the edges.
            int gain = sample < 80 ? sample : sample >= 720 ? 799 - sample : 80;
            tone[2 * i] = tone[2 * i + 1] = wave[(end ? sample / 2 : sample) % 16] * 8192 * gain / 80000;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        err = write_stereo(tone, 160);
        xSemaphoreGive(s_lock);
    }
    int64_t tail_until = esp_timer_get_time() + 120000;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!end && err == ESP_OK && s_recording) {
        // The mic is already powered. Drain its settling samples during the
        // cue tail, not after the user starts talking.
        for (int i = 0; i < 4 && err == ESP_OK; i++) {
            size_t got = 0;
            err = i2s_channel_read(s_rx, s_stereo, sizeof(s_stereo), &got, 100);
            if (err == ESP_ERR_TIMEOUT) err = ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    int64_t left = tail_until - esp_timer_get_time();
    if (left > 0) vTaskDelay(pdMS_TO_TICKS((left + 999) / 1000));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cue = false;
    release();
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) ESP_LOGW(TAG, "cue failed: %s", esp_err_to_name(err));
}

// One cue at a time: a turn's cues and the PWR beeps. Under s_cue_lock.
static SemaphoreHandle_t s_cue_lock;
static TaskHandle_t s_cue_task;

enum { BEEP_TAP = 1, BEEP_HOLD = 2 };

// PWR beeps play here, so the PWR task keeps polling during the tone: the
// higher tone for a tap, the lower for a hold.
static void cue_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t beep = 0;
        xTaskNotifyWait(0, UINT32_MAX, &beep, portMAX_DELAY);
        // A turn has its own cues and its reply; no beep over either.
        if (xSemaphoreTake(s_cue_lock, 0) != pdTRUE) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool recording = s_recording;
        bool playing = s_playing;
        xSemaphoreGive(s_lock);
        if (!recording && !playing) cue(beep == BEEP_HOLD);
        xSemaphoreGive(s_cue_lock);
    }
}

// epd_154g.c calls this when a PWR hold turns the screen.
void epd154g_turn_feedback(void) {
    if (s_cue_task) xTaskNotify(s_cue_task, BEEP_HOLD, eSetValueWithOverwrite);
}

// ---- Reply on the e-paper ----------------------------------------------------

// ---- Page rendering (host-tested) -------------------------------------------

// GNU Unifont's 8x16 cells (fonts/epaper_154g_caption_font.h): 24 columns
// and 11 lines on the 200 x 200 panel, with a margin all round. The page
// count, in the 5x8 pixel font, sits in the bottom margin at the right.
#define PAGE_COLS   24
#define PAGE_LINES  11
#define PAGE_LEFT   4
#define PAGE_TOP    10
#define CELL_W      8
#define CELL_H      16
#define PAGE_W      200
#define PAGE_BYTES  (PAGE_COLS * 4 * PAGE_LINES + PAGE_LINES)
#define COUNT_BYTES 24

// One band of the page: `height` rows of PAGE_W RGB565 pixels, high byte
// first, starting at row `y`.
typedef void (*page_band_fn)(int y, int height, const uint8_t *rgb565);

// Invalid UTF-8 consumes one byte. Never inspect a byte beyond the terminating NUL.
static uint32_t utf8_next(const char **cursor) {
    const unsigned char *p = (const unsigned char *)*cursor;
    if (!*p) return 0;
    uint32_t code = *p;
    unsigned count = 1;
    if (code >= 0xc2 && code <= 0xdf) { count = 2; code &= 0x1f; }
    else if (code >= 0xe0 && code <= 0xef) { count = 3; code &= 0x0f; }
    else if (code >= 0xf0 && code <= 0xf4) { count = 4; code &= 7; }
    else if (code >= 0x80) { (*cursor)++; return '?'; }
    for (unsigned i = 1; i < count; i++) {
        if (!p[i] || (p[i] & 0xc0) != 0x80) { (*cursor)++; return '?'; }
        code = code << 6 | (p[i] & 0x3f);
    }
    if ((count == 3 && code < 0x800) || (count == 4 && code < 0x10000)
        || (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff) {
        (*cursor)++; return '?';
    }
    *cursor += count;
    return code;
}

static void strip_letter(uint8_t *strip, int x, uint32_t code) {
    const uint8_t *glyph = epaper_154g_caption_glyph(code);
    if (!glyph) glyph = epaper_154g_caption_glyph('?');
    for (int row = 0; row < CELL_H; row++) {
        for (int col = 0; col < CELL_W; col++) {
            if (!(glyph[row] >> (7 - col) & 1)) continue;
            size_t at = ((size_t)row * PAGE_W + x + col) * 2;
            strip[at] = strip[at + 1] = 0;
        }
    }
}

static void strip_small(uint8_t *strip, int x, int y, unsigned char c) {
    if (c < PIXEL_FONT_FIRST || c > PIXEL_FONT_LAST) c = '?';
    const uint8_t *glyph = pixel_font[c - PIXEL_FONT_FIRST];
    for (int col = 0; col < PIXEL_FONT_WIDTH; col++) {
        for (int row = 0; row < PIXEL_FONT_HEIGHT; row++) {
            if (!(glyph[col] >> row & 1)) continue;
            size_t at = ((size_t)(y + row) * PAGE_W + x + col) * 2;
            strip[at] = strip[at + 1] = 0;
        }
    }
}

// Black on white, top to bottom through `strip` (PAGE_W x CELL_H pixels):
// the top margin, a band per line of `page` (lines split by '\n'), then the
// bottom margin with `count` at its right.
static void render_page(const char *page, const char *count, uint8_t *strip, page_band_fn band) {
    const size_t strip_bytes = (size_t)PAGE_W * CELL_H * 2;
    memset(strip, 0xFF, strip_bytes);
    band(0, PAGE_TOP, strip);
    const char *p = page;
    for (int line = 0; line < PAGE_LINES; line++) {
        memset(strip, 0xFF, strip_bytes);
        for (int col = 0; *p && *p != '\n'; col++) {
            uint32_t code = utf8_next(&p);
            if (col < PAGE_COLS) strip_letter(strip, PAGE_LEFT + col * CELL_W, code);
        }
        if (*p == '\n') p++;
        band(PAGE_TOP + line * CELL_H, CELL_H, strip);
    }
    memset(strip, 0xFF, strip_bytes);
    int x = PAGE_W - PAGE_LEFT - (int)strlen(count) * (PIXEL_FONT_WIDTH + 1);
    for (const char *c = count; *c; c++, x += PIXEL_FONT_WIDTH + 1) {
        strip_small(strip, x, 3, (unsigned char)*c);
    }
    band(PAGE_TOP + PAGE_LINES * CELL_H, PAGE_W - PAGE_TOP - PAGE_LINES * CELL_H, strip);
}

// ---- Page task ---------------------------------------------------------------

#define CACHE_PAGES 16

// The page task does what was asked last: show the cached page s_cache_shown
// with the page count, show the next one, or go back to the status screen.
// Older requests are dropped; a PWR tap never replaces a pending request.
typedef enum { PAGE_NONE, PAGE_SHOW, PAGE_NEXT, PAGE_STATUS } page_want_t;
static portMUX_TYPE s_page_mux = portMUX_INITIALIZER_UNLOCKED;
static page_want_t s_want;         // under s_page_mux
static TaskHandle_t s_page_task;
static uint8_t *s_strip;
// The reply's pages: written on the voice task, read on the page task.
static SemaphoreHandle_t s_cache_lock;
static char *s_cache;              // CACHE_PAGES pages of PAGE_BYTES
static int s_cache_pages, s_cache_shown;
// Voice task only: whether this turn has a reply, and the count drawn.
static bool s_replied;
static int s_drawn_pages;
// Reply audio sent to the speaker since the turn began, in 16 kHz frames:
// the reply's reading time once it is over.
static atomic_size_t s_played;
// Voice task only: how far into the reading time the last scan went.
static size_t s_scan_end;

// Muse wraps reply text to this page.
void muse_state_page(bool cjk, int *cols, int *lines) {
    (void)cjk;
    *cols = PAGE_COLS;
    *lines = PAGE_LINES;
}

static void draw_band(int y, int height, const uint8_t *rgb565) {
    led_status_draw_rect(0, y, PAGE_W, height, (const uint16_t *)rgb565);
}

// Draws through the same calls as display.draw_url; the e-paper refresh
// blocks here, not on the voice task.
static void page_task(void *arg) {
    (void)arg;
    static char page[PAGE_BYTES];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        taskENTER_CRITICAL(&s_page_mux);
        page_want_t want = s_want;
        s_want = PAGE_NONE;
        taskEXIT_CRITICAL(&s_page_mux);
        if (want == PAGE_STATUS) {
            led_status_show_animation();
            continue;
        }
        if (want != PAGE_SHOW && want != PAGE_NEXT) continue;
        char count[COUNT_BYTES] = "";
        xSemaphoreTake(s_cache_lock, portMAX_DELAY);
        bool any = s_cache_pages > 0;
        if (want == PAGE_NEXT) {
            // Round to the first after the last; one page has nothing to turn.
            any = s_cache_pages > 1;
            if (any) s_cache_shown = (s_cache_shown + 1) % s_cache_pages;
        }
        if (any) {
            memcpy(page, s_cache + (size_t)s_cache_shown * PAGE_BYTES, PAGE_BYTES);
            if (s_cache_pages > 1) {
                snprintf(count, sizeof(count), "%d/%d", s_cache_shown + 1, s_cache_pages);
            }
        }
        xSemaphoreGive(s_cache_lock);
        if (!any) continue;
        render_page(page, count, s_strip, draw_band);
        led_status_draw_done();
    }
}

// epd_154g.c calls this on a PWR tap.
void epd154g_pwr_tap(void) {
    if (s_cue_task) xTaskNotify(s_cue_task, BEEP_TAP, eSetValueWithOverwrite);
    if (!s_page_task) return;
    taskENTER_CRITICAL(&s_page_mux);
    bool idle = s_want == PAGE_NONE;
    if (idle) s_want = PAGE_NEXT;
    taskEXIT_CRITICAL(&s_page_mux);
    if (idle) xTaskNotifyGive(s_page_task);
}

static void queue_page_request(page_want_t want) {
    if (!s_page_task) return;
    taskENTER_CRITICAL(&s_page_mux);
    s_want = want;
    taskEXIT_CRITICAL(&s_page_mux);
    xTaskNotifyGive(s_page_task);
}

// On the voice task, which owns the turn: every page of the reply in order,
// found by asking Muse for the page at each tenth of a second of the reply's
// reading time, up to `until` frames, then the page Muse gives for the end.
// Before the reply is over its reading time isn't known (until is SIZE_MAX),
// so the scan stops at the first page that reads like the end; two pages that
// read the same can stop it early, and the end of the turn corrects it by
// scanning to the reading time or to where this scan stopped, whichever is
// later (reading can be cut short). Keeps the page shown when it still
// exists. Returns the page count.
static int cache_reply_pages(size_t until) {
    static char page[PAGE_BYTES], last[PAGE_BYTES];
    if (!muse_hatch_turn_caption(SIZE_MAX, last, sizeof(last))) return 0;
    muse_text_to_ascii(last, sizeof(last));
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    int n = 0;
    for (size_t at = 0, steps = 0; n < CACHE_PAGES && steps < 20000;
         at += VOICE_MIC_RATE / 10, steps++) {
        if (at > until) at = until;
        if (!muse_hatch_turn_caption(at, page, sizeof(page))) break;
        muse_text_to_ascii(page, sizeof(page));
        if (!n || strcmp(page, s_cache + (size_t)(n - 1) * PAGE_BYTES)) {
            memcpy(s_cache + (size_t)n++ * PAGE_BYTES, page, PAGE_BYTES);
        }
        if (at > s_scan_end) s_scan_end = at;
        if (until == SIZE_MAX ? !strcmp(page, last) : at == until) break;
    }
    if (n && n < CACHE_PAGES && strcmp(last, s_cache + (size_t)(n - 1) * PAGE_BYTES)) {
        memcpy(s_cache + (size_t)n++ * PAGE_BYTES, last, PAGE_BYTES);
    }
    s_cache_pages = n;
    if (s_cache_shown >= n) s_cache_shown = 0;
    xSemaphoreGive(s_cache_lock);
    return n;
}

// The voice states as the page sees them: a new turn goes back to the status
// screen; once the reply's text is in, which is when Muse starts speaking it
// or at the latest when the turn ends, its first page goes up with the page
// count, and again at the end if more text came after that.
static void page_follow_voice(led_voice_t state) {
    if (!s_page_task) return;
    if (state == LED_VOICE_LISTENING) {
        s_replied = false;
        atomic_store(&s_played, 0);
        s_scan_end = 0;
        if (s_drawn_pages) {
            s_drawn_pages = 0;
            xSemaphoreTake(s_cache_lock, portMAX_DELAY);
            s_cache_pages = s_cache_shown = 0;
            xSemaphoreGive(s_cache_lock);
            queue_page_request(PAGE_STATUS);
        }
        return;
    }
    if (state == LED_VOICE_BUFFERING || state == LED_VOICE_SPEAKING) s_replied = true;
    bool first = state == LED_VOICE_SPEAKING && !s_drawn_pages;
    bool last = state == LED_VOICE_IDLE && s_replied;
    if (!first && !last) return;
    size_t until = atomic_load(&s_played);
    if (until < s_scan_end) until = s_scan_end;
    int pages = cache_reply_pages(last && until ? until : SIZE_MAX);
    if (pages && pages != s_drawn_pages) {
        s_drawn_pages = pages;
        queue_page_request(PAGE_SHOW);
    }
}

// ---- Green LED: the voice states ---------------------------------------------

static atomic_int s_voice = LED_VOICE_IDLE;
static atomic_uint s_error_generation;
static atomic_bool s_mic_ready;
static TaskHandle_t s_led_task;

static bool voice_led_on(led_voice_t state, uint64_t ms) {
    switch (state) {
    case LED_VOICE_LISTENING: return true;
    case LED_VOICE_TRANSCRIBING:
    case LED_VOICE_THINKING: return ms % 1000 < 500;
    case LED_VOICE_BUFFERING: return ms % 500 < 250;
    case LED_VOICE_SPEAKING: return ms % 250 < 125;
    case LED_VOICE_ERROR: return ms < ERROR_BLINK_MS && ms % 240 < 120;
    default: return false;
    }
}

static void led_task(void *arg) {
    (void)arg;
    led_voice_t previous = LED_VOICE_IDLE;
    unsigned seen_error = atomic_load(&s_error_generation);
    uint64_t since = esp_timer_get_time() / 1000;
    for (;;) {
        uint64_t now = esp_timer_get_time() / 1000;
        led_voice_t state = atomic_load(&s_voice);
        // voice.c shows LISTENING before the mic is up; blink until it is.
        if (state == LED_VOICE_LISTENING && !atomic_load(&s_mic_ready)) state = LED_VOICE_BUFFERING;
        unsigned generation = atomic_load(&s_error_generation);
        if (state != previous || generation != seen_error) {
            since = now;
            previous = state;
            seen_error = generation;
        }
        gpio_set_level(VOICE_LED, voice_led_on(state, now - since) ? 0 : 1);
        // Sleep until the next state once the LED has nothing left to show.
        bool still = state == LED_VOICE_IDLE ||
                     (state == LED_VOICE_ERROR && now - since >= ERROR_BLINK_MS);
        ulTaskNotifyTake(pdTRUE, still ? portMAX_DELAY : pdMS_TO_TICKS(20));
    }
}

void led_status_set_voice(led_voice_t state) {
    page_follow_voice(state);
    atomic_store(&s_voice, state);
    if (state == LED_VOICE_ERROR) atomic_fetch_add(&s_error_generation, 1);
    if (s_led_task) xTaskNotifyGive(s_led_task);
}

void led_status_set_level(float level) { (void)level; }
void led_status_show_volume(int percent) { (void)percent; }

// ---- voice_board.h -----------------------------------------------------------

// voice.c reads the mic from a priority 4 task, below the network tasks; the
// reader runs at CAPTURE_PRIORITY until voice_board_mic_stop(). The start cue
// and the codec warm-up take about 0.3 s after voice.c shows LISTENING; the
// LED turns steady only once they are done.
esp_err_t voice_board_mic_start(void) {
    xSemaphoreTake(s_cue_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = power_up();
    s_recording = err == ESP_OK;
    s_frames = 0;
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        s_priority = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, CAPTURE_PRIORITY);
        cue(false);
        atomic_store(&s_mic_ready, true);
        if (s_led_task) xTaskNotifyGive(s_led_task);
    }
    xSemaphoreGive(s_cue_lock);
    return err;
}

void voice_board_mic_stop(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool recording = s_recording;
    bool long_enough = s_frames >= END_CUE_FRAMES;
    xSemaphoreGive(s_lock);
    if (!recording) return;
    atomic_store(&s_mic_ready, false);
    xSemaphoreTake(s_cue_lock, portMAX_DELAY);
    if (long_enough) cue(true);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_recording = false;
    release();
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_cue_lock);
    vTaskPrioritySet(NULL, s_priority);
}

size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak) {
    if (frames > AUDIO_CHUNK) frames = AUDIO_CHUNK;
    *peak = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t bytes = 0;
    esp_err_t err = s_recording ?
        i2s_channel_read(s_rx, s_stereo, frames * sizeof(int16_t) * 2, &bytes, 100) :
        ESP_ERR_INVALID_STATE;
    // TIMEOUT can include valid samples. Never drop those or end a turn early.
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        xSemaphoreGive(s_lock);
        return 0;
    }
    size_t n = bytes / (sizeof(int16_t) * 2);
    if (n > frames) n = frames;
    s_frames += n;
    for (size_t i = 0; i < n; i++) {
        pcm[i] = s_stereo[2 * i]; // ES8311 ADC's left slot, not a second mic
        int value = pcm[i];
        if (value < 0) value = -value;
        if (value > *peak) *peak = value;
    }
    xSemaphoreGive(s_lock);
    return n;
}

// The player calls this around every reply. The speaker powers up on the
// first audible frame instead, so a silent text reply leaves the codec off.
void voice_board_amp(bool on) {
    if (on) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_playing = false;
    release();
    xSemaphoreGive(s_lock);
}

esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    // The player produces 48 kHz. Average each three stereo frames to the
    // codec's 16 kHz bus; player chunks are always a multiple of three.
    if (count % 3) return ESP_ERR_INVALID_SIZE;
    atomic_fetch_add(&s_played, count / 3);
    bool audible = false;
    for (size_t i = 0; i < count * 2; i++) audible |= frames[i] / 65536 != 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // A cue owns the speaker; a reply keeps its pace without writing over it.
    if (!s_playing && audible && !s_cue) s_playing = speaker_on();
    bool playing = s_playing && !s_cue;
    xSemaphoreGive(s_lock);
    if (!playing) {
        vTaskDelay(pdMS_TO_TICKS(count * 1000 / VOICE_SPEAKER_RATE));
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    for (size_t at = 0; at < count && err == ESP_OK;) {
        size_t n = 0;
        for (; n < AUDIO_CHUNK && at < count; n++, at += 3) {
            for (size_t c = 0; c < 2; c++) {
                int64_t sum = (int64_t)frames[2 * at + c] + frames[2 * at + 2 + c] + frames[2 * at + 4 + c];
                s_stereo[2 * n + c] = sum / 3 / 65536;
            }
        }
        err = write_stereo(s_stereo, n);
    }
    xSemaphoreGive(s_lock);
    return err;
}

void voice_board_set_volume(int percent) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (s_dev) esp_codec_dev_set_out_vol(s_dev, s_volume);
    xSemaphoreGive(s_lock);
}

// BOOT is both the talk button and the setup button: it keeps its setup role
// while a pairing waits for its confirmation.
bool voice_board_muted(void) {
    return link_pairing_confirmation_required();
}

int voice_board_dial_steps(void) { return 0; }

esp_err_t voice_board_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    gpio_set_level(AUDIO_POWER, 1);
    gpio_set_level(AUDIO_PA, 0);
    gpio_set_level(VOICE_LED, 1);
    const gpio_config_t outputs = {
        .pin_bit_mask = 1ULL << AUDIO_POWER | 1ULL << AUDIO_PA | 1ULL << VOICE_LED,
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    esp_err_t err = gpio_config(&outputs);
    if (err != ESP_OK) return err;
    // The SHTC3 and the RTC share this bus: use it if it exists.
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    err = i2c_master_get_bus_handle(I2C_NUM_0, &bus);
    if (err != ESP_OK) err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) return err;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    // 180 ms of microphone, so a busy network task does not drop speech.
    chan.dma_desc_num = 12;
    err = i2s_new_channel(&chan, &s_tx, &s_rx);
    if (err != ESP_OK) return err;
    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = 14, .bclk = 15, .ws = 38, .dout = 45, .din = 16 },
    };
    err = i2s_channel_init_std_mode(s_tx, &std);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_rx, &std);
    if (err != ESP_OK) return err;
    audio_codec_i2s_cfg_t data = { .port = I2S_NUM_0, .tx_handle = s_tx, .rx_handle = s_rx };
    s_data = audio_codec_new_i2s_data(&data);
    audio_codec_i2c_cfg_t ctrl = {
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus,
    };
    s_ctrl = audio_codec_new_i2c_ctrl(&ctrl);
    if (!s_data || !s_ctrl) return ESP_ERR_NO_MEM;
    // Probe at boot with the PA low, then leave the entire audio rail off.
    err = power_up();
    power_down();
    if (err != ESP_OK) return err;
    if (xTaskCreate(led_task, "voice_led", 2048, NULL, 3, &s_led_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_cache_lock = xSemaphoreCreateMutex();
    s_cue_lock = xSemaphoreCreateMutex();
    s_strip = heap_caps_malloc((size_t)PAGE_W * CELL_H * 2, MALLOC_CAP_SPIRAM);
    s_cache = heap_caps_malloc((size_t)CACHE_PAGES * PAGE_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_cache_lock || !s_cue_lock || !s_strip || !s_cache) return ESP_ERR_NO_MEM;
    if (xTaskCreate(cue_task, "voice_cue", 3072, NULL, 3, &s_cue_task) != pdPASS ||
        xTaskCreate(page_task, "voice_page", 3072, NULL, 2, &s_page_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ES8311 ready: 16000 Hz duplex; audio rail off until used");
    return ESP_OK;
}
