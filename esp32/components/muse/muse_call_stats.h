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
 * The call video's stats overlay (five taps show it, a single tap hides it),
 * as plain C the host tests run (tests/test_muse_call_stats.py); muse_call.c
 * draws it and feeds it the numbers muse_video and mg.play already keep.
 *
 *   Show   five taps on the call video, each within MUSE_VSTATS_TAP_GAP_MS of
 *          the last. The first does what it always did (the controls); taps
 *          2-4 do nothing; the fifth hides the controls and shows the overlay.
 *          A tap on a control button (End call, - / +) is the button's, and
 *          starts no run. No Developer mode needed.
 *   Hide   one tap while it's up, consumed (the controls stay as they were).
 *          Hidden at every call's start and end.
 *   Text   refreshed once a second, small fixed-width lines; a field a board
 *          doesn't measure is left out, never shown as 0:
 *            H.264 232x232>466x466   codec, coded size > shown size
 *            312 kb/s 24 fps         received over the last second, pictures shown
 *            err 0 drop 3 kf 1       decode errors, dropped frames, keyframe requests (call totals)
 *            link 15.00 ms active    connection interval, link mode
 *            reply 80 ms under 2     reply playback depth, underruns (call total)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_VSTATS_TAP_GAP_MS 400   /* a run's taps, each within this of the last */
#define MUSE_VSTATS_TAPS 5           /* the run that shows it */
#define MUSE_VSTATS_LINES 5
#define MUSE_VSTATS_COLS 24          /* characters a line holds, with its terminator: 23 shown */

/* ---- the taps ---- */

typedef enum {
    MUSE_VSTATS_TAP_NORMAL,    /* the tap does what it always did (the controls) */
    MUSE_VSTATS_TAP_SWALLOW,   /* taps 2-4 of a run: nothing */
    MUSE_VSTATS_TAP_SHOW,      /* the fifth: hide the controls, show the overlay */
    MUSE_VSTATS_TAP_HIDE,      /* it was up: hide it, nothing else */
} muse_vstats_tap_t;

typedef struct {
    bool shown;
    uint8_t taps;              /* the run so far */
    int64_t last_ms;           /* the run's last tap */
} muse_vstats_taps_t;

/* A tap on the call video at now_ms; on_button: it landed on a shown control
 * button (End call, - / +), which takes it. */
muse_vstats_tap_t muse_vstats_tap(muse_vstats_taps_t *t, int64_t now_ms, bool on_button);
/* A call starts or ends: hidden, no run. */
void muse_vstats_call_edge(muse_vstats_taps_t *t);

/* ---- the numbers ---- */

/* Cumulative counters as the call's sources keep them; the overlay takes
 * the last second's rates from two samples a second apart. */
typedef struct {
    int64_t t_ms;              /* when it was taken */
    bool video;                /* a video stream runs (else no video lines) */
    const char *codec;         /* "H.264", "VP8"; NULL: unknown */
    uint16_t coded_w, coded_h; /* the stream's picture (0: unknown) */
    uint16_t shown_w, shown_h; /* on the panel (0: unknown) */
    uint32_t rx_bytes;         /* video bytes received since the stream started */
    uint16_t shown;            /* pictures that reached the panel, since the stream started (mod 2^16) */
    uint32_t dec_errors, drops, kf_requests;   /* since the call started (the overlay subtracts its base) */
    bool link;                 /* a BLE link (else no link line) */
    uint16_t itvl_1250us;      /* its connection interval, 1.25 ms units (0: not reported) */
    const char *link_mode;     /* "sleep", "ready", "active"; NULL: no link modes */
    bool play;                 /* the gadget plays replies (a speaker) */
    uint32_t play_stream;      /* the reply stream (bumped by each start) */
    uint32_t play_underruns;   /* that stream's underruns */
    uint32_t play_depth_ms;    /* reply audio queued now, ms */
} muse_vstats_sample_t;

/* The overlay's memory across samples: the previous one, and the call's bases. */
typedef struct {
    muse_vstats_sample_t prev;
    bool have_prev;
    bool have_base;
    uint32_t base_dec_errors, base_drops, base_kf;
    uint32_t under_total;      /* underruns of the call's earlier reply streams */
    uint32_t under_stream;     /* the stream they were counted to */
    uint32_t under_last;       /* its count when last seen */
    uint32_t under_skip;       /* its count at the call's start (not the call's) */
} muse_vstats_acc_t;

/* Nothing known: the next sample is the call's base (and nothing before it). */
void muse_vstats_acc_reset(muse_vstats_acc_t *a);
/* A call starts: its totals (errors, drops, keyframe requests, underruns) count from sample s. */
void muse_vstats_acc_base(muse_vstats_acc_t *a, const muse_vstats_sample_t *s);
/* The overlay opens (mid-call too): the call's totals stay, the rates start over. */
void muse_vstats_acc_open(muse_vstats_acc_t *a);

/* The lines for sample s (and the one before it in a), into lines; returns
 * how many (0..MUSE_VSTATS_LINES). Takes s as the next one's previous. */
int muse_vstats_lines(muse_vstats_acc_t *a, const muse_vstats_sample_t *s, char lines[][MUSE_VSTATS_COLS]);

/* ---- where it goes ---- */

typedef struct {
    int x, y, w, h;
} muse_vstats_box_t;

/* The overlay's box for a panel w x h (round, or corner_px rounded corners),
 * clear of the mic ring (ring px at the edge): centered, as high as the
 * panel's shape lets a box of box_w x box_h sit ring + margin inside it (the
 * subtitles' band is at the bottom). False if it can't fit in the top
 * half. */
bool muse_vstats_place(int w, int h, bool round, int corner_px, int ring, int margin, int box_w, int box_h,
                       muse_vstats_box_t *out);

#ifdef __cplusplus
}
#endif
