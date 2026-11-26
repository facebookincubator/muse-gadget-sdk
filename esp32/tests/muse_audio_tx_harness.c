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
 * muse_audio_tx (the speaker's DMA, cleared at a barge-in's touch-down)
 * against a model of ESP-IDF v6.0.1's I2S TX driver (esp_driver_i2s/
 * i2s_common.c): a ring of ndesc buffers the DMA plays without end, desc[0]
 * first after enable; at each buffer's end the EOF ISR runs on_sent, clears
 * the buffer (auto_clear, as every board sets it), drops the oldest free
 * buffer when the queue (ndesc - 1 deep) is full and queues the one just
 * sent; i2s_channel_write fills the current buffer and takes the next from
 * the queue when it's full, unset, or the queue holds a buffer or more
 * (uxQueueSpacesAvailable <= 1), blocking while there is none. muse_audio's
 * write splits into 20 ms pieces and notes each after it returns; the
 * playback task writes 10 ms chunks with some decode time between.
 *
 * Every written sample carries its number (1000, 1001, ...), so what the speaker
 * plays and what a clear hands back can be checked sample by sample.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_audio_tx.h"

#define RATE 16000
#define NDESC 3
#define F 160
#define CHUNK 320   /* muse_audio's write piece */

static int16_t bufs[NDESC][F * 2];
static int queue[NDESC], qn;   /* free buffers, oldest first; at most NDESC - 1 */
static int curr = -1, rw;      /* the writer's buffer and fill (frames) */
static int playing, pos;       /* the DMA's buffer and frame */
static int64_t frame_t;        /* frames played since enable: the clock */
static muse_audio_tx_t tx;
static int16_t ring[NDESC * F];
static int isr_late;           /* frames the on_sent runs (and stamps) after the buffer ended */
static int drop_events;        /* the next N events never reach on_sent (an ISR that merged them) */
static int16_t out[400000];    /* what the speaker played (left slot) */
static size_t nout;
static bool running = true;    /* the channel enabled (the codecs open) */

static int64_t now_us(void) { return frame_t * 1000000 / RATE; }

static void dma_frame(void)
{
    if (!running) {
        frame_t++;
        return;
    }
    out[nout++] = bufs[playing][2 * pos];
    assert(nout < sizeof(out) / sizeof(out[0]));
    frame_t++;
    if (++pos == F) {
        if (drop_events) {
            /* Merged with the next EOF: the ISR sees only that one; this buffer is neither cleared nor queued. */
            drop_events--;
        } else {
            muse_audio_tx_sent(&tx, bufs[playing], sizeof(bufs[playing]), now_us() + (int64_t)isr_late * 1000000 / RATE);
            memset(bufs[playing], 0, sizeof(bufs[playing]));   /* auto_clear */
            if (qn == NDESC - 1) {
                memmove(queue, queue + 1, (size_t)(--qn) * sizeof(int));
            }
            queue[qn++] = playing;
        }
        playing = (playing + 1) % NDESC;
        pos = 0;
    }
}

static void run(int frames)
{
    for (int i = 0; i < frames; i++) dma_frame();
}

static void i2s_write(const int16_t *mono, int n)
{
    while (n > 0) {
        if (rw == F || curr < 0 || NDESC - 1 - qn <= 1) {
            while (!qn) dma_frame();   /* blocks for a free buffer */
            curr = queue[0];
            memmove(queue, queue + 1, (size_t)(--qn) * sizeof(int));
            rw = 0;
        }
        int k = F - rw < n ? F - rw : n;
        for (int i = 0; i < k; i++) {
            bufs[curr][2 * (rw + i)] = mono[i];
            bufs[curr][2 * (rw + i) + 1] = mono[i];
        }
        rw += k;
        mono += k;
        n -= k;
    }
}

/* muse_audio_write: 20 ms pieces, none across a DMA buffer, each noted once in the DMA. */
static void audio_write(const int16_t *mono, int n)
{
    while (n > 0) {
        int k = (int)muse_audio_tx_begin(&tx, (size_t)(n > CHUNK ? CHUNK : n), now_us());
        i2s_write(mono, k);
        muse_audio_tx_wrote(&tx, mono, (size_t)k, false);
        mono += k;
        n -= k;
    }
}

static int16_t next_id = 1000;
static void write_ids(int n)
{
    int16_t m[2048];
    for (int i = 0; i < n; i++) {
        m[i] = next_id++;
    }
    audio_write(m, n);
}

