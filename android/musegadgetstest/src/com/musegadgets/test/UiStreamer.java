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
package com.musegadgets.test;

import android.os.SystemClock;
import android.util.Log;

import java.util.Arrays;
import java.util.Locale;

/**
 * The encoder half of a UI stream (protocols/UI.md): pictures from
 * UiRenderer in, frames to the link out, only when the picture changed.
 *
 * A change goes out at once at a coarse quantizer (changeQp), so it shows
 * quickly; when the picture then holds still for refine1Ms the same picture
 * goes again at refine1Qp, and after refine2Ms at refine2Qp: each a P frame
 * coding only what the gadget's picture still lacks, so text ends sharp.
 * A still picture sends nothing. One keyframe at the start and whenever the
 * gadget asks; a frame that would pass the gadget's largest is redone at a
 * coarser quantizer (keyframes) or replaced by a keyframe (P frames).
 * x264 (jni/mgx264.c, X264.openUi): constant QP per frame, one reference,
 * no deblocking, infinite GOP. With two bands each is its own encoder.
 */
final class UiStreamer implements UiRenderer.Listener {
    private static final String TAG = "MuseCam";

    /* The policy (the codec study's numbers; see protocols/UI.md): a change
     * at the finest of QPS whose frame fits budgetMs of the link, then
     * refinements to refine1Qp and refine2Qp while the picture holds still.
     * changeQp > 0 forces the change QP (bench). */
    static final int[] QPS = {20, 24, 28, 32};
    volatile int changeQp = 0, refine1Qp = 24, refine2Qp = 18, keyQp = 0;
    volatile int refine1Ms = 300, refine2Ms = 1000, budgetMs = 300;
    /* Quality "sharp": a third refinement of a still picture (0: none). */
    volatile int refine3Qp = 0, refine3Ms = 2000;
    /* Bench: the I420 of the last picture sent, for a snapshot. */
    volatile byte[] lastSent;
    /*
     * Lossless still patches (protocols/UI.md): with stillPatches on (the
     * gadget takes them, quality sharp), once a picture has held still for
     * stillMs after its first refinement, the parts of the gadget's picture
     * that aren't exactly the phone's RGB565 are sent as zlib RGB565 patches
     * in place of the last refinement. The phone knows what the gadget's
     * decoder has (x264's reconstruction): a macroblock is dirty when that
     * changed (the gadget then redraws it from the decoded picture) or when
     * the source changed since its last patch. One band only.
     */
    volatile boolean stillPatches;
    volatile int stillMs = 600, stillMaxBytes = 120 * 1024;
    private byte[] shown565, pending565, recon, reconPrev;
    private boolean[] dirty;
    private boolean stillTooBig, reconWarned;     /* this picture's patch didn't fit: the last refinement instead */
    long statStills, statStillBytes, statStillUs;
    int lastStillBytes;
    volatile int linkKbps = 300;
    /* Bytes per changed macroblock at each of QPS, learned from the frames sent (text-heavy start values). */
    private final double[] bpm = {110, 75, 50, 35};
    private int lastQp;
    private long changeNs;
    private int coarsest;   /* the coarsest QP sent since the picture was last fully refined */
    volatile boolean refine = true;
    volatile int maxFps = 15;
    /* Pacing (protocols/UI.md, Pacing): a change frame starts at most every
     * 1000/maxFps ms, counted from the previous frame's start, and while the
     * last change was small (smallBytes or less) every 1000/fastFps ms; it
     * may be encoded while the previous one is still going out
     * (pipelineBytes or less of it left on the phone), and starts only while
     * at most aheadFrames frames sent are not yet shown on the gadget (its
     * buffer_update echo), so a faster rate can't queue pictures there.
     * legacyPacing: the old rule (1000/maxFps after the previous encode
     * ended, the link idle), for comparisons. */
    volatile int fastFps = 30;
    volatile int smallBytes = 2048, pipelineBytes = 2048, aheadFrames = 1;
    volatile boolean legacyPacing;
    private static final long ECHO_STALE_MS = 300;
    private volatile int shownNum = -1;
    private volatile long shownMs;
    private long lastStartMs;
    String preset = "medium";
    String params = null;

