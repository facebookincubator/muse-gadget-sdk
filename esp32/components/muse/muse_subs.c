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

/* Live call subtitles on the screen (muse_subs.h). */

#include "muse_subs.h"
#include "muse_video.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "muse_board.h"
#include "muse_home.h"
#include "muse_input.h"
#include "muse_mic_ring.h"
#include "muse_passcode.h"
#include "muse_subs_core.h"

static const char *TAG = "muse_subs";
/* Mode and band transitions, on a tag the Nordic UART mirror carries (mg*):
 * a Mac client sees when the band shows and goes.
 * Never the words themselves. */
static const char *TAG_BAND = "mg.subs";

#define LINES MUSE_SUBS_VIDEO_LINES   /* over video; without video the tall band (s_tall) */
#define PAD_X MUSE_SUBS_PAD_X
#define PAD_Y MUSE_SUBS_PAD_Y
#define RADIUS 12      /* the backdrop's corners */
/* Without video every board draws the tall band the same way:
 * an LVGL rounded panel at the GUI scrim with one label holding its lines,
 * no pixel buffer (the lines as text only while LVGL's tick lays them out). */
/* The backdrop: its shape's coverage (0..255) in s_bg; how dark it makes
 * the picture is the scrim, per screen (MUSE_SUBS_SCRIM_GUI / _VIDEO). */

static SemaphoreHandle_t s_lock;
static muse_subs_t s_m;
static const lv_font_t *s_font;
static int s_bx, s_by, s_bw, s_bh;   /* the band on the panel */
static int s_line_h;                  /* a line's height */
static uint8_t *s_text[2];            /* over video: the text's coverage, the band's size: front and back (A8) */
static uint8_t *s_bg;                 /* A8 backdrop alpha (rounded rectangle) */
static volatile int s_front = -1;     /* the text drawn (-1: none shown) */
static volatile uint32_t s_render_gen;   /* every render_locked (muse_subs_overlay_rect's gen) */
static volatile bool s_video;         /* frames are going to the panel */
static bool s_front_video;            /* the front was drawn for video (the pieces' lines); else only the tall band's */
static uint32_t s_shown_gen = ~0u;
static mg_subs_caps_t s_caps;
static bool s_ready;
/* Without video: the tall band, an LVGL panel and one label. */
static lv_obj_t *s_obj;
static lv_obj_t *s_tall_lbl;
static muse_subs_tall_t s_tall;       /* where it may grow (from the clock's state when placed) */
static bool s_tall_placed, s_tall_clock;
static int s_tall_n;                  /* the lines it holds now: grows as words come, never shrinks mid-reply */
static uint32_t s_tall_gen = ~0u;
static muse_subs_scroll_t s_scroll;   /* lines that come at once scroll in one by one */
static bool s_scroll_due;             /* lines still waiting their turn: lay out again next tick */
static int s_obj_front = -2;
static bool s_auto_shot, s_auto_pending;   /* bench >shot.auto: the frame with a reply's last words */
static volatile bool s_band_on;       /* what the last band log said: the band is on screen */
static const char *s_off_why;         /* why the band goes next: state 0, clear, call end, talk press */
static bool s_bench_on;               /* the mode came from the console's >sub=, not the phone */
static bool s_from_bench;             /* muse_subs_bench's own events are being handled */
static uint32_t s_ignored_logged;     /* s_m.ignored when last seen */
static uint32_t s_spell_ignored;      /* ignored since the mode last went off: the log's limiter counts these */
static bool s_proven;                 /* the words on the band came from a proven central (or the console) */
/* A card that asks for a press or a code is up (muse_subs_card): the pairing confirm, the update prompt, the
 * re-setup prompt or a passcode keypad. Nothing of the band draws over it. LVGL's task writes it. */
static volatile bool s_card;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ---- glyphs ------------------------------------------------------------- */

static uint32_t next_cp(const char *s, int n, int *i)
{
    const uint8_t *p = (const uint8_t *)s;
    uint8_t c = p[*i];
    int k = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    if (*i + k > n) {
        (*i)++;
        return '?';
    }
    uint32_t cp = k == 1 ? c : k == 2 ? c & 0x1F : k == 3 ? c & 0x0F : c & 0x07;
    for (int j = 1; j < k; j++) {
        cp = cp << 6 | (p[*i + j] & 0x3F);
    }
    *i += k;
    return cp;
}

