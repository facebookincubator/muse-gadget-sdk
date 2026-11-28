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

/**
 * A connection to one gadget that shows video, whatever carries it: finding
 * gadgets for the picker, connecting, the gadget's capabilities, and a stream
 * of frames (VideoFrame) sent inside the gadget's credits. WifiLink is the
 * one over the local network (protocols/VIDEO.md).
 */
abstract class VideoLink {
    static final String TAG = "MuseCam";

    interface Listener {
        void onStatus(String status);
        void onReady(Caps caps);
        void onRequestStreaming(boolean want);
        void onKeyframeRequest();
        void onStreamStopped();
        /** The gadget refused a start (reason: Mg.errorName of its code; over Wi-Fi closed, busy, invalid, unsupported). */
        void onStartRefused(String reason);
        void onBufferUpdate(long avail, long received, int shown, int dropped, int decodeMs);
        /** The buffer message's echo: the last frame the gadget drew, ageMs before rxNs. */
        default void onShown(int frameNo, int ageMs, long rxNs) {}
        /** A line of the gadget's log, where the link carries it. */
        default void onDeviceLog(String line) {}
        /** wasReady: the connection got a usable link (a failed connect attempt: false). */
        void onDisconnected(boolean wasReady);
        /** A touch on the gadget's panel (UI input): action Mg.TOUCH_*, panel pixels. */
        default void onTouch(int action, int x, int y, int timeMs, int id) {}
        /** The gadget's HID state (UI input hid_status): mode in use, Mg.HID_STATUS_* flags. */
        default void onHidStatus(int mode, int flags) {}
        /** The gadget's user changed a UI control (Mg.CTL_*): its menu, a setting from its menu. */
        default void onUiControl(int id, int value) {}
        /** The gadget refused a command (mg_command_error): which, and its error code. */
        default void onRefused(int cmd, int code) {}
        /** This connection's token proof is done (BLE): its outcome, null if the gadget offers none. */
        default void onProof(TokenProof.Outcome o) {}
        /** A gadget found (for the device picker); rssi 0 where the link has none. */
        default void onSeen(String name, String address, int rssi) {}
    }

    /** What the gadget offers (its hello). */
    static final class Caps {
        int maxPlayoutMs;   /* the device paces frames up to this playout delay (0: no pacing) */
        int dispW, dispH, maxW = 240, maxH = 240, maxFps = 30, sugW = 232, sugH = 232, sugFps = 24, sugKbps = 450;
        boolean round;
        long buffer = 65536;
        boolean video;
        int maxTiles = 1;   /* bands it decodes in parallel */
        int tileSplitPct;   /* two bands: the top band's share of the height, %; 0: equal */
        boolean vp8;        /* decodes VP8 as well as H.264 (hello's codecs) */
        /* UI streams (capability ui): the picture it takes at its panel's size, 0: none. */
        int uiW, uiH, uiFps, uiBands = 1;
        long uiMaxFrame;
        int uiStill;   /* still patch formats (1 << 1: RGB565 zlib); 0: none */
        /* UI input: touch events, and which HID modes its HOGP service offers. */
        boolean touch, hidDigitizer, hidMouseAbs, hidMouseRel, uiControls;

        private static int clamp(int v, int lo, int hi) {
            return Math.max(lo, Math.min(hi, v));
        }

        /** Keeps what a gadget claims in range: the app sizes encoders and buffers from it. */
        void clamp() {
            clamp(MIN_BUFFER);
        }

        /** minBuffer: the smallest ring taken as reported (WIFI_MIN_BUFFER on the Wi-Fi link). */
        void clamp(long minBuffer) {
            maxW = clamp(maxW, 64, 720);
            maxH = clamp(maxH, 64, 720);
            sugW = clamp(sugW, 64, maxW);
            sugH = clamp(sugH, 64, maxH);
            /* A panel or UI picture under 16 px is no size at all (0: unknown): the encoders need a macroblock. */
            dispW = dispW < MIN_PANEL ? 0 : Math.min(dispW, 2048);
            dispH = dispH < MIN_PANEL ? 0 : Math.min(dispH, 2048);
            maxFps = clamp(maxFps, 1, 60);
            sugFps = clamp(sugFps, 1, maxFps);
            sugKbps = clamp(sugKbps, 0, 20000);
            maxPlayoutMs = clamp(maxPlayoutMs, 0, 2000);
            maxTiles = clamp(maxTiles, 1, 2);
            tileSplitPct = clamp(tileSplitPct, 0, 90);
            /* Tiny-RAM BLE gadgets (e.g. 2 x 3 KB) report small rings: keep theirs. */
            buffer = Math.max(minBuffer, Math.min(1 << 20, buffer));
            uiW = uiW < MIN_PANEL ? 0 : Math.min(uiW, 720) & ~1;
            uiH = uiH < MIN_PANEL ? 0 : Math.min(uiH, 720) & ~1;
            uiFps = clamp(uiFps, 1, 60);
            uiBands = clamp(uiBands, 1, 2);
            uiMaxFrame = Math.max(0, Math.min(buffer / 2, uiMaxFrame));   /* maxFrame(): the ring holds one while the next arrives */
        }

