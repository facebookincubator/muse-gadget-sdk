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

/* The call video's stats overlay, its rules (muse_call_stats.h). */

#include "muse_call_stats.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

muse_vstats_tap_t muse_vstats_tap(muse_vstats_taps_t *t, int64_t now_ms, bool on_button)
{
    if (t->shown) {
        t->shown = false;   /* one tap hides it, and is consumed */
        t->taps = 0;
        return MUSE_VSTATS_TAP_HIDE;
    }
    if (on_button) {
        t->taps = 0;   /* the button's tap: no run */
        return MUSE_VSTATS_TAP_NORMAL;
    }
    if (t->taps && now_ms - t->last_ms <= MUSE_VSTATS_TAP_GAP_MS) {
        t->taps++;
    } else {
        t->taps = 1;
    }
    t->last_ms = now_ms;
    if (t->taps >= MUSE_VSTATS_TAPS) {
        t->taps = 0;
        t->shown = true;
        return MUSE_VSTATS_TAP_SHOW;
    }
    return t->taps == 1 ? MUSE_VSTATS_TAP_NORMAL : MUSE_VSTATS_TAP_SWALLOW;
}

void muse_vstats_call_edge(muse_vstats_taps_t *t)
{
    t->shown = false;
    t->taps = 0;
}

void muse_vstats_acc_reset(muse_vstats_acc_t *a)
{
    memset(a, 0, sizeof(*a));
}

void muse_vstats_acc_base(muse_vstats_acc_t *a, const muse_vstats_sample_t *s)
{
    a->have_base = true;
    a->base_dec_errors = s->dec_errors;
    a->base_drops = s->drops;
    a->base_kf = s->kf_requests;
    a->under_stream = s->play_stream;
    a->under_last = s->play_underruns;
    a->under_skip = s->play_underruns;   /* that stream's underruns before the call */
    a->under_total = 0;
}

void muse_vstats_acc_open(muse_vstats_acc_t *a)
{
    a->have_prev = false;   /* no rate across the time it was hidden */
}

int muse_vstats_lines(muse_vstats_acc_t *a, const muse_vstats_sample_t *s, char lines[][MUSE_VSTATS_COLS])
{
    int n = 0;
    if (!a->have_base) {
        muse_vstats_acc_base(a, s);
    }
    if (s->video) {
        /* codec + coded -> shown size (the parts it knows) */
        char coded[12] = "", shown[12] = "";
        if (s->coded_w && s->coded_h) {
            snprintf(coded, sizeof(coded), "%ux%u", (unsigned)s->coded_w, (unsigned)s->coded_h);
        }
        if (s->shown_w && s->shown_h) {
            snprintf(shown, sizeof(shown), "%ux%u", (unsigned)s->shown_w, (unsigned)s->shown_h);
        }
        if (s->codec || coded[0] || shown[0]) {
            snprintf(lines[n++], MUSE_VSTATS_COLS, "%s%s%s%s%s", s->codec ? s->codec : "",
                     s->codec && (coded[0] || shown[0]) ? " " : "", coded, coded[0] && shown[0] ? ">" : "", shown);
        }
        /* kb/s and fps over the last second: only from two samples of one stream */
        const muse_vstats_sample_t *p = &a->prev;
        int64_t dt = s->t_ms - p->t_ms;
        if (a->have_prev && p->video && dt > 0 && s->rx_bytes >= p->rx_bytes) {
            uint32_t kbps = (uint32_t)(((uint64_t)(s->rx_bytes - p->rx_bytes) * 8 + (uint64_t)dt / 2) / (uint64_t)dt);
            uint16_t frames = (uint16_t)(s->shown - p->shown);
            uint32_t fps = (uint32_t)(((uint64_t)frames * 1000 + (uint64_t)dt / 2) / (uint64_t)dt);
            snprintf(lines[n++], MUSE_VSTATS_COLS, "%u kb/s %u fps", (unsigned)kbps, (unsigned)fps);
        }
        uint32_t err = s->dec_errors >= a->base_dec_errors ? s->dec_errors - a->base_dec_errors : s->dec_errors;
        uint32_t drop = s->drops >= a->base_drops ? s->drops - a->base_drops : s->drops;
        uint32_t kf = s->kf_requests >= a->base_kf ? s->kf_requests - a->base_kf : s->kf_requests;
        snprintf(lines[n++], MUSE_VSTATS_COLS, "err %u drop %u kf %u", (unsigned)err, (unsigned)drop, (unsigned)kf);
    }
    if (s->link && (s->itvl_1250us || s->link_mode)) {
        char itvl[16] = "";
        if (s->itvl_1250us) {
            unsigned us = s->itvl_1250us * 1250u;
            snprintf(itvl, sizeof(itvl), " %u.%02u ms", us / 1000, (us % 1000) / 10);
        }
        snprintf(lines[n++], MUSE_VSTATS_COLS, "link%s%s%s", itvl, s->link_mode ? " " : "",
                 s->link_mode ? s->link_mode : "");
    }
    if (s->play) {
        /* The call's underruns: each reply stream's count, kept across streams. */
        if (s->play_stream != a->under_stream) {
            a->under_total += a->under_last - a->under_skip;
            a->under_stream = s->play_stream;
            a->under_last = a->under_skip = 0;
        }
        if (s->play_underruns >= a->under_last) {
            a->under_last = s->play_underruns;
        }
        snprintf(lines[n++], MUSE_VSTATS_COLS, "reply %u ms under %u", (unsigned)s->play_depth_ms,
                 (unsigned)(a->under_total + a->under_last - a->under_skip));
    }
    a->prev = *s;
    a->have_prev = true;
    return n;
}

/* Half the panel's visible width at row y, inset px in from its edge
 * (muse_home_safe_half's shape, without the board). */
static double half_at(int w, int h, bool round, int corner_px, double y, double inset)
{
    if (round) {
        double r = (w < h ? w : h) / 2.0, dy = fabs(y - h / 2.0);
        double c = dy < r ? sqrt(r * r - dy * dy) : 0;
        return c > inset ? c - inset : 0;
    }
    double r = corner_px;
    if (r > 0 && (y < r || y > h - r)) {
        double yc = y < r ? r : h - r, dy = fabs(y - yc);
        if (dy >= r - inset) return 0;
        return w / 2.0 - r + sqrt((r - inset) * (r - inset) - dy * dy);
    }
    return w / 2.0 - inset;
}

bool muse_vstats_place(int w, int h, bool round, int corner_px, int ring, int margin, int box_w, int box_h,
                       muse_vstats_box_t *out)
{
    double inset = ring + margin;
    for (int y = (int)inset; y + box_h <= h / 2; y++) {
        /* The upper half narrows upwards: the box's top corners bind. */
        if (2 * half_at(w, h, round, corner_px, y, inset) >= box_w) {
            out->x = (w - box_w) / 2;
            out->y = y;
            out->w = box_w;
            out->h = box_h;
            return true;
        }
    }
    return false;
}