    private final VideoLink link;
    /* Bench: no gadget needed; frames go to dump only (UiSession's --ez uidry). */
    volatile boolean dry;
    volatile java.io.OutputStream dump;
    private int w, h, bands, splitRows;
    private long maxFrame;
    private long[] enc;
    private final Object lock = new Object();
    private byte[] pending;      /* the newest picture not yet looked at */
    private long pendingNs;
    private byte[] unfit;        /* the last picture no frame fitted (tried again once) */
    private byte[] shownPic;     /* the picture the gadget has (or will have) */
    private int stage;           /* 0: change sent, 1: first refinement sent, 2: done */
    private long changeMs, lastSendMs;
    private boolean needKey = true;
    private int frameNo;
    private long pts;
    private Thread thread;
    private volatile boolean running;
    /* When each frame's picture arrived from the source, by frame number: the latency echo. */
    private final long[] srcNs = new long[65536];
    /* Tap latency (the log's "tap ->" lines): the last tap's arrival, and per frame the tap it answers. */
    private volatile long touchNs;
    private final long[] tapNs = new long[65536];
    private final int[] tapEncMs = new int[65536];

    /** A tap (touch up) arrived from the gadget at rxNs (elapsedRealtimeNanos). */
    void markTouch(long rxNs) {
        touchNs = rxNs;
    }
    private final byte[] kind = new byte[65536];   /* 0 change, 1/2 refinement, 3 keyframe */
    /* Stats. */
    volatile long statFrames, statBytes, statKeys, statRefines, statChanges, statSkippedSame, statRedo;
    volatile long statEncUs;
    volatile int lastLatencyMs = -1, lastKeyBytes, lastChangeBytes, lastRefineBytes;
    private final int[] lat = new int[64];
    private int latN;

    UiStreamer(VideoLink link) {
        this.link = link;
    }

    /** Starts encoding w x h pictures (bands: 1 or 2; splitPct: the top band's share) into frames of at most maxFrame bytes. */
    boolean start(int w, int h, int bands, int splitPct, long maxFrame) {
        stop();
        this.w = w;
        this.h = h;
        this.bands = bands;
        this.maxFrame = maxFrame > 0 ? maxFrame : 60000;
        int codedH = (h + 15) & ~15;
        splitRows = bands > 1 ? Math.min(h - 16, Math.max(16, (codedH * splitPct / 100 + 8) & ~15)) : h;
        enc = new long[bands];
        for (int b = 0; b < bands; b++) {
            int bh = b == 0 ? splitRows : h - splitRows;
            enc[b] = X264.openUi(w, bh, preset, params);
            if (enc[b] == 0) {
                Log.e(TAG, "ui: x264 open failed for " + w + "x" + bh);
                closeEncoders();
                return false;
            }
        }
        synchronized (lock) {
            pending = null;
            shownPic = null;
            needKey = true;
            stage = 2;
            frameNo = 0;
            coarsest = 0;
        }
        shownNum = -1;   /* nothing of this stream shown yet */
        running = true;
        thread = new Thread(this::loop, "ui-enc");
        thread.start();
        Log.i(TAG, "ui: encoding " + w + "x" + h + (bands > 1 ? " in two bands (top " + splitRows + " rows)" : "")
                + ", max frame " + this.maxFrame + " B, change QP " + (changeQp > 0 ? changeQp : "auto (" + budgetMs + " ms of "
                + linkKbps + " kb/s)") + " -> " + refine1Qp + " @" + refine1Ms + " ms -> " + refine2Qp + " @" + refine2Ms + " ms");
        return true;
    }

    /** Waits for the encoder thread to end before freeing the encoders: it may be inside x264, which an interrupt doesn't stop. */
    void stop() {
        if (thread == Thread.currentThread()) {
            throw new IllegalStateException("ui: stop() from the encoder thread");
        }
        running = false;
        Thread t = thread;
        thread = null;
        if (t != null) {
            t.interrupt();
            boolean interrupted = false;
            while (t.isAlive()) {
                try {
                    t.join();
                } catch (InterruptedException e) {
                    interrupted = true;
                }
            }
            if (interrupted) {
                Thread.currentThread().interrupt();
            }
        }
        closeEncoders();
    }

    boolean running() {
        return running;
    }

    private void closeEncoders() {
        if (enc != null) {
            for (long e : enc) {
                if (e != 0) {
                    X264.close(e);
                }
            }
            enc = null;
        }
    }

    void requestKeyframe() {
        synchronized (lock) {
            needKey = true;
            lock.notifyAll();
        }
    }