static void reset(void)
{
    memset(bufs, 0, sizeof(bufs));
    qn = 0;
    curr = -1;
    rw = 0;
    playing = 0;
    pos = 0;
    nout = 0;
    isr_late = 0;
    drop_events = 0;
    running = true;
    next_id = 1000;   /* a faded small number could pass for the next one */
    muse_audio_tx_init(&tx, NDESC, F, RATE, ring, NDESC * F);
}

/* The playback task during a reply: 10 ms chunks, decode time between (0-3.75 ms). */
static void play(int chunks)
{
    for (int c = 0; c < chunks; c++) {
        write_ids(F);
        run(rand() % 61);
    }
}

/* The last of the reply's samples the speaker played at full value, before out[end]: a faded one is smaller
 * than its number, so it never continues the run. */
static int16_t heard_upto(size_t end)
{
    int16_t want = 1000;
    for (size_t i = 0; i < end; i++) {
        if (out[i] == want) want++;
    }
    return (int16_t)(want - 1);
}

/* From out[at] (the clear) on: silent once the margin and the fade are over (later by an ISR's lateness). */
static void check_silent_after(size_t at, int late)
{
    for (size_t i = at; i < nout; i++) {
        if (out[i]) assert(i - at < (size_t)(MUSE_AUDIO_TX_MARGIN + MUSE_AUDIO_TX_FADE + late + 2));
    }
}

static void check_contiguous(const int16_t *keep, size_t kept, int16_t last_written)
{
    for (size_t i = 1; i < kept; i++) assert(keep[i] == (int16_t)(keep[i - 1] + 1));
    if (kept) assert(keep[kept - 1] == last_written);
}

static size_t do_clear(int16_t *keep)
{
    return muse_audio_tx_clear(&tx, now_us(), keep, NDESC * F);
}

