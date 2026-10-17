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

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Log;
import android.util.Range;
import android.util.Size;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/**
 * Camera2 frames, cropped square, turned upright, scaled down to the stream's
 * size and blacked out past the round display's edge, into the phone's H.264
 * encoder (constrained baseline, CBR, low latency, no B-frames); each encoded
 * frame goes out with its frame header (VideoFrame), SPS and PPS before every
 * keyframe. Frames are paced to the target rate and skipped while the link is
 * behind, so the encoder's references never break.
 */
@SuppressLint("MissingPermission")
final class CameraStreamer {
    private static final String TAG = VideoLink.TAG;

    interface Sink {
        void onFrame(byte[] frame, boolean key);
        /* true: skip this camera frame (the link is behind). */
        boolean behind();
    }

    private final Context ctx;
    private final Sink sink;
    private final boolean front;
    private HandlerThread thread;
    private Handler handler;
    private CameraDevice camera;
    private CameraCaptureSession session;
    private ImageReader reader;
    private MediaCodec codec;
    private Surface preview;
    private int outW, outH, fps, kbps;
    private int sensorOrientation;
    private Range<Integer> aeRange;
    /* Lowest frame rate exposure may stretch to in dim light (30: fixed, the least latency). */
    volatile int minAeFps = 30;
    /* Sensor frame rate (--ei sensorfps 60 if the camera offers [60, 60]). */
    volatile int sensorFps = 30;
    private volatile boolean running;
    private byte[] config;          /* SPS + PPS */
    private int frameNo;
    private long nextFeedNs, ptsBase = -1;
    private boolean round;
    private int[] spanLo, spanHi;   /* the visible circle, per row */
    /* 4x4 luma blocks wholly outside the circle: the only pixels blacked out.
     * A mask edge on the encoder's 4x4 transform grid costs ~10% fewer bits
     * than the pixel-exact circle for the same picture inside it (x264
     * proxy; the hard edge through blocks rings into the circle) and decodes
     * as fast; see the round-display report. */
    private boolean[] blkOut;
    private int blkW;
    /* Intra refresh period in frames (0: 3 s). A full-picture refresh each
     * second spent ~20% of the bits on intra macroblocks (x264 proxy: 3 s
     * gives the same quality at ~22% fewer bits); a lost frame still gets a
     * keyframe at once when the gadget asks. */
    volatile int intraRefreshFrames;

    volatile int statFed, statSkipped, statEncoded, statKeys;
    /* Camera exposure (sensor timestamp) to encoded frame out, and in to out of the encoder. */
    volatile long statCamToEncUs, statEncUs, statConvUs;
    private final java.util.concurrent.ConcurrentHashMap<Long, long[]> inflight = new java.util.concurrent.ConcurrentHashMap<>();
    volatile long statEncBytes;
    volatile String encoderName = "";

    /* A test pattern instead of the camera: colour bars, a moving box. */
    volatile boolean pattern;
    /* A raw 640x480 I420 file instead of the camera (--es source file:<name>,
     * from files/): the same crop, filter and encoder, fed at the frame rate
     * and looped, so changes can be compared on the same high-motion clip. */
    volatile String srcFile;
    /* With srcFile: write files/bench/ref.yuv (every source frame as the
     * encoder would get it) and enc.bin (u32 length, u32 source frame, access
     * unit, for each frame encoded), for a host to score what's shown. */
    volatile boolean benchDump;
    volatile int benchLoops = 1;
    volatile LatencyMeter meter;
    /* --es venc "key=int,...": extra MediaFormat integers for the encoder (experiments). */
    volatile String encParams;
    /* Software encoder (libx264, X264.java) instead of MediaCodec: "x264"
     * with swPreset and swParams; null: the phone's hardware encoder. */
    volatile String swEncoder;
    volatile String swPreset = "fast", swParams;   /* fast: the same quality as medium here (one reference, no B), less CPU */
    private long sw;
    private volatile boolean swKey;
    private byte[] swIn;
    private final int[] swInfo = new int[2];
    /* Two bands (the gadget decodes each on its own core): the top half on
     * sw, the bottom on sw1, each its own x264 at half the bitrate; a frame
     * carries the top's length, its access unit, then the bottom's. */
    volatile int bands = 1;
    /* Two bands: the top band's share of the height, % (the gadget's
     * capability_tile_split: its cores aren't equally free), rounded to
     * whole macroblock rows; the bitrate splits the same way. */
    volatile int bandSplitPct = 50;
    private int band0Rows;
    private long sw1;
    private byte[] swIn1;
    private final int[] swInfo1 = new int[2];
    volatile long statQpSum, statQpN;
    /* Downscale for the panel (PanelScaler) instead of the 2x2 box; panelPx:
     * the gadget's display, which its GPU stretches the picture to. */
    volatile boolean panelFilter = true;
    volatile int panelPx = 360;
    private PanelScaler scY, scC;
    private int[] rotY, rotC;
    private byte[] smY, smU, smV;
    private int cropX, cropY, cropSide, srcCs, srcCp;
    volatile boolean intraRefresh = true;

    CameraStreamer(Context ctx, boolean front, Sink sink) {
        this.ctx = ctx;
        this.front = front;
        this.sink = sink;
    }

    void setPreview(Surface s) {
        preview = s;
    }