    @Override
    public void onPicture(byte[] i420, long tNs) {
        onPicture(i420, null, tNs);
    }

    @Override
    public void onPicture(byte[] i420, byte[] rgb565, long tNs) {
        if (i420.length != w * h * 3 / 2) {
            return;   /* from before a resize */
        }
        synchronized (lock) {
            pending = i420;
            pending565 = rgb565 != null && rgb565.length == w * h * 2 ? rgb565 : null;
            pendingNs = tNs;
            lock.notifyAll();
        }
    }

    @Override
    public boolean wantsRgb565() {
        return stillPatches && bands == 1;
    }

    /** The latency echo: frame number shown ageMs before rxNs. */
    void onShown(int num, int ageMs, long rxNs) {
        shownNum = num;
        shownMs = SystemClock.elapsedRealtime();
        long tap = tapNs[num & 0xffff];
        if (tap != 0) {
            tapNs[num & 0xffff] = 0;
            Log.i(TAG, String.format(Locale.US, "ui: tap -> shown %d ms (frame %d)", (rxNs - tap) / 1_000_000 - ageMs, num));
        }
        long src = srcNs[num & 0xffff];
        if (src == 0) {
            return;
        }
        srcNs[num & 0xffff] = 0;
        int ms = (int) ((rxNs - src) / 1_000_000L) - ageMs;
        if (ms < 0 || ms > 30000) {
            return;
        }
        if (kind[num & 0xffff] == 0 || kind[num & 0xffff] == KIND_KEY) {
            lastLatencyMs = ms;
            synchronized (lat) {
                lat[latN++ % lat.length] = ms;
            }
        }
        String k = kind[num & 0xffff] == KIND_KEY ? "keyframe" : kind[num & 0xffff] == KIND_STILL ? "still patch"
                : kind[num & 0xffff] == 0 ? "change" : "refine" + kind[num & 0xffff];
        Log.i(TAG, "ui: frame " + num + " (" + k + ") shown " + ms + " ms after the picture changed");
    }

    /** Median of the recent change-to-shown latencies (ms), -1: none. */
    int medianLatency() {
        synchronized (lat) {
            int n = Math.min(latN, lat.length);
            if (n == 0) {
                return -1;
            }
            int[] c = Arrays.copyOf(lat, n);
            Arrays.sort(c);
            return c[n / 2];
        }
    }

