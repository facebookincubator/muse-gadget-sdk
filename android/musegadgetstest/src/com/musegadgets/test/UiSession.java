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

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.media.projection.MediaProjectionManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;

/**
 * A UI stream end to end (protocols/UI.md): the source (a web page rendered
 * offscreen, or a square of the phone's screen), UiRenderer (square, warp,
 * the gadget's own size), UiStreamer (frames only on change, refinement),
 * and the way back: the gadget's touches mapped through the inverse warp
 * into the web view or, for the mirror, into the screen through
 * UiTouchService, and the gadget's HID told where the square is (hid_map).
 */
final class UiSession {
    private static final String TAG = "MuseCam";
    static final String WEB = "ui:web", MIRROR = "ui:mirror";
    static final int REQ_PROJECTION = 77;
    static final String[][] DEMOS = {
        {"Clock", "file:///android_asset/ui/clock.html"},
        {"Dashboard", "file:///android_asset/ui/dashboard.html"},
        {"Scrolling list", "file:///android_asset/ui/list.html"},
        {"Text page", "file:///android_asset/ui/text.html"},
    };

    private final Activity act;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final SharedPreferences prefs;
    private VideoLink link;
    private volatile UiRenderer renderer;
    private volatile UiStreamer streamer;
    private UiWebSource web;
    private Surface surface;
    private String source;
    private int outW, outH;
    private volatile boolean active;
    private Surface preview;
    private int previewW, previewH;
    /* Settings (remembered). */
    String url;
    int cssPx;               /* the page's CSS width; 0: the gadget's pixels */
    boolean fisheye, refine = true, touchBack = true;
    /* Pacing (UiStreamer): the fast rate for small changes (--ei uifast; 15 or less: off), and the old rule (--ez uilegacy). */
    int fastFps = 30;
    boolean legacyPacing;
    /* The gadget's panel is round (VideoLink caps): the fisheye and the round
     * mask apply only there; a square panel shows the square as it is, and its
     * edge "ring" is a band along the edges. */
    private boolean round = true;
    boolean sharp;           /* quality: a third, finer refinement of still pictures */
    /* How far the mirror window pans past the screen, by up to half its
     * size: 0 not at all, 1 past the top and bottom (the default: the
     * content fits across a round panel), 2 past every edge. */
    static final int OVERSCROLL_NONE = 0, OVERSCROLL_VERTICAL = 1, OVERSCROLL_ALL = 2;
    static final String[] OVERSCROLL_NAMES = {"None", "Vertical", "All"};
    int overscroll = OVERSCROLL_VERTICAL;
    /* The gadget's menu (drawn into the picture) and pan mode (drags move the mirror window). */
    private final UiMenu menu = new UiMenu();
    private volatile boolean menuOpen, pan;
    private boolean gesturePan, gestureMenu, gestureOff;
    private int lastPanX, lastPanY;
    /** Asked to end the stream from the gadget's menu (the activity stops it). */
    Runnable onStopRequested;
    int hidMode;             /* Mg.HID_*: what the gadget's HID does with the mirror's touches */
    double winX, winY, winSize = 1;   /* the mirrored square: fractions of the screen's width (x, size) and height (y) */
    int relCountsW = 1000, relCountsH = 2000;   /* relative pointer calibration */
    /* Stats. */
    volatile int lastTouchX = -1, lastTouchY = -1, touches;
    volatile String lastTouchWhere = "";
    volatile int hidStatusMode, hidStatusFlags;

    UiSession(Activity act) {
        this.act = act;
        prefs = act.getSharedPreferences("musegadgetstest", Activity.MODE_PRIVATE);
        url = prefs.getString("ui_url", DEMOS[0][1]);
        cssPx = prefs.getInt("ui_css", 0);
        fisheye = prefs.getBoolean("ui_fisheye", false);
        refine = prefs.getBoolean("ui_refine", true);
        touchBack = prefs.getBoolean("ui_touch", true);
        hidMode = prefs.getInt("ui_hid", Mg.HID_OFF);
        sharp = prefs.getBoolean("ui_sharp_v2", true);
        overscroll = Math.max(0, Math.min(2, prefs.getInt("ui_overscroll", OVERSCROLL_VERTICAL)));
        buildMenu();
        winX = prefs.getFloat("ui_wx", 0.1f);
        winY = prefs.getFloat("ui_wy", 0.25f);
        winSize = prefs.getFloat("ui_ws", 0.8f);
    }

