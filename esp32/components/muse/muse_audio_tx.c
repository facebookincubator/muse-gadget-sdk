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

#include "muse_audio_tx.h"

#include <string.h>

void muse_audio_tx_init(muse_audio_tx_t *t, unsigned ndesc, unsigned frames, unsigned rate, int16_t *ring,
                        uint32_t ring_len)
{
    memset(t, 0, sizeof(*t));
    t->ndesc = (uint8_t)(ndesc > MUSE_AUDIO_TX_MAX_DESC ? MUSE_AUDIO_TX_MAX_DESC : ndesc);
    t->frames = (uint16_t)frames;
    t->rate = rate;
    t->last = -1;
    t->ring = ring;
    t->ring_len = ring ? ring_len : 0;
    muse_audio_tx_restart(t);
}

/* The writer takes the oldest queued buffer, empty from its start. */
static void take(muse_audio_tx_t *t)
{
    if (t->qn) {
        t->curr = t->queue[0];
        for (int i = 1; i < t->qn; i++) {
            t->queue[i - 1] = t->queue[i];
        }
        t->qn--;
    } else {
        /* The driver had one the model didn't see queued (an event it never got): the one sent last, which
         * the writer takes when it keeps up. */
        t->curr = t->last;
    }
    t->rw = 0;
    if (t->curr >= 0) {
        t->lo[t->curr] = t->hi[t->curr] = 0;
    }
}

size_t muse_audio_tx_begin(muse_audio_tx_t *t, size_t n, int64_t now_us)
{
    if (!t->ring_len || !t->frames || !n) {
        return n;
    }
    t->waiting = false;
    if (muse_audio_tx_stalled(t, now_us)) {
        t->last = -1;   /* a write into a channel that started again (the codecs reopened) */
        muse_audio_tx_restart(t);
    }
    if (t->curr < 0 || t->rw >= t->frames || t->ndesc - 1 - t->qn <= 1) {
        if (t->qn) {
            take(t);
        } else {
            t->waiting = true;   /* it blocks for the next event's buffer */
            return n < t->frames ? n : t->frames;
        }
    }
    size_t room = (size_t)(t->frames - t->rw);
    return n < room ? n : room;
}

void muse_audio_tx_wrote(muse_audio_tx_t *t, const int16_t *mono, size_t n, bool mute)
{
    if (!t->ring_len || !n || !t->frames) {
        return;
    }
    if (t->waiting) {
        t->waiting = false;
        take(t);   /* the first queued while it waited */
    }
    size_t room = (size_t)(t->frames - t->rw);
    uint16_t k = (uint16_t)(n < room ? n : room);   /* begin kept it within the buffer */
    if (t->curr >= 0 && k) {
        int b = t->curr;
        if (t->hi[b] != t->rw || t->lo[b] == t->hi[b]) {
            t->lo[b] = t->rw;
        }
        t->hi[b] = (uint16_t)(t->rw + k);
    }
    t->rw = (uint16_t)(t->rw + k);
    size_t skip = n > t->ring_len ? n - t->ring_len : 0;   /* only the newest ring_len matter */
    for (size_t i = skip; i < n; i++) {
        t->ring[t->ring_pos] = mute ? 0 : mono[i];
        t->ring_pos = t->ring_pos + 1 == t->ring_len ? 0 : t->ring_pos + 1;
    }
}

size_t muse_audio_tx_clear(muse_audio_tx_t *t, int64_t now_us, int16_t *keep, size_t max)
{
    if (!t->ring_len || !t->rate || !t->frames || t->last < 0 || t->known < t->ndesc || !t->ndesc) {
        return 0;
    }
    int64_t period_us = (int64_t)t->frames * 1000000 / t->rate;
    int64_t elapsed = now_us - t->sent_us;
    if (elapsed < 0) {
        elapsed = 0;
    }
    if (elapsed > 2 * period_us) {
        return 0;   /* nothing finished for two buffers: the channel isn't running */
    }
    uint32_t read = (uint32_t)(elapsed * t->rate / 1000000);
    if (read > t->frames) {
        read = t->frames;   /* the next buffer's event is late: count it from its start (keeps more) */
    }
    uint32_t start = read + MUSE_AUDIO_TX_MARGIN;
    uint32_t total = (uint32_t)t->ndesc * t->frames;
    unsigned playing = (unsigned)(t->last + 1) % t->ndesc;

    /* Written frames from start on, in play order: the newest written. Clipped to before start, as the
     * buffers are about to be. */
    uint32_t keep_n = 0;
    for (unsigned k = 0; k < t->ndesc; k++) {
        int b = (int)((playing + k) % t->ndesc);
        uint32_t lo = k * t->frames + t->lo[b], hi = k * t->frames + t->hi[b];
        if (hi > start) {
            keep_n += hi - (lo > start ? lo : start);
            uint32_t cut = start > k * t->frames ? start - k * t->frames : 0;
            if (t->lo[b] >= cut) {
                t->lo[b] = (uint16_t)cut;   /* nothing left (the writer's next frames start a new range) */
            }
            t->hi[b] = (uint16_t)cut;
        }
    }
    if (keep_n > t->ring_len) {
        keep_n = t->ring_len;
    }
    if (keep_n > max) {
        keep_n = (uint32_t)max;
    }
    uint32_t from = (t->ring_pos + t->ring_len - keep_n) % t->ring_len;
    for (uint32_t i = 0; i < keep_n; i++) {
        keep[i] = t->ring[(from + i) % t->ring_len];
    }

    /* Silence from start on, in play order from the playing buffer: faded, then zeros. */
    for (uint32_t pos = start; pos < total; pos++) {
        int16_t *s = (int16_t *)t->bufs[(playing + pos / t->frames) % t->ndesc] + 2 * (pos % t->frames);
        uint32_t into = pos - start;
        if (into < MUSE_AUDIO_TX_FADE) {
            int32_t g = (int32_t)(MUSE_AUDIO_TX_FADE - into);
            s[0] = (int16_t)((int32_t)s[0] * g / (MUSE_AUDIO_TX_FADE + 1));
            s[1] = (int16_t)((int32_t)s[1] * g / (MUSE_AUDIO_TX_FADE + 1));
        } else {
            s[0] = 0;
            s[1] = 0;
        }
    }
    t->cleared++;
    return keep_n;
}