    private void loop() {
        while (running) {
            byte[] pic = null;
            long picNs = 0;
            int changedMbs = 0;
            int qp = 0;
            boolean key = false, refineFrame = false;
            byte[] pic565 = null;
            byte[] encIn = null;
            int keptMbs = -1;
            byte[] still565 = null;
            boolean[] stillMask = null;
            synchronized (lock) {
                long now = SystemClock.elapsedRealtime();
                /* Only the newest picture matters: start a frame only when
                 * the link has nothing of ours waiting on the phone. */
                long queued = dry ? 0 : link.queuedBytes();
                boolean linkFree = dry || (link.streaming() && queued == 0);
                boolean changeFree;
                long gap;
                if (legacyPacing) {
                    changeFree = linkFree;
                    gap = 1000L / Math.max(1, maxFps) - (now - lastSendMs);
                } else {
                    changeFree = (dry || (link.streaming() && queued <= pipelineBytes)) && gadgetKeepsUp(now);
                    gap = changeGapMs() - (now - lastStartMs);
                }
                if (changeFree && gap <= 0 && pending != null && (needKey || shownPic == null
                        || !Arrays.equals(pending, shownPic))) {
                    lastStartMs = now;
                    pic = pending;
                    pic565 = pending565;
                    picNs = pendingNs;
                    pending = null;
                    key = needKey || shownPic == null;
                    changedMbs = key ? mbCount() : changed(pic, shownPic);
                    qp = pickQp(key, changedMbs);
                } else if (pending != null && shownPic != null && !needKey && Arrays.equals(pending, shownPic)) {
                    pending = null;   /* the source redrew the same picture */
                    statSkippedSame++;
                } else if (linkFree && needKey && shownPic != null) {
                    pic = shownPic;   /* the gadget asked for a keyframe of what it shows */
                    pic565 = shown565;   /* the same picture: its patches stay possible */
                    picNs = SystemClock.elapsedRealtimeNanos();
                    key = true;
                    changedMbs = mbCount();
                    qp = pickQp(true, changedMbs);
                } else if (linkFree && stillReady() && shownPic != null && pending == null && stage >= 1
                        && now - changeMs >= stillMs) {
                    still565 = shown565;
                    stillMask = dirty.clone();
                    Arrays.fill(dirty, false);
                } else if (linkFree && refine && shownPic != null && pending == null && stage < maxStage()
                        && now - changeMs >= levelMs(stage)) {
                    pic = shownPic;
                    picNs = 0;
                    qp = levelQp(stage);
                    refineFrame = true;
                    changedMbs = 0;
                }
                if (still565 == null && pic == null) {
                    try {
                        lock.wait(gap > 0 && pending != null ? gap : 10);
                    } catch (InterruptedException e) {
                        return;
                    }
                    continue;
                }
                needKey = false;
                encIn = pic;
                if (pic != null && !key && stillPatches && bands == 1 && reconPrev != null && dirty != null) {
                    /* Macroblocks already exact on the gadget (clean, and the
                     * source unchanged there) get the decoder's own picture as
                     * the encoder's input: x264 skips them, the gadget's
                     * decoded picture doesn't change there, and the patches
                     * on its panel stay (else the residuals x264 re-codes
                     * everywhere make it redraw them from the lossy picture). */
                    encIn = pic.clone();
                    int bw = (w + 15) / 16, bh = (h + 15) / 16, kept = 0;
                    for (int by = 0; by < bh; by++) {
                        for (int bx = 0; bx < bw; bx++) {
                            if (!dirty[by * bw + bx] && (refineFrame || shownPic == null || !blockDiffers(pic, shownPic, bx, by))) {
                                copyBlock(reconPrev, encIn, bx, by);
                                kept++;
                            }
                        }
                    }
                    keptMbs = kept;
                }
            }
            if (still565 != null) {
                sendStill(still565, stillMask);
                continue;
            }
            long t0 = SystemClock.elapsedRealtimeNanos();
            byte[] frame = encode(encIn != null ? encIn : pic, key, qp);
            if (frame == null && !key) {
                /* Too big for the gadget as a P frame: a keyframe instead. */
                statRedo++;
                key = true;
                changedMbs = mbCount();
                qp = 32;
                frame = encode(pic, true, qp);
            }
            qp = lastQp;   /* what encode() ended at (it may go coarser to fit) */
            long encUs = (SystemClock.elapsedRealtimeNanos() - t0) / 1000;
            statEncUs += encUs;
            if (frame == null) {
                Log.w(TAG, "ui: no frame fits " + maxFrame + " B");
                synchronized (lock) {
                    /* x264's references moved on with the dropped picture: the next frame is a keyframe,
                     * tried no sooner than 100 ms from now. */
                    needKey = true;
                    if (pending == null && pic != unfit) {
                        pending = pic;   /* the newest picture gets one more try, as the next keyframe */
                        pendingNs = picNs;
                    }
                    unfit = pic;
                    try {
                        lock.wait(100);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
                continue;
            }
            int num;
            synchronized (lock) {
                num = frameNo++ & 0xffff;
                trackDirty(key, refineFrame ? null : pic, refineFrame ? null : shownPic);
                if (!refineFrame) {
                    shownPic = pic;
                    shown565 = pic565;
                    stillTooBig = false;
                    /* Refine from the coarsest QP since the picture was last
                     * fully refined (a fine small change after a coarse big
                     * one still leaves the big one to sharpen): to refine1Qp
                     * if coarser, then refine2Qp; 20 or finer needs none. */
                    coarsest = key ? qp : Math.max(coarsest, qp);
                    stage = 0;
                    while (stage < maxStage() && coarsest <= levelQp(stage) + (stage > 0 ? 2 : 0)) {
                        stage++;
                    }
                    changeMs = SystemClock.elapsedRealtime();
                } else {
                    stage++;
                    coarsest = Math.min(coarsest, qp);
                }
                lastSendMs = SystemClock.elapsedRealtime();
            }
            frame[2] = (byte) num;
            frame[3] = (byte) (num >> 8);
            if (!refineFrame) {
                changeNs = picNs;
            }
            srcNs[num] = refineFrame ? changeNs : picNs;   /* refinements: time from the change they sharpen */
            kind[num] = (byte) (key ? KIND_KEY : refineFrame ? stage : 0);
            long tap = touchNs;
            tapNs[num] = 0;
            if (!refineFrame && tap != 0 && picNs >= tap) {
                touchNs = 0;
                tapNs[num] = tap;
                tapEncMs[num] = (int) (encUs / 1000);
                Log.i(TAG, String.format(Locale.US, "ui: tap -> picture changed %d ms, -> encoded %d ms (encode %d ms)",
                        (picNs - tap) / 1_000_000, (SystemClock.elapsedRealtimeNanos() - tap) / 1_000_000, encUs / 1000));
            }
            lastSent = pic;
            int len = frame.length - VideoFrame.HEADER_LEN;
            if (changedMbs > 0) {
                int k = qpIndex(qp);
                if (k >= 0) {
                    bpm[k] = 0.7 * bpm[k] + 0.3 * Math.max(4, (double) len / changedMbs);
                }
            }
            statFrames++;
            statBytes += frame.length;
            if (key) {
                statKeys++;
                lastKeyBytes = len;
            } else if (refineFrame) {
                statRefines++;
                lastRefineBytes = len;
            } else {
                statChanges++;
                lastChangeBytes = len;
            }
            Log.i(TAG, String.format(Locale.US, "ui: frame %d %s QP %d, %d B%s", num,
                    key ? "keyframe" : refineFrame ? "refine" + kind[num] : "change", qp, len,
                    (changedMbs > 0 ? " (" + changedMbs + " blocks changed)" : "")
                            + (keptMbs >= 0 ? ", " + keptMbs + " kept as the gadget has them" : "")));
            java.io.OutputStream d = dump;
            if (d != null) {
                try {
                    d.write(frame, VideoFrame.HEADER_LEN + (bands > 1 ? 4 + (frame[8] & 0xff | (frame[9] & 0xff) << 8
                            | (frame[10] & 0xff) << 16) : 0), len - (bands > 1 ? 4 + (frame[8] & 0xff | (frame[9] & 0xff) << 8
                            | (frame[10] & 0xff) << 16) : 0));
                    d.flush();
                } catch (java.io.IOException e) {
                    dump = null;
                }
            }
            if (!dry) {
                link.sendFrame(frame);
            }
        }
    }

    /* The least time between change frames' starts: faster while changes are small. */
    private long changeGapMs() {
        int fps = Math.max(1, maxFps);
        if (fastFps > fps && lastChangeBytes > 0 && lastChangeBytes <= smallBytes) {
            fps = fastFps;
        }
        return 1000L / fps;
    }

    /* At most aheadFrames frames sent but not yet shown (the gadget's echo);
     * without an echo for a while (an older gadget, a lost update) it doesn't hold frames back. */
    private boolean gadgetKeepsUp(long now) {
        int shown = shownNum;
        if (dry || shown < 0 || frameNo == 0 || now - shownMs > ECHO_STALE_MS) {
            return true;
        }
        int ahead = ((frameNo - 1) - shown) & 0xffff;
        return ahead <= aheadFrames || ahead > 0x8000;
    }

    /* One frame (header with number 0, patched by the caller): every band's access unit. Null: too big. */
    private byte[] encode(byte[] pic, boolean key, int qp) {
        byte[][] aus = new byte[bands][];
        int total = 0;
        for (int attempt = 0; attempt < 6; attempt++) {
            total = 0;
            for (int b = 0; b < bands; b++) {
                byte[] band = bands == 1 ? pic : bandOf(pic, b);
                int bh = b == 0 ? splitRows : h - splitRows;
                aus[b] = X264.encodeQp(enc[b], band, pts, key, qp, null);
                if (aus[b] == null) {
                    return null;
                }
                total += aus[b].length + (b < bands - 1 ? 4 : 0);
            }
            pts++;
            lastQp = qp;
            if (total <= maxFrame) {
                break;
            }
            if (!key) {
                return null;
            }
            Log.i(TAG, "ui: keyframe " + total + " B at QP " + qp + " > " + maxFrame + ": again coarser");
            qp = Math.min(51, qp + 4);
            statRedo++;
        }
        if (total > maxFrame) {
            return null;
        }
        byte[] fr = new byte[VideoFrame.HEADER_LEN + total];
        fr[0] = (byte) VideoFrame.MAGIC;
        fr[1] = (byte) (key ? VideoFrame.KEYFRAME : 0);
        fr[4] = (byte) total;
        fr[5] = (byte) (total >> 8);
        fr[6] = (byte) (total >> 16);
        fr[7] = (byte) (total >> 24);
        int o = VideoFrame.HEADER_LEN;
        for (int b = 0; b < bands; b++) {
            if (b < bands - 1) {
                int n = aus[b].length;
                fr[o] = (byte) n;
                fr[o + 1] = (byte) (n >> 8);
                fr[o + 2] = (byte) (n >> 16);
                fr[o + 3] = (byte) (n >> 24);
                o += 4;
            }
            System.arraycopy(aus[b], 0, fr, o, aus[b].length);
            o += aus[b].length;
        }
        return fr;
    }

    private static final int KIND_KEY = 9, KIND_STILL = 8;

    private int maxStage() {
        if (stillPatches && bands == 1 && !stillTooBig) {
            return 1;   /* the patch is the last refinement */
        }
        return refine3Qp > 0 ? 3 : 2;
    }

    /* A patch is due: patches on, the picture's RGB565 known, something dirty. Under lock. */
    private boolean stillReady() {
        if (!stillPatches || bands != 1 || stillTooBig || shown565 == null || dirty == null) {
            return false;
        }
        for (boolean d : dirty) {
            if (d) {
                return true;
            }
        }
        return false;
    }

    /*
     * After an encode (under lock): the macroblocks whose decoded picture
     * changed (from x264's reconstruction), and those whose source changed
     * (pic vs prev), are no longer exactly on the gadget.
     */
    private void trackDirty(boolean key, byte[] pic, byte[] prev) {
        if (!stillPatches || bands != 1) {
            return;
        }
        int bw = (w + 15) / 16, bh = (h + 15) / 16;
        if (dirty == null || dirty.length != bw * bh) {
            dirty = new boolean[bw * bh];
            recon = new byte[w * h * 3 / 2];
            reconPrev = null;
        }
        boolean got = X264.reconI420(enc[0], recon);
        if (!got && !reconWarned) {
            reconWarned = true;
            Log.w(TAG, "ui: no reconstructed picture from x264: still patches cover every macroblock");
        }
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                if (key || !got || reconPrev == null || blockDiffers(recon, reconPrev, bx, by)
                        || (pic != null && prev != null && blockDiffers(pic, prev, bx, by))) {
                    dirty[by * bw + bx] = true;
                }
            }
        }
        if (got) {
            byte[] t = reconPrev == null ? new byte[recon.length] : reconPrev;
            reconPrev = recon;
            recon = t;
        }
    }

