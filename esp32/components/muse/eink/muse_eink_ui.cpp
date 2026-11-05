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
 * Muse's face on e-paper, drawn with FreeInk: the muse_ui.h API for boards
 * with CONFIG_MUSE_UI_FREEINK, in place of muse_ui.c. It shows what the LVGL
 * UI shows, where it shows it and in its fonts, in black on white:
 *
 * - The screen is drawn again when something on it changes, and Muse is
 *   animated a frame per frame_ms, each a fast (1-bit, differential)
 *   refresh. Fast refreshes leave a little ghosting, so every
 *   CLEAN_EVERY-th refresh is a full one, as is the first after a page
 *   change, an image, or sleep.
 * - Touch is read on its own task, so a press isn't missed while the panel
 *   refreshes: the mic, waking and the pairing confirmation act on it at
 *   once; taps and swipes reach the screen on its next pass.
 * - Asleep, the screen keeps its last picture (e-paper holds it with no
 *   power) after one clean refresh, and nothing is drawn until it wakes.
 */

#include <EInkDisplay.h>
#include <atomic>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_eink.h"
#include "muse_eink_priv.h"

extern "C" {
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_console.h"
#include "muse_input.h"
#include "muse_link.h"
#include "muse_mem.h"
#include "muse_menu.h"
#include "muse_pixel.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_wifi.h"
}

namespace muse_eink {

using freeink::ui::ActionEvent;
using freeink::ui::BitmapFormat;
using freeink::ui::BitmapMode;
using freeink::ui::BitmapRef;
using freeink::ui::InputSnapshot;
using freeink::ui::TextStyle;

static const char *TAG = "muse_eink";

#define CLEAN_EVERY 30          /* fast refreshes between full ones */
#define TOUCH_POLL_MS 15
#define SWIPE_PX 80             /* horizontally, and more than vertically */
#define LONG_PRESS_MS 600
#define METER_SEGS 14
#define METER_SEG_PX 9
#define METER_GAP_PX 4
#define CAPTION_W 256           /* 16 columns of unscii_16, as muse_ui.c */
#define CAPTION_LINE_SPACE 2
#define ART_BLANK_ROWS 3
#define MINI_CELL_PX 2
#define MIC_PX 26
#define MIC_HIT_PAD 12

App *g_app;
int g_w = 480, g_h = 800;

static EInkDisplay s_display(0, 0, 0, 0, 0, 0);   /* pins come from FreeInk's board profile */
static freeink::ui::DisplayTarget *s_target;
static TaskHandle_t s_task, s_touch_task;
static std::atomic<bool> s_ready{}, s_paused{}, s_dark{}, s_draw_now{}, s_want_face{};
static std::atomic<bool> s_face_active{true}, s_image_shown{};
static int s_fast_count;
static std::atomic<bool> s_need_clean{true};
static std::atomic<uint32_t> s_navigation_epoch{}, s_presented_epoch{};

/* The layout, worked out as muse_ui.c does for the full layout. */
static int s_canvas_px, s_dy, s_big_y;
struct ReplyBox {
    int px, y;              /* Muse's size and centre, offsets from the screen's centre */
    int cols, lines, w, h, top;
};
static ReplyBox s_read;     /* the read layout: replies on a speakerless board */

/* ─── Touch ─────────────────────────────────────────────────────────────── */

struct TouchEvent {
    int16_t x, y;
    bool down;
    int64_t time_us;
    uint32_t epoch;
};
static QueueHandle_t s_touch_q;
static std::atomic<bool> s_mic_visible{};

static bool mic_available(void)
{
    return s_mic_visible && s_face_active && !s_image_shown && !s_dark && !s_paused &&
           s_presented_epoch.load() == s_navigation_epoch.load();
}

struct TouchState {
    bool was_down, mic, swallow;
    int16_t x, y;
    uint32_t epoch;
};
static TouchState s_touch;

static void touch_sample(bool down, int16_t nx, int16_t ny)
{
    TouchState &t = s_touch;
    if (down) {
        t.x = nx;
        t.y = ny;
    }
    if (t.mic && !mic_available()) {
        muse_input_touch_talk(false);
        t.mic = false;
        t.swallow = true;   /* a held mic press must not become a Settings tap */
    }
    if (down && !t.was_down) {
        t.epoch = s_presented_epoch.load();
        /* As muse_ui.c's on_any_press. */
        if (muse_board->touch_talk && muse_link_state() == MUSE_LINK_CONFIRM) {
            muse_input_touch_talk(true);
            muse_input_touch_talk(false);
        }
        muse_state_poke();
        t.swallow = false;
        t.mic = false;
        if (muse_state_asleep() || s_dark) {
            /* The waking touch only wakes, like the LVGL UI's sleep cover. */
            muse_state_set_asleep(false);
            t.swallow = true;
            if (s_task) xTaskNotifyGive(s_task);
        } else if (mic_available()) {
            int dx = t.x - g_w / 2, dy = t.y - (g_h - 50 - MIC_PX / 2);
            const int r = MIC_PX / 2 + MIC_HIT_PAD;
            if (dx * dx + dy * dy <= r * r) {
                muse_input_touch_talk(true);
                t.mic = true;
            }
        }
    }
    if (!down && t.was_down && t.mic) {
        muse_input_touch_talk(false);
        t.mic = false;
        t.swallow = true;
    }
    /* A mic press keeps ownership until release, even if the finger moves. */
    if (!t.swallow && !t.mic && down != t.was_down) {
        TouchEvent e = {t.x, t.y, down, esp_timer_get_time(), t.epoch};
        if (xQueueSend(s_touch_q, &e, 0) == pdTRUE && !down && s_task) xTaskNotifyGive(s_task);
    }
    t.was_down = down;
}

static void touch_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_paused) {
            if (s_touch.mic) {
                muse_input_touch_talk(false);
                s_touch.mic = false;
                s_touch.swallow = true;
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
        int16_t x = 0, y = 0;
        bool down = muse_board->touch_read(&x, &y);
        touch_sample(down, x, y);
    }
}

