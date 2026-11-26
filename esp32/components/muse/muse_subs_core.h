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
#pragma once

/*
 * Live call subtitles (mg_command_subtitle,
 * protocols/mgcommands.h, Subtitles), the part with no IDF in it: a reply's
 * words as they come, when each is shown (the video frame on the panel that
 * carries its sound, or the phone's playout clock), word wrap into the band's
 * lines, and when the band goes. muse_subs.c holds one under a lock, feeds it
 * from the BLE worker and draws it; host tests drive it directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_SUBS_WORDS_MAX 128
#define MUSE_SUBS_POOL 1536            /* bytes of a reply's words kept (older ones scroll away) */
#define MUSE_SUBS_LINE_MAX 160         /* bytes of one laid-out line */
#define MUSE_SUBS_NOW 0xFFFFFFFFu      /* show_at: at once */
#define MUSE_SUBS_LINGER_MS 1500       /* the band stays this long after the reply's end */
#define MUSE_SUBS_LATE_MS 2000         /* a word with no time to judge it by (no frame, no clock) shows this long after it came */
#define MUSE_SUBS_IDLE_MS 12000        /* nothing new for this long (an end whose moment never came): the band goes */
#define MUSE_SUBS_NO_END_MS 3000       /* every word shown, no reply_end, nothing new for this long: as if it ended */
#define MUSE_SUBS_VIDEO_LINES 2        /* the band over video (<= 2 lines, the picture stays visible) */
#define MUSE_SUBS_TALL_MAX 24          /* the tall band's most lines (no video on screen) */
#define MUSE_SUBS_SCROLL_MS 150        /* lines that come at once scroll in one at a time, this far apart */
#define MUSE_SUBS_PAUSE_MAX_MS 1500    /* a talk press's pause that nothing ends lapses (the reply audio's own lapse) */

typedef enum { MUSE_SUBS_FRAME = 0, MUSE_SUBS_MS = 1 } muse_subs_timing_t;

typedef struct {
    uint32_t show_at;
    uint16_t pos;        /* in pool */
    uint8_t len;
    uint32_t off;        /* byte offset in the reply's text (words.char_offset based) */
    int64_t came_ms;
} muse_subs_word_t;

typedef struct {
    bool active;
    muse_subs_timing_t timing;
    bool have_reply;
    uint16_t reply_id;
    muse_subs_word_t words[MUSE_SUBS_WORDS_MAX];
    int nwords, revealed;
    char pool[MUSE_SUBS_POOL];
    int pool_len;
    uint32_t text_end;   /* the reply's text received so far, in bytes */
    bool ended;
    uint32_t end_at;
    int64_t end_seen_ms; /* the end's time came (0: not yet) */
    int64_t last_ms;     /* the last word came or showed */
    bool hidden;
    /* Where playback is: the frame on the panel (extended to 32 bits), the phone's clock. */
    bool have_frame;
    uint32_t frame;
    bool have_clock;
    uint32_t clock_ms;
    int64_t clock_at_ms;
    uint32_t gen;        /* bumps whenever what's shown changes */
    bool have_dropped;   /* the gadget's own talk press dropped this reply: its later words are ignored, until the
                          * next reply's first words or the mode going off */
    uint16_t dropped_id;
    uint32_t ignored;    /* words, reply_end and clock that came while subtitle mode was off (dropped) */
    /* The first word of the oldest line on screen (muse_subs_keep_from):
     * the words before it have scrolled away, and they're the ones dropped
     * first when the pool fills. A line's first word restarts the wrap where
     * it was, so the lines kept never reflow. */
    int keep_from;
    uint32_t drops;      /* times older words were dropped (the lines count from a new first word) */
    bool locked;         /* the passcode lock screen is up (muse_subs_lock) */
    bool paused;         /* a talk press's touch-down hides the band (muse_subs_pause) */
    int64_t paused_ms;
} muse_subs_t;

typedef struct {
    uint32_t show_at;
    const uint8_t *utf8;
    uint8_t len;
} muse_subs_in_word_t;

void muse_subs_init(muse_subs_t *s);
/* state: subtitle mode on (active) or off (the band goes), and the timing domain.
 * Only state on turns the mode on: words, reply_end
 * and clock while it's off are dropped and counted (ignored), never shown;
 * the device never turns subtitles on by itself. */