int main(void)
{
    srand(7);
    int16_t keep[NDESC * F];

    /* 1. Steady state, the ISR on time: 500 clears at random points of a reply. What the speaker played at
     * full value, then what the clear kept, is exactly what was written (at most one sample twice, the
     * microsecond clock's rounding; none skipped). Silent 6 ms after the clear. A resume (the kept ones
     * written again, then the reply) plays every sample once more from the first kept, in order. */
    size_t total_kept = 0, min_kept = 1u << 30, max_kept = 0;
    int rep_max = 0;
    for (int trial = 0; trial < 500; trial++) {
        reset();
        play(20 + rand() % 30);
        run(rand() % 61);
        size_t at = nout;
        int16_t last_written = (int16_t)(next_id - 1);
        size_t kept = do_clear(keep);
        run(4 * F);   /* the pause: nothing written */
        check_silent_after(at, 0);
        int16_t h = heard_upto(nout);
        assert(kept > 0);
        check_contiguous(keep, kept, last_written);
        int rep = h + 1 - keep[0];
        assert(rep >= 0 && rep <= 1);
        if (rep > rep_max) rep_max = rep;
        total_kept += kept;
        if (kept < min_kept) min_kept = kept;
        if (kept > max_kept) max_kept = kept;
        /* A second clear, nothing written since: nothing handed back again. */
        assert(do_clear(keep + kept) == 0);
        /* The tap: kept first, then the reply. */
        size_t resume_at = nout;
        audio_write(keep, (int)kept);
        play(10);
        run(4 * F);
        int16_t want = keep[0];
        size_t i = resume_at;
        while (i < nout && !out[i]) i++;
        for (; i < nout && out[i]; i++) {
            assert(out[i] == want);
            want++;
        }
        assert(want == next_id);   /* to the last written, once, in order, no gap */
    }
    printf("steady: 500 clears, kept %zu-%zu frames (mean %.1f ms), %d repeated at most, silent after 6 ms, "
           "resume exact\n", min_kept, max_kept, total_kept * 1000.0 / 500 / RATE, rep_max);

    /* 2. The ISR late (up to 4 ms, past the 2 ms margin), or merging two events: a clear may hand back
     * samples that already played, never skip one. */
    rep_max = 0;
    for (int trial = 0; trial < 300; trial++) {
        reset();
        isr_late = rand() % 65;
        play(20 + rand() % 20);
        if (trial % 3 == 0) {
            drop_events = 1;
            play(4);
        }
        run(rand() % 61);
        size_t at = nout;
        int16_t last_written = (int16_t)(next_id - 1);
        size_t kept = do_clear(keep);
        run(4 * F);
        check_silent_after(at, isr_late);
        assert(kept > 0);
        check_contiguous(keep, kept, last_written);
        int rep = heard_upto(nout) + 1 - keep[0];
        assert(rep >= 0);
        if (rep > rep_max) rep_max = rep;
    }
    printf("late ISR (0-4 ms) and merged events: never a sample skipped, at most %d repeated\n", rep_max);

    /* 3. A reply that has just started after silence (the writer idle, the queue full): everything not
     * yet played is kept, maybe a little more. */
    for (int trial = 0; trial < 200; trial++) {
        reset();
        run(F * 5 + rand() % F);
        write_ids(F * (1 + rand() % 2));
        run(rand() % (F / 2));
        int16_t last_written = (int16_t)(next_id - 1);
        size_t at = nout;
        size_t kept = do_clear(keep);
        run(4 * F);
        check_silent_after(at, 0);
        assert(kept > 0);
        check_contiguous(keep, kept, last_written);
        int rep = heard_upto(nout) + 1 - keep[0];
        assert(rep >= 0 && rep <= 1);
    }
    printf("after idle: exact\n");

    /* 3b. The codecs closed and opened again (rest and wake: the channel disabled, enabled; ESP-IDF empties
     * the queue, unsets the writer's buffer, starts the DMA at desc[0]): exact again after it. */
    for (int trial = 0; trial < 100; trial++) {
        reset();
        play(10 + rand() % 10);
        run(4 * F);   /* played out before the rest */
        running = false;
        run(F * 4 + rand() % F);
        running = true;
        qn = 0;
        curr = -1;
        rw = 0;
        playing = 0;
        pos = 0;
        memset(bufs, 0, sizeof(bufs));   /* what it held never mattered: nothing plays it before its event */
        play(20 + rand() % 10);
        run(rand() % 61);
        size_t at = nout;
        int16_t last_written = (int16_t)(next_id - 1);
        size_t kept = do_clear(keep);
        run(4 * F);
        check_silent_after(at, 0);
        assert(kept > 0);
        check_contiguous(keep, kept, last_written);
        int rep = heard_upto(nout) + 1 - keep[0];
        assert(rep >= 0 && rep <= 1);
    }
    printf("rest and wake: exact\n");

    /* 4. muse_audio's pieces off the buffers (a 437-sample write first, then 10 ms chunks): still exact. */
    for (int trial = 0; trial < 200; trial++) {
        reset();
        write_ids(437);
        play(20 + rand() % 20);
        run(rand() % 61);
        size_t at = nout;
        int16_t last_written = (int16_t)(next_id - 1);
        size_t kept = do_clear(keep);
        run(4 * F);
        check_silent_after(at, 0);
        assert(kept > 0);
        check_contiguous(keep, kept, last_written);
        int rep = heard_upto(nout) + 1 - keep[0];
        assert(rep >= 0 && rep <= 1);
    }
    printf("unaligned writes: exact\n");

    /* 5. The channel stopped (the codecs closed, no event for two buffers' time): nothing touched. Nor
     * before the ring is known (fewer events than buffers). */
    reset();
    play(20);
    running = false;
    run(3 * F);
    int16_t before[NDESC][F * 2];
    memcpy(before, bufs, sizeof(bufs));
    assert(do_clear(keep) == 0);
    assert(!memcmp(before, bufs, sizeof(bufs)));
    reset();
    run(F + 10);
    write_ids(F);
    assert(do_clear(keep) == 0);
    printf("stopped or unknown: untouched\n");

    /* 6. No click: a full-scale 1 kHz tone cut by a clear steps no more than the tone itself does plus one
     * fade step (a hard cut steps up to its full amplitude). */
    reset();
    double ph = 0;
    for (int c = 0; c < 40; c++) {
        int16_t m[F];
        for (int i = 0; i < F; i++) {
            m[i] = (int16_t)(20000 * sin(ph));
            ph += 2 * M_PI * 1000 / RATE;
        }
        audio_write(m, F);
        run(rand() % 61);
    }
    int maxstep_tone = 0;
    for (size_t i = 1; i < nout; i++) {
        int d = abs(out[i] - out[i - 1]);
        if (d > maxstep_tone) maxstep_tone = d;
    }
    size_t at = nout;
    assert(do_clear(keep) > 0);
    run(4 * F);
    int maxstep = 0;
    for (size_t i = at; i < nout; i++) {
        int d = abs(out[i] - out[i - 1]);
        if (d > maxstep) maxstep = d;
    }
    assert(maxstep <= maxstep_tone + 20000 / (MUSE_AUDIO_TX_FADE + 1) + 1);
    check_silent_after(at, 0);
    printf("no click: max step %d at the clear (the tone's own %d)\n", maxstep, maxstep_tone);
    return 0;
}