/* ─── Drawing helpers ───────────────────────────────────────────────────── */

int line_height(FontId font)
{
    return s_target->lineHeight(font);
}

/* The next codepoint's bytes. */
static size_t utf8_len(const char *s)
{
    return utf8_scalar_bytes(s);
}

static int run_width(FontId font, const char *text, size_t len)
{
    char buf[160];
    int w = 0;
    while (len) {
        size_t n = utf8_prefix_bytes(text, len < sizeof(buf) - 1 ? len : sizeof(buf) - 1);
        memcpy(buf, text, n);
        buf[n] = 0;
        sanitize_utf8(buf);
        w += s_target->measureText(font, buf, TextStyle{}).width;
        text += n;
        len -= n;
    }
    return w;
}

int text_width(FontId font, const char *text, int letter_space)
{
    const char *end = strchr(text, '\n');
    size_t len = end ? (size_t)(end - text) : strlen(text);
    int n = 0;
    for (size_t i = 0; i < len; i += utf8_len(text + i)) n++;
    return run_width(font, text, len) + (n > 1 ? (n - 1) * letter_space : 0);
}

static void draw_run(int x, int y, FontId font, const char *text, size_t len, Color color, int letter_space)
{
    TextStyle style;
    style.font = font;
    style.color = color;
    char buf[160];
    while (len) {
        /* A codepoint at a time with letter spacing, else as much as fits. */
        size_t n = letter_space ? utf8_len(text) : utf8_prefix_bytes(text, len < sizeof(buf) - 1 ? len : sizeof(buf) - 1);
        memcpy(buf, text, n);
        buf[n] = 0;
        sanitize_utf8(buf);
        int w = s_target->measureText(font, buf, style).width;
        s_target->text(Rect{(int16_t)x, (int16_t)y, (int16_t)(w + 2), (int16_t)line_height(font)}, buf, style);
        x += w + letter_space;
        text += n;
        len -= n;
    }
}

void draw_text(int x, int y, FontId font, const char *text, Color color, int letter_space)
{
    const char *end = strchr(text, '\n');
    draw_run(x, y, font, text, end ? (size_t)(end - text) : strlen(text), color, letter_space);
}

void draw_text_centered(int cx, int y, FontId font, const char *text, Color color, int letter_space)
{
    draw_text(cx - text_width(font, text, letter_space) / 2, y, font, text, color, letter_space);
}

/* The next line of `text` that fits w px: broken at a space if it can be,
 * else between codepoints (as CJK breaks). Returns its length; *next is
 * where the following line starts. */
static size_t fit_line(FontId font, const char *text, int w, const char **next)
{
    size_t brk = 0, i = 0;
    for (; text[i] && text[i] != '\n'; i += utf8_len(text + i)) {
        if (text[i] == ' ') brk = i;
        size_t end = i + utf8_len(text + i);
        if (run_width(font, text, end) > w && i > 0) {
            size_t n = brk ? brk : i;
            *next = text + n + (text[n] == ' ');
            return n;
        }
    }
    *next = text + i + (text[i] == '\n');
    return i;
}

int wrapped_lines(int w, FontId font, const char *text)
{
    int lines = 0;
    while (*text) {
        fit_line(font, text, w, &text);
        lines++;
    }
    return lines;
}

int draw_wrapped(int x, int y, int w, FontId font, const char *text, int max_lines, int line_space, bool center,
                 Color color)
{
    int lines = 0;
    while (*text && lines < max_lines) {
        const char *next;
        size_t n = fit_line(font, text, w, &next);
        char buf[160];
        n = utf8_prefix_bytes(text, n < sizeof(buf) - 4 ? n : sizeof(buf) - 4);
        memcpy(buf, text, n);
        buf[n] = 0;
        if (lines == max_lines - 1 && *next) {
            /* More than fits: end in dots, as LVGL's LONG_MODE_DOTS. */
            while (n && text_width(font, buf) + text_width(font, "...") > w) {
                do n--; while (n && ((unsigned char)buf[n] & 0xC0) == 0x80);
                buf[n] = 0;
            }
            strcat(buf, "...");
        }
        int lx = center ? x + (w - text_width(font, buf)) / 2 : x;
        draw_text(lx, y + lines * (line_height(font) + line_space), font, buf, color);
        lines++;
        text = next;
    }
    return lines;
}

static void dot(int cx, int cy, int r, Paint paint)
{
    s_target->fill(Rect{(int16_t)(cx - r), (int16_t)(cy - r), (int16_t)(2 * r), (int16_t)(2 * r)}, paint, (uint8_t)r);
}

/* ─── Muse ──────────────────────────────────────────────────────────────── */

/* Muse's pixel art in his own colours, dithered to ink on paper: the LVGL
 * UI's black background (C_BG, the art's only pure black) is the paper, and
 * every other colour gets ink by how dark it is, in a 4x4 ordered dither,
 * which a fast refresh shows as well as a full one. */
static uint8_t *s_art;          /* 1 bit per pixel, s_art_px square */
static int s_art_px;
static uint16_t *s_art_rows;    /* muse_pixel_scale's RGB565, ART_STRIP rows at a time */
#define ART_STRIP 16