    synchronized void start(int w, int h, int fps, int kbps, boolean round) throws Exception {
        stop();
        this.outW = w;
        this.outH = h;
        this.fps = fps;
        this.kbps = kbps;
        this.round = round;
        buildMask();
        thread = new HandlerThread("cam");
        thread.start();
        handler = new Handler(thread.getLooper());
        startEncoder();
        running = true;
        if (srcFile != null) {
            openFile();
        } else if (pattern) {
            handler.post(this::patternTick);
        } else {
            openCamera();
        }
    }

    synchronized void stop() {
        running = false;
        if (session != null) {
            try {
                session.close();
            } catch (Exception e) {
                // closing
            }
            session = null;
        }
        if (camera != null) {
            camera.close();
            camera = null;
        }
        if (reader != null) {
            reader.close();
            reader = null;
        }
        if (sw != 0) {
            synchronized (this) {
                X264.close(sw);
                sw = 0;
                if (sw1 != 0) {
                    X264.close(sw1);
                    sw1 = 0;
                }
            }
        }
        if (codec != null) {
            try {
                codec.stop();
            } catch (Exception e) {
                // already stopped
            }
            codec.release();
            codec = null;
        }
        if (thread != null) {
            thread.quitSafely();
            thread = null;
        }
        benchClose();
        if (file != null) {
            try {
                file.close();
            } catch (java.io.IOException e) {
                // closing
            }
            file = null;
        }
        config = null;
        frameNo = 0;
        ptsBase = -1;
    }

    boolean running() {
        return running;
    }

    void requestKeyframe() {
        if (sw != 0) {
            swKey = true;
            return;
        }
        MediaCodec c = codec;
        if (c != null) {
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
            try {
                c.setParameters(b);
            } catch (Exception e) {
                Log.w(TAG, "keyframe request failed", e);
            }
        }
    }