        static final long MIN_BUFFER = 2048, WIFI_MIN_BUFFER = 16384;
        static final int MIN_PANEL = 16;

        Caps copy() {
            Caps c = new Caps();
            c.set(this);
            return c;
        }

        /**
         * A private copy, clamped, to size anything from: a link's caps can change under the reader (a
         * hello or a capability report arriving), and no unclamped value reaches the arithmetic.
         */
        Caps snapshot() {
            Caps c = copy();
            c.clamp();
            return c;
        }

        void set(Caps o) {
            maxPlayoutMs = o.maxPlayoutMs;
            dispW = o.dispW; dispH = o.dispH; maxW = o.maxW; maxH = o.maxH; maxFps = o.maxFps;
            sugW = o.sugW; sugH = o.sugH; sugFps = o.sugFps; sugKbps = o.sugKbps;
            round = o.round;
            buffer = o.buffer;
            video = o.video;
            maxTiles = o.maxTiles;
            tileSplitPct = o.tileSplitPct;
            vp8 = o.vp8;
            uiW = o.uiW; uiH = o.uiH; uiFps = o.uiFps; uiBands = o.uiBands;
            uiMaxFrame = o.uiMaxFrame;
            uiStill = o.uiStill;
            touch = o.touch; hidDigitizer = o.hidDigitizer; hidMouseAbs = o.hidMouseAbs; hidMouseRel = o.hidMouseRel;
            uiControls = o.uiControls;
        }

        /** The largest frame the gadget's ring takes whole: half of it (it holds one frame while the next arrives). */
        long maxFrame() {
            return buffer / 2;
        }

        @Override
        public String toString() {
            return "display " + dispW + "x" + dispH + (round ? " round" : "") + ", max " + maxW + "x" + maxH + "@"
                    + maxFps + ", buffer " + buffer + ", suggests " + sugW + "x" + sugH + "@" + sugFps + " "
                    + sugKbps + " kb/s" + (maxTiles > 1 ? ", " + maxTiles + " bands" : "") + (vp8 ? ", VP8" : "")
                    + (uiW > 0 ? ", UI " + uiW + "x" + uiH + "@" + uiFps + " max frame " + uiMaxFrame
                            + (uiBands > 1 ? " in " + uiBands + " bands" : "") + (uiStill != 0 ? ", still patches" : "") : "")
                    + (touch ? ", touch" : "") + (hidDigitizer || hidMouseAbs || hidMouseRel ? ", HID" : "");
        }
    }

    /* Live sources (the camera): at most this much video the device hasn't
     * decoded yet (on its way plus waiting in its ring) before the next frame
     * starts; 0: sized for clips. A live frame that can't start waits on the
     * phone, where the camera skips frames instead of queueing them: every
     * frame's worth held anywhere is a frame period of latency. */
    volatile long liveAhead;
    volatile LatencyMeter meter;
    /* Stats: stream bytes sent, writes, time frames waited for the backlog
     * gate (statGateUs) and for credits (statStallUs). */
    volatile long statBytes, statWrites, statStallUs, statGateUs;

    abstract void start();
    abstract void close();
    abstract boolean ready();
    abstract Caps caps();
    /** Before start(): what to connect to (address wins when given); neither: list only. */
    abstract void setTarget(String address, String name);
    /** One line about the link for the stats (address, signal, ...). */
    abstract String describe();
    /** What kind of link it is, for the screen ("Wi-Fi"). */
    abstract String kind();
    /** The connected gadget's name, or null. */
    String connectedName() {
        return null;
    }