void muse_subs_state(muse_subs_t *s, bool active, muse_subs_timing_t timing);
/* words: flags bit0 final, bit1 replace (char_offset rewrites the tail). */
void muse_subs_words(muse_subs_t *s, uint16_t reply_id, uint8_t flags, uint16_t char_offset,
                     const muse_subs_in_word_t *w, int count, int64_t now_ms);
void muse_subs_reply_end(muse_subs_t *s, uint16_t reply_id, uint32_t show_at, int64_t now_ms);
/* clear: reply_id 0 = whatever is shown. */
void muse_subs_clear(muse_subs_t *s, uint16_t reply_id);
void muse_subs_clock(muse_subs_t *s, uint16_t reply_id, uint32_t playout_ms, int64_t now_ms);
/* The frame now on the panel (32-bit, counted from the video stream's start); no_frame: video stopped. */
void muse_subs_frame(muse_subs_t *s, uint32_t frame);
void muse_subs_no_frame(muse_subs_t *s);
/* The passcode lock screen is up (the caller shows nothing over it meanwhile): the words keep coming due, and a
 * reply whose reply_end moment comes while locked is dropped, never shown after the unlock (subtitle vector
 * locked_reply_ends_first). */
void muse_subs_lock(muse_subs_t *s, bool locked);
/* Reveals what's due and hides what's done. True if what's shown changed. */
bool muse_subs_update(muse_subs_t *s, int64_t now_ms);
bool muse_subs_visible(const muse_subs_t *s);
/* The band without video (muse_subs_tick): a text to show (front >= 0) and no video. Never while the passcode lock
 * is up (locked), whatever the last render left in front: the lock is asked at every tick, not only when the
 * model's refresh gets its mutex. */
bool muse_subs_band_shows(int front, bool video, bool locked);

/* Text width in pixels at the band's font. */
typedef int (*muse_subs_measure_t)(void *ctx, const char *utf8, int len);
/* The last max_lines lines of the shown words, wrapped at word boundaries to
 * max_w. No word is ever cut or ellipsized: one wider than a whole line
 * breaks across lines at character boundaries, with a hyphen after each piece
 * but the last. Lines oldest first; returns how many. */
int muse_subs_layout(const muse_subs_t *s, muse_subs_measure_t measure, void *ctx, int max_w,
                     char lines[][MUSE_SUBS_LINE_MAX], int max_lines);
/* As muse_subs_layout; *first_word (if not NULL): the word that starts the
 * oldest line returned (0 with none), for muse_subs_keep_from. */
int muse_subs_layout_from(const muse_subs_t *s, muse_subs_measure_t measure, void *ctx, int max_w,
                          char lines[][MUSE_SUBS_LINE_MAX], int max_lines, int *first_word);
/* The same lines (the last max_lines, oldest first, wrapped exactly as
 * muse_subs_layout wraps them) joined by '\n' into out (cap bytes with its
 * terminator; a line that doesn't fit is left out whole). No line buffers:
 * two passes over the words, for the tall band's many lines on a small
 * stack. *total (if not NULL): every line the shown words make (more than
 * returned once they scroll). Returns the lines written. */
int muse_subs_layout_joined(const muse_subs_t *s, muse_subs_measure_t measure, void *ctx, int max_w, int max_lines,
                            char *out, int cap, int *first_word, int *total);
/* How many lines the shown words make (muse_subs_layout's wrap). */
int muse_subs_line_count(const muse_subs_t *s, muse_subs_measure_t measure, void *ctx, int max_w);
/* As muse_subs_layout_joined, the max_lines lines that end before line
 * `end` (counted from the first word kept; < 0: every line). */
int muse_subs_layout_window(const muse_subs_t *s, muse_subs_measure_t measure, void *ctx, int max_w, int max_lines,
                            int end, char *out, int cap, int *first_word, int *total);

/* The tall band's scroll (older lines scroll up one line at a time, no
 * page jumps): how many of the lines (from the first word kept) it shows.
 * Words come at speech pace, a line at a time; when several lines come at
 * once (a burst after a stall, late words shown together) they still scroll
 * in one by one, MUSE_SUBS_SCROLL_MS apart. A drop of older words, or a
 * rewrite that shortens the text, shows every line at once. Zeroed: a new
 * band. */
typedef struct {
    int end;
    uint32_t drops;
    int64_t at_ms;
} muse_subs_scroll_t;
int muse_subs_scroll_end(muse_subs_scroll_t *sc, const muse_subs_t *s, int total, int64_t now_ms);
/* The words before `word` have scrolled off the band (the first word of the
 * oldest line shown): when the pool fills they go first, whole lines at a
 * time, so what's on screen never reflows. */
