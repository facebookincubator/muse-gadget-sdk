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

import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.SystemClock;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Video over the local network (protocols/VIDEO.md, Wi-Fi): finds gadgets
 * with mDNS (_musevideo._tcp), connects over TCP, and sends the frames inside
 * the gadget's credits (its buffer messages), control messages first. A
 * gadget can also be given by address (host or host:port), with no mDNS.
 */
final class WifiLink extends VideoLink {
    static final String SERVICE = "_musevideo._tcp";
    static final int DEFAULT_PORT = 7766;
    private static final int JSON = 0x4A;   /* 'J' */
    /* Largest write: TCP splits it anyway; smaller keeps a frame from
     * holding the socket long when credits are short. */
    private static final int CHUNK = 16384;

    private final Context ctx;
    private final Listener l;
    private final NsdManager nsd;
    private volatile String target, targetAddr;
    private volatile boolean closed, ready, discovering;
    private volatile boolean connectPaused;
    private volatile Socket sock;
    private volatile OutputStream out;
    private volatile String connected;   /* host:port */
    private volatile String connectedName;
    private final Caps caps = new Caps();
    /* Gadgets found: address (host:port) -> name. */
    private final Map<String, String> found = new ConcurrentHashMap<>();

    /* Outgoing: control messages first, then frames within credits. */
    private final Object lock = new Object();
    private final ArrayDeque<byte[]> controlQ = new ArrayDeque<>();
    private final ArrayDeque<byte[]> frameQ = new ArrayDeque<>();
    private byte[] cur;
    private int curOff;
    private long queuedBytes;
    private volatile boolean streaming;
    private long sent;                       /* stream bytes written since start */
    private long creditAvail, creditRecv;    /* from the latest buffer message */
    private long bufferSize = 65536;
    private volatile long maxAhead = 16384;
    private Thread sender, connector;

    WifiLink(Context ctx, Listener l) {
        this.ctx = ctx;
        this.l = l;
        nsd = ctx.getSystemService(NsdManager.class);
    }

    @Override
    boolean ready() {
        return ready;
    }

    @Override
    Caps caps() {
        return caps;
    }

    @Override
    void setTarget(String address, String name) {
        targetAddr = address;
        target = name;
    }

    @Override
    String describe() {
        String c = connected;
        return c != null ? "Wi-Fi " + c : "Wi-Fi, not connected";
    }

    /* ---- finding and connecting ---------------------------------------- */

    @Override
    void start() {
        closed = false;
        sender = new Thread(this::sendLoop, "video-send");
        sender.setPriority(Thread.MAX_PRIORITY);
        sender.start();
        connector = new Thread(this::connectLoop, "video-connect");
        connector.start();
        discover();
    }

    @Override
    void close() {
        closed = true;
        stopDiscovery();
        drop();
        if (sender != null) {
            sender.interrupt();
        }
        if (connector != null) {
            connector.interrupt();
        }
    }

    private final NsdManager.DiscoveryListener discoveryCb = new NsdManager.DiscoveryListener() {
        @Override
        public void onStartDiscoveryFailed(String type, int code) {
            discovering = false;
            Log.w(TAG, "mDNS discovery failed to start: " + code);
        }

        @Override
        public void onStopDiscoveryFailed(String type, int code) {}

        @Override
        public void onDiscoveryStarted(String type) {
            Log.i(TAG, "looking for " + type);
        }

        @Override
        public void onDiscoveryStopped(String type) {
            discovering = false;
        }

        @Override
        public void onServiceFound(NsdServiceInfo s) {
            resolve(s);
        }

        @Override
        public void onServiceLost(NsdServiceInfo s) {
            Log.i(TAG, "mDNS: " + s.getServiceName() + " gone");
        }
    };

    @SuppressWarnings("deprecation")   /* resolveService: its replacement needs API 34 */
    private void resolve(NsdServiceInfo s) {
        nsd.resolveService(s, new NsdManager.ResolveListener() {
            @Override
            public void onResolveFailed(NsdServiceInfo info, int code) {
                if (code == NsdManager.FAILURE_ALREADY_ACTIVE && !closed) {
                    SystemClock.sleep(200);   /* one resolve at a time: try again */
                    resolve(info);
                }
            }

            @Override
            public void onServiceResolved(NsdServiceInfo info) {
                if (info.getHost() == null) {
                    return;
                }
                String addr = info.getHost().getHostAddress() + ":" + info.getPort();
                String name = info.getServiceName();
                found.put(addr, name);
                Log.i(TAG, "found " + name + " at " + addr);
                l.onSeen(name, addr, 0);
                synchronized (lock) {
                    lock.notifyAll();   /* the connector may want it */
                }
            }
        });
    }