static void render_art(int px)
{
    static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    if (px != s_art_px) {
        muse_pixel_set_size(px);
        s_art_px = px;
    }
    int stride = (px + 7) / 8;
    memset(s_art, 0, (size_t)stride * px);
    for (int y0 = 0; y0 < px; y0 += ART_STRIP) {
        int y1 = y0 + ART_STRIP - 1 < px - 1 ? y0 + ART_STRIP - 1 : px - 1;
        muse_pixel_scale(s_art_rows, px, 0, px - 1, y0, y1);
        for (int y = y0; y <= y1; y++) {
            const uint16_t *row = s_art_rows + (size_t)(y - y0) * px;
            uint8_t *dst = s_art + (size_t)y * stride;
            for (int x = 0; x < px; x++) {
                uint16_t p = row[x];
                if (!p) continue;   /* the background */
                int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                int luma = (77 * (r * 255 / 31) + 150 * (g * 255 / 63) + 29 * (b * 255 / 31)) >> 8;
                if ((255 - luma) * 17 / 256 > BAYER[y & 3][x & 3]) dst[x >> 3] |= 0x80 >> (x & 7);
            }
        }
    }
}

static void draw_art(int cx, int cy)
{
    BitmapRef ref;
    ref.data = s_art;
    ref.width = (uint16_t)s_art_px;
    ref.height = (uint16_t)s_art_px;
    ref.format = BitmapFormat::BW1;
    ref.progmem = false;
    s_target->bitmap(Rect{(int16_t)(cx - s_art_px / 2), (int16_t)(cy - s_art_px / 2), (int16_t)s_art_px,
                          (int16_t)s_art_px},
                     ref, BitmapMode::Center);
}

/* ─── The face's state, as last drawn ───────────────────────────────────── */

static const char *const MODE_NAMES[MUSE_MODE_COUNT] = {
    "WAKING UP", "READY", "LISTENING", "THINKING", "SPEAKING", "ERROR", "GOODBYE",
};