/* A code point the font lacks, as one it has: typographic punctuation to ASCII. */
static uint32_t fallback_cp(uint32_t cp)
{
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2026: return '.';
    case 0x00A0: case 0x2009: case 0x200A: case 0x202F: return ' ';
    default: return '?';
    }
}

static bool glyph(uint32_t cp, uint32_t next, lv_font_glyph_dsc_t *g)
{
    memset(g, 0, sizeof(*g));
    if (lv_font_get_glyph_dsc(s_font, g, cp, next) && g->resolved_font == s_font) {
        return true;
    }
    memset(g, 0, sizeof(*g));
    return lv_font_get_glyph_dsc(s_font, g, fallback_cp(cp), next) && g->resolved_font == s_font;
}

static int measure(void *ctx, const char *s, int n)
{
    (void)ctx;
    int w = 0;
    for (int i = 0; i < n;) {
        uint32_t cp = next_cp(s, n, &i);
        int j = i;
        uint32_t nx = i < n ? next_cp(s, n, &j) : 0;
        lv_font_glyph_dsc_t g;
        if (glyph(cp, nx, &g)) {
            w += g.adv_w;
        }
    }
    return w;
}

static int bpp_of(lv_font_glyph_format_t f)
{
    switch (f) {
    case LV_FONT_GLYPH_FORMAT_A1: return 1;
    case LV_FONT_GLYPH_FORMAT_A2: return 2;
    case LV_FONT_GLYPH_FORMAT_A4: return 4;
    case LV_FONT_GLYPH_FORMAT_A8: return 8;
    default: return 0;
    }
}

/* One line of text into an A8 buffer of the band's size, its top at y. */
static void draw_line(uint8_t *buf, int x0, int y, const char *s)
{
    int n = (int)strlen(s);
    int x = x0;
    for (int i = 0; i < n;) {
        uint32_t cp = next_cp(s, n, &i);
        int j = i;
        uint32_t nx = i < n ? next_cp(s, n, &j) : 0;
        lv_font_glyph_dsc_t g;
        if (!glyph(cp, nx, &g)) {
            continue;
        }
        int bpp = bpp_of(g.format);
        if (bpp && g.box_w && g.box_h) {
            /* The font's own bits (lv_font_get_glyph_bitmap would expand them into an A8 draw buffer). */
            g.req_raw_bitmap = 1;
            const uint8_t *bm = g.resolved_font->get_glyph_bitmap ? g.resolved_font->get_glyph_bitmap(&g, NULL) : NULL;
            int gx = x + g.ofs_x;
            int gy = y + (s_font->line_height - s_font->base_line) - g.box_h - g.ofs_y;
            int stride_bits = g.stride ? g.stride * 8 : g.box_w * bpp;
            int maxv = (1 << bpp) - 1;
            for (int r = 0; bm && r < g.box_h; r++) {
                int py = gy + r;
                if (py < 0 || py >= s_bh) {
                    continue;
                }
                for (int c = 0; c < g.box_w; c++) {
                    int px = gx + c;
                    if (px < 0 || px >= s_bw) {
                        continue;
                    }
                    int bit = r * stride_bits + c * bpp;
                    int v = (bm[bit >> 3] >> (8 - bpp - (bit & 7))) & maxv;
                    uint8_t a = (uint8_t)(v * 255 / maxv);
                    uint8_t *d = &buf[py * s_bw + px];
                    if (a > *d) {
                        *d = a;
                    }
                }
            }
        }
        x += g.adv_w;
    }
}

/* ---- the band ----------------------------------------------------------- */