    private void discover() {
        if (closed || discovering) {
            return;
        }
        discovering = true;
        try {
            nsd.discoverServices(SERVICE, NsdManager.PROTOCOL_DNS_SD, discoveryCb);
        } catch (IllegalArgumentException e) {
            discovering = false;   /* already running */
        }
        if (!connectPaused && connected == null) {
            l.onStatus(target == null && targetAddr == null ? "Looking for a gadget on Wi-Fi..."
                                                            : "Looking for " + label() + " on Wi-Fi...");
        }
    }

    private void stopDiscovery() {
        if (discovering) {
            try {
                nsd.stopServiceDiscovery(discoveryCb);
            } catch (IllegalArgumentException e) {
                // not running
            }
            discovering = false;
        }
    }

    private String label() {
        return target != null ? target : targetAddr;
    }

    /* What to connect to now: the address given, else the first gadget
     * found whose name contains the target (any gadget without one); null
     * if none yet. */
    private String pick() {
        String a = targetAddr;
        if (a != null) {
            if (a.indexOf(':') < 0) {
                a += ":" + DEFAULT_PORT;
            }
            return a;
        }
        String t = target;
        for (Map.Entry<String, String> e : found.entrySet()) {
            if (t == null || e.getValue().contains(t)) {
                return e.getKey();
            }
        }
        return null;
    }

