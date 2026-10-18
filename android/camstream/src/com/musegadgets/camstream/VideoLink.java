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
        /** The gadget refused a start (reason: busy, invalid, unsupported). */
        void onStartRefused(String reason);
        void onBufferUpdate(long avail, long received, int shown, int dropped, int decodeMs);
        /** The buffer message's echo: the last frame the gadget drew, ageMs before rxNs. */
        default void onShown(int frameNo, int ageMs, long rxNs) {}
        /** A line of the gadget's log, where the link carries it. */
        default void onDeviceLog(String line) {}
        void onDisconnected();
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

        private static int clamp(int v, int lo, int hi) {
            return Math.max(lo, Math.min(hi, v));
        }

        /** Keeps what a gadget claims in range: the app sizes encoders and buffers from it. */
        void clamp() {
            maxW = clamp(maxW, 64, 720);
            maxH = clamp(maxH, 64, 720);
            sugW = clamp(sugW, 64, maxW);
            sugH = clamp(sugH, 64, maxH);
            dispW = clamp(dispW, 0, 2048);
            dispH = clamp(dispH, 0, 2048);
            maxFps = clamp(maxFps, 1, 60);
            sugFps = clamp(sugFps, 1, maxFps);
            sugKbps = clamp(sugKbps, 0, 20000);
            maxPlayoutMs = clamp(maxPlayoutMs, 0, 2000);
            maxTiles = clamp(maxTiles, 1, 2);
            tileSplitPct = clamp(tileSplitPct, 0, 90);
            buffer = Math.max(16384, Math.min(1 << 20, buffer));
        }

        @Override
        public String toString() {
            return "display " + dispW + "x" + dispH + (round ? " round" : "") + ", max " + maxW + "x" + maxH + "@"
                    + maxFps + ", buffer " + buffer + ", suggests " + sugW + "x" + sugH + "@" + sugFps + " "
                    + sugKbps + " kb/s" + (maxTiles > 1 ? ", " + maxTiles + " bands" : "") + (vp8 ? ", VP8" : "");
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

    /** bands: the picture as that many horizontal bands, each its own stream; vp8: VP8 frames, else H.264. */
    abstract void startStreaming(int w, int h, int fps, int kbps, int playoutMs, int bands, boolean vp8);

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
}
