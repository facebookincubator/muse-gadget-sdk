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
package com.musegadgets.camstream;

import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Log;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/**
 * Pre-recorded clips as looping video streams. Each clip (any H.264 or other
 * video the phone decodes, or an excerpt of one) is decoded once,
 * area-downscaled to the gadget's size, masked to its circle and re-encoded
 * at the stream's bitrate, so playing it costs the phone nothing but sending
 * it.
 *
 * The clip is encoded twice in one run, so the second pass's first frame
 * predicts from the last: a keyed loop (an IDR, then P frames) and a steady
 * loop (P frames only) that follows on from either loop. Playback sends the
 * keyed loop once, then steady loops, and a keyed loop again every
 * REFRESH_LOOPS loops to clear any drift. That way a loop doesn't cost a
 * keyframe (a 10-20 KB burst the link takes a few frame periods to carry),
 * so the wrap is as smooth as any other frame. A keyframe request, or a
 * switch to another clip, starts the next keyed loop at once.
 */
final class ClipStreamer {
    private static final String TAG = VideoLink.TAG;
    private static final int REFRESH_LOOPS = 6;
    private static final int MAX_QUEUED = 6000, CATCH_UP_MS = 250;

    interface Sink {
        void onFrame(byte[] frame, boolean key);
        long queuedBytes();
    }

    /** Where a clip comes from: sets the extractor's data source (a file, an asset, a URL). */
    interface Source {
        void open(MediaExtractor ex) throws java.io.IOException;
    }

    /** One clip, encoded for one size and bitrate. */
    static final class Clip {
        final String name;
        final int size, fps, kbps;
        final List<byte[]> keyed = new ArrayList<>(), steady = new ArrayList<>();
        /* A closed loop: every loop starts on its own IDR (keyed and steady are the same frames). */
        boolean closed;
        /* VP8 frames (Vpx.java), else H.264 access units. */
        boolean vp8;
        String info = "";

        Clip(String name, int size, int fps, int kbps) {
            this.name = name;
            this.size = size;
            this.fps = fps;
            this.kbps = kbps;
        }

        int maxFrame;   /* the largest access unit (a keyframe takes a while to go out: no hold for that) */

        void measure() {
            for (byte[] au : keyed) {
                maxFrame = Math.max(maxFrame, au.length);
            }
            for (byte[] au : steady) {
                maxFrame = Math.max(maxFrame, au.length);
            }
        }

        long loopBytes() {
            long n = 0;
            for (byte[] au : steady) {
                n += au.length;
            }
            return n;
        }

        int steadyKbps() {
            return steady.isEmpty() ? 0 : (int) (loopBytes() * 8L * fps / steady.size() / 1000);
        }
    }

    private final Sink sink;
    private HandlerThread thread;
    private Handler handler;
    private volatile boolean running;
    private volatile Clip clip;
    private Clip lastSent;
    private int pos, loop, frameNo;
    private boolean keyedLoop = true;
    private long nextNs;
    volatile int statSent, statLoops, statKeys;
    volatile long statHeldMs;

    ClipStreamer(Sink sink) {
        this.sink = sink;
    }

    Clip clip() {
        return clip;
    }

    /**
     * Decodes, downscales and encodes one clip; blocks for a second or two
     * (longer for a source on the network). fps: the clip's frame rate (the
     * source's frames are taken one for one). startUs and maxFrames pick an
     * excerpt (0 and 0: all of it); keep it to a few seconds, since every
     * frame is held decoded while it encodes.
     */
    static Clip encode(String name, Source src, int size, int fps, int kbps, boolean round, long startUs, int maxFrames)
            throws Exception {
        return encode(name, src, size, fps, kbps, round, startUs, maxFrames, false);
    }