    /* Copies 16x16 block (bx, by) of an I420 picture (luma and chroma) from src to dst. */
    private void copyBlock(byte[] src, byte[] dst, int bx, int by) {
        int x0 = bx * 16, x1 = Math.min(w, x0 + 16), y0 = by * 16, y1 = Math.min(h, y0 + 16);
        for (int y = y0; y < y1; y++) {
            System.arraycopy(src, y * w + x0, dst, y * w + x0, x1 - x0);
        }
        int cw = w / 2, uo = w * h, vo = uo + cw * (h / 2);
        for (int y = y0 / 2; y < y1 / 2; y++) {
            System.arraycopy(src, uo + y * cw + x0 / 2, dst, uo + y * cw + x0 / 2, (x1 - x0) / 2);
            System.arraycopy(src, vo + y * cw + x0 / 2, dst, vo + y * cw + x0 / 2, (x1 - x0) / 2);
        }
    }

    /* Does 16x16 block (bx, by) differ between two I420 pictures (luma and chroma)? */
    private boolean blockDiffers(byte[] a, byte[] b, int bx, int by) {
        int x0 = bx * 16, x1 = Math.min(w, x0 + 16), y0 = by * 16, y1 = Math.min(h, y0 + 16);
        for (int y = y0; y < y1; y++) {
            if (!Arrays.equals(a, y * w + x0, y * w + x1, b, y * w + x0, y * w + x1)) {
                return true;
            }
        }
        int cw = w / 2, uo = w * h, vo = uo + cw * (h / 2);
        for (int y = y0 / 2; y < y1 / 2; y++) {
            int o = y * cw;
            if (!Arrays.equals(a, uo + o + x0 / 2, uo + o + x1 / 2, b, uo + o + x0 / 2, uo + o + x1 / 2)
                    || !Arrays.equals(a, vo + o + x0 / 2, vo + o + x1 / 2, b, vo + o + x0 / 2, vo + o + x1 / 2)) {
                return true;
            }
        }
        return false;
    }