    void setBitrate(int kbps) {
        this.kbps = kbps;
        if (sw != 0) {
            synchronized (this) {
                int k0 = sw1 != 0 ? kbps * band0Rows / Math.max(1, outH) : kbps;
                if (sw != 0) {
                    X264.setBitrate(sw, k0);
                }
                if (sw1 != 0) {
                    X264.setBitrate(sw1, kbps - k0);
                }
            }
            return;
        }
        MediaCodec c = codec;
        if (c != null) {
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, kbps * 1000);
            c.setParameters(b);
        }
    }

    void setFps(int fps) {
        this.fps = Math.max(1, fps);
    }

    int fps() {
        return fps;
    }

    int kbps() {
        return kbps;
    }

    /* ---- encoder -------------------------------------------------------- */

    /*
     * Past 240 px, decoder-friendly motion vectors (x264 mv-cheap=2, from
     * jni/x264_mvcheap.py): no positions with both components fractional,
     * the ones tinyh264 interpolates slowest. On the ESP-Mosaico at 480 px a
     * frame decodes in 37 ms instead of 46, for about 0.2 dB. An x264 built
     * without the script refuses the option and goes on without it. An
     * explicit mv-cheap in params wins.
     */
    static String withMvCheap(String params, int width) {
        if (width <= 240) {
            return params;
        }
        String p = params == null ? "" : params;
        if (!p.contains("mv-cheap")) {
            p += (p.isEmpty() ? "" : ":") + "mv-cheap=2";
        }
        /* Trellis quantisation and the wider motion search: +0.05-0.09 dB at
         * 480 px, the same decode work, ~same encode time on an S22. */
        if (!p.contains("trellis")) {
            p += ":trellis=2";
        }
        if (!p.contains("me=")) {
            p += ":me=umh";
        }
        return p;
    }

    private void startEncoder() throws Exception {
        if ("x264".equals(swEncoder)) {
            if (!X264.available()) {
                throw new IllegalStateException("libmgx264 is not in this build (jni/build.sh)");
            }
            boolean two = bands == 2 && outH % 32 == 0;
            int h0 = two ? Math.max(16, Math.min(outH - 16, (outH * bandSplitPct / 100 + 8) / 16 * 16)) : outH;
            band0Rows = h0;
            int k0 = two ? kbps * h0 / outH : kbps;
            String params = withMvCheap(swParams, outW);
            sw = X264.open(outW, h0, fps, k0, swPreset, params);
            sw1 = two ? X264.open(outW, outH - h0, fps, kbps - k0, swPreset, params) : 0;
            if (sw == 0 || (two && sw1 == 0)) {
                throw new IllegalStateException("x264 open failed");
            }
            swIn = new byte[outW * h0 * 3 / 2];
            swIn1 = two ? new byte[outW * (outH - h0) * 3 / 2] : null;
            swKey = true;
            encoderName = "x264 " + swPreset + (params != null ? " " + params : "")
                    + (two ? ", two bands of " + h0 + " and " + (outH - h0) + " rows" : "");
            Log.i(TAG, "encoder " + encoderName + " " + outW + "x" + outH + " " + kbps + " kb/s");
            return;
        }
        MediaFormat f = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, outW, outH);
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
        f.setInteger(MediaFormat.KEY_BIT_RATE, kbps * 1000);
        f.setInteger(MediaFormat.KEY_FRAME_RATE, 30);
        /* Intra refresh instead of periodic keyframes: a keyframe is 5-8 times a
         * P frame and takes 150-250 ms to cross the link, a visible hitch every
         * time. A stripe of intra macroblocks per frame refreshes the picture
         * every 3 s instead; keyframes come only when the gadget asks. */
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, intraRefresh || intraRefreshFrames < 0 ? 3600 : 4);
        if (intraRefresh && intraRefreshFrames >= 0) {
            f.setInteger(MediaFormat.KEY_INTRA_REFRESH_PERIOD,
                    intraRefreshFrames > 0 ? intraRefreshFrames : Math.max(10, 3 * fps));
        }
        f.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline);
        f.setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.AVCLevel31);
        f.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR);
        f.setInteger(MediaFormat.KEY_PRIORITY, 0);
        f.setInteger(MediaFormat.KEY_LATENCY, 1);
        f.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);
        f.setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1);
        codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
        encoderName = codec.getName();
        if (encParams != null) {
            for (String kv : encParams.split(",")) {
                String[] p = kv.split("=");
                if (p.length == 2) {
                    f.setInteger(p[0].trim(), Integer.parseInt(p[1].trim()));
                    Log.i(TAG, "encoder param " + p[0].trim() + " = " + p[1].trim());
                }
            }
        }
        codec.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        codec.start();
        if (encParams != null) {
            android.os.Bundle pb = new android.os.Bundle();
            for (String kv : encParams.split(",")) {
                String[] p = kv.split("=");
                if (p.length == 2) {
                    pb.putInt(p[0].trim(), Integer.parseInt(p[1].trim()));
                }
            }
            try {
                codec.setParameters(pb);
            } catch (Exception e) {
                Log.w(TAG, "encoder params", e);
            }
        }
        Log.i(TAG, "encoder " + encoderName + " " + outW + "x" + outH + " " + kbps + " kb/s");
        Thread out = new Thread(this::drain, "enc-out");
        out.setPriority(Thread.MAX_PRIORITY);
        out.start();
    }

    private void drain() {
        MediaCodec c = codec;
        MediaCodec.BufferInfo bi = new MediaCodec.BufferInfo();
        while (codec == c && c != null) {
            int idx;
            try {
                idx = c.dequeueOutputBuffer(bi, 20000);
            } catch (Exception e) {
                return;
            }
            if (idx < 0) {
                continue;
            }
            ByteBuffer b = c.getOutputBuffer(idx);
            byte[] au = new byte[bi.size];
            b.position(bi.offset);
            b.get(au);
            c.releaseOutputBuffer(idx, false);
            if ((bi.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                config = au;
                continue;
            }
            boolean key = (bi.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
            long[] ts = inflight.remove(bi.presentationTimeUs);
            if (key && config != null && !startsWithSps(au)) {
                byte[] t = new byte[config.length + au.length];
                System.arraycopy(config, 0, t, 0, config.length);
                System.arraycopy(au, 0, t, config.length, au.length);
                au = t;
            }
            emit(au, key, ts);
        }
    }

    /* An access unit out of either encoder: framed and handed to the sink. */
    private void emit(byte[] au, boolean key, long[] ts) {
        {
            long now = SystemClock.elapsedRealtimeNanos();
            if (ts != null) {
                if (ts[0] > 0) {
                    statCamToEncUs += (now - ts[0]) / 1000;
                }
                statEncUs += (now - ts[1]) / 1000;
            }
            LatencyMeter m = meter;
            if (m != null && ts != null) {
                m.captured(frameNo, ts[0] > 0 ? ts[0] : ts[1], now);
            }
            if (ts != null && ts.length > 2 && ts[2] >= 0) {
                benchWriteEnc((int) ts[2], au, key);
            }
            byte[] fr = new byte[VideoFrame.HEADER_LEN + au.length];
            fr[0] = (byte) VideoFrame.MAGIC;
            fr[1] = (byte) (key ? VideoFrame.KEYFRAME : 0);
            fr[2] = (byte) frameNo;
            fr[3] = (byte) (frameNo >> 8);
            int n = au.length;
            fr[4] = (byte) n;
            fr[5] = (byte) (n >> 8);
            fr[6] = (byte) (n >> 16);
            fr[7] = (byte) (n >> 24);
            System.arraycopy(au, 0, fr, VideoFrame.HEADER_LEN, n);
            frameNo++;
            statEncoded++;
            statEncBytes += n;
            if (key) {
                statKeys++;
            }
            sink.onFrame(fr, key);
        }
    }

    private static boolean startsWithSps(byte[] au) {
        int i = 0;
        while (i < au.length - 4 && au[i] == 0) {
            i++;
        }
        return i < au.length - 1 && au[i] == 1 && (au[i + 1] & 0x1f) == 7;
    }

    /* ---- camera --------------------------------------------------------- */

    private void openCamera() throws Exception {
        CameraManager cm = ctx.getSystemService(CameraManager.class);
        String id = null;
        for (String c : cm.getCameraIdList()) {
            Integer facing = cm.getCameraCharacteristics(c).get(CameraCharacteristics.LENS_FACING);
            if (facing != null && facing == (front ? CameraCharacteristics.LENS_FACING_FRONT
                                                   : CameraCharacteristics.LENS_FACING_BACK)) {
                id = c;
                break;
            }
        }
        if (id == null) {
            throw new IllegalStateException("no camera");
        }
        CameraCharacteristics ch = cm.getCameraCharacteristics(id);
        /* A fixed 30 fps caps exposure at 33 ms; --ei aefps 20 lets it
         * stretch in dim light (brighter, a frame more latency). */
        aeRange = new Range<>(30, 30);
        StringBuilder ranges = new StringBuilder();
        for (Range<Integer> r : ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)) {
            ranges.append(r).append(' ');
            if (sensorFps > 30 && r.getLower() == sensorFps && r.getUpper() == sensorFps) {
                aeRange = r;   /* a faster sensor: a shorter pipeline, frames in between are skipped */
            }
        }
        Log.i(TAG, "AE ranges: " + ranges);
        for (Range<Integer> r : ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)) {
            if (aeRange.getLower() > 30) {
                break;
            }
            if (minAeFps < 30 && r.getUpper() == 30 && r.getLower() >= minAeFps && r.getLower() < aeRange.getLower()) {
                aeRange = r;
            }
        }
        sensorOrientation = ch.get(CameraCharacteristics.SENSOR_ORIENTATION);
        StreamConfigurationMap map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        Size best = null;
        for (Size s : map.getOutputSizes(ImageFormat.YUV_420_888)) {
            int shortSide = Math.min(s.getWidth(), s.getHeight());
            if (shortSide < Math.max(outW, outH) * 2) {
                continue;   /* at least 2x for a clean box filter */
            }
            if (best == null || s.getWidth() * s.getHeight() < best.getWidth() * best.getHeight()) {
                best = s;
            }
        }
        if (best == null) {
            best = map.getOutputSizes(ImageFormat.YUV_420_888)[0];
        }
        Log.i(TAG, "camera " + id + " " + best + " orientation " + sensorOrientation);
        reader = ImageReader.newInstance(best.getWidth(), best.getHeight(), ImageFormat.YUV_420_888, 3);
        reader.setOnImageAvailableListener(r -> {
            Image img = r.acquireLatestImage();
            if (img == null) {
                return;
            }
            try {
                feed(img);
            } catch (Exception e) {
                Log.w(TAG, "feed failed", e);
            } finally {
                img.close();
            }
        }, handler);
        final ImageReader mine = reader;
        cm.openCamera(id, new CameraDevice.StateCallback() {
            @Override
            public void onOpened(CameraDevice d) {
                synchronized (CameraStreamer.this) {
                    if (reader != mine || !running) {
                        d.close();   /* stopped (or restarted) while the camera was opening */
                        return;
                    }
                    camera = d;
                }
                try {
                    List<Surface> targets = new ArrayList<>();
                    targets.add(reader.getSurface());
                    if (preview != null) {
                        targets.add(preview);
                    }
                    d.createCaptureSession(targets, new CameraCaptureSession.StateCallback() {
                        @Override
                        public void onConfigured(CameraCaptureSession s) {
                            session = s;
                            try {
                                /* Preview, not record: no stabilization or temporal
                                 * noise reduction holding frames back (each is a
                                 * frame or two of latency). */
                                CaptureRequest.Builder b = d.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                                b.set(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_FAST);
                                b.set(CaptureRequest.EDGE_MODE, CaptureRequest.EDGE_MODE_FAST);
                                for (Surface t : targets) {
                                    b.addTarget(t);
                                }
                                b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, aeRange);
                                b.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                                        CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF);
                                s.setRepeatingRequest(b.build(), null, handler);
                            } catch (Exception e) {
                                Log.e(TAG, "capture request failed", e);
                            }
                        }

                        @Override
                        public void onConfigureFailed(CameraCaptureSession s) {
                            Log.e(TAG, "camera session failed");
                        }
                    }, handler);
                } catch (Exception e) {
                    Log.e(TAG, "camera session", e);
                }
            }

            @Override
            public void onDisconnected(CameraDevice d) {
                d.close();
            }

            @Override
            public void onError(CameraDevice d, int error) {
                Log.e(TAG, "camera error " + error);
                d.close();
            }
        }, handler);
    }

    private int patternFrame;

    private void patternTick() {
        if (!running || (codec == null && sw == 0)) {
            return;
        }
        handler.postDelayed(this::patternTick, 1000 / Math.max(1, fps));
        if (sink.behind()) {
            statSkipped++;
            return;
        }
        int idx = -1;
        if (sw == 0) {
            idx = codec.dequeueInputBuffer(5000);
            if (idx < 0) {
                statSkipped++;
                return;
            }
        }
        if (outY == null || outY.length != outW * outH) {
            outY = new byte[outW * outH];
            outU = new byte[outW * outH / 4];
            outV = new byte[outW * outH / 4];
        }
        /* BT.601 limited: white, yellow, cyan, green, magenta, red, blue, black. */
        int[][] bars = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
                        {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};
        int t = patternFrame++;
        int bx = (int) ((outW - 48) * (0.5 + 0.5 * Math.sin(t * 2 * Math.PI / 90)));
        int by = (int) ((outH - 48) * (0.5 + 0.5 * Math.cos(t * 2 * Math.PI / 120)));
        for (int y = 0; y < outH; y++) {
            for (int x = 0; x < outW; x++) {
                int[] c = bars[x * 8 / outW];
                int v = c[0];
                if (y < outH / 6) {
                    v = 16 + 219 * x / outW;   /* a grey ramp on top */
                }
                if (x >= bx && x < bx + 48 && y >= by && y < by + 48) {
                    v = 235;
                }
                if (blkOut[(y >> 2) * blkW + (x >> 2)]) {
                    v = 16;
                }
                outY[y * outW + x] = (byte) v;
            }
        }
        int cw = outW / 2;
        for (int y = 0; y < outH / 2; y++) {
            for (int x = 0; x < cw; x++) {
                int[] c = bars[(2 * x) * 8 / outW];
                int u = c[1], v = c[2];
                int X = 2 * x, Y = 2 * y;
                if (Y < outH / 6 || (X >= bx && X < bx + 48 && Y >= by && Y < by + 48)
                        || blkOut[(Y >> 2) * blkW + (X >> 2)]) {
                    u = v = 128;
                }
                outU[y * cw + x] = (byte) u;
                outV[y * cw + x] = (byte) v;
            }
        }
        long now = SystemClock.elapsedRealtimeNanos();
        if (sw != 0) {
            swEncode(now, now, -1);
            return;
        }
        Image in = codec.getInputImage(idx);
        Image.Plane[] dp = in.getPlanes();
        putPlane(dp[0], outY, outW, outH);
        putPlane(dp[1], outU, cw, outH / 2);
        putPlane(dp[2], outV, cw, outH / 2);
        long pts = (long) statFed * 1_000_000L / fps;
        inflight.put(pts, new long[] {now, now, -1});
        codec.queueInputBuffer(idx, 0, outW * outH * 3 / 2, pts, 0);
        statFed++;
    }

    static final double MASK_MARGIN = 3;

    private void buildMask() {
        spanLo = new int[outH];
        spanHi = new int[outH];
        double r = Math.min(outW, outH) / 2.0, cx = outW / 2.0, cy = outH / 2.0;
        for (int y = 0; y < outH; y++) {
            if (!round) {
                spanLo[y] = 0;
                spanHi[y] = outW;
                continue;
            }
            double dy = y + 0.5 - cy;
            double half = r * r - dy * dy;
            if (half <= 0) {
                spanLo[y] = spanHi[y] = (int) cx;
                continue;
            }
            /* Past the circle by MASK_MARGIN: a gadget that upscales the
             * picture with a bilinear filter (a GPU stretching it 1.5x, say) reads
             * a pixel beyond the edge, and a black block there showed as
             * dark specks along the panel's rim. */
            double hw = Math.sqrt(half) + MASK_MARGIN;
            spanLo[y] = Math.max(0, (int) Math.floor(cx - hw));
            spanHi[y] = Math.min(outW, (int) Math.ceil(cx + hw));
        }
        blkW = (outW + 3) / 4;
        int blkH = (outH + 3) / 4;
        blkOut = new boolean[blkW * blkH];
        for (int by = 0; by < blkH; by++) {
            for (int bx = 0; bx < blkW; bx++) {
                boolean out = true;
                for (int y = by * 4; y < Math.min(outH, by * 4 + 4); y++) {
                    if (spanHi[y] > bx * 4 && spanLo[y] < Math.min(outW, bx * 4 + 4)) {
                        out = false;
                    }
                }
                blkOut[by * blkW + bx] = out;
            }
        }
    }

    private void feed(Image img) {
        if (!running || (codec == null && sw == 0)) {
            return;
        }
        long now = SystemClock.elapsedRealtimeNanos();
        if (now < nextFeedNs) {
            return;   /* above the target rate */
        }
        long period = 1_000_000_000L / fps;
        nextFeedNs = Math.max(nextFeedNs + period, now - period / 2);
        if (sink.behind()) {
            statSkipped++;
            return;
        }
        if (sw != 0) {
            long c0 = SystemClock.elapsedRealtimeNanos();
            convert(img, null);
            long c1 = SystemClock.elapsedRealtimeNanos();
            statConvUs += (c1 - c0) / 1000;
            long sensor = img.getTimestamp();
            swEncode(Math.abs(c1 - sensor) < 2_000_000_000L ? sensor : 0, c1, -1);
            return;
        }
        int idx = codec.dequeueInputBuffer(5000);
        if (idx < 0) {
            statSkipped++;
            return;
        }
        Image in = codec.getInputImage(idx);
        long c0 = SystemClock.elapsedRealtimeNanos();
        convert(img, in);
        long c1 = SystemClock.elapsedRealtimeNanos();
        statConvUs += (c1 - c0) / 1000;
        /* Timestamps at the nominal rate, whatever was skipped: the encoder's
         * rate control spends bitrate / fps on each frame, so a frame stays
         * small when the link makes us skip (with real timestamps a skip
         * makes the next frame bigger, which makes the link slower...). */
        long pts = (long) statFed * 1_000_000L / fps;
        /* Camera2 YUV timestamps are elapsedRealtimeNanos on this phone (timestamp source REALTIME). */
        long sensor = img.getTimestamp();
        inflight.put(pts, new long[] {Math.abs(c1 - sensor) < 2_000_000_000L ? sensor : 0, c1, -1});
        codec.queueInputBuffer(idx, 0, outW * outH * 3 / 2, pts, 0);
        statFed++;
    }

    /* Precomputed per geometry: for each output pixel, the source luma index of
     * its 2x2 box (-1 outside the circle), and for each chroma sample the U and
     * V indices. Built on the first frame (strides are only known then). */
    private int[] lumaIdx, uIdx, vIdx;
    private int mapKey;
    private byte[] srcY, srcU, srcV, outY, outU, outV;

    private void buildMaps(int sw, int sh, int syr, int sur, int sup, int svr, int svp) {
        int side = Math.min(sw, sh);
        int x0 = (sw - side) / 2, y0 = (sh - side) / 2;
        double step = (double) side / outW;
        int rot = srcFile != null ? 0 : sensorOrientation % 360;
        lumaIdx = new int[outW * outH];
        for (int y = 0; y < outH; y++) {
            for (int x = 0; x < outW; x++) {
                if (blkOut[(y >> 2) * blkW + (x >> 2)]) {
                    lumaIdx[y * outW + x] = -1;
                    continue;
                }
                int[] m = map(x, y, rot, step, side);
                lumaIdx[y * outW + x] = (y0 + m[1]) * syr + x0 + m[0];
            }
        }
        int cw = outW / 2, chh = outH / 2;
        if (panelFilter && svr == sur && svp == sup) {
            buildPanel(x0, y0, side, rot, sur, sup);
        } else {
            scY = null;
        }
        uIdx = new int[cw * chh];
        vIdx = new int[cw * chh];
        for (int y = 0; y < chh; y++) {
            int Y = 2 * y;
            for (int x = 0; x < cw; x++) {
                int X = 2 * x;
                if (blkOut[(Y >> 2) * blkW + (X >> 2)]) {
                    uIdx[y * cw + x] = -1;
                    continue;
                }
                int[] m = map(X, Y, rot, step, side);
                int cx = (x0 + m[0]) / 2, cy = (y0 + m[1]) / 2;
                uIdx[y * cw + x] = cy * sur + cx * sup;
                vIdx[y * cw + x] = cy * svr + cx * svp;
            }
        }
        outY = new byte[outW * outH];
        outU = new byte[cw * chh];
        outV = new byte[cw * chh];
    }

    private static byte[] grab(ByteBuffer b, byte[] a) {
        b.rewind();
        int n = b.remaining();
        if (a == null || a.length < n) {
            a = new byte[n];
        }
        b.get(a, 0, n);
        return a;
    }

    private static void putPlane(Image.Plane p, byte[] src, int w, int h) {
        ByteBuffer b = p.getBuffer();
        int rs = p.getRowStride(), ps = p.getPixelStride();
        if (ps == 1) {
            for (int y = 0; y < h; y++) {
                b.position(y * rs);
                b.put(src, y * w, w);
            }
        } else {
            for (int y = 0; y < h; y++) {
                int o = y * rs;
                for (int x = 0; x < w; x++) {
                    b.put(o + x * ps, src[y * w + x]);
                }
            }
        }
    }

    /**
     * Center square of the camera image, turned upright for a phone held in
     * portrait, box-filtered down to outW x outH, black in the 4x4 blocks
     * wholly outside the circle.
     * Whole planes are copied in and out in bulk; the per-pixel work is
     * table lookups (about 2 ms at 232 px).
     */
    private void convert(Image src, Image dst) {
        Image.Plane[] sp = src.getPlanes();
        int syr = sp[0].getRowStride();
        int key = (src.getWidth() * 31 + src.getHeight()) * 31 * 31 + syr * 31 + sp[1].getRowStride()
                + sp[1].getPixelStride() * 7 + outW;
        if (lumaIdx == null || key != mapKey) {
            buildMaps(src.getWidth(), src.getHeight(), syr, sp[1].getRowStride(), sp[1].getPixelStride(),
                    sp[2].getRowStride(), sp[2].getPixelStride());
            mapKey = key;
        }
        srcY = grab(sp[0].getBuffer(), srcY);
        srcU = grab(sp[1].getBuffer(), srcU);
        srcV = grab(sp[2].getBuffer(), srcV);
        filterPlanes(syr);
        if (dst == null) {
            return;
        }
        Image.Plane[] dp = dst.getPlanes();
        putPlane(dp[0], outY, outW, outH);
        putPlane(dp[1], outU, outW / 2, outH / 2);
        putPlane(dp[2], outV, outW / 2, outH / 2);
    }

    /* PanelScaler for luma and chroma, and where each output pixel's value
     * lands once the source-oriented result is turned upright (-1: masked). */
    private void buildPanel(int x0, int y0, int side, int rot, int sur, int sup) {
        long t0 = SystemClock.elapsedRealtime();
        scY = new PanelScaler(side, outW, panelPx, 0.2, 3);
        scC = new PanelScaler(side / 2, outW / 2, panelPx / 2, 0.2, 3);
        cropX = x0;
        cropY = y0;
        cropSide = side;
        srcCs = sur;
        srcCp = sup;
        smY = new byte[outW * outW];
        smU = new byte[outW * outW / 4];
        smV = new byte[outW * outW / 4];
        rotY = gridMap(outW, rot, 1);
        rotC = gridMap(outW / 2, rot, 2);
        Log.i(TAG, "panel filter " + side + " -> " + outW + " (panel " + panelPx + "): "
                + (SystemClock.elapsedRealtime() - t0) + " ms to build");
    }

    private int[] gridMap(int n, int rot, int sub) {
        int[] m = new int[n * n];
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                int X = x * sub, Y = y * sub;
                if (blkOut[(Y >> 2) * blkW + (X >> 2)]) {
                    m[y * n + x] = -1;
                    continue;
                }
                int gx = front ? n - 1 - x : x, gy = y, sx, sy;
                switch (rot) {
                    case 90: sx = gy; sy = n - 1 - gx; break;
                    case 180: sx = n - 1 - gx; sy = n - 1 - gy; break;
                    case 270: sx = n - 1 - gy; sy = gx; break;
                    default: sx = gx; sy = gy; break;
                }
                m[y * n + x] = sy * n + sx;
            }
        }
        return m;
    }

    /* srcY/U/V (with the maps built for their geometry) -> outY/U/V. */
    private void filterPlanes(int syr) {
        if (scY != null) {
            scY.scale(srcY, syr, 1, cropX, cropY, smY);
            scC.scale(srcU, srcCs, srcCp, cropX / 2, cropY / 2, smU);
            scC.scale(srcV, srcCs, srcCp, cropX / 2, cropY / 2, smV);
            for (int i = 0, n = rotY.length; i < n; i++) {
                int s = rotY[i];
                outY[i] = s < 0 ? 16 : smY[s];
            }
            for (int i = 0, n = rotC.length; i < n; i++) {
                int s = rotC[i];
                if (s < 0) {
                    outU[i] = outV[i] = (byte) 128;
                } else {
                    outU[i] = smU[s];
                    outV[i] = smV[s];
                }
            }
            return;
        }
        int lastY = srcY.length - syr - 2;
        for (int i = 0, n = outW * outH; i < n; i++) {
            int s = lumaIdx[i];
            if (s < 0) {
                outY[i] = 16;
            } else {
                if (s > lastY) {
                    s = lastY;
                }
                outY[i] = (byte) (((srcY[s] & 0xff) + (srcY[s + 1] & 0xff) + (srcY[s + syr] & 0xff)
                        + (srcY[s + syr + 1] & 0xff) + 2) >> 2);
            }
        }
        int lastU = srcU.length - 1, lastV = srcV.length - 1;
        for (int i = 0, n = uIdx.length; i < n; i++) {
            int u = uIdx[i];
            if (u < 0) {
                outU[i] = outV[i] = (byte) 128;
            } else {
                outU[i] = srcU[Math.min(u, lastU)];
                outV[i] = srcV[Math.min(vIdx[i], lastV)];
            }
        }
    }

    /* ---- test source: a raw 640x480 I420 file ---------------------------- */

    private static final int FW = 640, FH = 480;
    private java.io.RandomAccessFile file;
    private int fileFrames, fileIdx, srcIdx;
    private long fileNextMs;
    private java.io.OutputStream refDump, encDump;

    private void openFile() throws java.io.IOException {
        java.io.File f = new java.io.File(srcFile);
        file = new java.io.RandomAccessFile(f, "r");
        fileFrames = (int) (f.length() / (FW * FH * 3 / 2));
        fileIdx = srcIdx = 0;
        srcY = new byte[FW * FH];
        srcU = new byte[FW * FH / 4];
        srcV = new byte[FW * FH / 4];
        buildMaps(FW, FH, FW, FW / 2, 1, FW / 2, 1);
        if (benchDump) {
            java.io.File d = new java.io.File(ctx.getExternalFilesDir(null), "bench");
            d.mkdirs();
            refDump = new java.io.BufferedOutputStream(new java.io.FileOutputStream(new java.io.File(d, "ref.yuv")), 1 << 20);
            encDump = new java.io.BufferedOutputStream(new java.io.FileOutputStream(new java.io.File(d, "enc.bin")), 1 << 16);
        }
        Log.i(TAG, "file source " + f + ": " + fileFrames + " frames at " + fps + " fps, " + benchLoops + " loop(s)");
        fileNextMs = SystemClock.uptimeMillis();
        handler.post(this::fileTick);
    }

    private void fileTick() {
        if (!running || (codec == null && sw == 0) || file == null) {
            return;
        }
        if (srcIdx >= fileFrames * benchLoops) {
            benchClose();
            Log.i(TAG, "file source done: " + srcIdx + " frames");
            return;
        }
        /* The clip's own rate (30 fps: ticks of 33, 33, 34 ms); the encoder
         * takes frames at fps, as from the camera. */
        fileNextMs += srcIdx % 3 == 2 ? 34 : 33;
        handler.postAtTime(this::fileTick, fileNextMs);
        long t0 = SystemClock.elapsedRealtimeNanos();
        try {
            file.seek((long) fileIdx * FW * FH * 3 / 2);
            file.readFully(srcY);
            file.readFully(srcU);
            file.readFully(srcV);
        } catch (java.io.IOException e) {
            Log.w(TAG, "file source read", e);
            return;
        }
        int idx = srcIdx++;
        fileIdx = (fileIdx + 1) % fileFrames;
        long nowNs = SystemClock.elapsedRealtimeNanos();
        if (fps < 30 && nowNs < nextFeedNs) {
            /* above the target rate: not a skip, and not shown */
            if (refDump != null) {
                filterPlanes(FW);
                try {
                    refDump.write(outY);
                    refDump.write(outU);
                    refDump.write(outV);
                } catch (java.io.IOException e) {
                    Log.w(TAG, "bench dump", e);
                }
            }
            return;
        }
        if (fps < 30) {
            long period = 1_000_000_000L / fps;
            nextFeedNs = Math.max(nextFeedNs + period, nowNs - period / 2);
        }
        boolean skip = sink.behind();
        int in = -1;
        if (!skip && sw == 0) {
            in = codec.dequeueInputBuffer(5000);
            skip = in < 0;
        }
        if (skip && refDump == null) {
            statSkipped++;
            return;
        }
        long c0 = SystemClock.elapsedRealtimeNanos();
        filterPlanes(FW);
        long c1 = SystemClock.elapsedRealtimeNanos();
        if (refDump != null) {
            try {
                refDump.write(outY);
                refDump.write(outU);
                refDump.write(outV);
            } catch (java.io.IOException e) {
                Log.w(TAG, "bench dump", e);
            }
        }
        if (skip) {
            statSkipped++;
            return;
        }
        statConvUs += (c1 - c0) / 1000;
        if (sw != 0) {
            swEncode(t0, c1, idx);
            return;
        }
        Image img = codec.getInputImage(in);
        Image.Plane[] dp = img.getPlanes();
        putPlane(dp[0], outY, outW, outH);
        putPlane(dp[1], outU, outW / 2, outH / 2);
        putPlane(dp[2], outV, outW / 2, outH / 2);
        long pts = (long) statFed * 1_000_000L / fps;
        inflight.put(pts, new long[] {t0, c1, idx});
        codec.queueInputBuffer(in, 0, outW * outH * 3 / 2, pts, 0);
        statFed++;
    }

    /* outY/U/V -> x264 -> emit, on the camera thread (a few ms at 232 px). */
    private void swEncode(long captureNs, long convertedNs, int srcIdx) {
        byte[] au;
        synchronized (this) {
            if (sw == 0) {
                return;
            }
            boolean idr = swKey;
            swKey = false;
            if (sw1 != 0) {
                au = encodeBands(idr);
                if (au == null) {
                    swKey = true;   /* a band came out empty: start both over */
                    return;
                }
            } else {
                int ys = outW * outH, cs = ys / 4;
                System.arraycopy(outY, 0, swIn, 0, ys);
                System.arraycopy(outU, 0, swIn, ys, cs);
                System.arraycopy(outV, 0, swIn, ys + cs, cs);
                au = X264.encode(sw, swIn, statFed, idr, swInfo);
            }
        }
        statFed++;
        if (au == null || au.length == 0) {
            return;
        }
        statQpSum += swInfo[1];
        statQpN++;
        emit(au, swInfo[0] != 0, new long[] {captureNs, convertedNs, srcIdx});
    }

    /* Both bands of outY/U/V, encoded with the same keyframe decision. */
    private byte[] encodeBands(boolean idr) {
        int h0 = band0Rows;
        for (int b = 0; b < 2; b++) {
            byte[] in = b == 0 ? swIn : swIn1;
            int r0 = b == 0 ? 0 : h0, rows = b == 0 ? h0 : outH - h0;
            int ys = outW * rows, cs = ys / 4;
            System.arraycopy(outY, outW * r0, in, 0, ys);
            System.arraycopy(outU, outW * r0 / 4, in, ys, cs);
            System.arraycopy(outV, outW * r0 / 4, in, ys + cs, cs);
        }
        byte[] a0 = X264.encode(sw, swIn, statFed, idr, swInfo);
        byte[] a1 = X264.encode(sw1, swIn1, statFed, idr, swInfo1);
        if (a0 == null || a1 == null || a0.length == 0 || a1.length == 0) {
            return null;
        }
        byte[] au = new byte[4 + a0.length + a1.length];
        int n = a0.length;
        au[0] = (byte) n;
        au[1] = (byte) (n >> 8);
        au[2] = (byte) (n >> 16);
        au[3] = (byte) (n >> 24);
        System.arraycopy(a0, 0, au, 4, n);
        System.arraycopy(a1, 0, au, 4 + n, a1.length);
        swInfo[0] = swInfo[0] != 0 && swInfo1[0] != 0 ? 1 : 0;
        swInfo[1] = (swInfo[1] + swInfo1[1]) / 2;
        return au;
    }

    private synchronized void benchWriteEnc(int idx, byte[] au, boolean key) {
        if (encDump == null) {
            return;
        }
        try {
            int n = au.length;
            encDump.write(new byte[] {(byte) n, (byte) (n >> 8), (byte) (n >> 16), (byte) (n >> 24),
                    (byte) idx, (byte) (idx >> 8), (byte) (idx >> 16), (byte) (idx >> 24)});
            encDump.write(au);
        } catch (java.io.IOException e) {
            Log.w(TAG, "bench dump", e);
        }
    }

    private synchronized void benchClose() {
        try {
            if (refDump != null) refDump.close();
            if (encDump != null) encDump.close();
        } catch (java.io.IOException e) {
            Log.w(TAG, "bench dump close", e);
        }
        refDump = encDump = null;
    }

    private final int[] mapOut = new int[2];

    /* Output pixel (x, y) of the upright square -> offset in the camera's crop. */
    private int[] map(int x, int y, int rot, double step, int side) {
        double fx = (x + 0.5) * step - 0.5, fy = (y + 0.5) * step - 0.5;
        if (front) {
            fx = side - 1 - fx;   /* a mirror, as people expect of a selfie camera */
        }
        int sx, sy;
        switch (rot) {
            case 90: sx = (int) fy; sy = (int) (side - 1 - fx); break;
            case 180: sx = (int) (side - 1 - fx); sy = (int) (side - 1 - fy); break;
            case 270: sx = (int) (side - 1 - fy); sy = (int) fx; break;
            default: sx = (int) fx; sy = (int) fy; break;
        }
        mapOut[0] = Math.max(0, Math.min(side - 2, sx));
        mapOut[1] = Math.max(0, Math.min(side - 2, sy));
        return mapOut;
    }
}