    void save() {
        prefs.edit().putString("ui_url", url).putInt("ui_css", cssPx).putBoolean("ui_fisheye", fisheye)
                .putBoolean("ui_refine", refine).putBoolean("ui_sharp_v2", sharp).putInt("ui_overscroll", overscroll).putBoolean("ui_touch", touchBack).putInt("ui_hid", hidMode)
                .putFloat("ui_wx", (float) winX).putFloat("ui_wy", (float) winY).putFloat("ui_ws", (float) winSize)
                .apply();
    }

    static boolean isUi(String source) {
        return source != null && source.startsWith("ui:");
    }

    boolean active() {
        return active;
    }

    String source() {
        return source;
    }

    UiStreamer streamer() {
        return streamer;
    }

    UiRenderer renderer() {
        return renderer;
    }

    int outW() {
        return outW;
    }

    /* Bench (adb extras): no gadget, frames only to files/ui_dump.264 at this size. */
    boolean dry;
    int drySize = 360;
    int qp, qr1, qr2;   /* the QP ladder (0: UiStreamer's) */
    boolean twoBands;   /* --ez uibands2: two bands where the gadget takes them */

    /** Starts streaming source (WEB or MIRROR) to the gadget behind link. Main thread. */
    boolean start(VideoLink link, String source) {
        stop();
        VideoLink.Caps c = link.caps().snapshot();   /* this stream's own, clamped: a hello arriving changes nothing under it */
        if (!X264.available()) {
            Log.e(TAG, "ui: this build has no x264 (jni/build.sh): UI streams need it");
            return false;
        }
        if (MIRROR.equals(source) && !dry && !proven(link)) {
            Log.w(TAG, "ui: no screen mirror: " + (link instanceof GadgetLink ? ((GadgetLink) link).proofLine() : "not a BLE link"));
            return false;
        }
        int w = dry ? drySize : c.uiW > 0 ? c.uiW : (c.dispW > 0 ? c.dispW : c.sugW);
        int h = dry ? drySize : c.uiH > 0 ? c.uiH : (c.dispH > 0 ? c.dispH : c.sugH);
        /* The display size stands in only up to the UI cap (720), and only from a macroblock up: else refused. */
        w = Math.min(w, 720) & ~1;
        h = Math.min(h, 720) & ~1;
        if (w < VideoLink.Caps.MIN_PANEL || h < VideoLink.Caps.MIN_PANEL) {
            Log.w(TAG, "ui: the gadget reports no usable size (" + w + "x" + h + "): no UI stream");
            return false;
        }
        this.link = link;
        this.source = source;
        outW = w;
        outH = h;
        round = dry || c.round;
        buildMenu();
        /* One band by default (the codec study: two save ~40 ms of first view
         * but cost 65% more bytes when the page scrolls). */
        int bands = !dry && twoBands && c.uiBands >= 2 && outH > 240 ? 2 : 1;
        long maxFrame = c.uiMaxFrame > 0 ? Math.min(c.uiMaxFrame, c.maxFrame()) : c.maxFrame();
        if (c.uiW <= 0) {
            Log.w(TAG, "ui: the gadget lists no UI capability; trying its display size " + outW + "x" + outH);
        }
        streamer = new UiStreamer(link);
        renderer = new UiRenderer(boundTo(streamer));
        streamer.refine = refine;
        streamer.fastFps = fastFps;
        streamer.legacyPacing = legacyPacing;
        streamer.stillPatches = sharp && stillSupported();
        streamer.refine3Qp = sharp && !streamer.stillPatches ? SHARP_QP : 0;
        Log.i(TAG, "ui: quality " + (sharp ? "sharp" : "fast") + (streamer.stillPatches ? ", lossless still patches"
                : sharp ? ", a QP " + SHARP_QP + " refinement (the gadget takes no still patches)" : ""));
        streamer.dry = dry;
        if (qp > 0) streamer.changeQp = streamer.keyQp = qp;
        streamer.linkKbps = c.sugKbps > 0 ? Math.min(c.sugKbps, 500) : 300;
        if (qr1 > 0) streamer.refine1Qp = qr1;
        if (qr2 > 0) streamer.refine2Qp = qr2;
        if (dry) {
            try {
                streamer.dump = new java.io.FileOutputStream(new java.io.File(act.getExternalFilesDir(null), "ui_dump.264"));
            } catch (java.io.IOException e) {
                Log.w(TAG, "ui: no dump file", e);
            }
        }
        int srcPx = srcPx();
        int[] scr = screenPx();
        surface = renderer.configure(outW, outH, MIRROR.equals(source) ? scr[0] : srcPx,
                MIRROR.equals(source) ? scr[1] : srcPx);
        applyView();
        if (preview != null) {
            renderer.setPreview(preview, previewW, previewH);
        }
        if (!streamer.start(outW, outH, bands, c.tileSplitPct > 0 ? c.tileSplitPct : 50, maxFrame)) {
            release();
            return false;
        }
        link.liveAhead = 2048;   /* the newest picture wins: a frame starts once the gadget has the last */
        if (link instanceof GadgetLink) {
            ((GadgetLink) link).proofRequired = MIRROR.equals(source) && !dry;
        }
        this.bands = bands;
        if (!dry) startLink();
        active = true;
        if (WEB.equals(source)) {
            web = new UiWebSource(act);
            web.start(surface, srcPx, cssPx > 0 ? cssPx : outW, url);
        } else {
            int g = ++mirrorGen;
            UiCaptureService.setTarget(surface, () -> {
                if (active && g == mirrorGen) {
                    sendHidMap();   /* the gadget's HID map, now that the screen is shared */
                }
            }, () -> main.post(() -> {
                if (active && g == mirrorGen && MIRROR.equals(this.source)) {
                    Log.i(TAG, "ui: the mirror ended: stopping the stream and the gadget's touches");
                    stopAndRefresh();
                }
            }), g);
            MediaProjectionManager mpm = act.getSystemService(MediaProjectionManager.class);
            /* The whole screen: touches map to its coordinates (a single shared app has others). */
            act.startActivityForResult(android.os.Build.VERSION.SDK_INT >= 34
                    ? mpm.createScreenCaptureIntent(android.media.projection.MediaProjectionConfig.createConfigForDefaultDisplay())
                    : mpm.createScreenCaptureIntent(), REQ_PROJECTION);
        }
        GadgetLink g = link instanceof GadgetLink ? (GadgetLink) link : null;
        if (g != null && c.touch && !dry) {
            g.enableTouch(true);   /* always: the menu and panning need the touches; touchBack gates injection */
        }
        menuOpen = false;
        pan = false;
        menu.setPressed(-1);
        sendControls();
        sendHidMap();
        return true;
    }