    /* The dirty macroblocks as patches: a rectangle per run of rows (as the gadget merges its redraws). */
    private void sendStill(byte[] rgb565, boolean[] mask) {
        long t0 = SystemClock.elapsedRealtimeNanos();
        int bw = (w + 15) / 16, bh = (h + 15) / 16;
        java.util.List<int[]> rects = new java.util.ArrayList<>();
        int[] cur = null;
        for (int by = 0; by < bh; by++) {
            int lo = bw, hi = -1;
            for (int bx = 0; bx < bw; bx++) {
                if (mask[by * bw + bx]) {
                    lo = Math.min(lo, bx);
                    hi = bx;
                }
            }
            if (hi < 0) {
                cur = null;
                continue;
            }
            int x0 = lo * 16, x1 = Math.min(w, (hi + 1) * 16) & ~1, y0 = by * 16, y1 = Math.min(h, y0 + 16) & ~1;
            if (cur != null) {
                int ux0 = Math.min(cur[0], x0), ux1 = Math.max(cur[2], x1);
                long union = (long) (ux1 - ux0) * (y1 - cur[1]);
                long sep = (long) (cur[2] - cur[0]) * (cur[3] - cur[1]) + (long) (x1 - x0) * (y1 - y0);
                if (union <= sep + sep / 4) {
                    cur[0] = ux0;
                    cur[2] = ux1;
                    cur[3] = y1;
                    continue;
                }
            }
            cur = new int[] {x0, y0, x1, y1};
            rects.add(cur);
        }
        if (rects.size() > 6) {
            int[] b = {w, h, 0, 0};
            for (int[] r : rects) {
                b[0] = Math.min(b[0], r[0]);
                b[1] = Math.min(b[1], r[1]);
                b[2] = Math.max(b[2], r[2]);
                b[3] = Math.max(b[3], r[3]);
            }
            rects.clear();
            rects.add(b);
        }
        java.util.List<byte[]> frames = new java.util.ArrayList<>();
        int total = 0, px = 0;
        java.util.zip.Deflater z = new java.util.zip.Deflater(6);
        byte[] buf = new byte[64 * 1024];
        for (int[] r : rects) {
            int rw = r[2] - r[0], rh = r[3] - r[1];
            if (rw <= 0 || rh <= 0) {
                continue;
            }
            byte[] raw = new byte[rw * rh * 2];
            for (int y = 0; y < rh; y++) {
                System.arraycopy(rgb565, ((r[1] + y) * w + r[0]) * 2, raw, y * rw * 2, rw * 2);
            }
            z.reset();
            z.setInput(raw);
            z.finish();
            java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream(raw.length / 8 + 64);
            bo.write(new byte[VideoFrame.HEADER_LEN + 9], 0, VideoFrame.HEADER_LEN + 9);
            while (!z.finished()) {
                int n = z.deflate(buf);
                bo.write(buf, 0, n);
            }
            byte[] fr = bo.toByteArray();
            int len = fr.length - VideoFrame.HEADER_LEN;
            fr[0] = (byte) VideoFrame.MAGIC;
            fr[1] = (byte) VideoFrame.STILL;
            fr[4] = (byte) len;
            fr[5] = (byte) (len >> 8);
            fr[6] = (byte) (len >> 16);
            fr[7] = (byte) (len >> 24);
            int o = VideoFrame.HEADER_LEN;
            int[] v = {r[0], r[1], rw, rh};
            for (int i = 0; i < 4; i++) {
                fr[o + 2 * i] = (byte) v[i];
                fr[o + 2 * i + 1] = (byte) (v[i] >> 8);
            }
            fr[o + 8] = 1;   /* mg_video_still_rgb565_zlib */
            frames.add(fr);
            total += fr.length;
            px += rw * rh;
        }
        z.end();
        long us = (SystemClock.elapsedRealtimeNanos() - t0) / 1000;
        boolean fits = total <= stillMaxBytes;
        for (byte[] fr : frames) {
            fits &= fr.length - VideoFrame.HEADER_LEN <= maxFrame;
        }
        synchronized (lock) {
            if (!fits) {
                /* Too big (a photo): the H.264 refinement instead, and patch nothing of this picture. */
                stillTooBig = true;
                /* Those macroblocks are still not exact on the gadget: dirty again, so the encoder codes them. */
                if (dirty != null && dirty.length == mask.length) {
                    for (int i = 0; i < mask.length; i++) {
                        dirty[i] |= mask[i];
                    }
                }
                Log.i(TAG, String.format(Locale.US, "ui: still patch %d B for %d px is too big: refining instead", total, px));
                return;
            }
            for (byte[] fr : frames) {
                int num = frameNo++ & 0xffff;
                fr[2] = (byte) num;
                fr[3] = (byte) (num >> 8);
                kind[num] = KIND_STILL;
                srcNs[num] = changeNs;
            }
            lastSendMs = SystemClock.elapsedRealtime();
        }
        statStills++;
        statStillBytes += total;
        statStillUs += us;
        lastStillBytes = total;
        Log.i(TAG, String.format(Locale.US, "ui: still patch: %d rect%s, %d px (%.0f%%), %d B, deflate %d ms",
                frames.size(), frames.size() == 1 ? "" : "s", px, 100.0 * px / (w * h), total, us / 1000));
        if (!dry) {
            for (byte[] fr : frames) {
                link.sendFrame(fr);
            }
        }
    }