    /** vp8: encoded with libvpx (Vpx.java, which the build must have). */
    static Clip encode(String name, Source src, int size, int fps, int kbps, boolean round, long startUs, int maxFrames,
            boolean vp8) throws Exception {
        long t0 = SystemClock.elapsedRealtime();
        List<byte[]> frames = decodeScaled(src, size, round, startUs, maxFrames);
        long t1 = SystemClock.elapsedRealtime();
        Clip c = new Clip(name, size, fps, kbps);
        c.vp8 = vp8;
        if (vp8) {
            encodeLoopsVp8(frames, c);
        } else {
            encodeLoops(frames, c);
        }
        c.measure();
        long keyBytes = c.keyed.get(0).length;
        c.info = String.format(java.util.Locale.US,
                "%s: %d frames @%d fps (%.1f s), keyframe %d B, loop %.0f kb/s; decode+scale %d ms, encode %d ms (%s)",
                name, c.steady.size(), c.fps, c.steady.size() / (float) c.fps, keyBytes, (float) c.steadyKbps(),
                t1 - t0, SystemClock.elapsedRealtime() - t1, vp8 ? "VP8" : "H.264");
        Log.i(TAG, "clip: " + c.info);
        if (saveDir != null) {
            save(c);
        }
        return c;
    }

    /** --ez saveclips: each encoded clip's loops as Annex-B, files/clips/<name>_<kbps>_{keyed,steady}.264. */
    static volatile java.io.File saveDir;

    private static void save(Clip c) {
        for (int k = 0; k < 2; k++) {
            java.io.File f = new java.io.File(saveDir, c.name + "_" + c.kbps + (k == 0 ? "_keyed" : "_steady") + ".264");
            try (java.io.FileOutputStream o = new java.io.FileOutputStream(f)) {
                for (byte[] au : k == 0 ? c.keyed : c.steady) {
                    o.write(au);
                }
            } catch (java.io.IOException e) {
                Log.w(TAG, "saving " + f, e);
            }
        }
    }

    /* ---- decoding and scaling ------------------------------------------- */