static void geometry(const muse_board_t *b)
{
    int w = b->width, h = b->height;
    int s = w < h ? w : h;
    int ring = muse_mic_ring_width(w, h);
#if CONFIG_LV_FONT_MONTSERRAT_28
    s_font = s >= 360 ? &lv_font_montserrat_28 : NULL;
#endif
#if CONFIG_LV_FONT_MONTSERRAT_20
    if (!s_font && s >= 240) {
        s_font = &lv_font_montserrat_20;
    }
#endif
    if (!s_font) {
        s_font = LV_FONT_DEFAULT;
    }
    s_line_h = s_font->line_height;
    muse_subs_band_t band;
    muse_subs_place(w, h, b->round, b->corner_px, ring, s_line_h, LINES, &band);
    s_bx = band.x;
    s_by = band.y;
    s_bw = band.w;
    s_bh = band.h;
}

/* The tall band's top limit: just below the top row, or below the
 * clock while the screen shows it (the time stays visible). The home's own
 * geometry (muse_home_geom), the ink of its top row and digits. */
static int tall_top(const muse_board_t *b, bool clock)
{
    muse_home_geom_t g;
    muse_home_geom(b->width, b->height, b->round, b->corner_px, &g);
    return (clock ? g.time_top + g.time_digits : g.batt_top + g.batt_h) + MUSE_SUBS_GAP;
}

static void place_tall(bool clock)
{
    const muse_board_t *b = muse_board ? muse_board : muse_board_get();
    muse_subs_place_tall(b->width, b->height, b->round, b->corner_px, muse_mic_ring_width(b->width, b->height),
                         s_line_h, tall_top(b, clock), &s_tall);
    s_tall_placed = true;
    s_tall_clock = clock;
}

static void make_backdrop(void)
{
    for (int y = 0; y < s_bh; y++) {
        for (int x = 0; x < s_bw; x++) {
            int dx = x < RADIUS ? RADIUS - x : x >= s_bw - RADIUS ? x - (s_bw - RADIUS - 1) : 0;
            int dy = y < RADIUS ? RADIUS - y : y >= s_bh - RADIUS ? y - (s_bh - RADIUS - 1) : 0;
            float d = sqrtf((float)(dx * dx + dy * dy));
            float a = d <= RADIUS - 1 ? 1.0f : d >= RADIUS ? 0.0f : RADIUS - d;
            s_bg[y * s_bw + x] = (uint8_t)(a * 255.0f + 0.5f);
        }
    }
}

bool muse_subs_band_shown(void)
{
    return s_band_on;
}

bool muse_subs_band_holds(void)
{
    return s_band_on && s_proven;
}

/* One line per band transition (shown or gone), with who turned it on or why it went. */
static void log_band_locked(bool on)
{
    if (on == s_band_on) {
        return;
    }
    s_band_on = on;
    if (on) {
        s_off_why = NULL;
        ESP_LOGI(TAG_BAND, "band on (%s)", s_bench_on ? "bench" : "client");
    } else {
        ESP_LOGI(TAG_BAND, "band off (%s)", s_off_why ? s_off_why : "reply end");
        s_off_why = NULL;
    }
}

/* Lays out what's shown and draws it into the back buffer; flips. Lock held by the caller. */
static void render_locked(void)
{
    s_render_gen++;
    /* Never over the passcode's screens: a locked gadget, and so a
     * phone's call held behind the keypad, shows no text until it's unlocked. */
    if (!muse_subs_visible(&s_m) || muse_passcode_locked()) {
        s_front = -1;
        return;
    }
    int back = s_front == 0 ? 1 : 0;
    s_front_video = s_video;
    if (!s_video) {
        /* No video: LVGL's tick lays the tall band out (tall_tick); the
         * pieces' lines wait for video (update draws them when it starts). */
        s_front = back;
        return;
    }
    char lines[LINES][MUSE_SUBS_LINE_MAX];
    int first = 0;
    int n = muse_subs_layout_from(&s_m, measure, NULL, s_bw - 2 * PAD_X, lines, LINES, &first);
    muse_subs_keep_from(&s_m, first);
    uint8_t *buf = s_text[back];
    memset(buf, 0, (size_t)s_bw * s_bh);
    /* The newest line at the bottom. */
    for (int i = 0; i < n; i++) {
        int y = PAD_Y + (LINES - n + i) * s_line_h;
        int tw = measure(NULL, lines[i], (int)strlen(lines[i]));
        draw_line(buf, (s_bw - tw) / 2, y, lines[i]);
    }
    s_front = back;
}