    /** bands: the picture as that many horizontal bands, each its own stream; vp8: VP8 frames, else H.264. */
    abstract void startStreaming(int w, int h, int fps, int kbps, int playoutMs, int bands, boolean vp8);

    /** A UI stream (protocols/UI.md): content = ui, at up to the ui capability's size. */
    void startUiStreaming(int w, int h, int fps, int kbps, int bands) {
        throw new UnsupportedOperationException("UI streams need the BLE link");
    }

    void startStreaming(int w, int h, int fps, int kbps, int playoutMs, int bands) {
        startStreaming(w, h, fps, kbps, playoutMs, bands, false);
    }
    abstract void stopStreaming();
    abstract boolean streaming();
    /** Queues one frame (header included). */
    abstract void sendFrame(byte[] frame);
    /** Frame bytes waiting to go (not counting what's on its way). */
    abstract long queuedBytes();
    /** Bytes the device hasn't taken out of its ring yet, as far as we know. */
    abstract long inFlight();

    /** playoutMs: ask the device to show the frames evenly this long after they start (pre-recorded clips). */
    void startStreaming(int w, int h, int fps, int kbps, int playoutMs) {
        startStreaming(w, h, fps, kbps, playoutMs, 1);
    }

    /* The device picker. */
    /** Keep finding gadgets for the picker, without dropping the current link or connecting. */
    abstract void scanForPicker();
    /** Picker closed without a choice. */
    abstract void pickerClosed();
    /** Connect to this one (from the picker), dropping any other link. */
    abstract void select(String address, String name);
    /** The connected gadget's address, or null. */
    abstract String connectedAddress();

    /** This connection proved the gadget (its token proof matched). A link that proves nothing (Wi-Fi): never. */
    boolean proven() {
        return false;
    }

    /**
     * The user's own start, held for one connection: the one it was made on, or the next one to get a
     * usable link. A connection that got one and ends, however far its gadget went (capabilities or
     * not), or a link that is remade, lets it go; a connect attempt that never got one keeps it.
     */
    static final class StartGrant {
        private int epoch, granted = -1;
        private int cameraYes = -1;   /* the connection the user let the gadget start the camera on */
        private boolean closed;       /* the user's Disconnect: nothing starts until a gadget is picked again */

        /**
         * A camera start that isn't the user's own (the gadget's ask over BLE, Start automatically) needs
         * the user's yes, once per connection. benchAuto: Start automatically (or --ez auto) with the app's
         * private bench marker present, the user's standing start on a bench phone.
         */
        boolean needsCameraAsk(boolean benchAuto) {
            return !held() && cameraYes != epoch && !benchAuto;
        }

        void allowCamera() {
            cameraYes = epoch;
        }

        void grant() {
            granted = epoch;
        }

        void revoke() {
            granted = -1;
        }

        boolean held() {
            return !closed && granted == epoch;
        }

        /** Moves on when a connection ends; one that never got a usable link doesn't count. */
        void connectionEnded(boolean wasReady) {
            if (wasReady) {
                epoch++;
            }
        }

        void linkRemade() {
            epoch++;
        }

        /**
         * Disconnect, first thing: the connection it ends moves on at once, so nothing from before it (the
         * gadget's ask in the close's grace, a retry, the camera's yes) starts anything again.
         */
        void close() {
            epoch++;
            closed = true;
        }

        /** A gadget picked again, or Connect. */
        void reopen() {
            closed = false;
        }

        boolean closed() {
            return closed;
        }

        /** The connection a retry or a held start belongs to. */
        int epoch() {
            return epoch;
        }

        /**
         * Whether a start may go ahead: the user's own on this connection, or one made on its own
         * (selfStart: Start automatically, the gadget's ask) once this connection proved the gadget.
         */
        boolean allows(boolean selfStart, boolean proven) {
            return !closed && (held() || selfStart && proven);
        }
    }

    /** What streams now, as a setting change sees it. */
    enum Running {
        NOTHING, UI, CAMERA, CLIP;

        /**
         * A setting (frame rate, picture size, camera, codec, bands, encoder) applies to what runs: it
         * restarts the stream it changes, of the kinds it names, and starts nothing; a UI stream (the
         * screen, a web page) keeps its own size and rate.
         */
        boolean restarts(Running... kinds) {
            if (this == NOTHING || this == UI) {
                return false;
            }
            for (Running k : kinds) {
                if (k == this) {
                    return true;
                }
            }
            return false;
        }
    }
}