    /* Every frame of the clip (or its excerpt) as I420 at size x size: centre-cropped to a square, area-averaged, circle-masked. */
    private static List<byte[]> decodeScaled(Source src, int size, boolean round, long startUs, int maxFrames)
            throws Exception {
        MediaExtractor ex = new MediaExtractor();
        src.open(ex);
        int track = -1;
        MediaFormat in = null;
        for (int i = 0; i < ex.getTrackCount(); i++) {
            MediaFormat f = ex.getTrackFormat(i);
            if (f.getString(MediaFormat.KEY_MIME).startsWith("video/")) {
                track = i;
                in = f;
                break;
            }
        }
        if (track < 0) {
            throw new IllegalArgumentException("no video track");
        }
        ex.selectTrack(track);
        if (startUs > 0) {
            ex.seekTo(startUs, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
        }
        MediaCodec dec = MediaCodec.createDecoderByType(in.getString(MediaFormat.KEY_MIME));
        in.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
        dec.configure(in, null, null, 0);
        dec.start();
        List<byte[]> out = new ArrayList<>();
        Scaler scaler = null;
        MediaCodec.BufferInfo bi = new MediaCodec.BufferInfo();
        boolean inputDone = false;
        try {
            while (true) {
                if (!inputDone) {
                    int idx = dec.dequeueInputBuffer(10000);
                    if (idx >= 0) {
                        int n = ex.readSampleData(dec.getInputBuffer(idx), 0);
                        if (n < 0) {
                            dec.queueInputBuffer(idx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputDone = true;
                        } else {
                            dec.queueInputBuffer(idx, 0, n, ex.getSampleTime(), 0);
                            ex.advance();
                        }
                    }
                }
                int o = dec.dequeueOutputBuffer(bi, 10000);
                if (o < 0) {
                    continue;
                }
                /* From the sync frame before startUs: frames before it only lead in. */
                if (bi.size > 0 && bi.presentationTimeUs >= startUs) {
                    Image img = dec.getOutputImage(o);
                    if (img != null) {
                        if (scaler == null) {
                            scaler = new Scaler(img.getCropRect().width(), img.getCropRect().height(), size, round);
                        }
                        out.add(scaler.scale(img));
                        img.close();
                    }
                }
                dec.releaseOutputBuffer(o, false);
                if ((bi.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0 || (maxFrames > 0 && out.size() >= maxFrames)) {
                    break;
                }
            }
        } finally {
            dec.stop();
            dec.release();
            ex.release();
        }
        if (out.isEmpty()) {
            throw new IllegalStateException("decoded no frames");
        }
        return out;
    }

    /**
     * Area-average downscaler for YUV_420_888 images: each source pixel of
     * the centred square adds to the one destination pixel it falls in, then
     * each destination pixel is divided by its count (a box filter that
     * keeps fur and edges crisp without aliasing).
     */
    private static final class Scaler {
        final int size, side, x0, y0;
        final boolean round;
        final int[] xmapY, ymapY, cntY, xmapC, ymapC, cntC;
        final int[] accY, accU, accV;
        final byte[] row;
        final boolean[] inY, inC;

        Scaler(int w, int h, int size, boolean round) {
            this.size = size & ~1;
            this.round = round;
            side = Math.min(w, h) & ~1;
            x0 = ((w - side) / 2) & ~1;
            y0 = ((h - side) / 2) & ~1;
            int s = this.size, cs = s / 2;
            xmapY = map(side, s);
            ymapY = xmapY;
            xmapC = map(side / 2, cs);
            ymapC = xmapC;
            cntY = counts(xmapY, s);
            cntC = counts(xmapC, cs);
            accY = new int[s * s];
            accU = new int[cs * cs];
            accV = new int[cs * cs];
            row = new byte[Math.max(w, 2 * w)];
            inY = new boolean[s * s];
            inC = new boolean[cs * cs];
            /* Black only in the 4x4 blocks wholly outside the circle plus
             * MASK_MARGIN pixels (as tools/clipenc's jpanel.py and the
             * camera): an edge on the transform grid costs fewer bits than
             * the exact circle, and the margin keeps a gadget's bilinear
             * upscale from pulling the black into the circle's edge. */
            double c = s / 2.0, r2 = (c + CameraStreamer.MASK_MARGIN) * (c + CameraStreamer.MASK_MARGIN);
            int bw = (s + 3) / 4;
            boolean[] blkIn = new boolean[bw * bw];
            for (int y = 0; y < s; y++) {
                for (int x = 0; x < s; x++) {
                    double dx = x + 0.5 - c, dy = y + 0.5 - c;
                    if (dx * dx + dy * dy <= r2) {
                        blkIn[(y >> 2) * bw + (x >> 2)] = true;
                    }
                }
            }
            for (int y = 0; y < s; y++) {
                for (int x = 0; x < s; x++) {
                    inY[y * s + x] = !round || blkIn[(y >> 2) * bw + (x >> 2)];
                }
            }
            for (int y = 0; y < cs; y++) {
                for (int x = 0; x < cs; x++) {
                    inC[y * cs + x] = !round || blkIn[(y >> 1) * bw + (x >> 1)];
                }
            }
        }

        private static int[] map(int src, int dst) {
            int[] m = new int[src];
            for (int i = 0; i < src; i++) {
                m[i] = (int) ((long) i * dst / src);
            }
            return m;
        }

        private static int[] counts(int[] m, int dst) {
            int[] c = new int[dst];
            for (int v : m) {
                c[v]++;
            }
            return c;
        }

        byte[] scale(Image img) {
            int s = size, cs = s / 2;
            Image.Plane[] p = img.getPlanes();
            int cropX = img.getCropRect().left + x0, cropY = img.getCropRect().top + y0;
            java.util.Arrays.fill(accY, 0);
            java.util.Arrays.fill(accU, 0);
            java.util.Arrays.fill(accV, 0);
            accumulate(p[0], cropX, cropY, side, xmapY, ymapY, accY, s);
            accumulate(p[1], cropX / 2, cropY / 2, side / 2, xmapC, ymapC, accU, cs);
            accumulate(p[2], cropX / 2, cropY / 2, side / 2, xmapC, ymapC, accV, cs);
            byte[] out = new byte[s * s * 3 / 2];
            for (int y = 0; y < s; y++) {
                for (int x = 0; x < s; x++) {
                    int i = y * s + x;
                    int n = cntY[x] * cntY[y];
                    out[i] = inY[i] ? (byte) ((accY[i] + n / 2) / n) : 16;
                }
            }
            int uo = s * s, vo = uo + cs * cs;
            for (int y = 0; y < cs; y++) {
                for (int x = 0; x < cs; x++) {
                    int i = y * cs + x;
                    int n = cntC[x] * cntC[y];
                    out[uo + i] = inC[i] ? (byte) ((accU[i] + n / 2) / n) : (byte) 128;
                    out[vo + i] = inC[i] ? (byte) ((accV[i] + n / 2) / n) : (byte) 128;
                }
            }
            return out;
        }

        private void accumulate(Image.Plane plane, int ox, int oy, int n, int[] xmap, int[] ymap, int[] acc, int dw) {
            ByteBuffer b = plane.getBuffer();
            int rs = plane.getRowStride(), ps = plane.getPixelStride();
            int len = (n - 1) * ps + 1;
            for (int y = 0; y < n; y++) {
                b.position((oy + y) * rs + ox * ps);
                b.get(row, 0, Math.min(len, b.remaining()));
                int base = ymap[y] * dw;
                for (int x = 0, k = 0; x < n; x++, k += ps) {
                    acc[base + xmap[x]] += row[k] & 0xff;
                }
            }
        }
    }

    /* ---- encoding ------------------------------------------------------- */

    /* Frames 0..N-1 twice through one encoder: the keyed loop, then the steady loop. */
    private static void encodeLoops(List<byte[]> frames, Clip c) throws Exception {
        if (c.size > 240 && X264.available()) {
            encodeLoopsX264(frames, c);
            return;
        }
        int size = c.size, n = frames.size(), total = 2 * n;
        MediaFormat f = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, size, size);
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
        f.setInteger(MediaFormat.KEY_BIT_RATE, c.kbps * 1000);
        f.setInteger(MediaFormat.KEY_FRAME_RATE, c.fps);
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 3600);   /* one keyframe: the keyed loop's first frame */
        f.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline);
        f.setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.AVCLevel31);
        f.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR);
        f.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);
        f.setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1);
        MediaCodec codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
        codec.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        codec.start();
        byte[] config = null;
        List<byte[]> aus = new ArrayList<>(total);
        int fed = 0;
        boolean inputDone = false;
        MediaCodec.BufferInfo bi = new MediaCodec.BufferInfo();
        try {
            while (true) {
                if (!inputDone) {
                    int idx = codec.dequeueInputBuffer(10000);
                    if (idx >= 0) {
                        long pts = (long) fed * 1_000_000L / c.fps;
                        if (fed == total) {
                            codec.queueInputBuffer(idx, 0, 0, pts, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputDone = true;
                        } else {
                            putI420(frames.get(fed % n), size, codec.getInputImage(idx));
                            codec.queueInputBuffer(idx, 0, size * size * 3 / 2, pts, 0);
                            fed++;
                        }
                    }
                }
                int o = codec.dequeueOutputBuffer(bi, 10000);
                if (o < 0) {
                    continue;
                }
                ByteBuffer b = codec.getOutputBuffer(o);
                byte[] au = new byte[bi.size];
                b.position(bi.offset);
                b.get(au);
                codec.releaseOutputBuffer(o, false);
                if ((bi.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                    config = au;
                } else if (au.length > 0) {
                    if (aus.isEmpty() && config != null && (au.length < 5 || (au[4] & 0x1f) != 7)) {
                        byte[] t = new byte[config.length + au.length];
                        System.arraycopy(config, 0, t, 0, config.length);
                        System.arraycopy(au, 0, t, config.length, au.length);
                        au = t;
                    }
                    aus.add(au);
                }
                if ((bi.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                    break;
                }
            }
        } finally {
            codec.stop();
            codec.release();
        }
        if (aus.size() != total) {
            throw new IllegalStateException("encoder gave " + aus.size() + " frames for " + total);
        }
        c.keyed.addAll(aus.subList(0, n));
        c.steady.addAll(aus.subList(n, total));
        for (byte[] au : c.steady) {
            if (isIdr(au)) {
                throw new IllegalStateException("encoder put a keyframe in the steady loop");
            }
        }
    }

    /*
     * The same with x264, where the app has it, for pictures past 240 px (the
     * ESP-Mosaico's 480): no deblocking (a phone's hardware encoder always
     * deblocks, which costs the gadget's decoder about 45% more), one IDR,
     * no intra refresh, a looser VBV for clips.
     */
    /*
     * Clips are encoded once, ahead of time, so x264 may look ahead: 40
     * frames of lookahead and macroblock-tree rate control put the bits
     * where they last (about 1.5 dB better on animated clips at 480 px than
     * the camera's settings). The peaks are held to what the link
     * carries: at most the clip's rate plus 2% over any 300 ms (the VBV), so
     * the busiest part of a clip arrives in time for the
     * gadget's playout delay instead of queueing up and stalling the
     * picture. No deblocking and decoder-friendly motion vectors keep the
     * decode within a frame time.
     */
    private static void encodeLoopsX264(List<byte[]> frames, Clip c) throws Exception {
        int n = frames.size();
        int maxKbps = c.kbps * 102 / 100, bufKbit = maxKbps * 300 / 1000;
        long h = X264.open(c.size, c.size, c.fps, c.kbps, "fast", CameraStreamer.withMvCheap(
                "no-deblock=1:intra-refresh=0:keyint=infinite:rc-lookahead=40:mbtree=1:vbv-maxrate=" + maxKbps
                        + ":vbv-bufsize=" + bufKbit, c.size));
        if (h == 0) {
            throw new IllegalStateException("x264 open failed");
        }
        int[] info = new int[2];
        List<byte[]> aus = new ArrayList<>(2 * n);
        try {
            for (int i = 0; i < 2 * n; i++) {
                byte[] au = X264.encode(h, frames.get(i % n), i, i == 0, info);
                if (au != null && au.length > 0) {
                    aus.add(au);   /* the lookahead holds the first frames back */
                }
            }
            for (int k = 0; k < 2 * n && aus.size() < 2 * n; k++) {
                byte[] au = X264.encode(h, null, 0, false, info);   /* flush */
                if (au == null || au.length == 0) {
                    break;
                }
                aus.add(au);
            }
        } finally {
            X264.close(h);
        }
        if (aus.size() != 2 * n) {
            throw new IllegalStateException("x264 gave " + aus.size() + " frames for " + 2 * n);
        }
        c.keyed.addAll(aus.subList(0, n));
        c.steady.addAll(aus.subList(n, 2 * n));
        for (byte[] au : c.steady) {
            if (isIdr(au)) {
                throw new IllegalStateException("encoder put a keyframe in the steady loop");
            }
        }
    }

    /*
     * The same with libvpx's VP8 encoder: the camera's settings for the
     * size (CameraStreamer.vp8Params), but VBR inside a 300 ms buffer, since
     * the whole loop is known. No lag, so one frame out for each in (no
     * hidden alt-ref frames to keep in step), and keyframes only when asked:
     * the keyed loop's first frame.
     */
    private static void encodeLoopsVp8(List<byte[]> frames, Clip c) {
        int n = frames.size();
        long h = Vpx.open(c.size, c.size, c.fps, c.kbps, null, CameraStreamer.vp8Params(null, c.size) + ":vbr=1:vbv-ms=300");
        if (h == 0) {
            throw new IllegalStateException("libvpx open failed");
        }
        int[] info = new int[2];
        try {
            for (int i = 0; i < 2 * n; i++) {
                byte[] f = Vpx.encode(h, frames.get(i % n), i, i == 0, info);
                if (f == null || f.length == 0) {
                    throw new IllegalStateException("libvpx gave no frame for picture " + i);
                }
                if (i >= n && (f[0] & 1) == 0) {
                    throw new IllegalStateException("encoder put a keyframe in the steady loop");
                }
                (i < n ? c.keyed : c.steady).add(f);
            }
        } finally {
            Vpx.close(h);
        }
    }

    private static boolean isIdr(byte[] au) {
        for (int i = 0; i + 4 < au.length; i++) {
            if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
                int t = au[i + 3] & 0x1f;
                if (t == 5) {
                    return true;
                }
                if (t == 1) {
                    return false;
                }
            }
        }
        return false;
    }

    private static void putI420(byte[] src, int size, Image dst) {
        Image.Plane[] p = dst.getPlanes();
        int cs = size / 2;
        ByteBuffer by = p[0].getBuffer();
        int yr = p[0].getRowStride();
        for (int y = 0; y < size; y++) {
            by.position(y * yr);
            by.put(src, y * size, size);
        }
        int uo = size * size, vo = uo + cs * cs;
        for (int k = 1; k <= 2; k++) {
            ByteBuffer b = p[k].getBuffer();
            int rs = p[k].getRowStride(), ps = p[k].getPixelStride(), off = k == 1 ? uo : vo;
            if (ps == 1) {
                for (int y = 0; y < cs; y++) {
                    b.position(y * rs);
                    b.put(src, off + y * cs, cs);
                }
            } else {
                for (int y = 0; y < cs; y++) {
                    for (int x = 0; x < cs; x++) {
                        b.put(y * rs + x * ps, src[off + y * cs + x]);
                    }
                }
            }
        }
    }

    /* ---- playback ------------------------------------------------------- */

    /** Plays clip from its keyed loop; while running, switches over at the next frame. */
    void play(Clip c) {
        Handler h = handler;
        if (running && h != null) {
            h.post(() -> {
                clip = c;
                restartKeyed();
            });
            return;
        }
        clip = c;
        start();
    }

    private void start() {
        stop();
        thread = new HandlerThread("clips");
        thread.start();
        handler = new Handler(thread.getLooper());
        running = true;
        restartKeyed();
        nextNs = SystemClock.elapsedRealtimeNanos();
        handler.post(this::tick);
    }

    void stop() {
        running = false;
        if (thread != null) {
            thread.quitSafely();
            thread = null;
            handler = null;
        }
    }

    /** Start the next keyed loop now (the gadget lost a frame). */
    void requestKeyframe() {
        Handler h = handler;
        if (h != null) {
            h.post(this::restartKeyed);
        }
    }

    private void restartKeyed() {
        pos = 0;
        loop = 0;
        keyedLoop = true;
    }

    private void tick() {
        Clip c = clip;
        if (!running || c == null) {
            return;
        }
        long period = 1_000_000_000L / c.fps;
        /* Encoded frames can't be skipped (the next ones predict from them):
         * when the link falls behind, the frame waits, and the frames after
         * it catch up on the clock (up to CATCH_UP_MS late) so a hiccup costs
         * no frames. */
        if (sink.queuedBytes() > Math.max(MAX_QUEUED, c.maxFrame + 2000)) {
            statHeldMs += 5;
            handler.postDelayed(this::tick, 5);
            return;
        }
        byte[] au = (keyedLoop ? c.keyed : c.steady).get(pos);
        boolean key = (keyedLoop || c.closed) && pos == 0;
        if (c != lastSent) {
            lastSent = c;
            Log.i(TAG, "clip: now sending " + c.name + " @" + c.kbps + " kb/s");
        }
        byte[] fr = new byte[VideoFrame.HEADER_LEN + au.length];
        fr[0] = (byte) VideoFrame.MAGIC;
        fr[1] = (byte) (key ? VideoFrame.KEYFRAME : 0);
        fr[2] = (byte) frameNo;
        fr[3] = (byte) (frameNo >> 8);
        fr[4] = (byte) au.length;
        fr[5] = (byte) (au.length >> 8);
        fr[6] = (byte) (au.length >> 16);
        fr[7] = (byte) (au.length >> 24);
        System.arraycopy(au, 0, fr, VideoFrame.HEADER_LEN, au.length);
        sink.onFrame(fr, key);
        frameNo++;
        statSent++;
        if (key) {
            statKeys++;
        }
        if (++pos == c.steady.size()) {
            pos = 0;
            statLoops++;
            loop++;
            keyedLoop = loop % REFRESH_LOOPS == 0;
        }
        nextNs += period;
        long wait = (nextNs - SystemClock.elapsedRealtimeNanos()) / 1_000_000L;
        if (wait < -CATCH_UP_MS) {
            nextNs = SystemClock.elapsedRealtimeNanos();
            wait = 0;
        }
        handler.postDelayed(this::tick, Math.max(0, wait));
    }
}