/* After a message: what's due shows and what went goes now, not at the next
 * frame or tick (a state 0 mid-reply clears the band at once: the next piece
 * of video drawn, or LVGL's next frame, has none). Lock held. */
static void refresh_locked(int64_t now)
{
    static bool was_locked;
    bool locked = muse_passcode_locked();
    if (locked != was_locked) {
        was_locked = locked;
        s_shown_gen = ~0u;   /* hidden at the lock, the latest shown at the unlock */
    }
    muse_subs_lock(&s_m, locked);   /* a reply that ends while locked is dropped */
    muse_subs_update(&s_m, now);
    if (s_front >= 0 && s_video && !s_front_video) {
        s_shown_gen = ~0u;   /* video started under the band: its lines for the pieces */
    }
    if (s_m.gen != s_shown_gen) {
        s_shown_gen = s_m.gen;
        render_locked();
    }
    log_band_locked(s_front >= 0);   /* after the render's frame is gone: the show task's stack is 3 KB */
}

static void update(int64_t now)
{
    if (!s_ready || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;   /* busy with a message: next time */
    }
    refresh_locked(now);
    if (s_auto_pending && s_front >= 0 && s_m.end_seen_ms && s_m.revealed == s_m.nwords) {
        s_auto_pending = false;
#if CONFIG_MUSE_VIDEO
        muse_video_shot_arm();   /* the reply's whole text is on the band: the next frame */
#endif
    }
    xSemaphoreGive(s_lock);
}

/* ---- the API ------------------------------------------------------------ */

static bool s_geom;

/* The band's place and the capabilities, from the board alone (the BLE side may ask before the UI is up). */
static bool setup_geometry(void)
{
    if (s_geom) {
        return true;
    }
    const muse_board_t *b = muse_board ? muse_board : muse_board_get();
    if (!b || b->width < 200 || b->height < 200) {
        return false;
    }
    geometry(b);
    int avg = measure(NULL, "abcdefghijklmnopqrstuvwxyz ", 27) / 27;
    s_caps = (mg_subs_caps_t){
        .flags = (1u << 3)   /* ms timing */
#if CONFIG_MUSE_VIDEO
                 | (1u << 2) /* frame timing */
#endif
        ,
        .lines = LINES,
        .chars_per_line = (uint8_t)((s_bw - 2 * PAD_X) / (avg > 0 ? avg : 10)),
        .max_words_bytes = 480,
    };
    s_geom = true;
    return true;
}

/* Without video, on every board: a rounded panel at the GUI scrim on the
 * system layer, below the mic ring, and one label for its lines (centered,
 * newest at the bottom), sized by tall_tick. */