struct Face {
    muse_mode_t mode;
    const char *state;
    char name[32];
    char power[32];
    bool wifi, ble, ble_connected;
    bool mic;
    int ring;               /* progress, 0..1000 */
    int bar_x;              /* where the thinking segment is */
    int lit;                /* meter segments */
    bool answering;
    char caption[MUSE_CAPTION_MAX];
    char pair_title[24], pair_code[24], pair_hint[40];
    bool operator==(const Face &o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};
static Face s_face, s_drawn;
static uint32_t s_caption_version;
static float s_level;

static const char *idle_name(muse_wifi_state_t wifi)
{
    static bool joined;
    switch (wifi) {
    case MUSE_WIFI_CONNECTED: joined = true; return MODE_NAMES[MUSE_MODE_IDLE];
    case MUSE_WIFI_OFF: return "WI-FI OFF";
    case MUSE_WIFI_NO_NETWORK: return "SET UP WI-FI";
    case MUSE_WIFI_NOT_NEARBY: return "NO WI-FI";
    default: return joined ? "RECONNECTING" : "CONNECTING";
    }
}

/* Reads everything the face shows, as muse_ui.c's frame_tick does. */
static void read_face(float now)
{
    Face &f = s_face;
    /* Everything is read again but the caption, which muse_state_caption()
     * hands over only when it changes. */
    static char caption[MUSE_CAPTION_MAX];
    memcpy(caption, f.caption, sizeof(caption));
    memset(&f, 0, sizeof(f));
    memcpy(f.caption, caption, sizeof(caption));
    float mode_t;
    f.mode = muse_state_mode(&mode_t);

    muse_wifi_status_t w;
    muse_wifi_status(&w);
    f.wifi = w.state == MUSE_WIFI_CONNECTED || w.state == MUSE_WIFI_CONNECTING || w.state == MUSE_WIFI_FAILED;
    f.state = f.mode == MUSE_MODE_IDLE ? idle_name(w.state) : MODE_NAMES[f.mode];
    muse_ble_status_t b;
    muse_ble_status(&b);
    f.ble = b.state != MUSE_BLE_OFF;
    f.ble_connected = b.state == MUSE_BLE_CONNECTED;

    muse_hatch_status_t h;
    muse_hatch_status(&h);
    bool paired = h.state != MUSE_HATCH_NOT_SET;
    if (!paired) {
        const char *shown = b.name;
        if ((int)strlen(shown) * text_width(FONT_UNSCII16, "M") > g_w) {
            const char *tail = strrchr(shown, '-');
            if (tail && tail[1]) shown = tail + 1;
        }
        strlcpy(f.name, shown, sizeof(f.name));
    }
    f.mic = paired && (muse_board->audio_init || muse_board->mic_init);

    bool confirm = !b.passkey && muse_link_state() == MUSE_LINK_CONFIRM;
    if (confirm) {
        strlcpy(f.pair_title, "Pair with Muse app", sizeof(f.pair_title));
        if (muse_board->touch_talk || !muse_board->audio_init) {
            strlcpy(f.pair_code, "Tap screen", sizeof(f.pair_code));
            strlcpy(f.pair_hint, "Tap to confirm pairing", sizeof(f.pair_hint));
        } else {
            strlcpy(f.pair_code, "Press button", sizeof(f.pair_code));
            snprintf(f.pair_hint, sizeof(f.pair_hint), "Press the %s button", muse_board->talk_button);
        }
    } else if (b.passkey) {
        strlcpy(f.pair_title, "Pairing code", sizeof(f.pair_title));
        snprintf(f.pair_code, sizeof(f.pair_code), "%06lu", (unsigned long)b.passkey);
        strlcpy(f.pair_hint, "Enter it on your phone", sizeof(f.pair_hint));
    }

    muse_power_t p = muse_state_power();
    if (p.battery_pct < 0) strlcpy(f.power, p.usb ? "USB POWER" : "", sizeof(f.power));
    else snprintf(f.power, sizeof(f.power), p.charging ? "CHARGING %d%%" : "BATTERY %d%%", p.battery_pct);

    if (f.mode == MUSE_MODE_LISTENING || f.mode == MUSE_MODE_SPEAKING) {
        f.ring = (int)(muse_state_progress() * 1000);
    } else if (f.mode == MUSE_MODE_THINKING) {
        f.ring = 1000 / 6;
        f.bar_x = (int)(now * g_w) % g_w;
    }
    float level = muse_state_level();
    s_level += (level - s_level) * (level > s_level ? 0.6f : 0.2f);
    if (f.mode == MUSE_MODE_LISTENING) f.lit = (int)lroundf(s_level * METER_SEGS);

    /* No speaker: a reply is read, so it always gets the read layout. */
    f.answering = f.mode == MUSE_MODE_THINKING || f.mode == MUSE_MODE_SPEAKING;
    muse_state_caption(f.caption, sizeof(f.caption), &s_caption_version);
}

/* ─── The face ──────────────────────────────────────────────────────────── */

enum : freeink::ui::ActionId { ACTION_MUSE = 1, ACTION_IMAGE };

static void draw_mic(int cx, int top, int size)
{
    /* muse_ui.c's make_mic: a capsule, its cradle, stem and base. */
    int stroke = size / 10 > 1 ? size / 10 : 1;
    Paint ink = Paint::solid(Color::Black);
    int hw = size * 3 / 8, hh = size * 9 / 16;
    s_target->fill(Rect{(int16_t)(cx - hw / 2), (int16_t)top, (int16_t)hw, (int16_t)hh}, ink, (uint8_t)(hw / 2));
    int cd = size * 5 / 8 + stroke, cr = cd / 2;
    for (int a = 0; a <= 180; a += 3) {
        float t = (float)a * (float)M_PI / 180.0f;
        int x = cx + (int)lroundf((cr - stroke / 2) * cosf(t)), y = top + cr + (int)lroundf((cr - stroke / 2) * sinf(t));
        s_target->fill(Rect{(int16_t)(x - stroke / 2), (int16_t)(y - stroke / 2), (int16_t)stroke, (int16_t)stroke}, ink);
    }
    s_target->fill(Rect{(int16_t)(cx - stroke / 2), (int16_t)(top + size * 5 / 8), (int16_t)stroke,
                        (int16_t)(size * 7 / 8 - size * 5 / 8)}, ink);
    s_target->fill(Rect{(int16_t)(cx - hw / 2), (int16_t)(top + size * 7 / 8), (int16_t)hw, (int16_t)stroke}, ink);
}

static void draw_status_line(const Face &f, int y)
{
    /* Wi-Fi and Bluetooth icons in montserrat_14, then the power label, in a
     * centred row 8 px apart. */
    int widths[3] = {f.wifi ? text_width(FONT_M14, LV_SYMBOL_WIFI) : 0,
                     f.ble ? text_width(FONT_M14, LV_SYMBOL_BLUETOOTH) : 0,
                     f.power[0] ? text_width(FONT_UNSCII8, f.power) : 0};
    int total = 0, n = 0;
    for (int w : widths) {
        if (w) {
            total += w;
            n++;
        }
    }
    total += n > 1 ? (n - 1) * 8 : 0;
    int x = g_w / 2 - total / 2;
    int lh = line_height(FONT_M14);
    if (widths[0]) {
        draw_text(x, y, FONT_M14, LV_SYMBOL_WIFI);
        x += widths[0] + 8;
    }
    if (widths[1]) {
        draw_text(x, y, FONT_M14, LV_SYMBOL_BLUETOOTH, f.ble_connected ? Color::Black : Color::DarkGray);
        x += widths[1] + 8;
    }
    if (widths[2]) draw_text(x, y + (lh - line_height(FONT_UNSCII8)) / 2, FONT_UNSCII8, f.power);
}

static void face_screen(Screen &screen, const Face &f)
{
    auto &frame = screen.frame();
    const int cx = g_w / 2, cy = g_h / 2;
    const int muse_px = f.answering ? s_read.px : s_canvas_px;
    const int muse_y = cy + (f.answering ? s_read.y : s_big_y);

    /* Progress as muse_ui.c shows it on a rectangle (no_ring): a 6 px bar
     * along the bottom edge, and a sliding segment while thinking. */
    int bar = f.ring * g_w / 1000;
    int bar_x = f.mode == MUSE_MODE_THINKING ? f.bar_x : 0;
    if (bar > 0) s_target->fill(Rect{(int16_t)bar_x, (int16_t)(g_h - 6), (int16_t)bar, 6}, Paint::solid(Color::Black));
    draw_art(cx, muse_y);
    frame.hit(Rect{(int16_t)(cx - muse_px / 2), (int16_t)(muse_y - muse_px / 2), (int16_t)muse_px, (int16_t)muse_px},
              ACTION_MUSE);

    draw_status_line(f, 20 + s_dy);
    if (!f.answering) {
        draw_text_centered(cx, 40 + s_dy, FONT_UNSCII16, f.state, Color::Black, 2);
        if (f.name[0]) draw_text_centered(cx, 60 + s_dy, FONT_UNSCII16, f.name, Color::DarkGray);
    }

    /* Bottom up, as the LVGL layout: two caption lines, the meter, then Muse. */
    const int cap_h = 2 * line_height(FONT_UNSCII16) + CAPTION_LINE_SPACE;
    const int cap_top = 179 - cap_h;
    if (f.answering) {
        draw_wrapped(cx - s_read.w / 2, cy + s_read.top, s_read.w, FONT_UNSCII16, f.caption, s_read.lines,
                     CAPTION_LINE_SPACE, false);
    } else {
        draw_wrapped(cx - CAPTION_W / 2, cy + cap_top, CAPTION_W, FONT_UNSCII16, f.caption, 2, CAPTION_LINE_SPACE,
                     true);
    }
    if (f.mode == MUSE_MODE_LISTENING) {
        int meter_y = cy + cap_top - 6 - METER_SEG_PX / 2;
        int span = METER_SEGS * (METER_SEG_PX + METER_GAP_PX) - METER_GAP_PX;
        for (int i = 0; i < METER_SEGS; i++) {
            int rank = abs(2 * i - (METER_SEGS - 1)) / 2;
            bool on = rank < (f.lit + 1) / 2;
            int x = cx - span / 2 + i * (METER_SEG_PX + METER_GAP_PX);
            Rect seg{(int16_t)x, (int16_t)(meter_y - METER_SEG_PX / 2), METER_SEG_PX, METER_SEG_PX};
            if (on) s_target->fill(seg, Paint::solid(Color::Black));
            else s_target->stroke(seg, Paint::solid(Color::Black), 1);
        }
    }

    /* The mic, bottom centre 50 px up; the touch task acts on presses there. */
    /* It stays while a reply is up, as in muse_ui.c, unless the reply's page
     * would cover it. */
    const int mic_top = g_h - 50 - MIC_PX;
    const bool covered = f.answering && cy + s_read.top + s_read.h > mic_top;
    if (f.mic && !covered) {
        int top = mic_top;
        draw_mic(cx, top, MIC_PX);
        s_mic_visible = true;
    } else {
        s_mic_visible = false;
    }
}

static void draw_dots(bool settings)
{
    for (int i = 0; i < 2; i++) {
        int x = g_w / 2 + (i ? 8 : -8), y = g_h - 14 - 4;
        if (i == (int)settings) dot(x, y, 4, Paint::solid(Color::Black));
        else s_target->stroke(Rect{(int16_t)(x - 4), (int16_t)(y - 4), 8, 8}, Paint::solid(Color::Black), 1, 4);
    }
}

static void draw_pairing(const Face &f)
{
    if (!f.pair_code[0]) return;
    Rect card{(int16_t)(g_w / 2 - 150), (int16_t)(g_h / 2 - 75), 300, 150};
    s_target->fill(card, Paint::solid(Color::White), 24);
    s_target->stroke(card, Paint::solid(Color::Black), 2, 24);
    int y = card.y + 18;
    draw_text_centered(g_w / 2, y, FONT_M20, f.pair_title);
    y += line_height(FONT_M20) + 10;
    draw_text_centered(g_w / 2, y, FONT_M28, f.pair_code, Color::Black, 6);
    y += line_height(FONT_M28) + 10;
    draw_wrapped(card.x + 16, y, card.width - 32, FONT_M14, f.pair_hint, 2, 0, true);
}

/* ─── Images (display.draw_url) ─────────────────────────────────────────── */

static SemaphoreHandle_t s_image_mutex;
static uint16_t *s_image_buf;       /* RGB565, the screen's size */
/* Dithered to four grays, once complete, as two 1-bit masks in screen
 * coordinates: ink in the LSB and the MSB plane of FreeInk's absolute
 * grayscale (black 00, dark 10, light 01, white 11, as LSB MSB). */
static uint8_t *s_image_bits;       /* the LSB mask, then the MSB mask */
static std::atomic<bool> s_image_new{};

static void image_hide(void)
{
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (s_image_buf) {
        heap_caps_free(s_image_buf);
        s_image_buf = nullptr;
    }
    if (s_image_bits) {
        heap_caps_free(s_image_bits);
        s_image_bits = nullptr;
    }
    bool shown = s_image_shown;
    s_image_shown = false;
    s_image_new = false;
    xSemaphoreGive(s_image_mutex);
    if (shown) {
        navigation_changed();
        s_need_clean = true;
        request_draw();
    }
}

/* Floyd-Steinberg to four grays, as muse_ui.c's dither_gray4_565: each pixel
 * black, 85, 170 or white, the panel's four tones. */
static void dither_image(void)
{
    int stride = (g_w + 7) / 8;
    size_t plane = (size_t)stride * g_h;
    uint8_t *bits = (uint8_t *)heap_caps_calloc(2 * plane, 1, MALLOC_CAP_SPIRAM);
    int16_t *err = (int16_t *)calloc(2 * (size_t)(g_w + 2), sizeof(int16_t));
    if (!bits || !err) {
        heap_caps_free(bits);
        free(err);
        return;
    }
    int16_t *cur = err, *next = err + g_w + 2;
    for (int y = 0; y < g_h; y++) {
        memset(next, 0, (size_t)(g_w + 2) * sizeof(*next));
        for (int x = 0; x < g_w; x++) {
            uint16_t p = s_image_buf[(size_t)y * g_w + x];
            int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            int v = ((77 * (r * 255 / 31) + 150 * (g * 255 / 63) + 29 * (b * 255 / 31)) >> 8) + cur[x + 1];
            int level = (v * 3 + 127) / 255;
            level = level < 0 ? 0 : level > 3 ? 3 : level;
            int e = v - level * 85;
            uint8_t bit = 0x80 >> (x & 7);
            size_t at = (size_t)y * stride + (x >> 3);
            if (level == 0 || level == 2) bits[at] |= bit;           /* LSB 0: black, light */
            if (level == 0 || level == 1) bits[plane + at] |= bit;   /* MSB 0: black, dark */
            cur[x + 2] += (int16_t)(e * 7 / 16);
            next[x] += (int16_t)(e * 3 / 16);
            next[x + 1] += (int16_t)(e * 5 / 16);
            next[x + 2] += (int16_t)(e / 16);
        }
        int16_t *t = cur;
        cur = next;
        next = t;
    }
    free(err);
    heap_caps_free(s_image_bits);
    s_image_bits = bits;
}

/* An image in four grays: each plane drawn through the target (which turns
 * it to the panel's orientation) and shown with one absolute grayscale
 * refresh. A panel without one gets the darker half as black. */
static bool present_image(void)
{
    /* The command/fetch tasks can hide or replace either plane. Keep their
     * lifetime locked through presentation, not just while loading a pointer. */
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (!s_image_shown || !s_image_bits) {
        xSemaphoreGive(s_image_mutex);
        return false;
    }
    uint8_t *fb = s_display.getFrameBuffer();
    size_t fb_len = (size_t)s_display.getDisplayWidthBytes() * s_display.getDisplayHeight();
    static uint8_t *planes;
    if (!planes) planes = (uint8_t *)heap_caps_malloc(2 * fb_len, MALLOC_CAP_SPIRAM);
    size_t plane = (size_t)((g_w + 7) / 8) * g_h;
    for (int i = 0; i < 2; i++) {
        memset(fb, 0xFF, fb_len);
        BitmapRef ref;
        ref.data = s_image_bits + i * plane;
        ref.width = (uint16_t)g_w;
        ref.height = (uint16_t)g_h;
        ref.format = BitmapFormat::BW1;
        ref.progmem = false;
        s_target->bitmap(Rect{0, 0, (int16_t)g_w, (int16_t)g_h}, ref, BitmapMode::Center);
        draw_pairing(s_face);
        if (planes) memcpy(planes + i * fb_len, fb, fb_len);
    }
    /* The framebuffer keeps the MSB plane: the black-and-dark half, for a
     * panel without four tones. */
    if (planes && s_display.displayGrayscaleBase(freeink::GrayscaleMode::Absolute)) {
        s_display.copyGrayscaleBuffers(planes, planes + fb_len);
        s_display.displayGrayBuffer();
    } else {
        s_display.displayBuffer(EInkDisplay::FULL_REFRESH);
    }
    s_need_clean = true;   /* back to black and white with a full refresh */
    s_fast_count = 0;
    xSemaphoreGive(s_image_mutex);
    return true;
}

/* ─── The loop ──────────────────────────────────────────────────────────── */

static bool s_settings;             /* the Settings page is showing */
static bool s_render_image;         /* the image hit region in the current frame */

void navigation_changed(void)
{
    s_navigation_epoch.fetch_add(1);
}

void request_draw(void)
{
    s_draw_now = true;
    if (s_task) xTaskNotifyGive(s_task);
}

void request_face(void)
{
    s_want_face = true;
    request_draw();
}

static void root_screen(Screen &screen, void *)
{
    s_render_image = s_image_shown;
    if (s_render_image) {
        s_mic_visible = false;
        /* Drawn by present_image(); a tap anywhere puts it away. */
        screen.frame().hit(Rect{0, 0, (int16_t)g_w, (int16_t)g_h}, ACTION_IMAGE);
        return;
    }
    if (s_settings) {
        s_mic_visible = false;
        settings_screen(screen);
        if (!settings_in_subpage()) draw_dots(true);
    } else {
        face_screen(screen, s_face);
        draw_dots(false);
    }
    draw_pairing(s_face);
}

static void on_muse(const ActionEvent &, void *)
{
    muse_state_make_happy();
}

static void on_image(const ActionEvent &, void *)
{
    image_hide();
}

static void present(void)
{
    bool clean = s_need_clean.exchange(false) || ++s_fast_count >= CLEAN_EVERY;
    int64_t t = esp_timer_get_time();
    s_display.displayBuffer(clean ? EInkDisplay::FULL_REFRESH : EInkDisplay::FAST_REFRESH);
    ESP_LOGD(TAG, "%s refresh %d ms", clean ? "full" : "fast", (int)((esp_timer_get_time() - t) / 1000));
    if (clean) s_fast_count = 0;
}

static void draw(const InputSnapshot &input)
{
    /* Paper first: FreeInk's fill plots pixel by pixel, a memset doesn't. */
    memset(s_display.getFrameBuffer(), 0xFF, (size_t)s_display.getDisplayWidthBytes() * s_display.getDisplayHeight());
    uint32_t epoch = s_navigation_epoch.load();
    g_app->render(input);
    if (!present_image()) {
        if (s_render_image) {
            /* A hide raced rendering: rebuild the face/Settings and its hit
             * regions before presenting, rather than showing a blank frame. */
            memset(s_display.getFrameBuffer(), 0xFF,
                   (size_t)s_display.getDisplayWidthBytes() * s_display.getDisplayHeight());
            epoch = s_navigation_epoch.load();
            g_app->render(input);
        }
        present();
    }
    /* Input sampled during a page refresh still belongs to the old page. */
    if (epoch == s_navigation_epoch.load()) s_presented_epoch = epoch;
}

static void set_page(bool settings)
{
    if (settings == s_settings) return;
    navigation_changed();
    s_settings = settings;
    s_mic_visible = false;   /* re-enable only after the face is drawn */
    s_face_active = !settings;
    if (!settings) settings_home();
    s_need_clean = true;
}

/* Touch events since the last pass, as FreeInkUI input: a press, then on its
 * release a tap, a long press or a swipe. */
static void handle_touch(void)
{
    static int16_t down_x, down_y;
    static int64_t down_us;
    static bool down;
    TouchEvent e;
    while (xQueueReceive(s_touch_q, &e, 0) == pdTRUE) {
        if (e.epoch != s_navigation_epoch.load()) {
            down = false;
            continue;
        }
        InputSnapshot in;
        in.touchX = e.x;
        in.touchY = e.y;
        if (e.down && !down) {
            down = true;
            down_x = e.x;
            down_y = e.y;
            down_us = e.time_us;
            continue;
        }
        if (e.down) continue;
        if (!down) continue;   /* an orphan release cannot become a tap */
        down = false;
        int dx = e.x - down_x, dy = e.y - down_y;
        if (abs(dy) >= SWIPE_PX && abs(dy) > abs(dx)) {
            /* Up and down page through a long Settings page. */
            if (s_settings && !settings_typing()) {
                settings_scroll(dy);
                request_draw();
                break;   /* rebuild the hit table before dispatching another tap */
            }
            continue;
        }
        if (abs(dx) >= SWIPE_PX && abs(dx) > abs(dy)) {
            /* Swipes move between Muse and Settings, as the LVGL tileview. */
            if (s_image_shown) continue;
            if (dx < 0 && !s_settings) set_page(true);
            else if (dx > 0 && s_settings && !settings_typing() && !settings_back()) set_page(false);
            request_draw();
            break;
        }
        in.touchX = down_x;
        in.touchY = down_y;
        in.touchReleased = true;
        in.longPress = e.time_us - down_us >= LONG_PRESS_MS * 1000;
        uint32_t epoch = s_navigation_epoch.load();
        if (g_app->route(in)) {
            request_draw();
            if (epoch != s_navigation_epoch.load() || settings_keyboard_changed()) break;
        }
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    const TickType_t frame = pdMS_TO_TICKS(muse_board->frame_ms > 0 ? muse_board->frame_ms : 1000);
    TickType_t next_frame = xTaskGetTickCount();
    float next_settings_tick = 0;
    for (;;) {
        TickType_t now_ticks = xTaskGetTickCount();
        TickType_t wait = (TickType_t)(next_frame - now_ticks) <= frame ? next_frame - now_ticks : 0;
        /* Asleep, a button wakes Muse without telling this task: look again
         * every 200 ms. Paused, display_pause(false) notifies. */
        ulTaskNotifyTake(pdTRUE, s_paused ? portMAX_DELAY : s_dark ? pdMS_TO_TICKS(200) : wait);
        if (s_paused) continue;
        float now = (float)esp_timer_get_time() / 1e6f;

        /* Asleep: one clean refresh of what's showing, then nothing until awake. */
        bool asleep = muse_state_asleep();
        if (asleep && !s_dark) {
            s_dark = true;
            s_need_clean = true;
            draw(InputSnapshot{});
            continue;
        }
        if (!asleep && s_dark) {
            s_dark = false;
            s_need_clean = true;
            s_draw_now = true;
        }
        if (s_dark) continue;

        if (s_want_face) {
            s_want_face = false;
            set_page(false);
            s_draw_now = true;
        }
        xSemaphoreTake(s_image_mutex, portMAX_DELAY);
        if (s_image_new && s_image_bits) {
            s_image_new = false;
            s_mic_visible = false;
            if (!s_image_shown) navigation_changed();
            s_image_shown = true;
            s_need_clean = true;
            s_draw_now = true;
        }
        xSemaphoreGive(s_image_mutex);

        handle_touch();

        muse_mode_t was = s_face.mode;
        read_face(now);
        if (s_face.mode != was && s_face.mode == MUSE_MODE_LISTENING) {
            image_hide();
            set_page(false);
        }
        if (s_settings && now >= next_settings_tick) {
            next_settings_tick = now + (muse_board->frame_ms > 0 ? muse_board->frame_ms : 1000) / 1000.0f;
            if (settings_tick()) s_draw_now = true;
        }

        /* Muse moves a frame per frame_ms while the face shows. */
        bool frame_due = (TickType_t)(xTaskGetTickCount() - next_frame) < portMAX_DELAY / 2;
        bool draw_requested = s_draw_now.exchange(false);
        bool redraw = draw_requested || !(s_face == s_drawn) || g_app->invalidated();
        if (s_image_shown) {
            /* An image stays as it is, unless the pairing card comes or goes over it. */
            redraw = draw_requested || strcmp(s_face.pair_code, s_drawn.pair_code) != 0;
        }
        if (frame_due) {
            next_frame = xTaskGetTickCount() + frame;
            if (!s_settings && !s_image_shown) {
                float mode_t;
                muse_state_mode(&mode_t);
                muse_pose_t pose = {s_face.mode, now, mode_t, s_level, muse_state_happiness()};
                muse_pixel_render(&pose);
                render_art(s_face.answering ? s_read.px : s_canvas_px);
                redraw = true;
            }
        }
        if (!redraw) continue;
        s_drawn = s_face;
        draw(InputSnapshot{});
    }
}

/* The read layout's page, as muse_ui.c's build_answer works it out for a
 * rectangle: Muse small under the status line and the most text that fits
 * under it, above the page dots. */
static void layout(void)
{
    s_canvas_px = MUSE_PX_W * 5;
    if (s_canvas_px > g_w) s_canvas_px = g_w / MUSE_PX_W * MUSE_PX_W;
    s_dy = (g_h - 466) / 2;
    const int cap_h = 2 * line_height(FONT_UNSCII16) + CAPTION_LINE_SPACE;
    const int meter_y = 179 - cap_h - 6 - METER_SEG_PX / 2;
    const int art_bottom = meter_y - METER_SEG_PX / 2 - 4;
    s_big_y = art_bottom - (s_canvas_px / 2 - ART_BLANK_ROWS * (s_canvas_px / MUSE_PX_W));

    const int cw = text_width(FONT_UNSCII16, "M");
    const int pitch = line_height(FONT_UNSCII16) + CAPTION_LINE_SPACE;
    ReplyBox &l = s_read;
    int status_bottom = 20 + s_dy + 16 - g_h / 2;
    l.px = MUSE_PX_W * MINI_CELL_PX;
    l.y = status_bottom + 2 + l.px / 2;
    int art_mini = l.y + l.px / 2 - ART_BLANK_ROWS * MINI_CELL_PX;
    l.top = art_mini + 8;
    l.cols = 16;
    l.lines = 2;
    int bottom = g_h / 2 - 28;
    for (int c = 12; c <= 24 && c * cw <= g_w - 16; c++) {
        int n = (bottom - l.top + CAPTION_LINE_SPACE) / pitch;
        while ((c + 1) * n > MUSE_CAPTION_MAX * 2 / 3) n--;
        if (c * n > l.cols * l.lines) {
            l.cols = c;
            l.lines = n;
        }
    }
    l.w = l.cols * cw;
    l.h = l.lines * pitch - CAPTION_LINE_SPACE;
    muse_state_set_page(l.cols, l.lines);
    ESP_LOGI(TAG, "reply pages: %d x %d read", l.cols, l.lines);
}

}  // namespace muse_eink

using namespace muse_eink;

/* ─── muse_ui.h ─────────────────────────────────────────────────────────── */

extern "C" esp_err_t muse_ui_start(void)
{
    if (!muse_board->touch_read) {
        ESP_LOGE(TAG, "a FreeInk board needs touch_read");
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_display.begin();
    if (!s_display.getFrameBuffer()) {
        ESP_LOGE(TAG, "display init failed");
        return ESP_FAIL;
    }
    static freeink::ui::DisplayTarget target(s_display.getFrameBuffer(), s_display.getDisplayWidth(),
                                             s_display.getDisplayHeight(), s_display.getDisplayWidthBytes());
    s_target = &target;
    g_w = target.logicalWidth();
    g_h = target.logicalHeight();
    if (g_w != muse_board->width || g_h != muse_board->height) {
        ESP_LOGW(TAG, "panel is %dx%d, board says %dx%d", g_w, g_h, muse_board->width, muse_board->height);
    }
    if (!fonts_init(target)) return ESP_ERR_NOT_FOUND;

    s_art = (uint8_t *)heap_caps_malloc((size_t)(MUSE_PX_W * 5 + 7) / 8 * MUSE_PX_W * 5, MALLOC_CAP_SPIRAM);
    s_art_rows = (uint16_t *)heap_caps_malloc((size_t)MUSE_PX_W * 5 * ART_STRIP * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_image_mutex = xSemaphoreCreateMutex();
    s_touch_q = xQueueCreate(32, sizeof(TouchEvent));
    if (!s_art || !s_art_rows || !s_image_mutex || !s_touch_q) return ESP_ERR_NO_MEM;

    layout();
    static App app(target, target.deviceContext());
    g_app = &app;
    app.setScreen(root_screen, nullptr);
    app.on(ACTION_MUSE, on_muse);
    app.on(ACTION_IMAGE, on_image);
    settings_setup(app);

    /* Internal RAM: Settings' handlers write NVS, which turns the cache off. */
    if (xTaskCreatePinnedToCoreWithCaps(ui_task, "muse_eink", 16384, NULL, MUSE_UI_PRIORITY, &s_task, MUSE_UI_CORE,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS ||
        xTaskCreatePinnedToCore(touch_task, "muse_touch", 3072, NULL, MUSE_UI_PRIORITY + 1, &s_touch_task,
                                MUSE_UI_CORE) !=
            pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGI(TAG, "UI up: %dx%d e-paper, %d px Muse, %d ms frames", g_w, g_h, s_canvas_px, muse_board->frame_ms);
    return ESP_OK;
}

extern "C" void muse_eink_display_pause(bool pause)
{
    s_paused = pause;
    if (!pause) {
        if (s_touch_task) xTaskNotifyGive(s_touch_task);
        request_draw();
    }
}

extern "C" bool muse_ui_dark(void)
{
    return s_dark;
}

extern "C" bool muse_ui_face_active(void)
{
    return s_face_active && !s_image_shown;
}

extern "C" void muse_ui_show_face(void)
{
    request_face();
}

extern "C" void muse_ui_set_swipe_enabled(bool enabled)
{
    (void)enabled;   /* swipes are Settings' own business here (settings_typing) */
}

extern "C" void muse_ui_preview_brightness(int pct)
{
    (void)pct;   /* no backlight */
}

extern "C" bool muse_ui_image_size(int *w, int *h)
{
    if (!s_ready || !heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) return false;
    *w = g_w;
    *h = g_h;
    return true;
}

extern "C" int muse_ui_image_bits(void)
{
    return 2;   /* four grays */
}

extern "C" bool muse_ui_image_draw(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!s_ready || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > g_w || y + h > g_h) return false;
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (!s_image_buf) {
        /* The first rectangle of an image: paper to draw onto. */
        s_image_buf = (uint16_t *)heap_caps_malloc((size_t)g_w * g_h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_image_buf) {
            xSemaphoreGive(s_image_mutex);
            return false;
        }
        for (size_t i = 0; i < (size_t)g_w * g_h; i++) s_image_buf[i] = 0xFFFF;
        muse_state_set_asleep(false);
    }
    const uint8_t *src = (const uint8_t *)pixels;
    for (int row = 0; row < h; row++) {
        uint16_t *dst = s_image_buf + (size_t)(y + row) * g_w + x;
        for (int i = 0; i < w; i++, src += 2) dst[i] = (uint16_t)(src[0] << 8 | src[1]);
    }
    xSemaphoreGive(s_image_mutex);
    return true;
}

extern "C" void muse_ui_image_done(void)
{
    /* E-paper shows the image whole, once it's all here. */
    if (!s_ready) return;
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (s_image_buf) {
        dither_image();
        s_image_new = s_image_bits != nullptr;
    }
    xSemaphoreGive(s_image_mutex);
    request_draw();
}

extern "C" void muse_ui_image_hide(void)
{
    if (s_ready) image_hide();
}

extern "C" void muse_ui_camera_hint(bool visible)
{
    (void)visible;   /* the Watcher's camera only */
}

extern "C" void muse_ui_request_snapshot(void)
{
    muse_console_write("\nSNAP OFF\n", 10);   /* so snap.py can say why */
}

/* The button menu is for boards without touch; FreeInk boards have Settings. */
extern "C" void muse_menu_key(muse_menu_key_t key)
{
    (void)key;
}

extern "C" bool muse_menu_is_open(void)
{
    return false;
}