    private int levelQp(int i) {
        return i == 0 ? refine1Qp : i == 1 ? refine2Qp : refine3Qp;
    }

    private int levelMs(int i) {
        return i == 0 ? refine1Ms : i == 1 ? refine2Ms : refine3Ms;
    }

    /** The quality changed: refine the still picture to the new last level now. */
    void refineAgain() {
        synchronized (lock) {
            if (shownPic != null) {
                stage = 0;
                while (stage < maxStage() && coarsest <= levelQp(stage) + (stage > 0 ? 2 : 0)) {
                    stage++;
                }
                changeMs = SystemClock.elapsedRealtime() - 60_000;
                lock.notifyAll();
            }
        }
    }

    private int mbCount() {
        return ((w + 15) / 16) * ((h + 15) / 16);
    }

    /* 16x16 luma blocks that differ between two pictures. */
    private int changed(byte[] a, byte[] b) {
        int bw = (w + 15) / 16, bh = (h + 15) / 16, n = 0;
        for (int by = 0; by < bh; by++) {
            int y0 = by * 16, y1 = Math.min(h, y0 + 16);
            for (int bx = 0; bx < bw; bx++) {
                int x0 = bx * 16, x1 = Math.min(w, x0 + 16);
                for (int y = y0; y < y1; y++) {
                    if (!Arrays.equals(a, y * w + x0, y * w + x1, b, y * w + x0, y * w + x1)) {
                        n++;
                        break;
                    }
                }
            }
        }
        return Math.max(1, n);
    }

