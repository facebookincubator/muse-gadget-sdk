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
 * The speaker's I2S TX DMA, as the playback side sees it, so it can be
 * cleared at once and what it held kept (the barge-in echo rule's follow-up):
 * at a talk press's touch-down the ~30 ms
 * the DMA already holds must not play into the pre-roll, and a tap's resume
 * must play it after all (a resumed reply loses no samples).
 *
 * Pure logic (no ESP-IDF), host-tested against a model of ESP-IDF's TX
 * driver (tests/test_muse_audio_tx.py); muse_audio_tx_esp.c holds the board
 * side: the channel's on_sent callback, the lock, the cache sync.
 *
 * Why not i2s_channel_disable()/enable(): every board with a speaker on
 * a board running its codec on one duplex I2S port, and in full duplex the RX
 * takes its BCLK and WS from the TX (i2s_ll_share_bck_ws: sig_loopback).
 * Stopping the TX stops the mic's clock in the very pre-roll this protects,
 * and the ES8311 losing LRCK mid-stream pops. The DMA buffers are cleared in
 * place instead, the bus never stops: their addresses and order come from
 * the on_sent events (the ring is desc[0] -> desc[1] -> ... -> desc[0]),
 * the read position from the time since the last one.
 *
 * What was written but not yet played: the core follows ESP-IDF's TX
 * driver step by step (esp_driver_i2s/i2s_common.c, v6.0.1): each event
 * queues the buffer just sent (ndesc - 1 deep, the oldest dropped when
 * full) and clears it (auto_clear, as every board sets it); each write
 * fills the writer's buffer and takes the oldest queued one when that is
 * full, unset, or the queue holds one or more (uxQueueSpacesAvailable <=
 * 1), exactly as i2s_channel_write does. muse_audio splits its writes so
 * none crosses a buffer (muse_audio_tx_begin says how much fits): the
 * driver's one choice of buffer is then made at the write's entry, where
 * the core makes it too, and a write that had to wait for a buffer takes the
 * first one queued while it waited, which the core sees by the time the
 * write returns. That gives each buffer's
 * written range, and with the read position, how many written frames lie
 * ahead of it. They are the newest written (the queue hands buffers out in
 * the order they play), kept, mono, in a ring the size of the DMA: a clear
 * hands back those, oldest first. Where the model can be off (an event the
 * ISR merged with the next, an ISR that ran late) it errs towards keeping a
 * few ms more, played twice, never fewer.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MUSE_AUDIO_TX_MAX_DESC 16
/* The ISR's helpers: always inlined, so they run from the IRAM callback that calls them. */
#define MUSE_AUDIO_TX_ISR static inline __attribute__((always_inline))
/* A clear leaves this much after the estimated read position untouched (the
 * on_sent ISR's latency, the estimate's error), then fades over the next
 * MUSE_AUDIO_TX_FADE frames to silence: a hard cut mid-waveform clicks
 * through the amp (the ES8311 / NS4150). Both still count as unplayed: they
 * are kept and replayed. At 16 kHz. */
#define MUSE_AUDIO_TX_MARGIN 32   /* 2 ms */
#define MUSE_AUDIO_TX_FADE 64     /* 4 ms */

typedef struct {
    void *bufs[MUSE_AUDIO_TX_MAX_DESC];   /* the DMA buffers, in play order, as the events showed them */
    uint16_t lo[MUSE_AUDIO_TX_MAX_DESC];  /* each one's written range, in frames: [lo, hi) */
    uint16_t hi[MUSE_AUDIO_TX_MAX_DESC];
    int8_t queue[MUSE_AUDIO_TX_MAX_DESC]; /* the driver's free buffers, oldest first (ndesc - 1 at most) */
    uint8_t qn;
    int8_t curr;                          /* the writer's buffer (-1: none, as after enable) */
    uint16_t rw;                          /* ...and how far it has filled it */
    bool waiting;                         /* the write in progress waits for a buffer to be queued */
    uint8_t ndesc;                        /* the channel's descriptors (the board's i2s_dma_desc) */
    uint8_t known;                        /* how many of bufs are learned */
    int8_t last;                          /* index in bufs of the buffer sent last; -1: none yet */
    uint16_t frames;                      /* frames per buffer (16-bit stereo: 4 bytes a frame) */
    uint32_t rate;                        /* frames a second */
    int64_t sent_us;                      /* when the last buffer finished */
    int16_t *ring;                        /* the last ring_len frames written, mono, ring_pos next */
    uint32_t ring_len, ring_pos;
    uint32_t cleared;                     /* clears that found the DMA running */
} muse_audio_tx_t;

/* ndesc x frames: the channel's ring; ring: room for ring_len mono frames
 * (at least ndesc x frames). */
void muse_audio_tx_init(muse_audio_tx_t *t, unsigned ndesc, unsigned frames, unsigned rate, int16_t *ring,
                        uint32_t ring_len);

/* The driver's EOF ISR for buffer index b (one event, as i2s_dma_tx_callback). */
MUSE_AUDIO_TX_ISR void muse_audio_tx_event(muse_audio_tx_t *t, int b)
{
    t->lo[b] = t->hi[b] = 0;   /* auto_clear */
    if (t->curr == b) {
        t->lo[b] = t->hi[b] = t->rw;   /* the writer goes on filling it from where it was */
    }
    if (t->qn && t->qn >= t->ndesc - 1) {   /* full: the oldest goes */
        for (int i = 1; i < t->qn; i++) {
            t->queue[i - 1] = t->queue[i];
        }
        t->qn--;
    }
    if (t->ndesc > 1) {
        t->queue[t->qn++] = (int8_t)b;
    }
}

/* Forget the driver's state (a new ring, or the channel restarted: enable empties the queue and unsets the
 * writer's buffer; the DMA starts again at desc[0]). A write waiting for a buffer still gets the first one
 * queued. */
MUSE_AUDIO_TX_ISR void muse_audio_tx_restart(muse_audio_tx_t *t)
{
    t->qn = 0;
    t->curr = -1;
    t->rw = 0;
    for (int i = 0; i < MUSE_AUDIO_TX_MAX_DESC; i++) {
        t->lo[i] = t->hi[i] = 0;
    }
}

/* No buffer finished for three buffers' time: the channel stopped (the codecs closed); its next start
 * begins afresh. */
MUSE_AUDIO_TX_ISR bool muse_audio_tx_stalled(const muse_audio_tx_t *t, int64_t now_us)
{
    return t->last >= 0 && t->rate && now_us - t->sent_us > 3 * (int64_t)t->frames * 1000000 / t->rate;
}

/* The channel's on_sent: buf (bytes long) has just finished playing at
 * now_us. Called from the ISR, with the lock. */
MUSE_AUDIO_TX_ISR void muse_audio_tx_sent(muse_audio_tx_t *t, void *buf, size_t bytes, int64_t now_us)
{
    unsigned frames = (unsigned)(bytes / 4);
    if (!frames || frames > 0xffff || !t->rate) {
        return;
    }
    if (frames != t->frames) {   /* the board's numbers were off: learn the ring again at this size */
        t->frames = (uint16_t)frames;
        t->known = 0;
        t->last = -1;
        muse_audio_tx_restart(t);
    }
    if (muse_audio_tx_stalled(t, now_us)) {
        t->last = -1;   /* nothing for three buffers: the channel stopped and started again */
        muse_audio_tx_restart(t);
    }
    int idx = -1;
    for (int i = 0; i < t->known; i++) {
        if (t->bufs[i] == buf) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (t->known < t->ndesc && t->known < MUSE_AUDIO_TX_MAX_DESC) {
            idx = t->known++;
            t->bufs[idx] = buf;
        } else {   /* not one of the ring's: learn it again */
            t->known = 1;
            t->bufs[0] = buf;
            idx = 0;
            muse_audio_tx_restart(t);
        }
    } else if (t->known < t->ndesc) {
        if (idx == 0 && t->last == t->known - 1) {
            t->ndesc = t->known;   /* back to the first before ndesc were seen: a shorter ring */
        } else {
            t->known = 1;   /* out of order while learning (a missed event): start over */
            t->bufs[0] = buf;
            idx = 0;
            muse_audio_tx_restart(t);
        }
    }
    /* Two EOFs before the ISR ran report only the second: the driver queues and clears only it, and so does
     * this (the other keeps its range: kept again if it comes round, played twice rather than skipped). */
    muse_audio_tx_event(t, idx);
    t->last = (int8_t)idx;
    t->sent_us = now_us;
}

/* Before a write of up to n frames: how many to write now so they stay in one DMA buffer (n when nothing
 * is attached); the driver's choice of buffer, made as i2s_channel_write makes it on entry. */
size_t muse_audio_tx_begin(muse_audio_tx_t *t, size_t n, int64_t now_us);
/* After that write (n frames, at most what begin said) reached the DMA (silence if mute). */
void muse_audio_tx_wrote(muse_audio_tx_t *t, const int16_t *mono, size_t n, bool mute);

/*
 * Clears what the DMA holds unplayed at now_us: from the read position +
 * MUSE_AUDIO_TX_MARGIN it fades over MUSE_AUDIO_TX_FADE frames to silence and
 * zeroes the rest of the ring, so the speaker goes quiet within those 6 ms
 * with no click. Copies the frames that were written there and hadn't played
 * (the faded ones included) into keep, oldest first, at most max, and returns
 * how many. 0, and nothing touched, when the ring isn't learned yet or no
 * buffer finished for two buffers' time (the channel is stopped: the codecs
 * closed). The caller syncs the buffers' cache afterwards where the DMA
 * reads through one.
 */
size_t muse_audio_tx_clear(muse_audio_tx_t *t, int64_t now_us, int16_t *keep, size_t max);