    private void connectLoop() {
        while (!closed) {
            String addr = connectPaused || sock != null ? null : pick();
            if (addr == null) {
                synchronized (lock) {
                    try {
                        lock.wait(500);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
                continue;
            }
            int colon = addr.lastIndexOf(':');
            String host = addr.substring(0, colon);
            int port = Integer.parseInt(addr.substring(colon + 1));
            String name = found.getOrDefault(addr, target != null ? target : host);
            l.onStatus("Connecting to " + name + " (" + addr + ")...");
            Socket s = new Socket();
            try {
                s.setTcpNoDelay(true);
                s.setKeepAlive(true);
                s.connect(new InetSocketAddress(host, port), 3000);
            } catch (IOException e) {
                Log.w(TAG, "connect to " + addr + ": " + e.getMessage());
                try {
                    s.close();
                } catch (IOException e2) {
                    // closing
                }
                found.remove(addr);   /* stale: mDNS finds it again */
                SystemClock.sleep(1000);
                continue;
            }
            try {
                OutputStream o = s.getOutputStream();
                InputStream in = s.getInputStream();
                synchronized (lock) {
                    sock = s;
                    out = o;
                    connected = addr;
                    connectedName = name;
                }
                stopDiscovery();
                readLoop(new DataInputStream(in));
            } catch (IOException e) {
                Log.i(TAG, "connection: " + e.getMessage());
            }
            boolean wasReady = ready;
            drop();
            if (wasReady || !closed) {
                l.onDisconnected();
            }
            if (!closed) {
                SystemClock.sleep(1000);
                discover();
            }
        }
    }

    private void drop() {
        Socket s;
        synchronized (lock) {
            s = sock;
            sock = null;
            out = null;
            connected = null;
            ready = false;
            streaming = false;
            frameQ.clear();
            controlQ.clear();
            cur = null;
            queuedBytes = 0;
            lock.notifyAll();
        }
        if (s != null) {
            try {
                s.close();
            } catch (IOException e) {
                // closing
            }
        }
    }

    @Override
    void scanForPicker() {
        connectPaused = true;
        stopDiscovery();
        discover();
        for (Map.Entry<String, String> e : found.entrySet()) {
            l.onSeen(e.getValue(), e.getKey(), 0);
        }
    }

    @Override
    void pickerClosed() {
        connectPaused = false;
        synchronized (lock) {
            lock.notifyAll();
        }
    }

    @Override
    void select(String address, String name) {
        connectPaused = false;
        targetAddr = address;
        target = name;
        if (address.equals(connected)) {
            return;
        }
        Socket s = sock;
        if (s != null) {
            try {
                s.close();   /* the connector drops it and finds the new one */
            } catch (IOException e) {
                // closing
            }
        }
        synchronized (lock) {
            lock.notifyAll();
        }
    }

    @Override
    String connectedAddress() {
        return ready ? connected : null;
    }

    /* ---- incoming ------------------------------------------------------ */

    private void readLoop(DataInputStream in) throws IOException {
        byte[] buf = new byte[4096];
        while (!closed) {
            int type = in.readUnsignedByte();
            if (type != JSON) {
                throw new IOException("not the video protocol (byte " + type + ")");
            }
            int n = in.readUnsignedByte() | in.readUnsignedByte() << 8;
            if (n > buf.length) {
                buf = new byte[n];
            }
            in.readFully(buf, 0, n);
            try {
                onMessage(new JSONObject(new String(buf, 0, n, StandardCharsets.UTF_8)));
            } catch (JSONException e) {
                Log.w(TAG, "bad message from the gadget: " + e.getMessage());
            }
        }
    }

    private void onMessage(JSONObject m) {
        switch (m.optString("type")) {
            case "hello":
                parseHello(m);
                synchronized (lock) {
                    bufferSize = caps.buffer;
                }
                ready = true;
                Log.i(TAG, "connected to " + connectedName + ": " + caps);
                l.onStatus("Connected to " + connectedName + " over Wi-Fi");
                l.onReady(caps);
                break;
            case "buffer": {
                long avail = m.optLong("free"), recv = m.optLong("received");
                synchronized (lock) {
                    creditAvail = avail;
                    creditRecv = recv;
                    lock.notifyAll();
                }
                l.onBufferUpdate(avail, recv, m.optInt("shown", -1), m.optInt("dropped", -1), m.optInt("decode_ms", -1));
                if (m.has("last_frame")) {
                    int num = m.optInt("last_frame"), age = m.optInt("last_age_ms");
                    long now = SystemClock.elapsedRealtimeNanos();
                    LatencyMeter lm = meter;
                    if (lm != null) {
                        lm.shown(num, age, now);
                    }
                    l.onShown(num, age, now);
                }
                break;
            }
            case "keyframe":
                l.onKeyframeRequest();
                break;
            case "want":
                l.onRequestStreaming(m.optBoolean("on"));
                break;
            case "stop":
                clearStream();
                l.onStreamStopped();
                break;
            case "error":
                Log.w(TAG, "gadget refused " + m.optString("for") + ": " + m.optString("reason"));
                if ("start".equals(m.optString("for"))) {
                    clearStream();
                    l.onStartRefused(m.optString("reason"));
                }
                break;
            default:
                break;   /* a newer gadget's: ignored */
        }
    }

    private void parseHello(JSONObject m) {
        JSONObject d = m.optJSONObject("display"), x = m.optJSONObject("max"), s = m.optJSONObject("suggested");
        if (d != null) {
            caps.dispW = d.optInt("width");
            caps.dispH = d.optInt("height");
            caps.round = d.optBoolean("round");
        }
        if (x != null) {
            caps.maxW = x.optInt("width", caps.maxW);
            caps.maxH = x.optInt("height", caps.maxH);
            caps.maxFps = x.optInt("fps", caps.maxFps);
        }
        if (s != null) {
            caps.sugW = s.optInt("width", caps.sugW);
            caps.sugH = s.optInt("height", caps.sugH);
            caps.sugFps = s.optInt("fps", caps.sugFps);
            caps.sugKbps = s.optInt("kbps", caps.sugKbps);
        }
        caps.buffer = m.optLong("buffer", caps.buffer);
        caps.maxPlayoutMs = m.optInt("max_playout_ms");
        caps.maxTiles = Math.max(1, m.optInt("max_bands", 1));
        caps.tileSplitPct = m.optInt("band_split_pct");
        caps.video = true;
    }

    private void clearStream() {
        synchronized (lock) {
            streaming = false;
            frameQ.clear();
            cur = null;
            queuedBytes = 0;
        }
    }

    /* ---- outgoing ------------------------------------------------------ */

    private static byte[] message(JSONObject m) {
        byte[] t = m.toString().getBytes(StandardCharsets.UTF_8);
        byte[] b = new byte[3 + t.length];
        b[0] = (byte) JSON;
        b[1] = (byte) t.length;
        b[2] = (byte) (t.length >> 8);
        System.arraycopy(t, 0, b, 3, t.length);
        return b;
    }

    @Override
    void startStreaming(int w, int h, int fps, int kbps, int playoutMs, int bands) {
        playoutMs = Math.min(playoutMs, caps.maxPlayoutMs);
        Log.i(TAG, "start " + w + "x" + h + "@" + fps + " " + kbps + " kb/s, playout " + playoutMs + " ms"
                + (bands > 1 ? ", " + bands + " bands" : "") + " (was streaming: " + streaming + ")");
        /* The device holds the playout delay's worth unread on top. */
        maxAhead = Math.max(12288, kbps * 1000L / 8 * (150 + playoutMs) / 1000);
        JSONObject m = new JSONObject();
        try {
            m.put("type", "start").put("codec", "h264").put("width", w).put("height", h).put("fps", fps)
                    .put("kbps", kbps);
            if (playoutMs > 0) {
                m.put("playout_ms", playoutMs);
            }
            if (bands > 1) {
                m.put("bands", bands);
            }
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
        synchronized (lock) {
            frameQ.clear();
            cur = null;
            queuedBytes = 0;
            sent = 0;
            creditAvail = 0;   /* nothing until the device's first buffer message */
            creditRecv = 0;
            streaming = true;
            controlQ.add(message(m));
            lock.notifyAll();
        }
    }

    @Override
    void stopStreaming() {
        synchronized (lock) {
            if (!streaming) {
                return;
            }
            streaming = false;
            frameQ.clear();
            cur = null;
            queuedBytes = 0;
            try {
                controlQ.add(message(new JSONObject().put("type", "stop")));
            } catch (JSONException e) {
                throw new IllegalStateException(e);
            }
            lock.notifyAll();
        }
    }

    @Override
    boolean streaming() {
        return streaming;
    }

    @Override
    void sendFrame(byte[] frame) {
        synchronized (lock) {
            if (!streaming) {
                return;
            }
            frameQ.add(frame);
            queuedBytes += frame.length;
            lock.notifyAll();
        }
    }

    @Override
    long queuedBytes() {
        synchronized (lock) {
            return queuedBytes;
        }
    }

    @Override
    long inFlight() {
        synchronized (lock) {
            return sent - creditRecv;
        }
    }

    private void sendLoop() {
        while (!Thread.currentThread().isInterrupted()) {
            byte[] ctl = null;
            byte[] data = null;
            int off = 0, n = 0, frameDone = -1;
            OutputStream o;
            synchronized (lock) {
                while (true) {
                    if (!ready || out == null) {
                        if (!waitLock(50)) {
                            return;
                        }
                        continue;
                    }
                    if (!controlQ.isEmpty()) {
                        ctl = controlQ.poll();
                        break;
                    }
                    if (streaming) {
                        if (cur == null && !frameQ.isEmpty()) {
                            /* A new frame starts only while the device holds
                             * little: what's on its way plus what waits in its
                             * ring, unread. That bounds the latency when the
                             * decoder is the slow end; the camera then skips
                             * frames here instead of the gadget falling behind.
                             * (A frame once started goes whole.) */
                            long backlog = sent - creditRecv + (bufferSize - creditAvail);
                            if (backlog > (liveAhead > 0 ? liveAhead : maxAhead)) {
                                long t0 = SystemClock.elapsedRealtimeNanos();
                                if (!waitLock(10)) {
                                    return;
                                }
                                statGateUs += (SystemClock.elapsedRealtimeNanos() - t0) / 1000;
                                continue;
                            }
                            cur = frameQ.poll();
                            curOff = 0;
                        }
                        if (cur != null) {
                            /* Inside the device's credits: anything past them
                             * would wait in the socket's buffers, which is
                             * pure latency. */
                            long room = creditAvail - (sent - creditRecv);
                            int k = (int) Math.min(Math.min(CHUNK, cur.length - curOff), Math.max(0, room));
                            if (k > 0) {
                                data = cur;
                                off = curOff;
                                n = k;
                                curOff += k;
                                sent += k;
                                queuedBytes -= k;
                                if (curOff >= cur.length) {
                                    frameDone = VideoFrame.number(cur);
                                    cur = null;
                                }
                                break;
                            }
                            long t0 = SystemClock.elapsedRealtimeNanos();
                            if (!waitLock(20)) {   /* out of credits: wait for a buffer message */
                                return;
                            }
                            statStallUs += (SystemClock.elapsedRealtimeNanos() - t0) / 1000;
                            continue;
                        }
                    }
                    if (!waitLock(20)) {
                        return;
                    }
                }
                o = out;
            }
            try {
                if (ctl != null) {
                    o.write(ctl);
                } else {
                    o.write(data, off, n);
                    statBytes += n;
                    statWrites++;
                }
                o.flush();
            } catch (IOException e) {
                Log.w(TAG, "write failed: " + e.getMessage());
                Socket s = sock;
                if (s != null) {
                    try {
                        s.close();   /* the reader sees it and reconnects */
                    } catch (IOException e2) {
                        // closing
                    }
                }
                continue;
            }
            LatencyMeter lm = meter;
            if (frameDone >= 0 && lm != null) {
                lm.sent(frameDone, SystemClock.elapsedRealtimeNanos());
            }
        }
    }

    /* Under lock. False once interrupted (closing). */
    private boolean waitLock(long ms) {
        try {
            lock.wait(ms);
            return true;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
    }
}
