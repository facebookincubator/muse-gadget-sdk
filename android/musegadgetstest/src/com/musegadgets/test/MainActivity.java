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
 *
 * Over musegadgets BLE instead (GadgetLink, mgcommands.h): --es link ble, or
 * the Link button; the choice is kept. BLE-only bench options: --es device
 * <address>, --ez tone true (an SBC loop beside the video; --es tonecodec
 * lc3), --es mic sbc|lc3 (capture the gadget's mic too), --ei bench SECS
 * (raw throughput), --ez l2cap true, --ei air BYTES, --ei wmax BYTES.
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
    /* UI streams (protocols/UI.md): a web page or the screen at the gadget's own size. */
    private UiSession uiSession;

    UiSession ui() {
        return uiSession;
    }

    /* UI stream extras: --es uiurl URL, --ez fisheye, --ez refine, --ei uicss, --ez uidry (no gadget: files/ui_dump.264),
     * --ei uisize (dry size), --ei hid 0-3, --ef wx/wy/ws (the mirror window), --ei qp/qr1/qr2 (the QP ladder). */
    private Bundle uiExtras;

    private void applyUiExtras() {
        Bundle e = uiExtras;
        if (e == null || uiSession == null) {
            return;
        }
        if (e.containsKey("uiurl")) uiSession.url = e.getString("uiurl");
        if (e.containsKey("fisheye")) uiSession.fisheye = e.getBoolean("fisheye");
        if (e.containsKey("refine")) uiSession.refine = e.getBoolean("refine");
        if (e.containsKey("sharp")) uiSession.sharp = e.getBoolean("sharp");
        if (e.containsKey("overscroll")) {
            int i = java.util.Arrays.asList(UiSession.OVERSCROLL_NAMES).indexOf(e.getString("overscroll"));
            if (i >= 0) uiSession.overscroll = i;
        }
        if (e.containsKey("uicss")) uiSession.cssPx = e.getInt("uicss");
        if (e.containsKey("uifast")) uiSession.fastFps = e.getInt("uifast");
        if (e.containsKey("uilegacy")) uiSession.legacyPacing = e.getBoolean("uilegacy");
        if (e.containsKey("hid")) uiSession.hidMode = e.getInt("hid");
        if (e.containsKey("wx")) uiSession.winX = e.getFloat("wx");
        if (e.containsKey("wy")) uiSession.winY = e.getFloat("wy");
        if (e.containsKey("ws")) uiSession.winSize = e.getFloat("ws");
        if (e.containsKey("wx") || e.containsKey("wy") || e.containsKey("ws")) {
            uiSession.setWindow(uiSession.winX, uiSession.winY, uiSession.winSize);   /* kept on the screen */
        }
        uiSession.dry = e.getBoolean("uidry", false);
        uiSession.drySize = e.getInt("uisize", 360);
        uiSession.qp = e.getInt("qp", 0);
        uiSession.qr1 = e.getInt("qr1", 0);
        uiSession.qr2 = e.getInt("qr2", 0);
        uiSession.twoBands = e.getBoolean("uibands2", false);
        uiSession.save();
    }
    /* What to connect to: --es target <name part> (first match), --es host
     * <address[:port]>, else the gadget last picked on the Gadget tab. */
    private String target, deviceAddr;
    private static final String PREFS = "musegadgetstest", PREF_LINK = "link";
    /* BLE (GadgetLink) instead of Wi-Fi: --es link ble|wifi, or the Link button. */
    private boolean ble;
    private String prefAddr() { return ble ? "device_addr" : "wifi_addr"; }
    private String prefOff() { return ble ? "gadget_off" : "wifi_gadget_off"; }
    /* Disconnect (the header's button, the Gadget tab): the user let the gadget go. No link,
     * no scanning and no reconnecting (after a drop, coming back to the foreground or a
     * restart) until they pick a gadget or press Connect. The last gadget stays remembered,
     * for Connect. Remembered across restarts. */
    private boolean gadgetOff, linkStarted;
    private String lastAddr, lastName;
    private boolean explicitTarget;   /* the launch named a gadget (--es target/host/device) */
    private String prefName() { return ble ? "device_name" : "wifi_name"; }
    /** The link as a GadgetLink, or null over Wi-Fi. */
    private GadgetLink bleLink() { return link instanceof GadgetLink ? (GadgetLink) link : null; }

    /* BLE bench options (see the class comment). */
    private int air = 3072, benchSecs;
    private boolean airSet, l2cap, tone;
    private String mic, toneCodec;
    private long lastDataIn, lastAudio;
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
    /* The user's own start (a pick, a codec change, Start automatically turned on), for one
     * connection; a stream that starts on its own on another waits for the token proof. UI thread. */
    private final VideoLink.StartGrant grant = new VideoLink.StartGrant();

    private boolean userStart() {
        return grant.held();
    }
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
    private volatile boolean wanted;   /* the gadget's ask on this connection; written on the UI thread */
    private String linkStatus = "", deviceLine = "";
    private volatile long devAvail, devRecv;
    private volatile int devShown, devDropped, devMs;
    private LatencyMeter meter;
    private boolean benchDump, latLog, live = true;
    private int aheadPct = 250;   /* --ei ahead: live budget, % of a frame */
    private int behindPct = 100;  /* --ei behind: live phone-side queue before skipping, % of a frame */

    /* Live camera: keep about two and a half frames ahead of the device's decoder. */
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
        importProofKeyFile(getIntent());
        /* Edge to edge, dark: AppScreen pads the app bar and tab bar by the system bars. */
        getWindow().setDecorFitsSystemWindows(false);
        getWindow().setStatusBarColor(Color.TRANSPARENT);
        getWindow().setNavigationBarColor(Ui.NAV);
        android.content.SharedPreferences up = getSharedPreferences(PREFS, MODE_PRIVATE);
        if (!getIntent().hasExtra("front")) {
            front = up.getBoolean(PREF_FRONT, false);
        }
        uiSession = new UiSession(this);
        uiSession.onStopRequested = () -> pick("stop");
        applyUiExtras();
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
        String[] perms = {Manifest.permission.CAMERA};   /* Nearby devices: when the BLE link starts */
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
            explicitTarget = true;
        }
        if (i.hasExtra("link")) {
            ble = "ble".equals(i.getStringExtra("link"));
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(PREF_LINK, ble ? "ble" : "wifi").apply();
        }
        if (i.hasExtra("host")) deviceAddr = i.getStringExtra("host");
        if (i.hasExtra("device")) deviceAddr = i.getStringExtra("device");
        explicitTarget |= i.hasExtra("host") || i.hasExtra("device");
        mic = i.getStringExtra("mic");
        airSet = i.hasExtra("air");
        air = i.getIntExtra("air", air);
        benchSecs = i.getIntExtra("bench", 0);
        l2cap = i.getBooleanExtra("l2cap", false);
        tone = i.getBooleanExtra("tone", false);
        toneCodec = i.getStringExtra("tonecodec");   /* sbc (default) or lc3 */
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
        uiExtras = i.getExtras() != null ? new Bundle(i.getExtras()) : null;
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
        boolean all = res.length > 0;
        for (int r : res) {
            all &= r == PackageManager.PERMISSION_GRANTED;
        }
        if (code == REQ_BLUETOOTH) {
            if (all && ble && linkStarted && link != null) {
                link.close();   /* its scan stopped at the missing permission: start over */
                makeLink();
                startLink();
                if (picker != null && picker.isShowing()) {
                    link.scanForPicker();
                }
            }
            refreshUi();
            return;
        }
        if (!all) {
            connStatus = "Needs camera permission";
            refreshUi();
            return;
        }
        begin();
    }

    private static final int REQ_BLUETOOTH = 2;

    /*
     * The extras (gadget address, source, auto start, ...) are for testing
     * from adb: the activity is exported, so in a build that isn't debuggable
     * any app could otherwise point the camera at a server of its choosing.
     */
    private Intent benchOnly(Intent i) {
        if (i == null) {
            return null;
        }
        if ((getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) == 0) {
            if (i.getExtras() != null) {
                Log.w(TAG, "launch extras ignored: not a debuggable build");
            }
            return new Intent(i).replaceExtras((Bundle) null);
        }
        if (i.getExtras() == null || benchMarker()) {
            return i;
        }
        Intent o = null;
        for (String k : MARKED_HOOKS) {
            if (i.hasExtra(k)) {
                if (o == null) {
                    o = new Intent(i);
                }
                o.removeExtra(k);
                Log.w(TAG, "--e " + k + " ignored: no files/" + BENCH_MARKER + " (adb shell run-as " + getPackageName()
                        + " touch files/" + BENCH_MARKER + ")");
            }
        }
        return o != null ? o : i;
    }

    /*
     * Debug builds are exported to every app on the phone, so the hooks that
     * record, read a file, load a URL, pick the gadget or link, or change what
     * streams, plays or starts on its own act only while files/bench exists in
     * this app's private storage: adb can create it (run-as), other apps can't.
     */
    private static final String BENCH_MARKER = "bench";

    private boolean benchMarker() {
        return new java.io.File(getFilesDir(), BENCH_MARKER).exists();
    }
    private static final String[] MARKED_HOOKS = {
        "rec_secs", "proofkeyfile", "proofgadget", "uiurl", "volume", "spkvol", "play_wav", "pick",
        "link", "host", "device", "target", "source", "clip", "pattern", "auto", "mic", "tone", "swparams",
        "dump", "front", "wx", "wy", "ws", "hid", "sw", "swpreset", "venc", "bench", "uidry",
        "saveclips", "benchdump", "latlog",
    };

    /* `am start ... --es pick <what>` on the running app presses a button (what: as in pick()). */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        intent = benchOnly(intent);
        if (intent.getExtras() != null) {
            uiExtras = new Bundle(intent.getExtras());
            applyUiExtras();
        }
        String what = intent.getStringExtra("pick");
        if (what != null) {
            pick(what);
        }
        /* Test hooks act only on a fresh `am start`, never on an intent Android replays
         * (from recents or a restored task): a replayed hook once acted again. */
        boolean replayed = (intent.getFlags() & Intent.FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY) != 0;
        if (replayed) {
            return;
        }
        importProofKeyFile(intent);
        if (intent.hasExtra("volume")) {
            setSpeakerVolume(intent.getIntExtra("volume", 0));   /* the gadget's speaker, 0-100 */
        }
        String pw = intent.getStringExtra("play_wav");   /* bench: speak a WAV from the phone's speaker (into a gadget's mic) */
        if (pw != null) {
            playWavOnPhone(new java.io.File(getFilesDir(), new java.io.File(pw).getName()));
        }
        if (intent.hasExtra("rec_secs")) {
            recordMic(intent.getIntExtra("rec_secs", 5));   /* measuring a gadget's speaker */
        }
    }

    /*
     * --es proofkeyfile <name> [--es proofgadget <advertised name>]: a gadget's
     * token proof key (mg_ble_client's proof_key) from a file in the app's
     * files/, read once and deleted, so the key is never on a command line.
     * Without proofgadget it goes with the gadget connected over BLE now.
     */
    private void importProofKeyFile(Intent i) {
        String f = i != null ? i.getStringExtra("proofkeyfile") : null;
        if (f == null) {
            return;
        }
        java.io.File file = new java.io.File(getFilesDir(), new java.io.File(f).getName());
        String gadget = i.hasExtra("proofgadget") ? i.getStringExtra("proofgadget") : proofGadget();
        try {
            String hex = new String(java.nio.file.Files.readAllBytes(file.toPath()), java.nio.charset.StandardCharsets.US_ASCII);
            if (gadget == null || !setProofKey(gadget, hex)) {
                Log.w(TAG, "proof key file " + file.getName() + ": " + (gadget == null
                        ? "no gadget (connect over BLE, or give --es proofgadget)" : "not a key") + "; nothing saved");
            }
        } catch (java.io.IOException e) {
            Log.w(TAG, "proof key file " + file.getName() + ": can't read it");
        }
        if (file.exists() && !file.delete()) {
            Log.w(TAG, "proof key file " + file.getName() + ": couldn't delete it");
        }
    }

    /** The gadget a proof key set now goes with: the one connected over BLE (its advertised name), else null. */
    String proofGadget() {
        GadgetLink g = bleLink();
        return g != null ? g.gadgetName() : null;
    }

    /** Sets (or with "" removes) a gadget's token proof key; false if it isn't one. The connected gadget proves it again. */
    boolean setProofKey(String gadget, String hex) {
        if (!ProofKeys.set(this, gadget, hex)) {
            return false;
        }
        GadgetLink g = bleLink();
        if (g != null && gadget.equals(g.gadgetName())) {
            if (uiSession != null && uiSession.active() && UiSession.MIRROR.equals(uiSession.source())) {
                pick("stop");   /* the screen goes only to a gadget proved with the key kept now */
            }
            g.proofKeyChanged();
        }
        refreshUi();
        return true;
    }

    /** Where the connected gadget's token proof stands, for the UI tab. */
    String proofLine() {
        GadgetLink g = bleLink();
        if (g == null) {
            return "Screen mirror needs the BLE link";
        }
        return g.proofLine();
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
        if ("disconnect".equals(what)) {
            disconnectGadget();
            return;
        }
        if ("connect".equals(what)) {
            connectLast();
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
            grant.grant();
            startClip(lastSize, link.caps());
            return;
        }
        if ("tone".equals(what)) {
            toggleTone();
            return;
        }
        if ("flushmic".equals(what)) {
            flushMic();
            return;
        }
        if ("link".equals(what)) {
            switchLink();
            return;
        }
        if ("uisnap".equals(what)) {
            uiSession.snapshot(getExternalFilesDir(null));   /* bench: files/ui_snap_*.png, ui_snap_sent.yuv */
            return;
        }
        if ("uimenu".equals(what)) {
            uiSession.setMenu(!uiSession.menuOpen());   /* bench: as the gadget's menu button */
            return;
        }
        if (link.streaming() || uiSession.active()) {
            stopStream();
        }
        if ("stop".equals(what)) {
            auto = false;
            grant.revoke();
            return;
        }
        source = what;
        auto = true;
        grant.grant();
        if (UiSession.isUi(what) && uiSession.dry) {
            ui.postDelayed(this::startStream, 300);
            return;
        }
        if (link.ready()) {
            startStreamSoon();
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
            wantVp8 = getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(PREF_CODEC, false);
        }
        refreshUi();
        android.content.SharedPreferences lp = getSharedPreferences(PREFS, MODE_PRIVATE);
        if (getIntent() == null || !getIntent().hasExtra("link")) {
            ble = "ble".equals(lp.getString(PREF_LINK, "wifi"));
        }
        if (target == null && deviceAddr == null) {
            deviceAddr = lp.getString(prefAddr(), null);
            target = lp.getString(prefName(), null);
        }
        lastAddr = deviceAddr;
        lastName = target;
        gadgetOff = !explicitTarget && lp.getBoolean(prefOff(), false);
        if (explicitTarget) {
            lp.edit().putBoolean(prefOff(), false).apply();
        }
        if (gadgetOff) {
            grant.close();   /* nothing starts until a gadget is picked */
            Log.i(TAG, "disconnected by the user last time: not connecting to " + lastName + " until a gadget is picked");
            deviceAddr = null;
            target = null;
            connStatus = "Not connected";
        }
        /* The meter before the link: makeLink() hands it to the link, and a
         * link made without one never reports latency. */
        meter = new LatencyMeter();
        meter.perFrame = latLog;
        makeLink();
        if (getIntent() != null && getIntent().getBooleanExtra("auto", false)) {
            grant.grant();   /* a bench's --ez auto true (it needs files/bench): the first connection */
        }
        cam = new CameraStreamer(this, front, this);
        cam.meter = meter;
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
        if (!gadgetOff) {
            startLink();
        }
        if (auto && UiSession.isUi(source) && uiSession.dry) {
            ui.postDelayed(this::startStream, 500);
        }
    }

    private void makeLink() {
        grant.linkRemade();   /* closing a link reports no disconnect */
        wanted = false;
        if (ble) {
            GadgetLink g = new GadgetLink(this, target, this);
            g.airCap = air;
            g.writeMax = getIntent() != null ? getIntent().getIntExtra("wmax", 495) : 495;
            g.bufferOverride = getIntent() != null ? getIntent().getIntExtra("vbuf", 0) : 0;   /* bench: a small gadget's ring */
            g.fpsOverride = getIntent() != null ? getIntent().getIntExtra("vfps", 0) : 0;
            g.audioParams = getIntent() != null ? getIntent().getIntExtra("aparams", 3) : 3;   /* 2 full 251-byte LL packets (495 + 4 L2CAP + 3 ATT): 512 adds a 17-byte third packet and measured ~20% slower */
            g.useL2cap = l2cap;
            link = g;
        } else {
            link = new WifiLink(this, this);
        }
        link.setTarget(deviceAddr, target);
        link.meter = meter;
    }

    /* The Link button: BLE <-> Wi-Fi, each with the gadget last picked on it. */
    private void switchLink() {
        if (link.streaming()) {
            stopStream();
        }
        link.close();
        ble = !ble;
        android.content.SharedPreferences p = getSharedPreferences(PREFS, MODE_PRIVATE);
        p.edit().putString(PREF_LINK, ble ? "ble" : "wifi").apply();
        deviceAddr = p.getString(prefAddr(), null);
        target = p.getString(prefName(), null);
        micStarted = false;
        makeLink();
        linkStatus = ble ? "BLE" : "Wi-Fi";
        lastAddr = deviceAddr;
        lastName = target;
        gadgetOff = p.getBoolean(prefOff(), false);
        if (gadgetOff) {
            grant.close();
            deviceAddr = null;
            target = null;
            link.setTarget(null, null);
            linkStarted = false;
        } else {
            grant.reopen();
            startLink();
        }
    }

    /*
     * The gadget's link only while the app is in the foreground or streams UI: a link kept (or
     * a reconnect attempt left pending) in the background holds the gadget, which then stops
     * advertising to anything else. 30 s after the app leaves the foreground with no UI
     * stream (or after one ends there), the link closes; it opens again when the app comes back.
     */
    private boolean visible, parked;
    private final android.os.Handler mainHandler = new android.os.Handler(android.os.Looper.getMainLooper());
    private final Runnable park = () -> {
        if (visible || parked || gadgetOff || link == null) return;
        if (uiSession != null && uiSession.active()) {
            /* A UI stream (the screen mirror above all: the user went to another app to show it)
             * holds the link while it runs; look again once it may have ended. */
            mainHandler.postDelayed(this.park, 30000);
            return;
        }
        Log.i(TAG, "background, no UI stream: closing the link to the gadget");
        if (link.streaming()) stopStream();
        link.close();
        parked = true;
        refreshUi();
    };

    private void parkLater() {
        mainHandler.removeCallbacks(park);
        if (!visible && !parked) mainHandler.postDelayed(park, 30000);
    }

    @Override
    protected void onStart() {
        super.onStart();
        visible = true;
        mainHandler.removeCallbacks(park);
        if (parked && link != null && !gadgetOff) {
            parked = false;
            Log.i(TAG, "foreground: opening the link to the gadget again");
            makeLink();
            startLink();
        }
    }

    @Override
    protected void onStop() {
        visible = false;
        parkLater();
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        mainHandler.removeCallbacks(park);
        ui.removeCallbacksAndMessages(null);
        if (uiSession != null) {
            uiSession.stop();   /* its encoder, GL thread, offscreen page or screen capture */
        }
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
        if (grant.closed()) {
            return;   /* Disconnect: nothing starts until a gadget is picked */
        }
        VideoLink.Caps c = link.caps().snapshot();
        if (UiSession.isUi(source)) {
            if (!uiSession.dry && bleLink() == null) {
                linkStatus = "UI streams need the BLE link";
                return;
            }
            mirrorAfterProof = false;
            GadgetLink pg = bleLink();
            if (UiSession.MIRROR.equals(source) && !uiSession.dry && !pg.proven()) {
                /* The screen goes only to a gadget that proved this phone's key on this connection. */
                mirrorAfterProof = pg.proofPending();
                linkStatus = mirrorAfterProof ? "Screen mirror waits for the gadget's token proof" : "No screen mirror: " + pg.proofLine();
                connStatus = linkStatus;
                Log.i(TAG, "ui: screen mirror not started: " + pg.proofLine());
                refreshUi();
                return;
            }
            previewDecoder.stop();
            cam.stop();
            cam.setPreview(null);
            screen.cameraTransform(false);
            if (previewTexture != null) {
                int ps = Math.max(240, c.uiW > 0 ? c.uiW : 360);
                previewTexture.setDefaultBufferSize(ps, ps);
                uiSession.setPreview(previewSurface, ps, ps);
            }
            if (!uiSession.start(link, source)) {
                linkStatus = "UI stream failed to start (see the log)";
            }
            return;
        }
        int s = size > 0 ? size : c.sugW;
        s = Math.min(s, Math.min(c.maxW, c.maxH)) & ~7;
        lastSize = s;
        if (isClip()) {
            startClip(s, c);
            return;
        }
        if ("camera".equals(source) && bleLink() != null && grant.needsCameraAsk(auto && benchMarker())) {
            askCamera();   /* the gadget's ask starts this phone's camera only on the user's yes */
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
        /* On BLE, every frame within half the gadget's ring. Under 16 KB (a tiny-RAM gadget) the
         * phone's encoder's keyframes (5-8 times a delta frame) don't fit: x264 holds them. */
        cam.maxFrameBytes = bleLink() != null ? (int) Math.max(0, c.maxFrame() - VideoFrame.HEADER_LEN) : 0;
        cam.vbvCeilKbps = maxKbps;
        boolean hwAsked = getIntent() != null && "hw".equals(getIntent().getStringExtra("sw"));   /* --es sw hw: test the fallback */
        if (cam.swEncoder == null && bleLink() != null && c.maxFrame() < 16384 && X264.available() && !hwAsked) {
            cam.swEncoder = "x264";
            Log.i(TAG, "a " + c.buffer + " B ring: x264, frames up to " + cam.maxFrameBytes + " B");
        }
        GadgetLink g = bleLink();
        if (g != null) {
            /* Bytes in the air before the gadget's credit comes back: 3 KB holds a
             * 15 ms link to ~800 kb/s, 6 KB lets it carry ~870 (more adds only
             * latency). Smaller pictures (under 600 kb/s suggested) keep 3 KB. */
            g.airCap = airSet ? air : c.sugKbps >= 600 ? 6144 : air;
        }
        /* Past 240 px on a gadget that decodes bands in parallel (one per
         * core), send two: x264 or VP8 only (one encoder per band). */
        int bands = c.maxTiles >= 2 && s > 240 && s % 32 == 0 && cam.swEncoder != null && !noBands ? 2 : 1;
        cam.bands = bands;
        cam.bandSplitPct = c.tileSplitPct > 0 ? c.tileSplitPct : 50;
        Log.i(TAG, "starting " + s + "x" + s + " @" + f + " " + kbps + " kb/s (adaptive up to " + maxKbps + ")"
                + (bands > 1 ? ", two bands" : ""));
        link.startStreaming(s, s, f, kbps, 0, bands, camVp8);
        startMic();
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
        /* A small gadget's frame cap and frame rate make other encodes of the same clip. */
        int cap = ClipStreamer.maxFrameBytes;
        return name + "|" + size + "|" + kbps + (clipVp8() ? "|vp8" : "") + (cap > 0 && cap < 16384 ? "|max" + cap : "")
                + (ClipStreamer.maxFps > 0 && ClipStreamer.maxFps < 24 ? "|fps" + ClipStreamer.maxFps : "");
    }

    /* Clips as VP8: picked, the gadget decodes it, and the build has libvpx. */
    private boolean clipVp8() {
        return wantVp8 && link != null && link.caps().vp8 && Vpx.available();
    }

    /* The Codec button: H.264 or VP8 from now on; a camera or clip stream that runs starts over in it. */
    void setCodec(boolean vp8) {
        wantVp8 = vp8;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(PREF_CODEC, vp8).apply();
        refreshUi();
        Log.i(TAG, "codec: " + (vp8 ? "VP8" : "H.264"));
        restartRunning(VideoLink.Running.CAMERA, VideoLink.Running.CLIP);
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
        /* 180, or less for a BLE gadget suggesting under 400 */
        int floor = bleLink() != null ? Math.min(180, clipTopKbps * 45 / 100) : 180;
        for (int i = 0; i < l.length; i++) {
            l[i] = Math.max(floor, l[i]);
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
        if (exact != null && exact.fits()) {
            return exact;
        }
        for (ClipStreamer.Clip c : clips.values()) {
            if (!c.name.equals(name) || c.size != s || c.vp8 != clipVp8() || !c.fits()) {
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
        if (!link.streaming() && !grant.allows(auto || wanted, askOk())) {
            return;   /* a late encode starts only what this connection still allows; a running clip just switches */
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

    /* --es mic sbc|lc3 (BLE): also capture the gadget's mic (start_mic), saved as files/mic.<codec>. */
    private boolean micStarted, micAfterProof;
    private int spkvolAfterProof = -1;

    private void startMic() {
        GadgetLink g = bleLink();
        if (g != null && mic != null && !micStarted && g.proofPending()) {
            micAfterProof = true;
            Log.i(TAG, "mic capture waits for the token proof");
            return;
        }
        if (g != null && mic != null && !micStarted) {
            micStarted = true;
            int codec = "lc3".equals(mic) ? 19 : 1;
            try {
                g.micOut = new java.io.BufferedOutputStream(new java.io.FileOutputStream(
                        new java.io.File(getExternalFilesDir(null), "mic." + mic)));
            } catch (java.io.IOException e) {
                Log.w(TAG, "mic file", e);
            }
            g.sendControl(new byte[] {(byte) Mg.CMD_START_MIC, (byte) codec});
            Log.i(TAG, "mic capture on, codec " + mic);
        }
    }

    private void flushMic() {
        GadgetLink g = bleLink();
        java.io.OutputStream mo = g == null ? null : g.micOut;
        if (mo != null) {
            try {
                mo.flush();
            } catch (java.io.IOException e) {
                Log.w(TAG, "mic file", e);
            }
        }
    }

    /* An SBC tone loop played on the gadget beside the video, over BLE (its
     * speaker volume decides whether it's heard). */
    private void toggleTone() {
        GadgetLink g = bleLink();
        if (g == null) {
            return;
        }
        if (g.audioOn()) {
            g.stopAudio();
            return;
        }
        boolean lc3 = "lc3".equals(toneCodec);
        try (java.io.InputStream in = getAssets().open(lc3 ? "tone.lc3" : "tone.sbc")) {
            g.startAudio(in.readAllBytes(), lc3 ? 40 : 60, lc3);
        } catch (java.io.IOException e) {
            Log.e(TAG, "tone asset", e);
        }
    }

    private void stopStream() {
        uiSession.stop();
        previewDecoder.stop();
        GadgetLink g = bleLink();
        if (g != null) {
            g.stopAudio();
        }
        flushMic();
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
    /* The picker's rows in a fixed order while it's open: the connected (or last
     * used) gadget first, then those already seen by name, then each new one at
     * the end. Signal and name update a row in place; a gadget that stops
     * advertising stays, greyed. Only Rescan rebuilds the order. */
    private final java.util.ArrayList<String> pickerOrder = new java.util.ArrayList<>();
    private final java.util.ArrayList<Boolean> pickerGrey = new java.util.ArrayList<>();
    private static final long PICKER_STALE_MS = 15000;
    private final Runnable pickerTick = new Runnable() {
        @Override
        public void run() {
            if (picker != null) {
                refreshPicker();
                ui.postDelayed(this, 2000);
            }
        }
    };

    private void startLink() {
        if (ble && !GadgetLink.permitted(this)) {
            requestPermissions(GadgetLink.PERMISSIONS, REQ_BLUETOOTH);
        }
        linkStarted = true;
        link.start();
    }

    boolean gadgetOff() {
        return gadgetOff;
    }

    /** The gadget Connect goes back to after a Disconnect (null: none remembered). */
    String lastGadget() {
        return lastName != null ? lastName : lastAddr;
    }

    /**
     * Disconnect: ends what's running on the gadget cleanly (a UI or video
     * stream, the mic), then closes the link and stays off (gadgetOff). Doesn't
     * unpair or forget: Connect, or picking the gadget again, comes back to it.
     */
    void disconnectGadget() {
        if (gadgetOff || link == null) {
            return;
        }
        /* First: nothing from before the Disconnect (the gadget's ask during the close's grace, a retry,
         * the camera's yes, a proof ending) starts anything again. */
        grant.close();
        wanted = false;
        startAfterProof = -1;
        askAfterProof = false;
        mirrorAfterProof = false;
        if (cameraAsk != null) {
            android.app.AlertDialog a = cameraAsk;
            cameraAsk = null;
            a.dismiss();
        }
        String name = gadgetName();
        Log.i(TAG, "disconnect " + name + " (the user's choice): no reconnecting until a gadget is picked");
        if (link.streaming() || uiSession.active()) {
            stopStream();     /* UI stream stop, stop_streaming */
        }
        GadgetLink g = bleLink();
        if (g != null && micStarted) {
            g.sendControl(new byte[] {(byte) Mg.CMD_STOP_MIC});
            micStarted = false;
        }
        gadgetOff = true;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(prefOff(), true).apply();
        mainHandler.removeCallbacks(park);
        parked = false;
        lastAddr = link.connectedAddress() != null ? link.connectedAddress() : deviceAddr;
        Seen seenAs = lastAddr != null ? seen.get(lastAddr) : null;
        lastName = seenAs != null && seenAs.name != null ? seenAs.name : name;
        if (lastAddr != null || lastName != null) {
            /* Connect comes back to this one, after a restart too. */
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(prefAddr(), lastAddr)
                    .putString(prefName(), lastName).apply();
        }
        deviceAddr = null;
        target = null;
        connStatus = "Disconnecting from " + name + "…";
        refreshUi();
        VideoLink old = link;
        /* A moment for the stop writes to go out before the link closes. */
        ui.postDelayed(() -> {
            old.close();
            if (link == old && gadgetOff) {
                makeLink();          /* no gadget, not started: nothing scans or connects */
                linkStarted = false;
                connStatus = "Not connected";
            }
            Log.i(TAG, "disconnected from " + name + " (stays off until a gadget is picked)");
            refreshUi();
        }, 600);
    }

    /** Connect after a Disconnect: the last gadget, or the picker when there's none. */
    void connectLast() {
        if (!gadgetOff) {
            return;
        }
        if (lastAddr == null && lastName == null) {
            showPicker();
            return;
        }
        gadgetOff = false;
        grant.reopen();
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(prefOff(), false).apply();
        deviceAddr = lastAddr;
        target = lastName;
        Log.i(TAG, "connect to " + lastGadget() + " again");
        link.close();
        makeLink();
        startLink();
        refreshUi();
    }

    /** Forget: Disconnect, unpair it from this phone if it's paired, and forget it here. */
    void forgetGadget() {
        String name = gadgetOff ? lastGadget() : gadgetName();
        if (name == null) {
            return;
        }
        new android.app.AlertDialog.Builder(this, Ui.DIALOG_THEME)
                .setTitle("Forget " + name + "?")
                .setMessage("The app disconnects from it and forgets it, its proof key included. If it's paired with "
                        + "this phone (for touch input), that pairing is removed too. To use it again, choose it from "
                        + "the list.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Forget", (d, w) -> {
                    String addr = link.connectedAddress() != null ? link.connectedAddress() : lastAddr;
                    disconnectGadget();
                    ui.postDelayed(() -> {
                        boolean unpaired = unpair(addr);
                        lastAddr = null;
                        lastName = null;
                        getSharedPreferences(PREFS, MODE_PRIVATE).edit().remove(prefAddr()).remove(prefName()).apply();
                        if (ProofKeys.fingerprint(this, name) != null) {
                            ProofKeys.set(this, name, "");   /* set up again, it needs its new key */
                        }
                        Log.i(TAG, "forgot " + name + (unpaired ? ", and unpaired it" : ""));
                        connStatus = "Not connected";
                        refreshUi();
                    }, 800);
                })
                .show();
    }

    /** Removes this phone's pairing with addr, if any; false when there's none or it can't. */
    private boolean unpair(String addr) {
        if (!ble || addr == null) {
            return false;
        }
        try {
            android.bluetooth.BluetoothAdapter a = getSystemService(android.bluetooth.BluetoothManager.class).getAdapter();
            android.bluetooth.BluetoothDevice d = a.getRemoteDevice(addr);
            if (d.getBondState() == android.bluetooth.BluetoothDevice.BOND_NONE) {
                return false;
            }
            Object ok = d.getClass().getMethod("removeBond").invoke(d);
            return Boolean.TRUE.equals(ok);
        } catch (ReflectiveOperationException | RuntimeException e) {
            Log.w(TAG, "couldn't unpair " + addr + ": " + e);
            android.widget.Toast.makeText(this, "Remove it in Bluetooth settings too: it's still paired",
                    android.widget.Toast.LENGTH_LONG).show();
            return false;
        }
    }

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
                if (pos < pickerGrey.size() && pickerGrey.get(pos)) {
                    sp.setSpan(new android.text.style.ForegroundColorSpan(Ui.TEXT2), 0, row.length(), 0);
                }
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
            String name = s != null ? s.name : addr.equalsIgnoreCase(lastAddr) && lastName != null ? lastName : addr;
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(prefAddr(), addr).putString(prefName(), name)
                    .putBoolean(prefOff(), false).apply();
            deviceAddr = addr;
            target = name;
            lastAddr = addr;
            lastName = name;
            gadgetOff = false;
            grant.reopen();
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
                .setNeutralButton("Rescan", null)
                .create();
        picker.setOnShowListener(d -> picker.getButton(android.app.AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
            /* The one way to reorder: forget what was seen, start the list over. */
            String keep = pinnedPickerAddr();
            seen.keySet().removeIf(a -> !a.equalsIgnoreCase(keep));
            buildPickerOrder();
            refreshPicker();
            link.scanForPicker();
        }));
        picker.setOnDismissListener(d -> {
            link.pickerClosed();
            picker = null;
            ui.removeCallbacks(pickerTick);
            if (gadgetOff) {
                /* Closed without a choice after a Disconnect: stop the picker's scan, stay off. */
                link.close();
                makeLink();
                linkStarted = false;
                connStatus = "Not connected";
                refreshUi();
            }
        });
        picker.show();
        buildPickerOrder();
        refreshPicker();
        ui.removeCallbacks(pickerTick);
        ui.postDelayed(pickerTick, 2000);
        if (!linkStarted) {
            startLink();   /* after a Disconnect: a link with no gadget, for the picker's scan */
        }
        link.scanForPicker();
    }

    /* The connected gadget, else the last one used (null: none): pinned at the top from the start. */
    private String pinnedPickerAddr() {
        String cur = link.connectedAddress();
        return cur != null ? cur : lastAddr;
    }

    /* The order at opening (or Rescan): the pinned gadget, then those seen lately by name. */
    private void buildPickerOrder() {
        pickerOrder.clear();
        String pin = pinnedPickerAddr();
        if (pin != null) {
            pickerOrder.add(pin);   /* from the start, seen yet or not, so it never pushes the rows down later */
        }
        long now = SystemClock.elapsedRealtime();
        java.util.List<java.util.Map.Entry<String, Seen>> rows = new java.util.ArrayList<>();
        for (java.util.Map.Entry<String, Seen> e : seen.entrySet()) {
            if (now - e.getValue().at < PICKER_STALE_MS && !e.getKey().equalsIgnoreCase(pin)) {
                rows.add(e);
            }
        }
        rows.sort((a, b) -> {
            int c = String.valueOf(a.getValue().name).compareToIgnoreCase(String.valueOf(b.getValue().name));
            return c != 0 ? c : a.getKey().compareToIgnoreCase(b.getKey());
        });
        for (java.util.Map.Entry<String, Seen> e : rows) {
            pickerOrder.add(e.getKey());
        }
    }

    /* Rows in pickerOrder; a gadget seen for the first time goes at the end. Never reorders. */
    private void refreshPicker() {
        if (picker == null) {
            return;
        }
        long now = SystemClock.elapsedRealtime();
        String cur = link.connectedAddress();
        String pin = pinnedPickerAddr();
        for (java.util.Map.Entry<String, Seen> e : seen.entrySet()) {
            if (now - e.getValue().at < PICKER_STALE_MS && !pickerOrder.contains(e.getKey())) {
                pickerOrder.add(e.getKey());
            }
        }
        pickerAddrs.clear();
        pickerGrey.clear();
        java.util.ArrayList<String> rows = new java.util.ArrayList<>();
        for (String addr : pickerOrder) {
            Seen sn = seen.get(addr);
            if (sn == null) {
                /* The pinned gadget, not heard from yet. */
                pickerAddrs.add(addr);
                pickerGrey.add(true);
                rows.add((lastName != null && addr.equalsIgnoreCase(lastAddr) ? lastName : addr) + "  (last used)\n"
                        + addr + " · looking…");
                continue;
            }
            boolean on = addr.equalsIgnoreCase(cur);
            long age = now - sn.at;
            boolean stale = !on && age >= PICKER_STALE_MS;
            String tag = on ? "  (connected)" : addr.equalsIgnoreCase(pin) ? "  (last used)" : "";
            String detail = stale ? " · not seen for " + age / 1000 + " s"
                    : sn.rssi != 0 ? " · " + sn.rssi + " dBm" : "";
            pickerAddrs.add(addr);
            pickerGrey.add(stale);
            rows.add(String.format(java.util.Locale.US, "%s%s\n%s%s", sn.name, tag, addr, detail));
        }
        if (rows.isEmpty()) {
            rows.add("Looking for gadgets…");
        }
        /* In place: same rows, new text; the list keeps its scroll and the rows their places. */
        pickerRows.setNotifyOnChange(false);
        pickerRows.clear();
        pickerRows.addAll(rows);
        pickerRows.notifyDataSetChanged();
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
        VideoLink from = link;
        linkStatus = "Connected: " + caps;
        connStatus = "Connected over " + link.kind();
        clipSize = Math.min(size > 0 ? size : caps.sugW, Math.min(caps.maxW, caps.maxH)) & ~7;
        clipRound = caps.round;
        ClipStreamer.maxFrameBytes = bleLink() != null ? (int) Math.max(0, caps.maxFrame() - VideoFrame.HEADER_LEN) : 0;
        ClipStreamer.maxFps = bleLink() != null ? caps.maxFps : 0;
        /* A gadget suggesting a high bitrate (a big picture, or Wi-Fi) gets
         * clips aimed at 92% of it, peaks held under the link
         * (ClipStreamer.encodeLoopsX264), and a 300 ms playout delay for
         * the peaks' queue. */
        if (!clipKbpsSet) {
            /* Below 450 over BLE (a small gadget's link or decoder): its own suggestion. */
            clipTopKbps = caps.sugKbps >= 600 ? caps.sugKbps * 92 / 100
                    : bleLink() != null && caps.sugKbps > 0 && caps.sugKbps < 450 ? caps.sugKbps : 450;
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
        GadgetLink g = bleLink();
        if (g != null) {
            /* --ei spkvol N: the gadget's speaker_volume set on connect, before any playback (benches). */
            int sv = getIntent() != null ? getIntent().getIntExtra("spkvol", -1) : -1;
            spkvolAfterProof = -1;
            micAfterProof = false;
            if (sv >= 0 && sv <= 100 && g.proofPending()) {
                spkvolAfterProof = sv;   /* a set-up gadget takes the setting only after the proof */
            } else if (sv >= 0 && sv <= 100) {
                g.setSpeakerVolume(sv);
                Log.i(TAG, "speaker_volume " + sv + " sent (--ei spkvol)");
            }
            g.askSpeakerVolume();
        }
        if (g != null && tone && !g.audioOn()) {
            ui.postDelayed(() -> {
                startMic();
                toggleTone();
            }, 1500);
        }
        if (g != null && benchSecs > 0) {
            g.benchReceive(benchSecs);
            return;
        }
        ui.post(() -> {
            if (from != link) {
                return;   /* a link already replaced */
            }
            if ((userStart() || (auto || wanted) && askAllowed()) && !link.streaming()) {
                startStream();
            }
        });
    }

    /* A screen mirror asked for before the token proof finished: it starts on a match. */
    private boolean mirrorAfterProof;
    /* A start the gadget refused until the proof (the connection's epoch, -1: none): asked again on a match. */
    private int startAfterProof = -1;
    /* The gadget's own ask to stream, before the token proof finished: it starts on a match. */
    private boolean askAfterProof;

    /*
     * Whether the gadget's ask, or Start automatically on a new connection, may
     * start a stream (the user's own start on this connection needs none): only
     * after this connection's token proof matched; a gadget without a proof key,
     * or any gadget over Wi-Fi (nothing proves it there), streams only on a tap.
     * A proof still running holds the ask until it ends. On the UI thread.
     */
    private boolean askOk() {
        return link != null && link.proven();
    }

    private boolean askAllowed() {
        GadgetLink g = bleLink();
        if (askOk()) {
            askAfterProof = false;
            return true;
        }
        if (g == null) {
            askAfterProof = false;
            linkStatus = "Tap a source to stream: over Wi-Fi the gadget proves nothing, so only your own start streams";
            Log.i(TAG, "gadget's ask held: Wi-Fi");
            refreshUi();
            return false;
        }
        askAfterProof = g.proofPending();
        linkStatus = askAfterProof ? "The gadget's stream waits for its token proof"
                : g.hasProofKey() ? "Not streaming on the gadget's ask: " + g.proofLine()
                : "Tap a source to stream: no proof key for this gadget";
        Log.i(TAG, "gadget's ask held: " + g.proofLine());
        refreshUi();
        return false;
    }

    @Override
    public void onProof(TokenProof.Outcome o) {
        ui.post(() -> {
            GadgetLink g = bleLink();
            if (g == null) {
                return;
            }
            if (spkvolAfterProof >= 0) {
                g.setSpeakerVolume(spkvolAfterProof);
                Log.i(TAG, "speaker_volume " + spkvolAfterProof + " sent after the token proof (--ei spkvol)");
                spkvolAfterProof = -1;
            }
            if (micAfterProof) {
                micAfterProof = false;
                startMic();
            }
            if (askAfterProof) {
                askAfterProof = false;
                if (g.proven() && (wanted || auto) && !link.streaming() && !uiSession.active()) {
                    startStream();
                } else if (!g.proven()) {
                    linkStatus = "Not streaming on the gadget's ask: " + g.proofLine();
                }
            }
            if (startAfterProof >= 0) {
                int ep = startAfterProof;
                startAfterProof = -1;
                if (g.proven()) {
                    askAgain(ep);
                } else if (ep == grant.epoch()) {
                    refused("The gadget refused the stream: " + g.proofLine());
                }
            }
            if (mirrorAfterProof) {
                mirrorAfterProof = false;
                if (g.proven() && UiSession.MIRROR.equals(source) && !link.streaming() && !uiSession.active()) {
                    startStream();
                } else if (!g.proven()) {
                    linkStatus = "No screen mirror: " + g.proofLine();
                    connStatus = linkStatus;
                }
            }
            refreshUi();
        });
    }

    @Override
    public void onRefused(int cmd, int code) {
        if (cmd != Mg.CMD_START_MIC) {
            return;
        }
        ui.post(() -> {
            if (micStarted) {
                closeMic();
                linkStatus = "The gadget refused the mic" + (code == Mg.ERROR_PROOF_REQUIRED ? ": it needs the token proof"
                        : " (" + Mg.errorName(code) + ")");
                Log.w(TAG, linkStatus);
                refreshUi();
            }
        });
    }

    /*
     * The gadget's ask for the camera over BLE (or Start automatically on a new connection): the user's
     * yes holds for the rest of this connection; a no answers the gadget as a camera that can't start does
     * (stop_streaming, buffer_drop). Main thread.
     */
    private android.app.AlertDialog cameraAsk;

    private void askCamera() {
        if (cameraAsk != null) {
            return;
        }
        int ep = grant.epoch();
        String name = gadgetName() != null ? gadgetName() : "The gadget";
        Log.i(TAG, "camera: " + name + " asks to start the camera; asking the user");
        cameraAsk = new android.app.AlertDialog.Builder(this, Ui.DIALOG_THEME)
                .setTitle("Start the camera?")
                .setMessage(name + " asks to show this phone's camera. Allow it until it disconnects?")
                .setNegativeButton("Don't allow", (d, w) -> cameraAnswered(false, ep))
                .setPositiveButton("Allow", (d, w) -> cameraAnswered(true, ep))
                .setOnCancelListener(d -> cameraAnswered(false, ep))
                .show();
    }

    private void cameraAnswered(boolean yes, int ep) {
        cameraAsk = null;
        if (ep != grant.epoch()) {
            return;   /* another connection by now: it asks again */
        }
        Log.i(TAG, "camera: the user " + (yes ? "allowed" : "declined") + " the gadget's ask");
        if (yes) {
            grant.allowCamera();
            if (link.ready() && !link.streaming() && !uiSession.active() && (wanted || auto)) {
                startStream();
            }
            return;
        }
        wanted = false;
        GadgetLink g = bleLink();
        if (g != null) {
            g.sendControl(new byte[] {(byte) Mg.CMD_STREAM_VIDEO, (byte) Mg.V_STOP_STREAMING, (byte) Mg.BUFFER_DROP});
        }
        linkStatus = "Camera not started: you declined the gadget's ask";
        refreshUi();
    }

    @Override
    public void onRequestStreaming(boolean want) {
        Log.i(TAG, "gadget asks for streaming: " + want);
        ui.post(() -> {
            wanted = want;
            if (!want) {
                askAfterProof = false;
            }
            if (want && link.ready() && !link.streaming() && (userStart() || askAllowed())) {
                startStream();
            } else if (!want && link.streaming()) {
                stopStream();
            }
        });
    }

    @Override
    public void onKeyframeRequest() {
        Log.i(TAG, "keyframe requested");
        uiSession.requestKeyframe();
        cam.requestKeyframe();
        if (player != null) {
            player.requestKeyframe();
        }
    }

    @Override
    public void onStreamStopped() {
        Log.i(TAG, "gadget stopped the stream");
        ui.post(() -> {
            uiSession.stop();
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
        GadgetLink g = bleLink();
        if ("proof required".equals(reason) && g != null && g.proofPending()) {
            /* A set-up gadget takes a stream only after this connection's proof: ask again on a match. */
            Log.i(TAG, "gadget refused the stream until the token proof matches");
            ui.post(() -> {
                if (!g.proofPending()) {   /* it ended meanwhile: act on its outcome now */
                    if (g.proven()) {
                        askAgain(grant.epoch());
                    } else {
                        refused("The gadget refused the stream: " + g.proofLine());
                    }
                    return;
                }
                startAfterProof = grant.epoch();
                linkStatus = "The stream waits for the gadget's token proof";
                refreshUi();
            });
            return;
        }
        if (!"busy".equals(reason)) {
            /* Only busy passes (the gadget booting, or its screen waking): anything else stays refused. */
            Log.w(TAG, "gadget refused the stream (" + reason + ")");
            ui.post(() -> refused("The gadget refused the stream (" + reason + ")"));
            return;
        }
        Log.i(TAG, "gadget refused the stream (" + reason + "); trying again in 1 s");
        ui.post(() -> {
            int ep = grant.epoch();
            ui.postDelayed(() -> askAgain(ep), uiSession.active() ? 500 : 1000);
        });
    }

    /* A start that stays refused: what fed the stream stops, nothing restarts it on its own, a tap starts it again. UI thread. */
    private void refused(String why) {
        uiSession.stop();
        previewDecoder.stop();
        cam.stop();
        if (player != null) {
            player.stop();
        }
        auto = false;
        wanted = false;   /* the gadget's ask went with the refusal */
        grant.revoke();
        startAfterProof = -1;
        linkStatus = why;
        connStatus = linkStatus;
        refreshUi();
    }

    /* startStream() in 300 ms, only if it is still the same connection and ready: a stream meant for one
     * connection never goes to the next. UI thread. */
    private void startStreamSoon() {
        int ep = grant.epoch();
        ui.postDelayed(() -> {
            if (ep != grant.epoch() || !link.ready() || link.streaming()) {
                return;
            }
            if (grant.allows(auto || wanted, askOk())) {
                startStream();
            } else if (!linkStatus.startsWith("The gadget refused")) {   /* a refusal keeps its own reason */
                linkStatus = "Stopped: tap a source to start again";   /* a Stop or Start automatically off meanwhile */
                connStatus = linkStatus;
                refreshUi();
            }
        }, 300);
    }

    /* A refused start (busy, or waiting for the proof) asked for again on the same connection. UI thread. */
    private void askAgain(int ep) {
        if (ep != grant.epoch() || !link.ready() || link.streaming()) {
            return;
        }
        if (!grant.allows(uiSession.active() || auto || wanted, askOk())) {
            return;   /* a saved UI source, too, needs the user's start or the proof */
        }
        if (uiSession.active()) {
            /* A UI stream: the encoder keeps going and starts on a keyframe. */
            uiSession.restartLink();
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
    public void onShown(int frameNo, int ageMs, long rxNs) {
        uiSession.onShown(frameNo, ageMs, rxNs);
    }

    @Override
    public void onTouch(int action, int x, int y, int timeMs, int id) {
        uiSession.onTouch(action, x, y, id);
    }

    @Override
    public void onHidStatus(int mode, int flags) {
        uiSession.onHidStatus(mode, flags);
    }

    @Override
    public void onUiControl(int id, int value) {
        uiSession.onUiControl(id, value);
        ui.post(this::refreshUi);
    }

    @Override
    protected void onActivityResult(int req, int code, Intent data) {
        super.onActivityResult(req, code, data);
        if (req == UiSession.REQ_PROJECTION) {
            uiSession.onProjectionResult(code, data);
        }
    }

    /** The connected gadget's bond state (BluetoothDevice.BOND_*), -1 without a BLE link. */
    int bondState() {
        GadgetLink g = bleLink();
        return g != null ? g.bondState() : -1;
    }

    /* Pairing makes the gadget a keyboard and pointer for the whole phone: with a proof key saved
     * for it, only once it has proved that key on this connection. */
    void bond() {
        GadgetLink g = bleLink();
        if (g == null) {
            return;
        }
        if (g.hasProofKey() && !g.proven()) {
            linkStatus = "Pair once the gadget has proved its key: " + g.proofLine();
            Log.w(TAG, linkStatus);
            refreshUi();
            return;
        }
        g.bond();
    }

    @Override
    public void onDeviceLog(String line) {
        if (line.contains("video") || line.contains("mg.ble") || line.contains("mg.ui") || line.contains("hid")) {
            Log.i(TAG, "gadget: " + line.trim());
            if (line.contains("fps decoded")) {
                deviceLine = line.trim();
            }
        }
    }

    @Override
    public void onDisconnected(boolean wasReady) {
        Log.i(TAG, "disconnected");
        ui.post(() -> {
            grant.connectionEnded(wasReady);   /* the next connection proves its token first, capabilities or not */
            wanted = false;   /* the gadget's ask was this connection's */
            if (cameraAsk != null) {
                android.app.AlertDialog a = cameraAsk;
                cameraAsk = null;   /* the next connection asks again */
                a.dismiss();
            }
            if (micStarted) {
                closeMic();   /* the next connection starts it again */
            }
            uiSession.stop();
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
        restartRunning(VideoLink.Running.CAMERA);
        refreshUi();
    }

    void setSize(int k) {
        size = k;
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_SIZE, k).apply();
        Log.i(TAG, "picture size " + (k > 0 ? k + " px" : "auto"));
        restartRunning(VideoLink.Running.CAMERA, VideoLink.Running.CLIP);
        refreshUi();
    }

    /* What streams now: a UI stream, the camera (or the bars), a clip, or nothing. */
    private VideoLink.Running running() {
        if (link == null) {
            return VideoLink.Running.NOTHING;
        }
        if (uiSession.active() || UiSession.isUi(source) && link.streaming()) {
            return VideoLink.Running.UI;
        }
        return !link.streaming() ? VideoLink.Running.NOTHING : isClip() ? VideoLink.Running.CLIP : VideoLink.Running.CAMERA;
    }

    /*
     * A setting's restart: only the stream of those kinds that runs now, as it was started (the start it
     * already had: no new grant), and nothing when it isn't running; a UI stream keeps going.
     */
    private void restartRunning(VideoLink.Running... kinds) {
        if (!running().restarts(kinds)) {
            return;
        }
        stopStream();
        startStreamSoon();
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
        int f = lastSize > 320 ? Math.max(minKbps, maxKbps * 2 / 3) : minKbps;
        /* A BLE gadget suggesting less than twice the floor (a tiny one, ~80 kb/s): half its suggestion. */
        int sug = bleLink() != null ? link.caps().sugKbps : 0;
        if (sug > 0 && sug < 2 * f) {
            f = Math.max(24, sug / 2);
        }
        return f;
    }

    /* ---- stats ---------------------------------------------------------- */

    private long lastUiLogMs, lastUiBytes;

    private void tick() {
        ui.postDelayed(this::tick, 1000);
        if (link == null) {
            return;
        }
        long now = android.os.SystemClock.elapsedRealtime();
        if (uiSession.active() && uiSession.streamer() != null) {
            if (now - lastUiLogMs >= 2000) {
                long b = link.statBytes;
                Log.i(TAG, String.format(java.util.Locale.US, "ui stats: %.1f kb/s on the link, %s, gadget shown %d, decode %d ms",
                        (b - lastUiBytes) * 8.0 / Math.max(1, now - lastUiLogMs), uiSession.streamer().describe(), devShown, devMs));
                lastUiBytes = b;
                lastUiLogMs = now;
            }
        }
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
        GadgetLink gl = bleLink();
        if (gl != null) {
            long abytes = gl.statAudioBytes;
            lat += String.format(java.util.Locale.US, "  audio out %.0f kb/s, %d type switches",
                    (abytes - lastAudio) * 8.0 / dt, gl.statTypeSwitches);
            lastAudio = abytes;
            long din = gl.statDataIn;
            lat += String.format(java.util.Locale.US, "  mic in %.0f kb/s", (din - lastDataIn) * 8.0 / dt);
            uiAudioKbps = (abytes - lastAudioUi) * 8.0 / dt;
            uiMicKbps = (din - lastDataIn) * 8.0 / dt;
            lastAudioUi = abytes;
            lastDataIn = din;
        } else {
            uiAudioKbps = uiMicKbps = 0;
        }
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
            GadgetLink lg = bleLink();
            if (lg != null) {
                Log.i(TAG, lg.linkLine());
            }
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
        if (gl != null && (gl.audioOn() || micStarted)) {
            st.extra("AUDIO", "tone out", String.format(java.util.Locale.US, "%.0f kb/s", uiAudioKbps), false);
            st.extra("AUDIO", "mic in", String.format(java.util.Locale.US, "%.0f kb/s", uiMicKbps), false);
        }
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
                : UiSession.WEB.equals(source) ? "Web page" : UiSession.MIRROR.equals(source) ? "Screen mirror"
                : (front ? "Front camera" : "Camera");
    }

    /** Streaming, or a clip being encoded to stream. */
    boolean streamingOrPreparing() {
        return link != null && (link.streaming() || preparing());
    }

    /** The wanted clip isn't encoded yet. */
    boolean preparing() {
        if (!isClip() || link == null || !link.ready() || !grant.allows(auto || wanted, askOk())) {
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

    /** The UI stream's renderer draws into the preview. */
    boolean uiPreview() {
        return UiSession.isUi(source) && uiSession != null && uiSession.active() && uiSession.renderer() != null
                && previewSurface != null;
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
        if ("camera".equals(source)) {
            restartRunning(VideoLink.Running.CAMERA);
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
        restartRunning(VideoLink.Running.CAMERA);
        refreshUi();
    }

    boolean autoStart() {
        return auto;
    }

    void setAutoStart(boolean on) {
        auto = on;
        if (on) {
            grant.grant();   /* this connection's, or the one coming up */
        } else {
            grant.revoke();
        }
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

    /* ---- musegadgets BLE, for the screen (BleTab) ----------------------- */

    private double uiAudioKbps, uiMicKbps;
    private long lastAudioUi;

    boolean ble() {
        return ble;
    }

    /** Wi-Fi or BLE, each with the gadget last picked on it (the old Link button). */
    void setBle(boolean on) {
        if (on != ble && link != null) {
            switchLink();
        }
        refreshUi();
    }

    boolean bleReady() {
        return bleLink() != null && link.ready();
    }

    boolean toneOn() {
        GadgetLink g = bleLink();
        return g != null && g.audioOn();
    }

    void setTone(boolean on) {
        if (on != toneOn()) {
            toggleTone();
        }
    }

    boolean toneLc3() {
        return "lc3".equals(toneCodec);
    }

    /** SBC or LC3 for the tone; a tone that's playing starts over in it. */
    void setToneLc3(boolean lc3) {
        toneCodec = lc3 ? "lc3" : "sbc";
        if (toneOn()) {
            toggleTone();
            toggleTone();
        }
    }

    int speakerVolume() {
        GadgetLink g = bleLink();
        return g != null ? g.speakerVolume : -1;
    }

    /** The gadget's speaker_volume setting (mgcommands set_settings; 0 turns its speaker off). */
    /* Bench: the phone's own mic, unprocessed, 48 kHz mono PCM16 to files/rec.raw (for measuring a
     * gadget's speaker: level, distortion, response). */
    @SuppressWarnings("MissingPermission")
    private void recordMic(int asked) {
        int secs = Math.max(1, Math.min(60, asked));   /* any app can send the extra to a debuggable build */
        new Thread(() -> {
            int rate = 48000;
            int src = android.media.MediaRecorder.AudioSource.UNPROCESSED;
            android.media.AudioManager am = getSystemService(android.media.AudioManager.class);
            if (am == null || am.getProperty(android.media.AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED) == null) {
                src = android.media.MediaRecorder.AudioSource.VOICE_RECOGNITION;
            }
            int min = android.media.AudioRecord.getMinBufferSize(rate, android.media.AudioFormat.CHANNEL_IN_MONO,
                    android.media.AudioFormat.ENCODING_PCM_16BIT);
            android.media.AudioRecord r = new android.media.AudioRecord(src, rate, android.media.AudioFormat.CHANNEL_IN_MONO,
                    android.media.AudioFormat.ENCODING_PCM_16BIT, Math.max(min, rate));
            byte[] buf = new byte[rate / 10 * 2];
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(new java.io.File(getFilesDir(), "rec.raw"))) {
                r.startRecording();
                Log.i(TAG, "rec: " + secs + " s from source " + src);
                long end = SystemClock.elapsedRealtime() + secs * 1000L;
                while (SystemClock.elapsedRealtime() < end) {
                    int n = r.read(buf, 0, buf.length);
                    if (n > 0) out.write(buf, 0, n);
                }
                Log.i(TAG, "rec: done");
            } catch (Exception e) {
                Log.w(TAG, "rec: " + e);
            } finally {
                r.release();
            }
        }, "rec").start();
    }

    private void playWavOnPhone(java.io.File f) {
        new Thread(() -> {
            try {
                byte[] all = java.nio.file.Files.readAllBytes(f.toPath());
                int rate = (all[24] & 0xff) | (all[25] & 0xff) << 8 | (all[26] & 0xff) << 16;
                android.media.AudioTrack t = new android.media.AudioTrack.Builder()
                        .setAudioAttributes(new android.media.AudioAttributes.Builder()
                                .setUsage(android.media.AudioAttributes.USAGE_MEDIA).build())
                        .setAudioFormat(new android.media.AudioFormat.Builder().setSampleRate(rate)
                                .setEncoding(android.media.AudioFormat.ENCODING_PCM_16BIT)
                                .setChannelMask(android.media.AudioFormat.CHANNEL_OUT_MONO).build())
                        .setTransferMode(android.media.AudioTrack.MODE_STATIC).setBufferSizeInBytes(all.length - 44).build();
                t.write(all, 44, all.length - 44);
                t.play();
                Log.i(TAG, "play_wav: " + f.getName() + " " + (all.length - 44) / 2 / rate + " s");
                Thread.sleep((all.length - 44) * 1000L / 2 / rate + 300);
                t.release();
            } catch (Exception e) {
                Log.w(TAG, "play_wav: " + e);
            }
        }, "play_wav").start();
    }

    void setSpeakerVolume(int v) {
        GadgetLink g = bleLink();
        if (g == null) {
            return;
        }
        if (g.proofPending()) {
            spkvolAfterProof = v;   /* a set-up gadget takes settings only after the proof */
            Log.i(TAG, "speaker volume " + v + " sent once the token proof ends");
            return;
        }
        g.setSpeakerVolume(v);
        Log.i(TAG, "speaker volume " + v);
    }

    private void closeMic() {
        GadgetLink g = bleLink();
        if (g != null) {
            flushMic();
            java.io.OutputStream mo = g.micOut;
            g.micOut = null;
            if (mo != null) {
                try {
                    mo.close();
                } catch (java.io.IOException e) {
                    Log.w(TAG, "mic file", e);
                }
            }
        }
        micStarted = false;
    }

    String micCodec() {
        return micStarted ? mic : null;
    }

    /** Capture the gadget's mic as sbc or lc3 into files/mic.<codec>, or stop (null). */
    void setMic(String codec) {
        GadgetLink g = bleLink();
        if (g == null) {
            return;
        }
        micAfterProof = false;
        if (micStarted) {
            g.sendControl(new byte[] {(byte) Mg.CMD_STOP_MIC});
            closeMic();
            Log.i(TAG, "mic capture off");
        }
        mic = codec;
        if (codec != null) {
            startMic();
        }
    }

    double audioKbps() {
        return uiAudioKbps;
    }

    double micKbps() {
        return uiMicKbps;
    }
}
