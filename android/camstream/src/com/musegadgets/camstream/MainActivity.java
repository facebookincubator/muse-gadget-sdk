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

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.SurfaceTexture;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.Surface;
import android.view.TextureView;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * Muse Gadgets Camera: finds the gadget on the local network, and streams the camera
 * (or an open sample clip) to it when the gadget's menu asks (its "want" message)
 * or, with auto on (a bench option), as soon as it's connected. Stats on
 * screen and in logcat (tag MuseCam), the gadget's decode stats included.
 *
 * Intent extras (adb shell am start -n com.musegadgets.camstream/.MainActivity
 * --es target A1B2C3 --ei fps 24 --ei kbps 1000 --ez auto true --ez front false;
 * --es host 192.168.1.20 skips mDNS).
 */
public final class MainActivity extends Activity
        implements VideoLink.Listener, CameraStreamer.Sink, ClipStreamer.Sink {
    private static final String TAG = VideoLink.TAG;

    private TextView status;
    private TextureView preview;
    private Surface previewSurface;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private VideoLink link;
    private CameraStreamer cam;
    /* What to connect to: --es target <name part> (first match), --es host
     * <address[:port]>, else the gadget last picked in the Device dialog. */
    private String target, deviceAddr;
    private static final String PREFS = "camstream", PREF_ADDR = "wifi_addr", PREF_NAME = "wifi_name";
    private int fps = 30, kbps = 450, size = 0;
    private boolean noBands;   /* --ez nobands true: one stream even where the gadget takes two bands */
    /* The Codec button (or --es codec h264|vp8): VP8 for the camera and the
     * clips where the gadget lists it and this build has libmgvpx
     * (jni/build_vpx.sh), else H.264. Remembered. */
    private boolean wantVp8, wantVp8Set;
    private android.widget.Button codecButton;
    private String baseSw;      /* the H.264 encoder: x264, or null for the phone's */
    private boolean camVp8;     /* the camera stream that's on is VP8 */
    private static final String PREF_CODEC = "codec";
    private boolean auto, front, pattern, intraRefresh = true, adaptive = true;
    /* What streams: "camera", "pattern" or "clip:<name>" (--es source, or the buttons). */
    private String source = "camera";
    /* Sample clips (SampleClips): fetched, decoded and encoded for the gadget when first played. */
    private static final String CLIP = "clip:";
    private ClipStreamer player;
    private final java.util.Map<String, ClipStreamer.Clip> clips = new java.util.concurrent.ConcurrentHashMap<>();
    /* The encoder thread (clipLoop): the wanted clip first, then any others to have ready. */
    private final Object clipLock = new Object();
    private Thread clipThread;
    private volatile int clipSize = 232;
    private volatile boolean clipRound = true;
    private final java.util.Set<String> failedClips = java.util.concurrent.ConcurrentHashMap.newKeySet();
    private int clipTopKbps = 450;
    private boolean clipKbpsSet;
    /* What the clips are encoded at now: clipTopKbps, or less while the link can't carry it. */
    private volatile int clipKbps = 450;
    private long lastHeldMs;
    private int heldSeconds, clipCalmSeconds;
    private int lastSize = 232;
    private int aeFps = 30, sensorFps = 30;
    private volatile boolean wanted;
    private String linkStatus = "", deviceLine = "";
    private volatile long devAvail, devRecv;
    private volatile int devShown, devDropped, devMs;
    private LatencyMeter meter;
    private boolean benchDump, latLog, live = true;
    private int aheadPct = 250;   /* --ei ahead: live budget, % of a frame */
    private int behindPct = 100;  /* --ei behind: live phone-side queue before skipping, % of a frame */

    /* Live camera: keep about one and a half frames ahead of the device's decoder. */
    private void setLiveBudget() {
        long frame = kbps * 1000L / 8 / Math.max(1, curFps);
        link.liveAhead = live && !isClip() ? Math.max(1600, frame * aheadPct / 100) : 0;
    }
    private int benchLoops = 1;
    private TextView sending;
    private long lastStatMs, lastBytes, lastCamEnc, lastEncUs, lastConv, lastGate;
    private int lastFed, lastEnc, lastShown;
    private volatile int curFps;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        readExtras(getIntent());
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        preview = new TextureView(this);
        root.addView(preview, new FrameLayout.LayoutParams(1080, 1080, Gravity.CENTER));
        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setTextSize(13);
        status.setTypeface(android.graphics.Typeface.MONOSPACE);
        status.setPadding(24, 120, 24, 24);
        root.addView(status, new FrameLayout.LayoutParams(-1, -2, Gravity.TOP));
        android.widget.LinearLayout bars = new android.widget.LinearLayout(this);
        bars.setOrientation(android.widget.LinearLayout.VERTICAL);
        java.util.List<String[]> row1 = new java.util.ArrayList<>();
        row1.add(new String[] {"Camera", "camera"});
        row1.add(new String[] {"Bars", "pattern"});
        for (SampleClips.Sample c : SampleClips.ALL) {
            row1.add(new String[] {c.label, CLIP + c.name});
        }
        for (String[][] specs : new String[][][] {row1.toArray(new String[0][]),
                {{"Device", "device"}, {"Rate", "rate"}, {"H.264", "codec"}, {"Stop", "stop"}}}) {
            android.widget.LinearLayout bar = new android.widget.LinearLayout(this);
            bar.setOrientation(android.widget.LinearLayout.HORIZONTAL);
            for (String[] spec : specs) {
                android.widget.Button btn = new android.widget.Button(this);
                btn.setText(spec[0]);
                btn.setAllCaps(false);
                btn.setOnClickListener(v -> pick(spec[1]));
                if ("rate".equals(spec[1])) {
                    rateButton = btn;
                }
                if ("codec".equals(spec[1])) {
                    codecButton = btn;
                }
                bar.addView(btn, new android.widget.LinearLayout.LayoutParams(0, -2, 1));
            }
            bars.addView(bar, new android.widget.LinearLayout.LayoutParams(-1, -2));
        }
        /* What goes to the gadget now, in big type above the buttons. */
        sending = new TextView(this);
        sending.setTextColor(0xFFFFD54F);
        sending.setTextSize(18);
        sending.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        sending.setGravity(Gravity.CENTER);
        sending.setShadowLayer(6, 0, 0, Color.BLACK);
        sending.setPadding(16, 8, 16, 16);
        sending.setText("Not sending");
        android.widget.LinearLayout bottom = new android.widget.LinearLayout(this);
        bottom.setOrientation(android.widget.LinearLayout.VERTICAL);
        bottom.addView(sending, new android.widget.LinearLayout.LayoutParams(-1, -2));
        bottom.addView(bars, new android.widget.LinearLayout.LayoutParams(-1, -2));
        root.addView(bottom, new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM));
        setContentView(root);
        preview.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            @Override
            public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) {
                st.setDefaultBufferSize(640, 640);
                previewSurface = new Surface(st);
            }

            @Override
            public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) {}

            @Override
            public boolean onSurfaceTextureDestroyed(SurfaceTexture st) {
                previewSurface = null;
                return true;
            }

            @Override
            public void onSurfaceTextureUpdated(SurfaceTexture st) {}
        });
        String[] perms = {Manifest.permission.CAMERA};
        boolean all = true;
        for (String p : perms) {
            all &= checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED;
        }
        if (all) {
            begin();
        } else {
            requestPermissions(perms, 1);
        }
        ui.postDelayed(this::tick, 1000);
    }

    private void readExtras(Intent i) {
        if (i == null) {
            return;
        }
        if (i.hasExtra("target")) {
            target = i.getStringExtra("target");
            deviceAddr = null;
        }
        if (i.hasExtra("host")) deviceAddr = i.getStringExtra("host");
        fps = i.getIntExtra("fps", fps);
        kbpsSet = i.hasExtra("kbps");
        kbps = i.getIntExtra("kbps", kbps);
        size = i.getIntExtra("size", size);
        noBands = i.getBooleanExtra("nobands", false);
        if (i.hasExtra("codec") || "vp8".equals(i.getStringExtra("sw"))) {
            wantVp8 = "vp8".equalsIgnoreCase(i.getStringExtra("codec")) || "vp8".equals(i.getStringExtra("sw"));
            wantVp8Set = true;
        }
        auto = i.getBooleanExtra("auto", auto);
        front = i.getBooleanExtra("front", front);
        dumpLeft = i.getIntExtra("dump", 0);
        pattern = i.getBooleanExtra("pattern", false);
        if (pattern) source = "pattern";
        intraRefresh = i.getBooleanExtra("ir", true);
        if (i.hasExtra("source")) source = i.getStringExtra("source");
        if (i.hasExtra("clip")) source = CLIP + i.getStringExtra("clip");
        clipTopKbps = i.getIntExtra("clipkbps", clipTopKbps);
        clipKbpsSet = i.hasExtra("clipkbps");
        clipKbps = clipTopKbps;
        playoutSet = i.hasExtra("playout");
        playoutMs = i.getIntExtra("playout", playoutMs);
        maxKbpsSet = i.hasExtra("maxkbps");
        maxKbps = i.getIntExtra("maxkbps", maxKbps);
        adaptive = i.getBooleanExtra("adaptive", true);
        aeFps = i.getIntExtra("aefps", aeFps);
        sensorFps = i.getIntExtra("sensorfps", sensorFps);
        benchDump = i.getBooleanExtra("benchdump", false);
        benchLoops = i.getIntExtra("benchloops", 1);
        latLog = i.getBooleanExtra("latlog", false);
        live = i.getIntExtra("live", 1) != 0;
        aheadPct = i.getIntExtra("ahead", aheadPct);
        behindPct = i.getIntExtra("behind", behindPct);
        if (i.getBooleanExtra("saveclips", false)) {
            java.io.File d = new java.io.File(getExternalFilesDir(null), "clips");
            d.mkdirs();
            ClipStreamer.saveDir = d;
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] res) {
        for (int r : res) {
            if (r != PackageManager.PERMISSION_GRANTED) {
                status.setText("Needs camera permission");
                return;
            }
        }
        begin();
    }

    /* `am start ... --es pick <what>` on the running app presses a button (what: as in pick()). */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        String what = intent.getStringExtra("pick");
        if (what != null) {
            pick(what);
        }
    }

    /* The debug buttons: switch what streams, or stop. */
    private void pick(String what) {
        if (link == null) {
            return;
        }
        Log.i(TAG, "pick " + what);
        if ("device".equals(what)) {
            showPicker();
            return;
        }
        if ("rate".equals(what)) {
            showRatePicker();
            return;
        }
        if ("codec".equals(what) || "h264".equals(what) || "vp8".equals(what)) {
            setCodec("codec".equals(what) ? !wantVp8 : "vp8".equals(what));
            return;
        }
        if (what.startsWith(CLIP) && isClip() && link.streaming()) {
            source = what;   /* one clip to another: the stream goes on, the clip switches */
            startClip(lastSize, link.caps());
            return;
        }
        if (link.streaming()) {
            stopStream();
        }
        if ("stop".equals(what)) {
            auto = false;
            return;
        }
        source = what;
        auto = true;
        if (link.ready()) {
            ui.postDelayed(this::startStream, 300);
        }
    }

    private void begin() {
        android.content.SharedPreferences rp = getSharedPreferences(PREFS, MODE_PRIVATE);
        fixedClipKbps = rp.getInt(PREF_CLIP_KBPS, 0);
        fixedCamKbps = rp.getInt(PREF_CAM_KBPS, 0);
        if (!getIntent().hasExtra("size")) {
            size = rp.getInt(PREF_SIZE, 0);
        }
        if (!getIntent().hasExtra("fps")) {
            fps = rp.getInt(PREF_FPS, fps);
        }
        Intent in = getIntent();
        if (in != null && in.hasExtra("rateclip")) {
            setClipRate(in.getIntExtra("rateclip", 0));
        }
        if (in != null && in.hasExtra("ratec")) {
            setCamRate(in.getIntExtra("ratec", 0));
        }
        updateRateButton();
        if (!wantVp8Set) {
            wantVp8 = rp.getBoolean(PREF_CODEC, false);
        }
        updateCodecButton();
        if (target == null && deviceAddr == null) {
            android.content.SharedPreferences p = getSharedPreferences(PREFS, MODE_PRIVATE);
            deviceAddr = p.getString(PREF_ADDR, null);
            target = p.getString(PREF_NAME, null);
        }
        link = new WifiLink(this, this);
        link.setTarget(deviceAddr, target);
        cam = new CameraStreamer(this, front, this);
        meter = new LatencyMeter();
        meter.perFrame = latLog;
        cam.meter = meter;
        link.meter = meter;
        cam.benchDump = benchDump;
        cam.encParams = getIntent() != null ? getIntent().getStringExtra("venc") : null;
        cam.intraRefreshFrames = getIntent() != null ? getIntent().getIntExtra("irframes", 0) : 0;
        if (getIntent() != null) {
            cam.panelFilter = getIntent().getIntExtra("pf", 1) != 0;
            /* The software encoder when this build has it (jni/build.sh;
             * --es sw hw: the phone's hardware encoder regardless). */
            String sw = getIntent().getStringExtra("sw");
            cam.swEncoder = sw != null && !"vp8".equals(sw) ? ("hw".equals(sw) ? null : sw) : (X264.available() ? "x264" : null);
            baseSw = cam.swEncoder;
            String pre = getIntent().getStringExtra("swpreset");
            if (pre != null) {
                cam.swPreset = pre;
            }
            String swp = getIntent().getStringExtra("swparams");
            cam.swParams = swp != null ? swp : "no-deblock=1";
        }
        cam.benchLoops = benchLoops;
        cam.pattern = pattern;
        cam.intraRefresh = intraRefresh;
        cam.minAeFps = aeFps;
        cam.sensorFps = sensorFps;
        link.start();
    }

    @Override
    protected void onDestroy() {
        if (cam != null) cam.stop();
        if (link != null) {
            link.stopStreaming();
            link.close();
        }
        super.onDestroy();
    }

    /* ---- streaming ------------------------------------------------------ */

    private void startStream() {
        VideoLink.Caps c = link.caps();
        int s = size > 0 ? size : c.sugW;
        s = Math.min(s, Math.min(c.maxW, c.maxH)) & ~7;
        lastSize = s;
        if (isClip()) {
            startClip(s, c);
            return;
        }
        int f = Math.min(fps, c.maxFps);
        curFps = f;
        /* The gadget's suggested bitrate, unless given: it says what its link
         * and decoder are tuned for. A gadget suggesting less than 450 kb/s
         * also caps the adaptive bitrate near it: its quality comes from the
         * encoding at that rate, not from using more of the link. */
        if (fixedCamKbps > 0) {
            kbps = fixedCamKbps;
        } else if (!kbpsSet && c.sugKbps > 0) {
            kbps = c.sugKbps;
        }
        if (!maxKbpsSet) {
            /* A gadget suggesting 600 or more starts there and may climb
             * to a quarter more, where the link carries it; the adaptation
             * finds how much it does. */
            maxKbps = c.sugKbps > 0 && c.sugKbps < 450 ? Math.max(kbps, c.sugKbps * 11 / 10)
                    : c.sugKbps >= 600 ? c.sugKbps * 5 / 4 : 650;
        }
        /* VP8 where it was picked, the gadget decodes it and the build has libvpx. */
        camVp8 = wantVp8 && c.vp8 && Vpx.available();
        if (wantVp8 && !camVp8) {
            Log.i(TAG, "VP8 picked, but " + (c.vp8 ? "this build has no libmgvpx" : "the gadget doesn't decode it") + ": H.264");
        }
        cam.swEncoder = camVp8 ? "vp8" : baseSw;
        /* Past 240 px on a gadget that decodes bands in parallel (one per
         * core), send two: x264 or VP8 only (one encoder per band). */
        int bands = c.maxTiles >= 2 && s > 240 && s % 32 == 0 && cam.swEncoder != null && !noBands ? 2 : 1;
        cam.bands = bands;
        cam.bandSplitPct = c.tileSplitPct > 0 ? c.tileSplitPct : 50;
        Log.i(TAG, "starting " + s + "x" + s + " @" + f + " " + kbps + " kb/s (adaptive up to " + maxKbps + ")"
                + (bands > 1 ? ", two bands" : ""));
        link.startStreaming(s, s, f, kbps, 0, bands, camVp8);
        setLiveBudget();
        try {
            cam.pattern = "pattern".equals(source);
            cam.srcFile = source.startsWith("file:")
                    ? new java.io.File(getExternalFilesDir(null), source.substring(5)).getPath() : null;
            cam.setPreview(previewSurface);
            if (c.dispW > 0 && c.dispH > 0) {
                cam.panelPx = Math.min(c.dispW, c.dispH);   /* what the gadget scales the picture to */
            }
            cam.start(s, s, f, kbps, c.round);
        } catch (Exception e) {
            Log.e(TAG, "camera start failed", e);
            link.stopStreaming();
        }
    }

    private boolean isClip() {
        return source.startsWith(CLIP);
    }

    private String clipKey(String name, int size, int kbps) {
        return name + "|" + size + "|" + kbps + (clipVp8() ? "|vp8" : "");
    }

    /* Clips as VP8: picked, the gadget decodes it, and the build has libvpx. */
    private boolean clipVp8() {
        return wantVp8 && link != null && link.caps().vp8 && Vpx.available();
    }

    /* The Codec button: H.264 or VP8 from now on, for whatever streams; the stream starts over in it. */
    private void setCodec(boolean vp8) {
        wantVp8 = vp8;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(PREF_CODEC, vp8).apply();
        updateCodecButton();
        Log.i(TAG, "codec: " + (vp8 ? "VP8" : "H.264"));
        if (link != null && link.streaming()) {
            String src = source;
            stopStream();
            source = src;
            auto = true;
            ui.postDelayed(this::startStream, 300);
        }
    }

    private void updateCodecButton() {
        if (codecButton != null) {
            codecButton.setText(wantVp8 ? "VP8" : "H.264");
        }
    }

    /* The device shows clips evenly this long after they start, so a keyframe that takes a few frame times on the link doesn't stutter. */
    private int playoutMs = 200;
    private boolean playoutSet;

    /*
     * The bitrates clips are encoded at: clipTopKbps,
     * then three steps down (none below 180 kb/s). Past 320 px the steps are
     * small (90%, 80%, 70%): a big clip at less than 70% turns blocky, and a
     * link that carries a little less than the top rate (the phone in a
     * pocket) should cost a little quality, not most of it.
     */
    private int[] clipLevels() {
        int[] l = clipSize > 320 ? new int[] {clipTopKbps, clipTopKbps * 90 / 100, clipTopKbps * 80 / 100, clipTopKbps * 70 / 100}
                : new int[] {clipTopKbps, clipTopKbps * 78 / 100, clipTopKbps * 60 / 100, clipTopKbps * 45 / 100};
        for (int i = 0; i < l.length; i++) {
            l[i] = Math.max(180, l[i]);
        }
        return l;
    }

    /* Shows the clip now (in the best version already encoded) and has the encoder make the right one. */
    private void startClip(int s, VideoLink.Caps c) {
        if (player == null) {
            player = new ClipStreamer(this);
        }
        clipSize = s;
        clipRound = c.round;
        playWanted();
        wakeClipWorker();
    }

    private void wakeClipWorker() {
        synchronized (clipLock) {
            if (clipThread == null) {
                clipThread = new Thread(this::clipLoop, "clips");
                clipThread.setPriority(Thread.NORM_PRIORITY - 1);
                clipThread.start();
            }
            clipLock.notifyAll();
        }
    }

    /*
     * One clip at a time (each takes a few seconds), always the wanted clip
     * at the current bitrate first, so a tap waits at most for the clip being
     * encoded, never for a whole batch. Then the other clips already played
     * at another bitrate (their source fetched once already), so a step in
     * bitrate finds them ready.
     */
    private void clipLoop() {
        while (true) {
            int s = clipSize, kbps = clipKbps;
            boolean round = clipRound;
            String next = null;
            if (isClip()) {
                String want = source.substring(CLIP.length());
                if (needsClip(want, s, kbps)) {
                    next = want;
                }
            }
            for (String name : clipsToKeepReady()) {
                if (next == null && needsClip(name, s, kbps)) {
                    next = name;
                }
            }
            if (next == null) {
                synchronized (clipLock) {
                    try {
                        clipLock.wait(1000);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
                continue;
            }
            if (encodeClip(next, s, round, kbps) != null) {
                ui.post(this::playWanted);
            }
        }
    }

    /* Clips played before at this size: worth having at every bitrate. */
    private java.util.List<String> clipsToKeepReady() {
        java.util.List<String> names = new java.util.ArrayList<>();
        for (ClipStreamer.Clip c : clips.values()) {
            if (c.size == clipSize && !names.contains(c.name)) {
                names.add(c.name);
            }
        }
        return names;
    }

    private boolean needsClip(String name, int s, int kbps) {
        String key = clipKey(name, s, kbps);
        return !clips.containsKey(key) && !failedClips.contains(key);
    }

    /*
     * The wanted clip's best encoded version: at clipKbps, else the
     * nearest lower bitrate, else the lowest higher one; null if none yet.
     */
    private ClipStreamer.Clip bestClip(String name, int s) {
        ClipStreamer.Clip exact = clips.get(clipKey(name, s, clipKbps)), below = null, above = null;
        if (exact != null) {
            return exact;
        }
        for (ClipStreamer.Clip c : clips.values()) {
            if (!c.name.equals(name) || c.size != s || c.vp8 != clipVp8()) {
                continue;
            }
            if (c.kbps < clipKbps && (below == null || c.kbps > below.kbps)) {
                below = c;
            } else if (c.kbps > clipKbps && (above == null || c.kbps < above.kbps)) {
                above = c;
            }
        }
        return below != null ? below : above;
    }

    /* On the UI thread: plays the best version of the wanted clip if it isn't what's playing. */
    private void playWanted() {
        if (!isClip() || player == null || !link.ready()) {
            return;
        }
        String name = source.substring(CLIP.length());
        ClipStreamer.Clip best = bestClip(name, clipSize);
        if (best == null) {
            if (!failedClips.contains(clipKey(name, clipSize, clipKbps))) {
                linkStatus = "Preparing " + clipLabel(name) + "...";
            }
            return;
        }
        if (best == player.clip() && link.streaming()) {
            return;
        }
        playClip(best);
    }

    private ClipStreamer.Clip encodeClip(String name, int s, boolean round, int kbps) {
        String key = clipKey(name, s, kbps);
        ClipStreamer.Clip clip = clips.get(key);
        if (clip != null) {
            return clip;
        }
        SampleClips.Sample sample = SampleClips.get(name);
        try {
            if (sample == null) {
                throw new IllegalArgumentException("no clip called " + name);
            }
            clip = ClipStreamer.encode(name, sample.source(), s, sample.fps, kbps, round, sample.startS * 1_000_000L,
                    sample.seconds * sample.fps, clipVp8());
            clips.put(key, clip);
            return clip;
        } catch (Exception e) {
            Log.e(TAG, "clip " + name + " failed", e);
            failedClips.add(key);
            linkStatus = clipLabel(name) + " failed: " + e + (sample != null ? " (needs the Internet)" : "");
            return null;
        }
    }

    private void playClip(ClipStreamer.Clip clip) {
        ClipStreamer.Clip cur = player.clip();
        boolean same = link.streaming() && cur != null && cur.size == clip.size && cur.fps == clip.fps
                && curFps == clip.fps && cur.vp8 == clip.vp8;
        if (!same) {
            if (link.streaming()) {
                player.stop();
                link.stopStreaming();
            }
            curFps = clip.fps;
            link.startStreaming(clip.size, clip.size, clip.fps, Math.max(clip.kbps, clip.steadyKbps()), playoutMs, 1,
                    clip.vp8);
        }
        player.play(clip);
        SampleClips.Sample sample = SampleClips.get(clip.name);
        linkStatus = (sample != null ? sample.credit + "\n" : "") + clip.info;
    }

    private void stopStream() {
        cam.stop();
        if (player != null) {
            player.stop();
        }
        link.stopStreaming();
    }

    @Override
    public long queuedBytes() {
        return link.queuedBytes();
    }

    @Override
    public void onFrame(byte[] frame, boolean key) {
        link.sendFrame(frame);
        if (dumpLeft > 0) {
            dump(frame);
        }
    }

    /* --ei dump N: the first N frames sent, as u32 length + access unit, for a host decoder. */
    private int dumpLeft;
    private java.io.FileOutputStream dumpOut;

    private void dump(byte[] frame) {
        try {
            if (dumpOut == null) {
                dumpOut = new java.io.FileOutputStream(new java.io.File(getExternalFilesDir(null), "dump.au"));
            }
            int n = frame.length - VideoFrame.HEADER_LEN;
            dumpOut.write(new byte[] {(byte) n, (byte) (n >> 8), (byte) (n >> 16), (byte) (n >> 24)});
            dumpOut.write(frame, VideoFrame.HEADER_LEN, n);
            if (--dumpLeft == 0) {
                dumpOut.close();
                Log.i(TAG, "dump written");
            }
        } catch (java.io.IOException e) {
            Log.w(TAG, "dump failed", e);
            dumpLeft = 0;
        }
    }

    @Override
    public boolean behind() {
        /* More than about two frames' worth waiting on the phone: skip this
         * camera frame rather than add latency. */
        long frame = kbps * 1000L / 8 / Math.max(1, curFps);
        long budget = live ? Math.max(1200, frame * behindPct / 100) : Math.max(4000, frame * 2);
        return link.queuedBytes() > budget;
    }

    /* ---- device picker ------------------------------------------------ */

    private static final class Seen {
        String name;
        int rssi;
        long at;
    }

    private final java.util.LinkedHashMap<String, Seen> seen = new java.util.LinkedHashMap<>();
    private android.app.AlertDialog picker;
    private android.widget.ArrayAdapter<String> pickerRows;
    private final java.util.ArrayList<String> pickerAddrs = new java.util.ArrayList<>();

    /* Lists the gadgets found on the network; tapping one connects to it
     * (and drops the current link). The choice is remembered. */
    private void showPicker() {
        if (picker != null && picker.isShowing()) {
            return;
        }
        pickerRows = new android.widget.ArrayAdapter<>(this, android.R.layout.simple_list_item_1, new java.util.ArrayList<>());
        android.widget.ListView list = new android.widget.ListView(this);
        list.setAdapter(pickerRows);
        list.setOnItemClickListener((parent, v, pos, id) -> {
            if (pos >= pickerAddrs.size()) {
                return;
            }
            String addr = pickerAddrs.get(pos);
            Seen s = seen.get(addr);
            String name = s != null ? s.name : addr;
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(PREF_ADDR, addr).putString(PREF_NAME, name).apply();
            deviceAddr = addr;
            target = name;
            Log.i(TAG, "picked " + name + " " + addr);
            if (link.streaming()) {
                stopStream();
            }
            link.select(addr, name);
            picker.dismiss();
        });
        picker = new android.app.AlertDialog.Builder(this)
                .setTitle("Connect to")
                .setView(list)
                .setNegativeButton("Cancel", null)
                .create();
        picker.setOnDismissListener(d -> {
            link.pickerClosed();
            picker = null;
        });
        picker.show();
        refreshPicker();
        link.scanForPicker();
    }

    private void refreshPicker() {
        if (picker == null) {
            return;
        }
        long now = SystemClock.elapsedRealtime();
        String cur = link.connectedAddress();
        java.util.List<java.util.Map.Entry<String, Seen>> rows = new java.util.ArrayList<>();
        for (java.util.Map.Entry<String, Seen> e : seen.entrySet()) {
            if (now - e.getValue().at < 15000 || e.getKey().equalsIgnoreCase(cur)) {
                rows.add(e);
            }
        }
        rows.sort((a, b) -> b.getValue().rssi - a.getValue().rssi);
        pickerAddrs.clear();
        pickerRows.clear();
        for (java.util.Map.Entry<String, Seen> e : rows) {
            boolean on = e.getKey().equalsIgnoreCase(cur);
            pickerAddrs.add(e.getKey());
            pickerRows.add(String.format(java.util.Locale.US, "%s%s\n%s", e.getValue().name,
                                         on ? "  (connected)" : "", e.getKey()));
        }
        if (rows.isEmpty()) {
            pickerRows.add("Looking on this Wi-Fi network...");
        }
    }

    @Override
    public void onSeen(String name, String address, int rssi) {
        ui.post(() -> {
            Seen s = seen.get(address);
            if (s == null) {
                s = new Seen();
                seen.put(address, s);
            }
            boolean changed = !name.equals(s.name) || Math.abs(rssi - s.rssi) >= 4 || s.at == 0;
            s.name = name;
            s.rssi = rssi;
            s.at = SystemClock.elapsedRealtime();
            if (changed) {
                refreshPicker();
            }
        });
    }

    /* ---- VideoLink.Listener ------------------------------------------- */

    @Override
    public void onStatus(String s) {
        linkStatus = s;
        Log.i(TAG, s);
    }

    @Override
    public void onReady(VideoLink.Caps caps) {
        linkStatus = "Connected: " + caps;
        clipSize = Math.min(size > 0 ? size : caps.sugW, Math.min(caps.maxW, caps.maxH)) & ~7;
        clipRound = caps.round;
        /* A gadget suggesting a high bitrate (a big picture, or Wi-Fi) gets
         * clips aimed at 92% of it, peaks held under the link
         * (ClipStreamer.encodeLoopsX264), and a 300 ms playout delay for
         * the peaks' queue. */
        if (!clipKbpsSet) {
            clipTopKbps = caps.sugKbps >= 600 ? caps.sugKbps * 92 / 100 : 450;
        }
        if (!playoutSet) {
            playoutMs = caps.sugKbps >= 600 && caps.maxPlayoutMs >= 300 ? 300 : 200;
        }
        if (fixedClipKbps > 0 && java.util.Arrays.stream(clipLevels()).anyMatch(k -> k == fixedClipKbps)) {
            clipKbps = fixedClipKbps;
        } else if (!java.util.Arrays.stream(clipLevels()).anyMatch(k -> k == clipKbps)) {
            clipKbps = clipLevels()[0];
        }
        wakeClipWorker();
        ui.post(() -> {
            if ((auto || wanted) && !link.streaming()) {
                startStream();
            }
        });
    }

    @Override
    public void onRequestStreaming(boolean want) {
        Log.i(TAG, "gadget asks for streaming: " + want);
        wanted = want;
        ui.post(() -> {
            if (want && !link.streaming()) {
                startStream();
            } else if (!want && link.streaming()) {
                stopStream();
            }
        });
    }

    @Override
    public void onKeyframeRequest() {
        Log.i(TAG, "keyframe requested");
        cam.requestKeyframe();
        if (player != null) {
            player.requestKeyframe();
        }
    }

    @Override
    public void onStreamStopped() {
        Log.i(TAG, "gadget stopped the stream");
        ui.post(() -> {
            cam.stop();
            if (player != null) {
                player.stop();
            }
        });
    }

    @Override
    public void onStartRefused(String reason) {
        Log.i(TAG, "gadget refused the stream (" + reason + "); trying again in 1 s");
        ui.postDelayed(() -> {
            if (!(auto || wanted) || !link.ready() || link.streaming()) {
                return;
            }
            if (isClip() && player != null && player.clip() != null) {
                ClipStreamer.Clip clip = player.clip();
                link.startStreaming(clip.size, clip.size, clip.fps, Math.max(clip.kbps, clip.steadyKbps()), playoutMs, 1,
                        clip.vp8);
                player.requestKeyframe();
            } else if (!isClip() && cam.running()) {
                /* The camera and encoder keep going: just ask again and start on a keyframe. */
                link.startStreaming(lastSize, lastSize, curFps, kbps, 0, cam.bands, camVp8);   /* the same bands and codec as the frames */
                cam.requestKeyframe();
            } else {
                startStream();
            }
        }, 1000);
    }

    @Override
    public void onBufferUpdate(long avail, long received, int shown, int dropped, int decodeMs) {
        devAvail = avail;
        devRecv = received;
        devShown = shown;
        devDropped = dropped;
        devMs = decodeMs;
    }

    @Override
    public void onDeviceLog(String line) {
        if (line.contains("video")) {
            Log.i(TAG, "gadget: " + line.trim());
            if (line.contains("fps decoded")) {
                deviceLine = line.trim();
            }
        }
    }

    @Override
    public void onDisconnected() {
        Log.i(TAG, "disconnected");
        ui.post(() -> {
            cam.stop();
            if (player != null) {
                player.stop();
            }
        });
    }

    /* ---- adaptive bitrate (camera) --------------------------------------- */

    private int maxKbps = 650, minKbps = 200;
    private boolean kbpsSet, maxKbpsSet;
    /* The Rate button: a fixed bitrate for the clips and for the camera
     * (0: automatic, the adaptive bitrate / the clips' ladder). Remembered. */
    private int fixedClipKbps, fixedCamKbps;
    private android.widget.Button rateButton;
    private static final String PREF_CLIP_KBPS = "clip_kbps", PREF_CAM_KBPS = "cam_kbps";
    private static final int[] CAM_RATES = {150, 200, 250, 300, 400, 500, 650, 800, 1000, 1200};
    /* Picture sizes for gadgets that decode more than 240 px (the ESP-Mosaico). */
    private static final int[] SIZES = {240, 320, 480};   /* 2x, 3/2 and 1:1 on its 480 panel */
    private static final String PREF_SIZE = "size", PREF_FPS = "fps";

    private void showRatePicker() {
        int[] clips = clipLevels();
        java.util.List<String> labels = new java.util.ArrayList<>();
        java.util.List<int[]> picks = new java.util.ArrayList<>();   /* {0 clip / 1 camera, kb/s} */
        labels.add("Clips: auto (step down when the link falls behind)");
        picks.add(new int[] {0, 0});
        for (int k : clips) {
            labels.add("Clips: " + k + " kb/s");
            picks.add(new int[] {0, k});
        }
        labels.add("Camera: auto (adaptive, up to " + maxKbps + ")");
        picks.add(new int[] {1, 0});
        for (int k : CAM_RATES) {
            labels.add("Camera: " + k + " kb/s");
            picks.add(new int[] {1, k});
        }
        VideoLink.Caps caps = link != null ? link.caps() : null;
        int maxSize = caps != null ? Math.min(caps.maxW, caps.maxH) : 240;
        if (maxSize > 240) {
            labels.add("Size: auto (the gadget's " + (caps != null ? caps.sugW : 0) + " px)");
            picks.add(new int[] {2, 0});
            for (int k : SIZES) {
                if (k <= maxSize) {
                    labels.add("Size: " + k + " px");
                    picks.add(new int[] {2, k});
                }
            }
        }
        /* The camera's frame rate: 24 fps puts a quarter more bits in each
         * frame (about 0.8 dB at 480 px and 600 kb/s), 30 moves more smoothly. */
        labels.add("Camera frame rate: 30 fps (smoothest)");
        picks.add(new int[] {3, 30});
        labels.add("Camera frame rate: 24 fps (sharper at the same bitrate)");
        picks.add(new int[] {3, 24});
        String[] items = new String[labels.size()];
        for (int i = 0; i < items.length; i++) {
            int[] p = picks.get(i);
            boolean on = p[0] == 0 ? p[1] == fixedClipKbps : p[0] == 1 ? p[1] == fixedCamKbps : p[0] == 2 ? p[1] == size
                    : p[1] == fps;
            items[i] = (on ? "\u2713 " : "    ") + labels.get(i);
        }
        new android.app.AlertDialog.Builder(this)
                .setTitle("Bitrate")
                .setItems(items, (d, which) -> {
                    int[] p = picks.get(which);
                    if (p[0] == 0) {
                        setClipRate(p[1]);
                    } else if (p[0] == 1) {
                        setCamRate(p[1]);
                    } else if (p[0] == 2) {
                        setSize(p[1]);
                    } else {
                        setFps(p[1]);
                    }
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void setClipRate(int k) {
        fixedClipKbps = k;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_CLIP_KBPS, k).apply();
        Log.i(TAG, "clip bitrate " + (k > 0 ? k + " kb/s (fixed)" : "auto"));
        int target = k > 0 ? k : clipLevels()[0];
        if (target != clipKbps) {
            clipKbps = target;
            heldSeconds = 0;
            clipCalmSeconds = 0;
            if (isClip() && link != null && link.streaming()) {
                startClip(lastSize, link.caps());
            }
        }
        updateRateButton();
    }

    private void setCamRate(int k) {
        fixedCamKbps = k;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_CAM_KBPS, k).apply();
        Log.i(TAG, "camera bitrate " + (k > 0 ? k + " kb/s (fixed)" : "auto"));
        if (k > 0) {
            kbps = k;
            if (cam != null && cam.running()) {
                cam.setBitrate(k);
                setLiveBudget();
            }
        }
        updateRateButton();
    }

    /* A new picture size restarts what streams (0: the gadget's suggestion). */
    private void setFps(int f) {
        fps = f;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_FPS, f).apply();
        Log.i(TAG, "camera frame rate " + f + " fps");
        if (link != null && link.streaming() && !isClip()) {
            stopStream();
            ui.postDelayed(this::startStream, 300);
        }
        updateRateButton();
    }

    private void setSize(int k) {
        size = k;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_SIZE, k).apply();
        Log.i(TAG, "picture size " + (k > 0 ? k + " px" : "auto"));
        if (link != null && link.streaming()) {
            stopStream();
            ui.postDelayed(this::startStream, 300);
        }
        updateRateButton();
    }

    /*
     * The picture going to the gadget: its size (the gadget may decode
     * several), frame rate, bands, the bitrate aimed at and what the link
     * carried, and its source.
     */
    private String sendingLine(double sentKbps) {
        if (link == null || !link.streaming()) {
            return "Not sending";
        }
        java.util.Locale us = java.util.Locale.US;
        if (isClip()) {
            ClipStreamer.Clip c = player != null ? player.clip() : null;
            if (c == null) {
                return "Preparing clip";
            }
            return String.format(us, "Sending %s %d×%d · %d fps\n%d kb/s (%s) · link %.0f kb/s\n%s", c.vp8 ? "VP8" : "H.264",
                    c.size, c.size, c.fps,
                    c.kbps, fixedClipKbps > 0 ? "fixed" : "auto", sentKbps, clipLabel(c.name));
        }
        String src = "pattern".equals(source) ? "Bars" : source != null && source.startsWith("file:") ? "Clip " + source.substring(5)
                : "Camera";
        return String.format(us, "Sending %s %d×%d · %d fps%s\n%d kb/s (%s) · link %.0f kb/s\n%s · %s", camVp8 ? "VP8" : "H.264",
                lastSize, lastSize, curFps,
                cam.bands == 2 ? " · 2 bands" : "", kbps, fixedCamKbps > 0 ? "fixed" : "auto", sentKbps, src,
                cam.encoderName.isEmpty() ? "encoder starting" : cam.encoderName.split(" ")[0]);
    }

    private static String clipLabel(String name) {
        SampleClips.Sample c = SampleClips.get(name);
        return c != null ? c.label : name;
    }

    private void updateRateButton() {
        if (rateButton == null) {
            return;
        }
        if (fixedClipKbps == 0 && fixedCamKbps == 0 && size == 0) {
            rateButton.setText("Rate");
        } else {
            rateButton.setText((fixedClipKbps > 0 ? "Clip " + fixedClipKbps : "Clip -") + "\n"
                    + (fixedCamKbps > 0 ? "Cam " + fixedCamKbps : "Cam -") + (size > 0 ? "\n" + size + "px" : ""));
        }
    }
    private int lastSkipped;
    private long lastGateUs;
    private int calmSeconds, gadgetBehindSeconds;

    /*
     * The camera's bitrate, once a second. When the link falls behind (frames
     * skipped for it (three or more a second; one or two only hold the
     * rate), or the send gate holding them 150 ms a second), go to
     * what it carried that second less 5% (at least 10% down) in one step;
     * when the gadget falls behind (showing clearly fewer frames than it's
     * sent, decoding near the frame time), 10% down; after two calm seconds,
     * 10% up, to maxKbps, so a dip is made good in a few seconds. Never below
     * camFloorKbps(). It used to cut 20% whenever the gadget's decode time
     * times the frame rate passed 85% of a second, which the ESP-Mosaico's
     * 480 px bands (28-29 ms at 30 fps) always did: the rate fell to the
     * 200 kb/s floor and never came back.
     */
    /*
     * Clips can't skip frames, so a link that carries less than the
     * clip plays it in slow motion. Once a second: when frames waited on the
     * link for more than 40 ms of the last second, two seconds running,
     * re-encode a step down (down to 180 kb/s) and switch at once; after 8
     * calm seconds, step back up towards clipTopKbps. 40 ms, not more: by the
     * time the phone holds frames that long, the gadget's playout delay has
     * run dry and it shows frames late (a link that dropped from ~580 to
     * ~510 kb/s, the phone farther from the board, under a 552 kb/s clip).
     */
    private void adaptClip() {
        if (!isClip() || player == null || !link.streaming() || fixedClipKbps > 0) {
            lastHeldMs = player == null ? 0 : player.statHeldMs;
            return;
        }
        long held = player.statHeldMs - lastHeldMs;
        lastHeldMs = player.statHeldMs;
        heldSeconds = held > 40 ? heldSeconds + 1 : 0;
        clipCalmSeconds = held > 20 ? 0 : clipCalmSeconds + 1;
        int[] levels = clipLevels();
        int at = 0;
        while (at < levels.length - 1 && levels[at] > clipKbps) {
            at++;
        }
        int next = clipKbps;
        if (heldSeconds >= 2 && at < levels.length - 1) {
            next = levels[at + 1];
        } else if (clipCalmSeconds >= 8 && at > 0) {
            next = levels[at - 1];
        }
        if (next != clipKbps) {
            Log.i(TAG, "clip bitrate " + clipKbps + " -> " + next + " kb/s (held " + held + " ms in the last second)");
            clipKbps = next;
            heldSeconds = 0;
            clipCalmSeconds = 0;
            startClip(lastSize, link.caps());
        }
    }

    private void adaptBitrate(long dtMs, long sentBytes, double encFps, double shownFps) {
        if (!link.streaming() || isClip() || !adaptive || fixedCamKbps > 0) {
            return;
        }
        int skipped = cam.statSkipped;
        long gate = link.statGateUs;
        int newSkips = skipped - lastSkipped;
        long gateMsPerS = (gate - lastGateUs) / 1000 * 1000 / Math.max(1, dtMs);
        lastSkipped = skipped;
        lastGateUs = gate;
        int fps = Math.max(1, curFps);
        int sentKbps = (int) (sentBytes * 8 / Math.max(1, dtMs));
        /* One or two frames skipped (a big frame now and then) only holds
         * the rate; three or more, or the gate holding frames, lower it. */
        boolean linkBehind = newSkips > 2 || gateMsPerS > 150;
        boolean linkTight = newSkips > 0;
        /* The gadget falls behind: it shows clearly fewer frames than it's
         * sent, decoding close to the frame time. */
        boolean gadgetBehind = devMs > 0 && devMs * fps > 900 && encFps > 5 && shownFps < encFps * 0.85;
        gadgetBehindSeconds = gadgetBehind ? gadgetBehindSeconds + 1 : 0;
        int floor = camFloorKbps();
        int k = cam.kbps(), was = k;
        String why;
        if (linkBehind) {
            /* What the link carried this second, with a margin: one step to
             * where it fits, not 20% a second down to the floor. */
            k = Math.max(floor, Math.min(k * 9 / 10, sentKbps * 95 / 100));
            calmSeconds = 0;
            why = "link behind";
        } else if (gadgetBehindSeconds >= 2) {
            /* Bits don't cost the decoder much (pixels do): a little, once. */
            k = Math.max(floor, k * 9 / 10);
            gadgetBehindSeconds = 0;
            calmSeconds = 0;
            why = "gadget behind";
        } else if (linkTight) {
            calmSeconds = 0;
            return;
        } else if (++calmSeconds >= 2 && k < maxKbps) {
            k = Math.min(maxKbps, k * 110 / 100 + 1);
            why = "calm";
        } else {
            return;
        }
        if (k != was) {
            cam.setBitrate(k);
            kbps = k;
            setLiveBudget();
            Log.i(TAG, "bitrate " + was + " -> " + k + " kb/s, " + why + " (sent " + sentKbps + " kb/s, skips " + newSkips
                    + ", gate " + gateMsPerS + " ms/s, gadget " + String.format(java.util.Locale.US, "%.1f/%.1f", shownFps, encFps)
                    + " fps, decode " + devMs + " ms)");
        }
    }

    /* The adaptive bitrate's floor: below two thirds of the gadget's
     * suggestion a 480 px picture turns blocky (a smaller one, chosen with
     * Rate, looks better on a link that slow); minKbps for small pictures. */
    private int camFloorKbps() {
        return lastSize > 320 ? Math.max(minKbps, maxKbps * 2 / 3) : minKbps;
    }

    /* ---- stats ---------------------------------------------------------- */

    private void tick() {
        ui.postDelayed(this::tick, 1000);
        if (link == null) {
            return;
        }
        long now = android.os.SystemClock.elapsedRealtime();
        long dt = Math.max(1, now - lastStatMs);
        long bytes = link.statBytes;
        boolean av = isClip() && player != null;
        int fed = av ? player.statSent : cam.statFed, enc = av ? player.statSent : cam.statEncoded, shown = devShown;
        int dEnc = Math.max(1, enc - lastEnc);
        long camEnc = cam.statCamToEncUs, encUs = cam.statEncUs, conv = cam.statConvUs;
        String lat = String.format(java.util.Locale.US, "cam->enc %.0f ms (convert %.1f, encoder %.1f)  gate %.0f ms/s",
                (camEnc - lastCamEnc) / 1000.0 / dEnc, (conv - lastConv) / 1000.0 / Math.max(1, fed - lastFed),
                (encUs - lastEncUs) / 1000.0 / dEnc, (link.statGateUs - lastGate) / 1000.0 * 1000 / dt);
        lastCamEnc = camEnc;
        lastEncUs = encUs;
        lastConv = conv;
        lastGate = link.statGateUs;
        String s = String.format(java.util.Locale.US,
                "%s\n%s  %d kb/s target  %s\nsend %.0f kb/s  in flight %d  queued %d\ncamera->enc %.1f fps  encoded %.1f fps"
                        + "  skipped %d  keys %d\n%s\ngadget shown %.1f fps  dropped %d  decode %d ms  free %d\n%s",
                linkStatus, link.describe(), isClip() ? clipKbps : kbps,
                (link.streaming() ? "STREAMING" : "idle")
                        + ((isClip() ? fixedClipKbps : fixedCamKbps) > 0 ? " (fixed rate)" : " (auto rate)"),
                (bytes - lastBytes) * 8.0 / dt, link.inFlight(), link.queuedBytes(),
                (fed - lastFed) * 1000.0 / dt, (enc - lastEnc) * 1000.0 / dt, cam.statSkipped, cam.statKeys, lat,
                (shown - lastShown) * 1000.0 / dt, devDropped, devMs, devAvail, deviceLine);
        status.setText(s);
        if (link.streaming() && meter != null && !isClip()) {
            Log.i(TAG, meter.take());
        }
        if (link.streaming()) {
            Log.i(TAG, "stats: " + s.replace('\n', '|'));
        }
        adaptBitrate(dt, bytes - lastBytes, (enc - lastEnc) * 1000.0 / dt, (shown - lastShown) * 1000.0 / dt);
        sending.setText(sendingLine((bytes - lastBytes) * 8.0 / dt));
        adaptClip();
        lastStatMs = now;
        lastBytes = bytes;
        lastFed = fed;
        lastEnc = enc;
        lastShown = shown;
    }
}