    private static int qpIndex(int qp) {
        for (int i = 0; i < QPS.length; i++) {
            if (QPS[i] == qp) {
                return i;
            }
        }
        return -1;
    }

    /* The finest of QPS whose frame (changed blocks x learned bytes per block) fits budgetMs of the link. */
    private int pickQp(boolean key, int changedMbs) {
        int forced = key ? keyQp : changeQp;
        if (forced > 0) {
            return forced;
        }
        double budget = Math.min(maxFrame * 0.9, linkKbps * 1000.0 / 8 * budgetMs / 1000);
        for (int i = 0; i < QPS.length; i++) {
            if (changedMbs * bpm[i] <= budget) {
                return QPS[i];
            }
        }
        return QPS[QPS.length - 1];
    }

    /* Band b of an I420 picture, as its own I420 picture. */
    private byte[] bandOf(byte[] pic, int b) {
        int y0 = b == 0 ? 0 : splitRows, bh = b == 0 ? splitRows : h - splitRows;
        byte[] o = new byte[w * bh * 3 / 2];
        System.arraycopy(pic, y0 * w, o, 0, w * bh);
        int cw = w / 2, u = w * h, v = u + w * h / 4, ou = w * bh, ov = ou + w * bh / 4;
        System.arraycopy(pic, u + y0 / 2 * cw, o, ou, cw * bh / 2);
        System.arraycopy(pic, v + y0 / 2 * cw, o, ov, cw * bh / 2);
        return o;
    }

    /** One line for the stats. */
    String describe() {
        return String.format(Locale.US, "%d frames (%d key, %d change, %d refine), last key %d B, change %d B, "
                        + "refine %d B, latency %d ms (median %d)", statFrames, statKeys, statChanges, statRefines,
                lastKeyBytes, lastChangeBytes, lastRefineBytes, lastLatencyMs, medianLatency());
    }
}