    /** The screen-capture consent's result (from the activity). */
    void onProjectionResult(int code, Intent data) {
        if (!active || !MIRROR.equals(source)) {
            return;
        }
        if (code != Activity.RESULT_OK || data == null) {
            Log.i(TAG, "ui: screen capture not allowed: stopping the stream and the gadget's touches");
            stopAndRefresh();
            return;
        }
        Intent i = new Intent(act, UiCaptureService.class).putExtra(UiCaptureService.EXTRA_CODE, code)
                .putExtra(UiCaptureService.EXTRA_DATA, data).putExtra(UiCaptureService.EXTRA_GEN, mirrorGen);
        act.startForegroundService(i);
        main.postDelayed(() -> {
            int[] s = UiCaptureService.screenSize();
            if (s[0] > 0 && renderer != null) {
                renderer.setSourceSize(s[0], s[1]);
            }
        }, 500);
    }

    private int bands = 1;
    private int mirrorGen;   /* which start a mirror's end belongs to */

    /* As the app's Stop (no automatic restart), with the gadget's HID map off. */
    private void stopAndRefresh() {
        Runnable r = onStopRequested;
        if (r != null) {
            r.run();
        }
        stop();   /* in case the app had no link to stop it through */
        if (act instanceof MainActivity) {
            ((MainActivity) act).refreshUi();
        }
    }

    private void startLink() {
        VideoLink.Caps c = link.caps().snapshot();
        link.startUiStreaming(outW, outH, Math.min(c.uiFps > 0 ? c.uiFps : 15, 30), c.sugKbps > 0 ? c.sugKbps : 400, bands);
    }

    /** The gadget refused the start (its screen was waking): ask again; the next frame is a keyframe. */
    void restartLink() {
        if (active && link != null && !dry) {
            startLink();
            requestKeyframe();
        }
    }

