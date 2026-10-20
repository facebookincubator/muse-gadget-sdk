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
import android.view.Surface;
import android.view.TextureView;
import android.view.WindowManager;

/**
 * Muse Gadgets Test: finds the gadget on the local network, and streams the camera
 * (or an open sample clip) to it when the gadget's menu asks (its "want" message)
 * or, with auto on (a bench option), as soon as it's connected. Stats on
 * screen and in logcat (tag MuseCam), the gadget's decode stats included.
 * The screen (AppScreen) is a preview with the stats over it, above tabs of
 * settings-style lists (Tabs); this class runs the streams.
 *
 * Intent extras (adb shell am start -n com.musegadgets.test/.MainActivity
 * --es target A1B2C3 --ei fps 24 --ei kbps 1000 --ez auto true --ez front false;
 * --es host 192.168.1.20 skips mDNS).
 */
public final class MainActivity extends Activity
        implements VideoLink.Listener, CameraStreamer.Sink, ClipStreamer.Sink {
    private static final String TAG = VideoLink.TAG;

    private AppScreen screen;
    private TextureView preview;
    private Surface previewSurface;
    private SurfaceTexture previewTexture;
    /* A clip's (or the bars') frames, decoded onto the preview. */
    private final PreviewDecoder previewDecoder = new PreviewDecoder();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private VideoLink link;
    private CameraStreamer cam;
    /* What to connect to: --es target <name part> (first match), --es host
     * <address[:port]>, else the gadget last picked on the Gadget tab. */
    private String target, deviceAddr;
    private static final String PREFS = "musegadgetstest", PREF_ADDR = "wifi_addr", PREF_NAME = "wifi_name";
    private int fps = 30, kbps = 450, size = 0;
    private boolean noBands;   /* --ez nobands true: one stream even where the gadget takes two bands */
    /* The Codec button (or --es codec h264|vp8): VP8 for the camera and the
     * clips where the gadget lists it and this build has libmgvpx
     * (jni/build_vpx.sh), else H.264. Remembered. */
    private boolean wantVp8, wantVp8Set;
    private String baseSw;      /* the H.264 encoder: x264, or null for the phone's */
    private boolean camVp8;     /* the camera stream that's on is VP8 */
    private static final String PREF_CODEC = "codec";
    private boolean auto, front, pattern, intraRefresh = true, adaptive = true;
    /* What streams: "camera", "pattern" or "clip:<name>" (--es source, or the buttons). */
    private String source = "camera";
    /* Sample clips (SampleClips): fetched, decoded and encoded for the gadget when first played. */
    static final String CLIP = "clip:";
    private ClipStreamer player;
    private final java.util.Map<String, ClipStreamer.Clip> clips = new java.util.concurrent.ConcurrentHashMap<>();
    /* The encoder thread (clipLoop): the wanted clip first, then any others to have ready. */
    private final Object clipLock = new Object();
    private Thread clipThread;
    private volatile int clipSize = 232;
    private volatile boolean clipRound = true;
    private final java.util.Set<String> failedClips = java.util.concurrent.ConcurrentHashMap.newKeySet();
    /* Why a clip failed, by name, for its row. */
    private final java.util.Map<String, String> failReasons = new java.util.concurrent.ConcurrentHashMap<>();
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
    private long lastStatMs, lastBytes, lastCamEnc, lastEncUs, lastConv, lastGate;
    private int lastFed, lastEnc, lastShown;
    private volatile int curFps;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setIntent(benchOnly(getIntent()));
        readExtras(getIntent());
        /* Edge to edge, dark: AppScreen pads the app bar and tab bar by the system bars. */
        getWindow().setDecorFitsSystemWindows(false);
        getWindow().setStatusBarColor(Color.TRANSPARENT);
        getWindow().setNavigationBarColor(Ui.NAV);
        android.content.SharedPreferences up = getSharedPreferences(PREFS, MODE_PRIVATE);
        if (!getIntent().hasExtra("front")) {
            front = up.getBoolean(PREF_FRONT, false);
        }
        screen = new AppScreen(this);
        screen.overlay.setMode(up.getInt(PREF_OVERLAY, StatsOverlay.COMPACT));
        preview = screen.preview;
        setContentView(screen.root);
        String tab = getIntent().getStringExtra("tab");   /* --es tab gadget: open on that tab */
        if (tab != null && !tab.isEmpty()) {
            screen.select(screen.tabIndex(Character.toUpperCase(tab.charAt(0)) + tab.substring(1).toLowerCase(java.util.Locale.US)));
        }
        preview.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            @Override
            public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) {
                st.setDefaultBufferSize(640, 480);
                previewTexture = st;
                previewSurface = new Surface(st);
            }

            @Override
            public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) {}

            @Override
            public boolean onSurfaceTextureDestroyed(SurfaceTexture st) {
                previewDecoder.stop();
                previewSurface = null;
                previewTexture = null;
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
                connStatus = "Needs camera permission";
                refreshUi();
                return;
            }
        }
        begin();
    }

    /*
     * The extras (gadget address, source, auto start, ...) are for testing
     * from adb: the activity is exported, so in a build that isn't debuggable
     * any app could otherwise point the camera at a server of its choosing.
     */
    private Intent benchOnly(Intent i) {
        if (i == null || (getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            return i;
        }
        if (i.getExtras() != null) {
            Log.w(TAG, "launch extras ignored: not a debuggable build");
        }
        return new Intent(i).replaceExtras((Bundle) null);
    }

    /* `am start ... --es pick <what>` on the running app presses a button (what: as in pick()). */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        intent = benchOnly(intent);
        String what = intent.getStringExtra("pick");
        if (what != null) {
            pick(what);
        }
    }

    /* What the lists and adb's "pick" do: switch what streams, or stop. */
    void pick(String what) {
        if (link == null) {
            return;
        }
        Log.i(TAG, "pick " + what);
        if ("device".equals(what)) {
            showPicker();
            return;
        }
        if ("rate".equals(what)) {
            screen.show("Video", "Encoding");
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
        refreshUi();
        if (!wantVp8Set) {
            wantVp8 = rp.getBoolean(PREF_CODEC, false);
        }
        refreshUi();
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
        previewDecoder.stop();
        synchronized (clipLock) {
            if (clipThread != null) {
                clipThread.interrupt();   /* else it outlives the activity, encoding for nobody */
            }
        }
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
                    ? new java.io.File(getExternalFilesDir(null), new java.io.File(source.substring(5)).getName()).getPath()
                    : null;   /* a name in the app's files, never a path out of them */
            /* The camera draws its own preview; the bars (or a file) are shown as sent, decoded. */
            if ("camera".equals(source)) {
                previewDecoder.stop();
                if (previewTexture != null) {
                    int[] ps = previewSize();
                    previewTexture.setDefaultBufferSize(ps[0], ps[1]);
                }
                screen.cameraTransform(true);
                cam.setPreview(previewSurface);
            } else {
                cam.setPreview(null);
                screen.cameraTransform(false);
                if (bands == 1) {
                    previewDecoder.start(previewSurface, s, camVp8);
                } else {
                    previewDecoder.stop();
                }
            }
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
    void setCodec(boolean vp8) {
        wantVp8 = vp8;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(PREF_CODEC, vp8).apply();
        refreshUi();
        Log.i(TAG, "codec: " + (vp8 ? "VP8" : "H.264"));
        if (link != null && link.streaming()) {
            String src = source;
            stopStream();
            source = src;
            auto = true;
            ui.postDelayed(this::startStream, 300);
        }
    }

    void refreshUi() {
        if (screen != null) {
            screen.refresh();
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
    int[] clipLevels() {
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
        while (!Thread.currentThread().isInterrupted()) {
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
            failReasons.put(name, e instanceof java.io.IOException ? "it needs the Internet the first time"
                    : String.valueOf(e.getMessage()));
            linkStatus = clipLabel(name) + " failed: " + e
                    + (sample != null && e instanceof java.io.IOException ? " (needs the Internet)" : "");
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
        if (!previewDecoder.running(clip.size, clip.vp8)) {
            screen.cameraTransform(false);
            previewDecoder.start(previewSurface, clip.size, clip.vp8);
        }
        player.play(clip);
        SampleClips.Sample sample = SampleClips.get(clip.name);
        linkStatus = (sample != null ? sample.credit + "\n" : "") + clip.info;
        refreshUi();
    }

    private void stopStream() {
        previewDecoder.stop();
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
        previewDecoder.feed(frame, key);
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
    void showPicker() {
        if (picker != null && picker.isShowing()) {
            return;
        }
        /* Two lines a gadget: its name, then its address and signal, smaller and muted. */
        pickerRows = new android.widget.ArrayAdapter<String>(this, android.R.layout.simple_list_item_1,
                new java.util.ArrayList<>()) {
            @Override
            public android.view.View getView(int pos, android.view.View convert, android.view.ViewGroup parent) {
                android.widget.TextView t = (android.widget.TextView) super.getView(pos, convert, parent);
                String row = getItem(pos);
                int nl = row.indexOf('\n');
                android.text.SpannableString sp = new android.text.SpannableString(row);
                if (nl > 0) {
                    sp.setSpan(new android.text.style.RelativeSizeSpan(0.8f), nl, row.length(), 0);
                    sp.setSpan(new android.text.style.ForegroundColorSpan(Ui.TEXT2), nl, row.length(), 0);
                }
                t.setText(sp);
                t.setPadding(Ui.dp(MainActivity.this, 24), Ui.dp(MainActivity.this, 10), Ui.dp(MainActivity.this, 24),
                        Ui.dp(MainActivity.this, 10));
                return t;
            }
        };
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
        picker = new android.app.AlertDialog.Builder(this, Ui.DIALOG_THEME)
                .setTitle("Choose a gadget")
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
            int rssi = e.getValue().rssi;
            pickerRows.add(String.format(java.util.Locale.US, "%s%s\n%s%s", e.getValue().name,
                                         on ? "  (connected)" : "", e.getKey(), rssi != 0 ? " · " + rssi + " dBm" : ""));
        }
        if (rows.isEmpty()) {
            pickerRows.add("Looking for gadgets…");
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
        connStatus = s;
        Log.i(TAG, s);
    }

    @Override
    public void onReady(VideoLink.Caps caps) {
        linkStatus = "Connected: " + caps;
        connStatus = "Connected over " + link.kind();
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
            previewDecoder.stop();
            cam.stop();
            if (player != null) {
                player.stop();
            }
        });
    }

    @Override
    public void onStartRefused(String reason) {
        if ("closed".equals(reason)) {
            /* Phone camera isn't open on the gadget: its "want" starts the stream. */
            Log.i(TAG, "gadget refused the stream: open Phone camera on it");
            linkStatus = "Open Phone camera on the gadget";
            connStatus = linkStatus;
            return;
        }
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
            previewDecoder.stop();
            cam.stop();
            if (player != null) {
                player.stop();
            }
        });
    }

    /* ---- adaptive bitrate (camera) --------------------------------------- */

    private int maxKbps = 650, minKbps = 200;
    private boolean kbpsSet, maxKbpsSet;
    /* Camera bitrate and Clip bitrate (Video tab): a fixed bitrate for the clips
     * and for the camera (0: automatic, the adaptive bitrate / the clips' ladder). Remembered. */
    private int fixedClipKbps, fixedCamKbps;
    private static final String PREF_CLIP_KBPS = "clip_kbps", PREF_CAM_KBPS = "cam_kbps";
    static final int[] CAM_RATES = {150, 200, 250, 300, 400, 500, 650, 800, 1000, 1200};
    /* Picture sizes for gadgets that decode more than 240 px (the ESP-Mosaico). */
    static final int[] SIZES = {240, 320, 480};   /* 2x, 3/2 and 1:1 on its 480 panel */
    private static final String PREF_SIZE = "size", PREF_FPS = "fps";

    void setClipRate(int k) {
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
        refreshUi();
    }

    void setCamRate(int k) {
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
        refreshUi();
    }

    /* A new picture size restarts what streams (0: the gadget's suggestion). */
    void setFps(int f) {
        fps = f;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_FPS, f).apply();
        Log.i(TAG, "camera frame rate " + f + " fps");
        if (link != null && link.streaming() && !isClip()) {
            stopStream();
            ui.postDelayed(this::startStream, 300);
        }
        refreshUi();
    }

    void setSize(int k) {
        size = k;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_SIZE, k).apply();
        Log.i(TAG, "picture size " + (k > 0 ? k + " px" : "auto"));
        if (link != null && link.streaming()) {
            stopStream();
            ui.postDelayed(this::startStream, 300);
        }
        refreshUi();
    }

    private static String clipLabel(String name) {
        SampleClips.Sample c = SampleClips.get(name);
        return c != null ? c.label : name;
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
        double camEncMs = (camEnc - lastCamEnc) / 1000.0 / dEnc, convMs = (conv - lastConv) / 1000.0 / Math.max(1, fed - lastFed),
                encMs = (encUs - lastEncUs) / 1000.0 / dEnc, gateMs = (link.statGateUs - lastGate) / 1000.0 * 1000 / dt;
        String lat = String.format(java.util.Locale.US, "cam->enc %.0f ms (convert %.1f, encoder %.1f)  gate %.0f ms/s",
                camEncMs, convMs, encMs, gateMs);
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
        if (link.streaming() && meter != null && !isClip()) {
            Log.i(TAG, meter.take());
        }
        if (link.streaming()) {
            Log.i(TAG, "stats: " + s.replace('\n', '|'));
        }
        adaptBitrate(dt, bytes - lastBytes, (enc - lastEnc) * 1000.0 / dt, (shown - lastShown) * 1000.0 / dt);
        adaptClip();
        StatsOverlay.Stats st = new StatsOverlay.Stats();
        st.sendKbps = (bytes - lastBytes) * 8.0 / dt;
        st.gateMsPerS = gateMs;
        st.fedFps = (fed - lastFed) * 1000.0 / dt;
        st.encFps = (enc - lastEnc) * 1000.0 / dt;
        st.shownFps = (shown - lastShown) * 1000.0 / dt;
        st.camToEncMs = camEncMs;
        st.convertMs = convMs;
        st.encodeMs = encMs;
        fillStats(st);
        screen.overlay.update(st);
        refreshUi();
        lastStatMs = now;
        lastBytes = bytes;
        lastFed = fed;
        lastEnc = enc;
        lastShown = shown;
    }

    /* ---- for the screen (AppScreen, the tabs) --------------------------- */

    private static final String PREF_FRONT = "front", PREF_OVERLAY = "overlay";
    /* The link's state in a line, for the Gadget tab and the app bar. */
    private String connStatus = "";
    private int uiSkipped, uiDropped;

    /** The rest of the overlay's numbers: what's being sent, the link, the gadget. */
    private void fillStats(StatsOverlay.Stats st) {
        ClipStreamer.Clip pc = isClip() && player != null ? player.clip() : null;
        st.connected = connected();
        st.streaming = link.streaming();
        st.preparing = preparing();
        st.clip = isClip();
        st.codec = (isClip() ? pc != null && pc.vp8 : camVp8) ? "VP8" : "H.264";
        st.size = pc != null ? pc.size : lastSize;
        st.fps = curFps;
        st.bands = isClip() ? 1 : cam.bands;
        st.targetKbps = isClip() ? (pc != null ? pc.kbps : clipKbps) : kbps;
        st.fixedRate = (isClip() ? fixedClipKbps : fixedCamKbps) > 0;
        st.source = sourceLabel();
        st.encoder = isClip() ? "" : cam.encoderName.isEmpty() ? "starting" : cam.encoderName.split(" ")[0];
        st.linkName = gadgetName() != null ? gadgetName() : "";
        st.linkKind = link.kind();
        st.linkDetail = link.describe();
        st.inFlight = link.inFlight();
        st.queued = link.queuedBytes();
        st.skipped = isClip() ? 0 : cam.statSkipped;
        st.skippedNew = Math.max(0, cam.statSkipped - uiSkipped);
        uiSkipped = cam.statSkipped;
        st.keys = isClip() && player != null ? player.statKeys : cam.statKeys;
        st.dropped = devDropped;
        st.droppedNew = Math.max(0, devDropped - uiDropped);
        uiDropped = devDropped;
        st.decodeMs = devMs;
        st.free = devAvail;
        st.buffer = link.ready() ? link.caps().buffer : 0;
        SampleClips.Sample sample = pc != null ? SampleClips.get(pc.name) : null;
        st.note = sample != null ? sample.credit : "";
    }

    boolean connected() {
        return link != null && link.ready();
    }

    /** The gadget connected to, else the one being looked for (null: any). */
    String gadgetName() {
        String n = link != null ? link.connectedName() : null;
        return n != null ? n : target;
    }

    String connectionLine() {
        return connStatus.isEmpty() ? (link != null ? link.describe() : "") : connStatus;
    }

    String linkDetail() {
        return link != null ? link.describe() : "";
    }

    String deviceLine() {
        return deviceLine;
    }

    String decodeLine() {
        if (!connected()) {
            return "-";
        }
        return String.format(java.util.Locale.US, "%d dropped, %d ms", devDropped, devMs);
    }

    VideoLink.Caps caps() {
        return connected() ? link.caps() : null;
    }

    String source() {
        return source;
    }

    boolean sourceIs(String what) {
        return what.equals(source);
    }

    String sourceLabel() {
        if (isClip()) {
            return clipLabel(source.substring(CLIP.length()));
        }
        return "pattern".equals(source) ? "Color bars" : source.startsWith("file:") ? "File " + source.substring(5)
                : (front ? "Front camera" : "Camera");
    }

    /** Streaming, or a clip being encoded to stream. */
    boolean streamingOrPreparing() {
        return link != null && (link.streaming() || preparing());
    }

    /** The wanted clip isn't encoded yet. */
    boolean preparing() {
        if (!isClip() || link == null || !link.ready() || !(auto || wanted)) {
            return false;
        }
        String name = source.substring(CLIP.length());
        return bestClip(name, clipSize) == null && !failedClips.contains(clipKey(name, clipSize, clipKbps));
    }

    /** How a clip stands, after its description: "" or " · ready at 480 px", "preparing", "failed". */
    String clipState(String name) {
        if (failedClips.contains(clipKey(name, clipSize, clipKbps))) {
            String why = failReasons.get(name);
            return why != null ? "\nFailed: " + why : "\nFailed";
        }
        ClipStreamer.Clip c = bestClip(name, clipSize);
        if (c != null) {
            return String.format(java.util.Locale.US, "\nReady · %d px · %d kb/s · %d KB loop", c.size, c.kbps,
                    c.loopBytes() / 1024);
        }
        return sourceIs(CLIP + name) && preparing() ? "\nPreparing…" : "";
    }

    boolean cameraPreview() {
        return "camera".equals(source) && cam != null && cam.running();
    }

    boolean decoderShown() {
        return previewDecoder.shown();
    }

    /** A source row's tap: stream what (or switch to it). */
    void play(String what) {
        pick(what);
        refreshUi();
    }

    /* A 4:3 size for the camera's preview (upright 3:4, cropped square by AppScreen). */
    private int[] previewSize() {
        try {
            android.hardware.camera2.CameraManager cm = getSystemService(android.hardware.camera2.CameraManager.class);
            for (String id : cm.getCameraIdList()) {
                android.hardware.camera2.CameraCharacteristics ch = cm.getCameraCharacteristics(id);
                Integer facing = ch.get(android.hardware.camera2.CameraCharacteristics.LENS_FACING);
                if (facing == null || (facing == android.hardware.camera2.CameraCharacteristics.LENS_FACING_FRONT) != front) {
                    continue;
                }
                android.util.Size best = null;
                for (android.util.Size z : ch.get(android.hardware.camera2.CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                        .getOutputSizes(SurfaceTexture.class)) {
                    if (z.getWidth() * 3 == z.getHeight() * 4 && z.getHeight() >= 720
                            && (best == null || z.getHeight() < best.getHeight())) {
                        best = z;
                    }
                }
                if (best != null) {
                    return new int[] {best.getWidth(), best.getHeight()};
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "preview size", e);
        }
        return new int[] {960, 720};
    }

    boolean frontCamera() {
        return front;
    }

    /** Front or back camera; remembered, and a camera stream starts over on it. */
    void setFront(boolean f) {
        if (f == front) {
            return;
        }
        front = f;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(PREF_FRONT, f).apply();
        if (cam != null) {
            cam.setFront(f);
        }
        Log.i(TAG, "camera: " + (f ? "front" : "back"));
        if (link != null && link.streaming() && "camera".equals(source)) {
            stopStream();
            ui.postDelayed(this::startStream, 300);
        }
    }

    int fps() {
        return fps;
    }

    int size() {
        return size;
    }

    boolean wantVp8() {
        return wantVp8;
    }

    /** The Codec row's description: whether VP8 can be had. */
    String codecNote() {
        if (!Vpx.available()) {
            return "This build has no VP8 encoder (jni/build_vpx.sh)";
        }
        VideoLink.Caps c = caps();
        if (c == null) {
            return "VP8 where the gadget decodes it";
        }
        return c.vp8 ? "The gadget decodes both" : "This gadget decodes H.264 only";
    }

    int fixedCamKbps() {
        return fixedCamKbps;
    }

    int fixedClipKbps() {
        return fixedClipKbps;
    }

    int maxKbps() {
        return maxKbps;
    }

    boolean twoBands() {
        return !noBands;
    }

    /** Two bands where the gadget takes them (session only, like --ez nobands); a camera stream starts over. */
    void setTwoBands(boolean on) {
        noBands = !on;
        restartLive();
    }

    boolean x264Available() {
        return X264.available();
    }

    boolean useX264() {
        return baseSw != null;
    }

    /** x264 or the phone's encoder for H.264 (session only, like --es sw). */
    void setUseX264(boolean on) {
        baseSw = on && X264.available() ? "x264" : null;
        restartLive();
    }

    boolean intraRefresh() {
        return cam != null && cam.intraRefresh;
    }

    void setIntraRefresh(boolean on) {
        if (cam != null) {
            cam.intraRefresh = on;
        }
        restartLive();
    }

    boolean panelFilter() {
        return cam != null && cam.panelFilter;
    }

    void setPanelFilter(boolean on) {
        if (cam != null) {
            cam.panelFilter = on;
        }
        restartLive();
    }

    /* A live stream (the camera, the bars) starts over with new settings. */
    private void restartLive() {
        if (link != null && link.streaming() && !isClip()) {
            stopStream();
            ui.postDelayed(this::startStream, 300);
        }
        refreshUi();
    }

    boolean autoStart() {
        return auto;
    }

    void setAutoStart(boolean on) {
        auto = on;
        if (on && link != null && link.ready() && !link.streaming()) {
            startStream();
        }
    }

    int overlayMode() {
        return screen != null ? screen.overlay.mode() : StatsOverlay.COMPACT;
    }

    void setOverlayMode(int m) {
        screen.overlay.setMode(m);
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_OVERLAY, m).apply();
        refreshUi();
    }

    String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            return "";
        }
    }
}