void muse_subs_keep_from(muse_subs_t *s, int word);

/* The band's place on a panel. The text box (the band less MUSE_SUBS_PAD_X
 * and MUSE_SUBS_PAD_Y) lies inside the visible area at its top and bottom
 * rows alike: inside the circle within the mic ring on a round panel, clear
 * of the rounded corners on a rectangular one. */
#define MUSE_SUBS_PAD_X 12
#define MUSE_SUBS_PAD_Y 6
#define MUSE_SUBS_GAP 6   /* between the band and the ring */
typedef struct {
    int x, y, w, h;
} muse_subs_band_t;
void muse_subs_place(int panel_w, int panel_h, bool round, int corner_px, int ring_px, int line_h, int lines,
                     muse_subs_band_t *out);

/* The tall band: with no video on screen the band grows up
 * from the bottom of the safe area. Its bottom, left edge and width are the
 * two-line band's (muse_subs_place), so the text wraps at one width over
 * video and without it, and on a round panel that width is the bottom
 * line's chord for every line: nothing reflows as lines scroll up. It may
 * grow until its top reaches top_y (the row just below the top row, or below
 * the clock where the screen shows one) and while its text box's top corners
 * stay inside the visible area (on a round panel: the circle within the
 * ring, so the top line is no wider than the chord allows); `lines` is how
 * many lines fit at line_h, never fewer than MUSE_SUBS_VIDEO_LINES and at
 * most MUSE_SUBS_TALL_MAX. */
typedef struct {
    int x, w;       /* as the two-line band */
    int bottom;     /* the band's bottom edge (exclusive): the two-line band's y + h */
    int line_h;
    int lines;      /* how many lines fit */
    int text_top;   /* the top line's top at that many lines */
} muse_subs_tall_t;
void muse_subs_place_tall(int panel_w, int panel_h, bool round, int corner_px, int ring_px, int line_h, int top_y,
                          muse_subs_tall_t *out);
/* The tall band holding n lines (1..t->lines): its rectangle, the bottom fixed. */
void muse_subs_tall_rect(const muse_subs_tall_t *t, int n, muse_subs_band_t *out);

/* The shown words' text, as sent (trailing spaces kept): the reveal model's output. */
int muse_subs_revealed_text(const muse_subs_t *s, char *out, int cap);
/* A 16-bit frame number extended to 32 bits by counting wraps (*ext holds the
 * last; reset: the first number of a stream starts it). */
uint32_t muse_subs_extend_frame(uint32_t *ext, bool *reset, uint16_t number);

/* The gadget's own push-to-talk turn (a barge-in): the band clears at once,
 * without waiting for the phone's clear, and words still coming for that
 * reply are ignored. */
void muse_subs_barge_in(muse_subs_t *s);
/* A talk press's touch-down (on): nothing is visible until it ends (off: a tap, the reply plays on) or lapses after
 * MUSE_SUBS_PAUSE_MAX_MS; the reply's words keep coming and coming due meanwhile, so the band comes back where
 * the reply now is. */
void muse_subs_pause(muse_subs_t *s, bool on, int64_t now_ms);

/* Valid UTF-8 copy of at most cap - 1 bytes (bad bytes become '?', never a split code point). */
int muse_subs_utf8_clean(char *dst, int cap, const uint8_t *src, int len);

/* The band's scrim: behind the text the picture is
 * darkened to 1/8 of its brightness on GUI screens (home, faces, menus) and to
 * at most 0.35 over video (nominally 3/8; 7:1 is the binding target, and 3/8
 * of white gives only ~6.3:1 in sRGB), so pure white text keeps >= 7:1 against
 * the brightest pixel behind it (3/8 over the avatar's white body made text
 * unreadable on a gadget). As a black backdrop's alpha: 255 * 7/8, and 168
 * (white to 0.341: 7.2:1). */
#define MUSE_SUBS_SCRIM_GUI 223
#define MUSE_SUBS_SCRIM_VIDEO 168

/* One RGB565 pixel under the band: darkened by bg (the backdrop's alpha,
 * 0..255), then white by fg (the text's coverage, 0..255). */
uint16_t muse_subs_blend565(uint16_t px, unsigned bg, unsigned fg);

#ifdef __cplusplus
}
#endif