    void stop() {
        if (!active && renderer == null) {
            return;
        }
        active = false;
        GadgetLink g = link instanceof GadgetLink ? (GadgetLink) link : null;
        /* Screen control ends: the gadget keeps its HID map in flash, so turn its pointer off. */
        if (g != null && MIRROR.equals(source) && !dry && g.ready() && g.uiInput() && g.proven()) {
            g.sendHidMap(Mg.HID_OFF, 0, 0, 0, 0, false, 0, 0);
        }
        if (g != null && link.caps().touch) {
            g.enableTouch(false);
        }
        if (link != null) {
            link.stopStreaming();
            link.liveAhead = 0;
        }
        if (g != null) {
            g.proofRequired = false;
        }
        release();
    }

    /* The screen's pixels and the way back into it only for a gadget that proved this phone's key on this connection. */
    private static boolean proven(VideoLink link) {
        return link instanceof GadgetLink && ((GadgetLink) link).proven();
    }

    /*
     * Samsung's adaptive refresh lets the phone's display render at 10-24 Hz
     * while its screen is static (the display mode stays 120 Hz; the render
     * rate drops within it), and the offscreen page (UiWebSource's
     * VirtualDisplay) composes on the same vsync. So a gadget tap waited
     * ~170 ms for the page to change about half the time, 50 ms otherwise
     * (Galaxy S22); pinned at 120 Hz in developer settings: 58 ms median,
     * 64 ms p90. The system raises the rate for a window that's drawing and
     * asks for it, so for a moment after each gadget touch this app's window
     * draws every frame, asking for a high rate (Android 15's frame rate
     * categories). Only while the window is visible (with the screen off the
     * page already composed at a steady rate: 57 ms median).
     */
    private static final long PULSE_NS = 500_000_000L;
    private long pulseUntilNs;
    private boolean pulsing;
    private final android.view.Choreographer.FrameCallback pulse = new android.view.Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameNs) {
            android.view.View v = act.getWindow().getDecorView();
            if (frameNs < pulseUntilNs && v.isShown()) {
                v.invalidate();
                android.view.Choreographer.getInstance().postFrameCallback(this);
            } else {
                pulsing = false;
                if (android.os.Build.VERSION.SDK_INT >= 35) {
                    v.setRequestedFrameRate(android.view.View.REQUESTED_FRAME_RATE_CATEGORY_DEFAULT);
                }
            }
        }
    };

    private void pulseRefresh() {
        act.runOnUiThread(() -> {
            android.view.View v = act.getWindow().getDecorView();
            if (!v.isShown()) {
                return;
            }
            pulseUntilNs = System.nanoTime() + PULSE_NS;
            if (!pulsing) {
                pulsing = true;
                if (android.os.Build.VERSION.SDK_INT >= 35) {
                    v.setRequestedFrameRate(android.view.View.REQUESTED_FRAME_RATE_CATEGORY_HIGH);
                }
                v.invalidate();
                android.view.Choreographer.getInstance().postFrameCallback(pulse);
            }
        });
    }

    private void release() {
        if (web != null) {
            web.stop();
            web = null;
        }
        if (MIRROR.equals(source)) {
            UiCaptureService.stopMirror();
            UiTouchService.cancel();
        }
        if (streamer != null) {
            streamer.stop();
        }
        if (renderer != null) {
            renderer.release();
            renderer = null;
        }
        active = false;
    }

    void requestKeyframe() {
        if (streamer != null) {
            streamer.requestKeyframe();
        }
    }

    void setPreview(Surface s, int w, int h) {
        preview = s;
        previewW = w;
        previewH = h;
        if (renderer != null) {
            renderer.setPreview(s, w, h);
        }
    }

    /* The web page renders at the gadget's pixels, or twice them with the fisheye (supersampled). */
    private int srcPx() {
        return fisheyeOn() ? outW * 2 : outW;
    }

    /** The fisheye as streamed: the setting, on a round panel only (a square one has no corners to save). */
    boolean fisheyeOn() {
        return fisheye && round;
    }

    /** The gadget streamed to has a round panel (true until a stream starts). */
    boolean round() {
        return round;
    }

    private int[] screenPx() {
        android.graphics.Rect b = act.getSystemService(android.view.WindowManager.class).getMaximumWindowMetrics().getBounds();
        return new int[] {b.width(), b.height()};
    }

    /* The square in the source and the warp, to the renderer. */
    private void applyView() {
        if (renderer == null) {
            return;
        }
        if (MIRROR.equals(source)) {
            int[] s = screenPx();
            double sizePx = winSize * s[0];
            renderer.setView(winX, winY, winSize, sizePx / s[1], fisheyeOn(), round, 4);
        } else {
            renderer.setView(0, 0, 1, 1, fisheyeOn(), round, fisheyeOn() ? 4 : 1);
        }
    }

    /* ---- settings that apply live -------------------------------------- */

    void setFisheye(boolean on) {
        fisheye = on;
        save();
        sendControl(Mg.CTL_FISHEYE, fisheyeOn() ? 1 : 0);
        if (web != null) {
            web.resize(srcPx(), cssPx > 0 ? cssPx : outW);
            if (renderer != null) {
                renderer.setSourceSize(srcPx(), srcPx());
            }
        }
        applyView();
        sendHidMap();
    }

    void setRefine(boolean on) {
        refine = on;
        save();
        if (streamer != null) {
            streamer.refine = on;
        }
    }

    void setUrl(String u) {
        url = u;
        save();
        if (web != null) {
            web.load(u);
        }
    }

    void setCss(int css) {
        cssPx = css;
        save();
        if (web != null) {
            web.resize(srcPx(), cssPx > 0 ? cssPx : outW);
        }
    }

    void setWindow(double x, double y, double size) {
        int[] s = screenPx();
        winSize = Math.max(0.1, Math.min(1, size));
        double hFrac = winSize * s[0] / s[1];
        /* Past the screen's edges by up to half the window, so the screen's
         * edge (its status bar, nav bar) can sit at the panel's centre,
         * where the fisheye is nearly one to one: black beyond. */
        double ox = overscroll == OVERSCROLL_ALL ? winSize / 2 : 0;
        double oy = overscroll != OVERSCROLL_NONE ? hFrac / 2 : 0;
        winX = Math.max(-ox, Math.min(1 - winSize + ox, x));
        winY = Math.max(-oy, Math.min(1 - hFrac + oy, y));
        save();
        applyView();
        sendHidMap();
    }

    void setTouchBack(boolean on) {
        touchBack = on;
        save();
        if (!on) {
            UiTouchService.cancel();   /* touches already queued don't play */
        }
        GadgetLink g = link instanceof GadgetLink ? (GadgetLink) link : null;
        if (g != null && active && !dry && link.caps().touch) {
            g.enableTouch(true);
        }
    }

    void setHidMode(int m) {
        hidMode = m;
        save();
        sendHidMap();
    }

    void setCalibration(int cw, int ch) {
        relCountsW = cw;
        relCountsH = ch;
        sendHidMap();
    }

    static final int SHARP_QP = 12;

    /* The gadget takes lossless still patches (RGB565, zlib). */
    private boolean stillSupported() {
        return link != null && (link.caps().uiStill & (1 << 1)) != 0;
    }

    void setSharp(boolean on) {
        sharp = on;
        save();
        sendControl(Mg.CTL_QUALITY, on ? 1 : 0);
        UiStreamer st = streamer;
        if (st != null) {
            st.stillPatches = on && stillSupported();
            st.refine3Qp = on && !st.stillPatches ? SHARP_QP : 0;
            if (on && st.stillPatches) {
                st.requestKeyframe();   /* the patches start from a picture the phone tracks from its first frame */
            } else if (on) {
                st.refineAgain();
            }
        }
    }

    void setPan(boolean on) {
        pan = on && MIRROR.equals(source);
        sendControl(Mg.CTL_PAN, pan ? 1 : 0);
    }

    boolean pan() {
        return pan;
    }

    private void sendControl(int id, int value) {
        GadgetLink g = link instanceof GadgetLink ? (GadgetLink) link : null;
        if (g != null && active) {
            g.sendUiControl(id, value);
        }
    }

    /* What each control is, to the gadget, at the start of a UI stream. */
    private void sendControls() {
        sendControl(Mg.CTL_FISHEYE, fisheyeOn() ? 1 : 0);
        sendControl(Mg.CTL_QUALITY, sharp ? 1 : 0);
        sendControl(Mg.CTL_PAN, pan ? 1 : 0);
        sendControl(Mg.CTL_MENU, menuOpen ? 1 : 0);
        sendControl(Mg.CTL_EDGE_PAN, MIRROR.equals(source) ? 1 : 0);
        sendControl(Mg.CTL_OVERSCROLL, overscroll);
    }

    private void buildMenu() {
        menu.items.clear();
        if (MIRROR.equals(source)) {
            /* The phone's own navigation, whatever the window shows: Android's
             * nav bar sits at the screen's very bottom, outside the round
             * panel or squeezed into its rim. */
            menu.items.add(new UiMenu.Item("Back", null, () -> nav(0)).row(1));
            menu.items.add(new UiMenu.Item("Home", null, () -> nav(1)).row(1));
            /* Recents only through the touch service: over HID the gadget's AL Select Task
             * (0x1A2) reaches the kernel as KEY_APPSELECT, but this S22's key layout maps it
             * to nothing (KEYCODE_UNKNOWN). */
            if (UiTouchService.enabled()) {
                menu.items.add(new UiMenu.Item("Recents", null, () -> nav(2)).row(1));
            }
        }
        if (round) {
            menu.items.add(new UiMenu.Item("Fisheye", () -> fisheye ? "On" : "Off", () -> setFisheye(!fisheye)));
        }
        if (MIRROR.equals(source)) {
            /* Pan only makes sense for the mirror: the web page scrolls with drags. */
            menu.items.add(new UiMenu.Item("Pan", () -> pan ? "On" : "Off", () -> {
                setPan(!pan);
                if (pan) {
                    setMenu(false);   /* straight to panning */
                }
            }).row(3));
            menu.items.add(new UiMenu.Item("Recenter", null, () -> {
                recenter();
                setMenu(false);
            }).row(3));
            menu.items.add(new UiMenu.Item("Overscroll", () -> OVERSCROLL_NAMES[overscroll],
                    () -> setOverscroll((overscroll + 1) % 3)));
        }
        menu.items.add(new UiMenu.Item("Quality", () -> sharp ? "Sharp" : "Fast", () -> setSharp(!sharp)));
        menu.items.add(new UiMenu.Item("Stop", null, () -> {
            setMenu(false);
            Runnable r = onStopRequested;
            if (r != null) {
                main.post(r);
            }
        }).row(2));
        menu.items.add(new UiMenu.Item("Close", null, () -> setMenu(false)).row(2));
    }

    void setOverscroll(int mode) {
        overscroll = Math.max(0, Math.min(2, mode));
        save();
        sendControl(Mg.CTL_OVERSCROLL, overscroll);
        setWindow(winX, winY, winSize);   /* re-clamped */
    }

    /** The mirror window back to the screen's middle (its size kept). */
    void recenter() {
        int[] s = screenPx();
        double hFrac = winSize * s[0] / s[1];
        setWindow((1 - winSize) / 2, (1 - hFrac) / 2, winSize);
        Log.i(TAG, "ui: mirror window recentered");
    }

    /* HID Consumer page usages for Back, Home and Recents (AL Select Task/Application). */
    private static final int[] NAV_USAGE = {0x224, 0x223, 0x1A2};
    private static final String[] NAV_NAME = {"Back", "Home", "Recents"};

    /** Back, Home or Recents on the phone: the touch service's global action, else the gadget's HID keys. */
    void nav(int which) {
        setMenu(false);
        if (MIRROR.equals(source) && !UiCaptureService.running()) {
            Log.i(TAG, "ui: " + NAV_NAME[which] + " ignored: no mirror");
            return;
        }
        if (MIRROR.equals(source) && !proven(link)) {
            Log.i(TAG, "ui: " + NAV_NAME[which] + " ignored: the gadget hasn't proved its key");
            return;
        }
        int[] ga = {android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK,
                android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_HOME,
                android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_RECENTS};
        String how;
        if (UiTouchService.globalAction(ga[which])) {
            how = "touch service";
        } else if (hidMode != Mg.HID_OFF && link instanceof GadgetLink) {
            ((GadgetLink) link).sendHidKey(NAV_USAGE[which]);
            how = "HID key 0x" + Integer.toHexString(NAV_USAGE[which]);
        } else {
            how = "nothing: turn on the touch service or HID";
        }
        lastTouchWhere = NAV_NAME[which] + " (" + how + ")";
        Log.i(TAG, "ui: " + NAV_NAME[which] + " via " + how);
    }

    /** The gadget's menu opened or closed (its button), or an item closed it. */
    void setMenu(boolean open) {
        if (open) {
            buildMenu();   /* its items depend on the source (mirror: nav keys, Pan mode) and the panel */
        }
        menuOpen = open;
        menu.setPressed(-1);
        UiRenderer r = renderer;
        if (r != null) {
            r.overlay = open ? menu : null;
            r.rerender();
        }
        sendControl(Mg.CTL_MENU, open ? 1 : 0);
        Log.i(TAG, "ui: menu " + (open ? "open" : "closed"));
    }

    boolean menuOpen() {
        return menuOpen;
    }

    /** A control from the gadget (its menu button, or a setting it changed). */
    void onUiControl(int id, int value) {
        main.post(() -> {
            if (!active) {
                return;
            }
            switch (id) {
                case Mg.CTL_MENU: setMenu(value != 0); break;
                case Mg.CTL_FISHEYE: setFisheye(value != 0); break;
                case Mg.CTL_PAN: setPan(value != 0); break;
                case Mg.CTL_QUALITY: setSharp(value != 0); break;
                case Mg.CTL_OVERSCROLL: setOverscroll(value); break;
                case Mg.CTL_STOP:
                    if (onStopRequested != null) {
                        onStopRequested.run();
                    }
                    break;
                default: break;
            }
        });
    }

    /** Bench: what the phone sends right now, as PNGs (and the page as the WebView draws it) in the app's files. */
    void snapshot(java.io.File dir) {
        UiRenderer r = renderer;
        if (r == null) {
            return;
        }
        java.io.File base = new java.io.File(dir, "ui_snap");
        r.snapshot(base);
        if (web != null) {
            web.snapshot(new java.io.File(dir, "ui_snap_web.png"));
        }
        UiStreamer st = streamer;
        byte[] i420 = st != null ? st.lastSent : null;
        if (i420 != null) {
            try (java.io.FileOutputStream o = new java.io.FileOutputStream(new java.io.File(dir, "ui_snap_sent.yuv"))) {
                o.write(i420);
                Log.i(TAG, "ui: saved the last picture sent (I420 " + outW + "x" + outH + ")");
            } catch (java.io.IOException e) {
                Log.w(TAG, "ui: snapshot", e);
            }
        }
    }

    /** Tells the gadget's HID where the mirrored square is (for the web source: off). */
    void sendHidMap() {
        GadgetLink g = link instanceof GadgetLink ? (GadgetLink) link : null;
        if (g == null || !g.ready() || !g.uiInput()) {
            return;
        }
        if (!active || dry || !MIRROR.equals(source) || hidMode == Mg.HID_OFF || !UiCaptureService.running() || !g.proven()) {
            g.sendHidMap(Mg.HID_OFF, 0, 0, 0, 0, false, 0, 0);
            return;
        }
        int[] s = screenPx();
        g.sendHidMap(hidMode, winX, winY, winSize, winSize * s[0] / s[1], fisheyeOn(), relCountsW, relCountsH);
    }

    /* ---- from the link -------------------------------------------------- */

    void onTouch(int action, int x, int y, int id) {
        int tg = UiTouchService.generation();   /* before the checks: a cancel() after them drops it */
        touches++;
        long rxNs = android.os.SystemClock.elapsedRealtimeNanos();
        Log.i(TAG, "ui: touch " + (action == Mg.TOUCH_DOWN ? "down" : action == Mg.TOUCH_UP ? "up" : "move") + " " + x + "," + y
                + ((id & Mg.TOUCH_ID_PAN) != 0 ? " (pan)" : ""));
        if (action == Mg.TOUCH_UP && (id & Mg.TOUCH_ID_PAN) == 0) {
            UiStreamer st = streamer;
            if (st != null) {
                st.markTouch(rxNs);   /* tap -> the picture it changes, for the latency log */
            }
        }
        /* The rest on the main thread, where stop() and the renderer change: this runs on Bluetooth's. */
        main.post(() -> touchOnMain(action, x, y, id, tg));
    }

    private void touchOnMain(int action, int x, int y, int id, int tg) {
        lastTouchX = x;
        lastTouchY = y;
        if (action == Mg.TOUCH_DOWN && active) {
            pulseRefresh();   /* the page's next frames come at the display's full rate */
        }
        if (!active) {
            return;
        }
        if (MIRROR.equals(source) && !proven(link)) {
            lastTouchWhere = "the gadget hasn't proved its key (ignored)";
            return;
        }
        if (action == Mg.TOUCH_DOWN) {
            gestureMenu = menuOpen;
            /* The gadget decides tap or pan (an edge-ring or pan-mode touch
             * that moved: protocols/mgcommands.h, MG_TOUCH_ID_PAN). */
            gesturePan = !gestureMenu && MIRROR.equals(source) && (id & Mg.TOUCH_ID_PAN) != 0;
            lastPanX = x;
            lastPanY = y;
        }
        if (gestureMenu) {
            int i = menu.hit(x, y);
            if (action == Mg.TOUCH_UP) {
                menu.setPressed(-1);
                if (i >= 0) {
                    UiMenu.Item it = menu.items.get(i);
                    Log.i(TAG, "ui: menu " + it.label);
                    main.post(() -> {
                        it.tap.run();
                        UiRenderer r = renderer;
                        if (r != null && menuOpen) {
                            r.rerender();
                        }
                    });
                }
            } else if (action == Mg.TOUCH_DOWN) {
                menu.setPressed(i);
                UiRenderer r = renderer;
                if (r != null) {
                    r.rerender();
                }
            }
            return;
        }
        if (gesturePan) {
            /* The window follows the finger the other way: drag down to see above. */
            int[] s = screenPx();
            double perPx = winSize * s[0] / outW;   /* screen pixels per panel pixel */
            double nx = winX - (x - lastPanX) * perPx / s[0];
            double ny = winY - (y - lastPanY) * perPx / s[1];
            lastPanX = x;
            lastPanY = y;
            if (action != Mg.TOUCH_DOWN) {
                setWindow(nx, ny, winSize);
            }
            lastTouchWhere = String.format(java.util.Locale.US, "pan to %.0f%%, %.0f%%", winX * 100, winY * 100);
            if (action == Mg.TOUCH_UP) {
                Log.i(TAG, "ui: panned the mirror window " + lastTouchWhere + (pan ? " (pan mode)" : " (edge drag)"));
            }
            return;
        }
        if (!touchBack) {
            return;
        }
        double[] f = new double[2];
        UiWarp.panelToSquare(x, y, outW, outH, fisheyeOn(), f);
        if (WEB.equals(source) && web != null) {
            web.touch(action, f[0], f[1]);
            lastTouchWhere = String.format(java.util.Locale.US, "page %.0f%%, %.0f%%", f[0] * 100, f[1] * 100);
        } else if (MIRROR.equals(source)) {
            if (hidMode != Mg.HID_OFF) {
                return;   /* the gadget's HID drives the screen itself */
            }
            if (!UiCaptureService.running()) {
                lastTouchWhere = "no mirror (ignored)";
                return;   /* touches reach the phone only while the user shares the screen */
            }
            int[] s = screenPx();
            float sx = (float) ((winX + f[0] * winSize) * s[0]);
            float sy = (float) (winY * s[1] + f[1] * winSize * s[0]);
            /* Off the screen (the black past a panned window's edge): a
             * gesture that goes down there is dropped; one that moves out
             * stays on the edge. */
            boolean off = sx < 0 || sy < 0 || sx > s[0] - 1 || sy > s[1] - 1;
            if (action == Mg.TOUCH_DOWN) {
                gestureOff = off;
            }
            if (gestureOff) {
                lastTouchWhere = "off the screen (ignored)";
                return;
            }
            sx = Math.max(0, Math.min(s[0] - 1, sx));
            sy = Math.max(0, Math.min(s[1] - 1, sy));
            boolean ok = UiTouchService.inject(tg, action, sx, sy);
            if (action != Mg.TOUCH_MOVE) {
                Log.i(TAG, String.format(java.util.Locale.US, "ui: touch to screen %.0f,%.0f%s", sx, sy, ok ? "" : " (touch service off)"));
            }
            lastTouchWhere = String.format(java.util.Locale.US, "screen %.0f, %.0f%s", sx, sy, ok ? "" : " (touch service off)");
        }
    }

    void onHidStatus(int mode, int flags) {
        hidStatusMode = mode;
        hidStatusFlags = flags;
    }

    void onShown(int num, int ageMs, long rxNs) {
        UiStreamer s = streamer;
        if (s != null && active) {
            s.onShown(num, ageMs, rxNs);
        }
    }

    /* A renderer's pictures go only to the streamer made with it, while that one is current:
     * a late frame from a released renderer never reaches the next stream. */
    private UiRenderer.Listener boundTo(UiStreamer st) {
        return new UiRenderer.Listener() {
            @Override
            public void onPicture(byte[] i420, long tNs) {
                onPicture(i420, null, tNs);
            }

            @Override
            public void onPicture(byte[] i420, byte[] rgb565, long tNs) {
                if (streamer == st) {
                    st.onPicture(i420, rgb565, tNs);
                }
            }

            @Override
            public boolean wantsRgb565() {
                return streamer == st && st.wantsRgb565();
            }
        };
    }
}
