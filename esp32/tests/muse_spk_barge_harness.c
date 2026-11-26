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
 * The ESP-Mosaico's speech chain (muse_spk_dsp.c, behind mg_chain.c) through
 * the barge-in pause (the speaker's I2S DMA cleared at a
 * talk press's touch-down, its unplayed samples kept; a tap's resume writes
 * them first, ramped in over 5 ms, then the reply goes on), on the real
 * muse_audio_tx core against tests/muse_audio_tx_harness.c's model of
 * ESP-IDF v6.0.1's TX driver. The player's steps are mg_ble.c's, in its
 * order: each 10 ms reply chunk through the chain, then written; at the
 * touch-down the clear; nothing written while the pause holds; at the tap
 * the rewind as it is (post-chain), then the chain from the next sample.
 *
 * The reference is the same reply through a chain never paused (R). Checked:
 * the chain's own output is R exactly, so its state never jumped; the rewind
 * is R's samples (post-chain, not run through the chain again) and is written
 * once; the speaker plays R up to the clear, the clear's fade, silence, then
 * R again from the first sample it hadn't played (at most one played twice,
 * the clear's clock rounding), the ramp, and on; no step larger than the
 * signal's own at the pause or the resume. Then a flush or a hold's claim in
 * the pause (nothing of the reply plays again, the chain emptied), a stop
 * keep (the chain's last 2 ms play out), a flush without a pause (the
 * chain's tail, then the fade, no click), and a new stream starting afresh.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_chain.h"
#include "muse_audio_tx.h"
#include "muse_spk_dsp.h"

#define RATE 16000
#define NDESC 3
#define F 160
#define CHUNK 320
#define REWIND_MAX (RATE * 40 / 1000)   /* MG_REWIND_MAX */
#define RAMP (RATE / 200)               /* MG_REWIND_RAMP */

/* ---- the TX driver model (as muse_audio_tx_harness.c) ---- */
static int16_t bufs[NDESC][F * 2];
static int queue[NDESC], qn;
static int curr = -1, rw;
static int playing, pos;
static int64_t frame_t;
static muse_audio_tx_t tx;
static int16_t ring[NDESC * F];
static int16_t out[600000];
static size_t nout;

static int64_t now_us(void) { return frame_t * 1000000 / RATE; }

static void dma_frame(void)
{
    out[nout++] = bufs[playing][2 * pos];
    assert(nout < sizeof(out) / sizeof(out[0]));
    frame_t++;
    if (++pos == F) {
        muse_audio_tx_sent(&tx, bufs[playing], sizeof(bufs[playing]), now_us());
        memset(bufs[playing], 0, sizeof(bufs[playing]));
        if (qn == NDESC - 1) memmove(queue, queue + 1, (size_t)(--qn) * sizeof(int));
        queue[qn++] = playing;
        playing = (playing + 1) % NDESC;
        pos = 0;
    }
}
static void run(int frames) { for (int i = 0; i < frames; i++) dma_frame(); }

static void i2s_write(const int16_t *mono, int n)
{
    while (n > 0) {
        if (rw == F || curr < 0 || NDESC - 1 - qn <= 1) {
            while (!qn) dma_frame();
            curr = queue[0];
            memmove(queue, queue + 1, (size_t)(--qn) * sizeof(int));
            rw = 0;
        }
        int k = F - rw < n ? F - rw : n;
        for (int i = 0; i < k; i++) bufs[curr][2 * (rw + i)] = bufs[curr][2 * (rw + i) + 1] = mono[i];
        rw += k;
        mono += k;
        n -= k;
    }
}

/* muse_audio_write_chained: pieces in one DMA buffer, each noted once in the DMA (what a clear hands back). */
static int16_t written[600000];   /* everything written, in order (the rewind's writes included) */
static size_t nwritten;
static void speaker(const int16_t *mono, int n)
{
    memcpy(written + nwritten, mono, (size_t)n * sizeof(int16_t));
    nwritten += (size_t)n;
    while (n > 0) {
        int k = (int)muse_audio_tx_begin(&tx, (size_t)(n > CHUNK ? CHUNK : n), now_us());
        i2s_write(mono, k);
        muse_audio_tx_wrote(&tx, mono, (size_t)k, false);
        mono += k;
        n -= k;
    }
}

/* ---- the chain, as mg_glue.c hands it to the player ---- */
static muse_spk_chain_t s_chain;
static int16_t chain_out[600000];   /* everything the chain produced, in order */
static size_t nchain;
static void chain_run(int16_t *pcm, size_t n)
{
    muse_spk_chain_run(&s_chain, pcm, n, RATE);
    memcpy(chain_out + nchain, pcm, n * sizeof(int16_t));
    nchain += n;
}
static void chain_reset(void) { muse_spk_chain_reset(&s_chain); }

/* ---- the reply: a loud 1 kHz tone with a slow swell (the compressor and the limiter both working) ---- */
static int16_t reply_sample(long i)
{
    double a = 0.35 + 0.6 * (0.5 + 0.5 * sin(2 * M_PI * 1.3 * i / RATE));
    return (int16_t)lround(32767 * a * sin(2 * M_PI * 1000.0 * i / RATE));
}

static int16_t R[600000];   /* the reference: the reply through a chain never paused */
static void reference(long n)
{
    muse_spk_chain_t c = { 0 };
    for (long i = 0; i < n; i += F) {
        int16_t b[F];
        for (int k = 0; k < F; k++) b[k] = reply_sample(i + k);
        muse_spk_chain_run(&c, b, F, RATE);
        memcpy(R + i, b, sizeof(b));
    }
}

static mg_chain_t chain;
static long src;   /* the reply's next sample */
static const int16_t silence[F];
static int16_t scratch[F];

static void reset_all(void)
{
    memset(bufs, 0, sizeof(bufs));
    qn = 0;
    curr = -1;
    rw = 0;
    playing = pos = 0;
    frame_t = 0;
    nout = nwritten = nchain = 0;
    muse_audio_tx_init(&tx, NDESC, F, RATE, ring, NDESC * F);
    memset(&s_chain, 0, sizeof(s_chain));
    mg_chain_init(&chain, chain_run, chain_reset);
    src = 0;
}

/* The player's main write: a reply chunk through the chain, then the speaker (decode time between). */
static void play_chunks(int n)
{
    for (int c = 0; c < n; c++) {
        int16_t b[F];
        for (int k = 0; k < F; k++) b[k] = reply_sample(src + k);
        src += F;
        mg_chain_audio(&chain, b, F);
        speaker(b, F);
        run(rand() % 61);
    }
}

static int max_step(const int16_t *x, size_t from, size_t to)
{
    int m = 0;
    for (size_t i = from + 1; i < to; i++) {
        int d = abs(x[i] - x[i - 1]);
        if (d > m) m = d;
    }
    return m;
}

int main(void)
{
    srand(11);
    reference(RATE * 30);
    const int natural = max_step(R, RATE / 2, RATE * 30);   /* the reply's own largest step, post-chain */

    /* 1. Pause and tap, 300 times at random points: exact. */
    int worst_step = 0, max_rep = 0;
    size_t min_kept = 1u << 30, max_kept = 0;
    for (int trial = 0; trial < 300; trial++) {
        reset_all();
        play_chunks(30 + rand() % 40);
        run(rand() % 61);
        /* The touch-down: the clear (the player's speaker_clear), the chain left alone. */
        size_t at = nout;
        int16_t rewind[REWIND_MAX];
        size_t kept = muse_audio_tx_clear(&tx, now_us(), rewind, REWIND_MAX);
        assert(kept > 0);
        if (kept < min_kept) min_kept = kept;
        if (kept > max_kept) max_kept = kept;
        /* The rewind is post-chain: the last `kept` samples the chain produced (and wrote), as they are. */
        assert(!memcmp(rewind, chain_out + nchain - kept, kept * sizeof(int16_t)));
        long chain_before = (long)nchain;
        run(F * (3 + rand() % 60));   /* the pause: nothing written (up to the 1.5 s lapse) */
        size_t resume_at = nout;
        /* Silent from 6 ms after the clear until the resume. */
        for (size_t i = at + MUSE_AUDIO_TX_MARGIN + MUSE_AUDIO_TX_FADE + 2; i < resume_at; i++) assert(out[i] == 0);
        /* Before the clear the speaker played R from its start (after the DMA's first, empty buffers). */
        size_t off = 0;
        while (off < at && memcmp(out + off, R, 64 * sizeof(int16_t))) off++;
        assert(off < at);
        for (size_t i = off; i < at; i++) assert(out[i] == R[i - off]);
        long heard = -1;   /* the last of R played at its full value (the clear's margin plays, then its fade) */
        for (size_t k = off; k < resume_at; k++)
            if (out[k] && out[k] == R[k - off]) heard = (long)(k - off);
        long first_kept = chain_before - (long)kept;
        int rep = (int)(heard + 1 - first_kept);   /* played whole and kept: played twice */
        assert(rep >= 0 && rep <= 1 + MUSE_AUDIO_TX_MARGIN);
        if (rep > max_rep) max_rep = rep;
        /* The tap: the rewind ramped in, written as it is, then the reply through the chain. */
        int16_t ramped[REWIND_MAX];
        memcpy(ramped, rewind, kept * sizeof(int16_t));
        for (size_t i = 0; i < RAMP && i < kept; i++)   /* mg_play_ramp_in */
            ramped[i] = (int16_t)((int32_t)ramped[i] * (int32_t)(i + 1) / (int32_t)(RAMP + 1));
        size_t w0 = nwritten;
        speaker(ramped, (int)kept);
        play_chunks(20);
        run(4 * F);
        /* The chain never jumped: its whole output is R, exactly, pause or not. */
        assert(!memcmp(chain_out, R, nchain * sizeof(int16_t)));
        /* The rewind was written once, and the chain went on from the sample after it. */
        assert(nwritten - w0 == kept + 20 * F);
        assert(!memcmp(written + w0 + kept, R + chain_before, 20 * F * sizeof(int16_t)));
        /* The speaker after the pause: the ramped rewind, then R from the sample after it, exactly. */
        size_t q = resume_at;
        while (q + kept < nout && memcmp(out + q, ramped, kept * sizeof(int16_t))) q++;
        assert(q + kept < nout);
        for (size_t k = 0; k < (size_t)20 * F && q + kept + k < nout; k++) assert(out[q + kept + k] == R[chain_before + (long)k]);
        /* No click at the pause (the clear's fade) or at the resume (the ramp, the rewind into the chain's next). */
        int step_pause = max_step(out, at > 2 ? at - 2 : 0, at + MUSE_AUDIO_TX_MARGIN + MUSE_AUDIO_TX_FADE + 4);
        int step_resume = max_step(out, q > 1 ? q - 1 : 0, q + kept + F);
        int s = step_pause > step_resume ? step_pause : step_resume;
        if (s > worst_step) worst_step = s;
        assert(step_pause <= natural + 29205 / (MUSE_AUDIO_TX_FADE + 1) + 1);
        assert(step_resume <= natural + 29205 / (RAMP + 1) + 1);
    }
    printf("pause and tap: 300, kept %zu-%zu samples, chain output exact (no state jump), rewind post-chain "
           "and written once, speaker exact after the ramp, %d replayed at most (the clear's margin), "
           "largest step %d (the reply's own %d)\n", min_kept, max_kept, max_rep, worst_step, natural);

    /* 2. A hold's claim in the pause (a stop_streaming drop) or a flush in it (a call's claim): nothing of the
     * reply plays again, not the chain's look-ahead either; the next stream starts the chain afresh. */
    for (int kind = 0; kind < 2; kind++) {
        for (int trial = 0; trial < 50; trial++) {
            reset_all();
            play_chunks(30 + rand() % 30);
            int16_t rewind[REWIND_MAX];
            size_t kept = muse_audio_tx_clear(&tx, now_us(), rewind, REWIND_MAX);
            assert(kept > 0);
            run(F * 10);
            size_t after = nout;
            /* mg_ble.c: drop (fresh / IDLE) or flush, with nrewind: the chain discarded, the rewind dropped. */
            mg_chain_discard(&chain);
            if (kind == 0) {
                for (int t = 0; t < 5; t++) speaker(mg_chain_silence(&chain, silence, scratch, F), F);   /* the tail */
            } else {
                for (int t = 0; t < 3; t++) speaker(mg_chain_silence(&chain, silence, scratch, F), F);   /* nothing to play yet */
            }
            run(4 * F);
            for (size_t k = after; k < nout; k++) assert(out[k] == 0);   /* silent: no reply, no 2 ms tail */
            assert(chain.quiet);
            /* The next reply: a fresh chain, its output a fresh reference's. */
            long n0 = (long)nchain;
            src = 0;
            play_chunks(10);
            assert(!memcmp(chain_out + n0, R, 10 * F * sizeof(int16_t)));
        }
    }
    printf("hold's claim and flush in the pause: nothing of the reply again, the next stream afresh\n");

    /* 3. A stop keep (played out): the chain's last 2 ms play (silence through it), then it's quiet and
     * silence passes untouched. */
    for (int trial = 0; trial < 50; trial++) {
        reset_all();
        play_chunks(20 + rand() % 20);
        long end = src;
        for (int t = 0; t < 5; t++) speaker(mg_chain_silence(&chain, silence, scratch, F), F);
        run(6 * F);
        assert(chain.quiet);
        /* What the chain produced from the reply's end on: R's last 2 ms (computed with zeros after it). */
        muse_spk_chain_t c = { 0 };
        int16_t full[RATE * 3];
        for (long i = 0; i < end + 5 * F; i += F) {
            for (int k = 0; k < F; k++) full[i + k] = i + k < end ? reply_sample(i + k) : 0;
            muse_spk_chain_run(&c, full + i, F, RATE);
        }
        assert(!memcmp(written, full, (size_t)(end + F) * sizeof(int16_t)));   /* the tail included */
        bool tail = false;
        for (long k = end; k < end + MUSE_SPK_DSP_LOOKAHEAD; k++) tail |= written[k] != 0;
        assert(tail);
        size_t q0 = nwritten;
        speaker(mg_chain_silence(&chain, silence, scratch, F), F);
        for (size_t k = q0; k < nwritten; k++) assert(written[k] == 0);
    }
    printf("stop keep: the chain's last 2 ms play, then quiet\n");

    /* 4. A flush without a pause (the client's barge-in): the chain's 2 ms go first, then the FIFO's 5 ms fade
     * (mg_ble.c fades the FIFO before the chain), no step over the reply's own. */
    for (int trial = 0; trial < 50; trial++) {
        reset_all();
        play_chunks(20 + rand() % 20);
        size_t at = nout;
        int16_t b[F];
        for (int k = 0; k < F; k++) b[k] = reply_sample(src + k);
        src += F;
        for (int k = 0; k < RAMP; k++) b[k] = (int16_t)((int32_t)b[k] * (RAMP - k) / (RAMP + 1));
        memset(b + RAMP, 0, (F - RAMP) * sizeof(int16_t));
        mg_chain_audio(&chain, b, F);
        speaker(b, F);
        for (int t = 0; t < 3; t++) speaker(mg_chain_silence(&chain, silence, scratch, F), F);
        run(6 * F);
        assert(max_step(out, at > 0 ? at - 1 : 0, nout) <= natural);
        assert(chain.quiet);
    }
    printf("flush without a pause: the chain's tail, then the fade, no click\n");

    /* 5. The control: the chain at the speaker's sink instead (the rewind run through it a second time at the
     * tap, as a chain inside muse_audio_write_chained would) puts samples on the speaker that aren't R's: the
     * checks above would catch it. */
    {
        reset_all();
        play_chunks(40);
        int16_t rewind[REWIND_MAX];
        size_t kept = muse_audio_tx_clear(&tx, now_us(), rewind, REWIND_MAX);
        long chain_before = (long)nchain;
        run(F * 10);
        mg_chain_audio(&chain, rewind, kept);   /* (wrong: post-chain samples through the chain again) */
        speaker(rewind, (int)kept);
        play_chunks(10);
        int differ = 0;
        for (size_t k = 0; k < kept + 10 * F; k++) differ += written[nwritten - kept - 10 * F + k] != R[chain_before - (long)kept + (long)k];
        assert(differ > (int)kept / 2);
        printf("control: a chain after the rewind replays %d of %zu samples wrong\n", differ, kept + 10 * F);
    }
    return 0;
}