static void build_tall(void)
{
    s_obj = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_obj);
    lv_obj_set_pos(s_obj, s_bx, s_by);
    lv_obj_set_size(s_obj, s_bw, s_bh);
    lv_obj_set_style_bg_color(s_obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_obj, (lv_opa_t)MUSE_SUBS_SCRIM_GUI, 0);   /* GUI screens: 1/8 */
    lv_obj_set_style_radius(s_obj, RADIUS, 0);
    lv_obj_remove_flag(s_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_tall_lbl = lv_label_create(s_obj);
    lv_obj_set_style_text_font(s_tall_lbl, s_font, 0);
    lv_obj_set_style_text_color(s_tall_lbl, lv_color_white(), 0);   /* pure white */
    lv_obj_set_style_text_align(s_tall_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_tall_lbl, 0, 0);
    /* The lines come wrapped (muse_subs_layout_joined, the band's width less
     * its padding): the label, the band's full width, never wraps them again. */
    lv_label_set_long_mode(s_tall_lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(s_tall_lbl, s_bw);
    lv_obj_set_pos(s_tall_lbl, 0, PAD_Y);
    lv_label_set_text_static(s_tall_lbl, "");
    lv_obj_add_flag(s_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_background(s_obj);   /* the ring stays on top */
}

static int tall_lines(bool clock)
{
    place_tall(clock);
    int n = s_tall.lines;
    s_tall_placed = false;
    return n;
}

void muse_subs_ui_init(void)
{
    if (s_ready || !setup_geometry()) {
        return;
    }
    size_t n = (size_t)s_bw * s_bh;
    s_text[0] = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_text[1] = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_bg = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock = xSemaphoreCreateMutex();
    if (!s_text[0] || !s_text[1] || !s_bg || !s_lock) {
        ESP_LOGW(TAG, "no memory for subtitles");
        return;
    }
    /* Both text bitmaps start blank: a band published without video (render_locked) leaves its bitmap
     * unwritten, and the first video piece may read it before a render for video. */
    memset(s_text[0], 0, n);
    memset(s_text[1], 0, n);
    make_backdrop();
    muse_subs_init(&s_m);
    build_tall();
    s_ready = true;
    ESP_LOGI(TAG, "subtitles: band %dx%d at %d,%d, %d lines of ~%u characters over video (%d px font); "
             "without video up to %d lines (%d below the clock)", s_bw, s_bh, s_bx, s_by, LINES,
             s_caps.chars_per_line, s_line_h, tall_lines(false), tall_lines(true));
}

bool muse_subs_ready(void) { return s_ready; }

const mg_subs_caps_t *muse_subs_caps(void)
{
    return setup_geometry() ? &s_caps : NULL;
}

void muse_subs_event(const mg_subs_event_t *ev)
{
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = now_ms();
    switch (ev->sub) {
    case 2:
        if (!ev->active && muse_subs_visible(&s_m)) {
            s_off_why = "state 0";
        }
        if (!ev->active) {
            s_spell_ignored = 0;   /* a new off-spell: its first ignored messages log again */
        }
        if (ev->active != s_m.active) {
            if (ev->active) {
                s_bench_on = s_from_bench;
            }
            ESP_LOGI(TAG_BAND, "state %d (%s timing%s)", ev->active ? 1 : 0, ev->timing ? "ms" : "frame",
                     s_from_bench ? ", bench" : "");
        }
        muse_subs_state(&s_m, ev->active, ev->timing ? MUSE_SUBS_MS : MUSE_SUBS_FRAME);
        if (ev->active) {
            s_proven = ev->proven;
        }
        if (ev->active && ev->proven && !muse_passcode_locked()) {
            /* A muted reply arrives as subtitles: the user has to see them, and
             * a sleeping screen draws nothing (LVGL and the band are paused).
             * The owner's state 1 wakes it, as a reply's audio would be heard.
             * Never while the passcode lock is on: captions don't draw over it.
             * An unproven central's never does: its words show only on a
             * screen that's already on. */
            muse_input_wake("subtitles");
        }
        break;
    case 3: {
        /* Only state 1 turns subtitles on: words while the mode is
         * off are dropped (muse_subs_words counts them), never a band. */
        muse_subs_in_word_t w[MG_SUBS_MAX_WORDS];
        for (int i = 0; i < ev->count; i++) {
            w[i].show_at = ev->words[i].show_at;
            w[i].utf8 = ev->words[i].utf8;
            w[i].len = ev->words[i].len;
        }
        bool active = s_m.active;
        if (active) {
            s_proven = ev->proven;   /* a band an unproven central fills never holds the panel */
        }
        if (active && ev->proven) {
            /* Keeps a lit screen on while the reply's words come; never
             * wakes a dark one (one the user darkened since stays dark).
             * Only the owner's: an unproven central's words never reset the
             * idle timer (auto-sleep, the passcode's lock after idle). */
            muse_input_keep_lit("subtitles");
        }
        muse_subs_words(&s_m, ev->reply_id, ev->flags, ev->char_offset, w, ev->count, now);
        if (active && (s_m.nwords <= ev->count || ev->flags & 0x02)) {
            s_auto_pending = s_auto_shot;
            /* A reply's first words (or a rewrite): where the reveal stands. */
            ESP_LOGI(TAG, "reply %u: %d word(s) from byte %u, first show_at %u, frame on panel %u (%s timing)",
                     ev->reply_id, ev->count, ev->char_offset, ev->count ? (unsigned)ev->words[0].show_at : 0,
                     (unsigned)s_m.frame, s_m.timing == MUSE_SUBS_MS ? "ms" : "frame");
        }
        break;
    }
    case 4:
        muse_subs_reply_end(&s_m, ev->reply_id, ev->show_at, now);
        if (s_m.active) {
            ESP_LOGI(TAG, "reply %u ends at %u: %d of %d word(s) shown, frame on panel %u, band %s", ev->reply_id,
                     (unsigned)ev->show_at, s_m.revealed, s_m.nwords, (unsigned)s_m.frame, s_front >= 0 ? "on" : "off");
        }
        break;
    case 5:
        if (muse_subs_visible(&s_m) && (ev->reply_id == 0 || ev->reply_id == s_m.reply_id)) {
            s_off_why = "clear";
        }
        muse_subs_clear(&s_m, ev->reply_id);
        break;
    case 6:
        muse_subs_clock(&s_m, ev->reply_id, ev->playout_ms, now);
        break;
    }
    if (s_m.ignored != s_ignored_logged) {
        /* The first three of each off-spell (since a state 0, a call's end
         * or boot), then every 50th: a client that keeps sending while the
         * mode is off doesn't flood the log, and a bench sees each spell. */
        s_spell_ignored += s_m.ignored - s_ignored_logged;
        s_ignored_logged = s_m.ignored;
        if (s_spell_ignored <= 3 || s_spell_ignored % 50 == 0) {
            ESP_LOGW(TAG_BAND, "ignored %s: subtitles off (%u this spell, %u in all)",
                     ev->sub == 3 ? "words" : ev->sub == 4 ? "reply_end" : "clock", (unsigned)s_spell_ignored,
                     (unsigned)s_m.ignored);
        }
    }
    if (ev->sub == 2 || ev->sub == 5) {
        refresh_locked(now);   /* state 0 and clear take the band away now; words show with their frame or tick */
    }
    xSemaphoreGive(s_lock);
}

void muse_subs_call_end(void)
{
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Subtitle mode is per call (mgcommands.h, Live call subtitles): it
     * ends with the call, so the next call shows nothing until its own
     * state 1 (the device never carries it over or turns it on itself). */
    if (muse_subs_visible(&s_m)) {
        s_off_why = "call end";
    }
    if (s_m.active) {
        ESP_LOGI(TAG_BAND, "state 0 (call end)");
    }
    muse_subs_state(&s_m, false, s_m.timing);
    s_spell_ignored = 0;
    refresh_locked(now_ms());
    xSemaphoreGive(s_lock);
}

void muse_subs_video_start(void)
{
    s_video = false;
}

void muse_subs_frame_shown(uint32_t frame32)
{
    if (!s_ready) {
        return;
    }
    s_video = true;
    if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
        muse_subs_frame(&s_m, frame32);
        xSemaphoreGive(s_lock);
    }
    update(now_ms());
}

void muse_subs_video_yield(void)
{
    s_video = false;   /* the frame-timing state stays: the next picture's frame number carries on */
}

void muse_subs_video_stop(void)
{
    s_video = false;
    if (s_ready && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        muse_subs_no_frame(&s_m);
        xSemaphoreGive(s_lock);
    }
}

static inline void blend_px(uint8_t *d, unsigned bg, unsigned fg)
{
    if (!bg && !fg) {
        return;
    }
    /* Drawn straight to the panel: over video (and UI streams), the 3/8 scrim. */
    unsigned v = muse_subs_blend565((uint16_t)((unsigned)d[0] << 8 | d[1]), bg * MUSE_SUBS_SCRIM_VIDEO / 255, fg);
    d[0] = (uint8_t)(v >> 8);
    d[1] = (uint8_t)v;
}


void muse_subs_overlay(uint8_t *dst, int stride, int x, int y, int w, int rows)
{
    int f = s_front;
    if (f < 0 || !s_ready || muse_passcode_locked() || s_card) {
        return;
    }
    int r0 = s_by - y, r1 = r0 + s_bh;
    r0 = r0 < 0 ? 0 : r0;
    r1 = r1 > rows ? rows : r1;
    int c0 = s_bx - x, c1 = c0 + s_bw;
    c0 = c0 < 0 ? 0 : c0;
    c1 = c1 > w ? w : c1;
    const uint8_t *text = s_text[f];
    for (int r = r0; r < r1; r++) {
        int by = y + r - s_by;
        const uint8_t *bg = s_bg + (size_t)by * s_bw;
        const uint8_t *tx = text + (size_t)by * s_bw;
        uint8_t *d = dst + (size_t)r * stride + (size_t)c0 * 2;
        for (int c = c0; c < c1; c++, d += 2) {
            int bx = x + c - s_bx;
            blend_px(d, bg[bx], tx[bx]);
        }
    }
}

bool muse_subs_overlay_rect(muse_board_win_t *r, uint32_t *gen)
{
    int f = s_front;
    bool card = s_card;
    if (gen) {
        *gen = (s_render_gen * 4u + (uint32_t)(f + 1)) * 2u + (card ? 1u : 0u);   /* a card's coming or going too */
    }
    if (f < 0 || !s_ready || muse_passcode_locked() || card) {
        return false;
    }
    *r = (muse_board_win_t){ (int16_t)s_bx, (int16_t)s_by, (int16_t)s_bw, (int16_t)s_bh };
    return true;
}

/* The tall band (no video on screen), LVGL's task: the last
 * lines that fit, wrapped at the bottom line's width (one width for every
 * line, so nothing reflows as they scroll up one at a time), newest at the
 * bottom; the band grows up from the bottom as lines come, to the top row
 * or, while the screen shows it, the clock. Laid out again only when what's
 * shown changed or the clock came or went. */
static void tall_tick(int f)
{
    bool clock = muse_home_clock_shown();
    if (!s_scroll_due && f == s_obj_front && s_m.gen == s_tall_gen && s_tall_placed && clock == s_tall_clock) {
        return;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;   /* busy with a message: next tick */
    }
    if (!s_tall_placed || clock != s_tall_clock) {
        place_tall(clock);
    }
    /* The lines as one text only while they're laid out (LVGL copies it):
     * no buffer kept, and this one fits every line the pool's words make. */
    size_t cap = (size_t)s_m.pool_len + (size_t)s_m.nwords + (size_t)s_tall.lines + 1;
    char *txt = malloc(cap);
    if (!txt) {
        xSemaphoreGive(s_lock);
        return;   /* next tick */
    }
    int first = 0;
    int total = muse_subs_line_count(&s_m, measure, NULL, s_bw - 2 * PAD_X);
    int end = muse_subs_scroll_end(&s_scroll, &s_m, total, now_ms());   /* no page jumps */
    s_scroll_due = end < total;
    int n = muse_subs_layout_window(&s_m, measure, NULL, s_bw - 2 * PAD_X, s_tall.lines, end, txt, (int)cap, &first,
                                    NULL);
    muse_subs_keep_from(&s_m, first);
    s_tall_gen = s_m.gen;
    xSemaphoreGive(s_lock);
    if (n > s_tall_n) {
        s_tall_n = n;   /* grows as lines come, never shrinks mid-reply */
    }
    if (s_tall_n > s_tall.lines) {
        s_tall_n = s_tall.lines;   /* the clock came back: below it */
    }
    if (s_tall_n < 1) {
        s_tall_n = 1;
    }
    muse_subs_band_t r;
    muse_subs_tall_rect(&s_tall, s_tall_n, &r);
    lv_obj_set_pos(s_obj, r.x, r.y);
    lv_obj_set_size(s_obj, r.w, r.h);
    lv_label_set_text(s_tall_lbl, txt);   /* (copied) */
    free(txt);
    /* Newest at the bottom: fewer lines than the band holds (a rewrite) sit low. */
    lv_obj_set_pos(s_tall_lbl, 0, PAD_Y + (s_tall_n - (n > 0 ? n : 0)) * s_line_h);
}

void muse_subs_tick(void)
{
    if (!s_ready) {
        return;
    }
    if (!s_video) {
        update(now_ms());
    }
    int f = s_front;
    bool show = muse_subs_band_shows(f, s_video, muse_passcode_locked()) && !s_card;   /* never over a card */
    if (show) {
        tall_tick(f);
    } else {
        s_tall_n = 0;   /* the next band starts small again */
        memset(&s_scroll, 0, sizeof(s_scroll));
        s_scroll_due = false;
    }
    s_obj_front = show ? f : -2;
    if (lv_obj_has_flag(s_obj, LV_OBJ_FLAG_HIDDEN) == show) {
        lv_obj_set_flag(s_obj, LV_OBJ_FLAG_HIDDEN, !show);
    }
}

void muse_subs_bench(const char *text, int ms_per_word)
{
    if (!s_ready) {
        return;
    }
    static uint16_t reply = 1000;
    reply++;
    mg_subs_word_t words[MG_SUBS_MAX_WORDS];
    int count = 0;
    const char *p = text;
    while (*p && count < MG_SUBS_MAX_WORDS) {
        const char *e = p;
        while (*e && *e != ' ') {
            e++;
        }
        while (*e == ' ') {
            e++;
        }
        int len = (int)(e - p);
        words[count].utf8 = (const uint8_t *)p;
        words[count].len = (uint8_t)(len > 255 ? 255 : len);
        words[count].show_at = ms_per_word > 0 ? (uint32_t)(count * ms_per_word) : MUSE_SUBS_NOW;
        count++;
        p = e;
    }
    mg_subs_event_t st = { .sub = 2, .active = 1, .timing = 1, .proven = true };   /* the console: the owner's */
    s_from_bench = true;   /* logged as the bench's, not the phone's */
    muse_subs_event(&st);
    mg_subs_event_t clk = { .sub = 6, .reply_id = reply, .playout_ms = 0, .proven = true };
    mg_subs_event_t w = { .sub = 3, .reply_id = reply, .flags = 1, .count = (uint8_t)count, .words = words,
                          .proven = true };
    muse_subs_event(&w);
    muse_subs_event(&clk);
    s_from_bench = false;
    /* No reply_end: the band stays until >sub.clear, or MUSE_SUBS_IDLE_MS after its last word (time to look). */
}

void muse_subs_auto_shot(bool on)
{
    s_auto_shot = on;
    s_auto_pending = false;
}

void muse_subs_card(bool up)
{
    if (up == s_card) {
        return;
    }
    s_card = up;
    if (up && s_obj && !lv_obj_has_flag(s_obj, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_obj, LV_OBJ_FLAG_HIDDEN);   /* now, not at the next tick: the card is never under it */
    }
    if (up && s_ready) {
        /* An unproven central's words go for good as the card comes up (the update prompt, the re-setup prompt):
         * no lure written beside a prompt comes back when it goes. The owner's come back. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_proven && s_m.have_reply) {
            if (muse_subs_visible(&s_m)) {
                s_off_why = "card";
            }
            muse_subs_clear(&s_m, 0);
            refresh_locked(now_ms());
        }
        xSemaphoreGive(s_lock);
    }
}

bool muse_subs_band_drawn(void)
{
    if (!s_ready) {
        return false;
    }
    if (s_obj && !lv_obj_has_flag(s_obj, LV_OBJ_FLAG_HIDDEN)) {
        return true;   /* the tall band, on the system layer above every card */
    }
    return s_front >= 0 && s_video && !s_card && !muse_passcode_locked();   /* blended into the video */
}

void muse_subs_talk_pause(bool on)
{
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (on && muse_subs_visible(&s_m)) {
        s_off_why = "talk press";
    }
    muse_subs_pause(&s_m, on, now_ms());
    refresh_locked(now_ms());
    xSemaphoreGive(s_lock);
}

void muse_subs_talk_press(void)
{
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool shown = muse_subs_visible(&s_m);
    if (shown) {
        s_off_why = "talk press";
    }
    muse_subs_barge_in(&s_m);
    refresh_locked(now_ms());
    xSemaphoreGive(s_lock);
    if (shown) {
        ESP_LOGI(TAG, "talk press: band cleared");
    }
}
