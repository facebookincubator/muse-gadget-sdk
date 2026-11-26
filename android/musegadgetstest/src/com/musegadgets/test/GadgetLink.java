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

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothStatusCodes;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.os.SystemClock;
import android.util.Log;

import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;

/**
 * One musegadgets connection: scan, connect, set the link up for speed (2M PHY,
 * 517-byte MTU, high connection priority), subscribe, and stream video frames
 * on Data with Write Without Response inside the device's credits
 * (buffer_update), Control messages first. Every GATT operation waits for its
 * callback, as Android requires.
 */
@SuppressLint("MissingPermission")
final class GadgetLink extends VideoLink {
    /* The LE L2CAP channel's PSM when the gadget offers one (capability l2cap_psm). */
    private volatile int psm;

    private final Context ctx;
    private final Listener l;
    /* What to connect to: an exact address (the device picker), else the
     * first name containing `target`; neither = scan and list only. */
    private volatile String target;
    private volatile String targetAddr;
    private final BluetoothAdapter adapter;
    private BluetoothLeScanner scanner;
    private volatile BluetoothGatt gatt;
    private BluetoothGattCharacteristic control, data, nusTx;
    private final StringBuilder logLine = new StringBuilder();   /* the device log mirror's partial line */
    private volatile int mtu = 23;
    volatile int connInterval, connLatency, txPhy;
    /* The gadget's Device Information model number, "" if it has none. */
    volatile String model = "";
    private volatile boolean ready, closed;
    /* Connections in a row that dropped before they were set up (a gadget
     * without the musegadgets service, say): each waits twice as long before
     * the next try, up to 30 s, instead of hammering it every second. */
    private int failedConnects;
    private final Caps caps = new Caps();
    private boolean gotFeatures;
    /* The advertised name of the gadget connected to (or being connected to). */
    private volatile String connName;

    /* Token proof (TokenProof): with a key for the gadget (ProofKeys), every
     * connection proves the gadget holds it. Reset on connect and disconnect.
     * The fields below, to proofCode, are under proofLock or volatile. */
    private final Object proofLock = new Object();
    private TokenProof proof;              /* while it runs */
    private javax.crypto.SecretKey proofKey;
    private String proofFp;
    private int proofConn;                 /* counts connections, for the delayed checks */
    private volatile boolean proven, proofDone;
    private volatile TokenProof.Outcome proofOutcome;   /* null when done: the gadget offers no token proof */
    private volatile int proofCode = -1;
    /** Screen mirror: frames go only to a gadget that proved its key on this connection. */
    volatile boolean proofRequired;

    /* GATT operation completion. */
    private final Semaphore op = new Semaphore(0);
    private volatile int opStatus;

    /* Outgoing: Control messages first, then frames within credits. */
    private final Object lock = new Object();
    private final ArrayDeque<byte[]> controlQ = new ArrayDeque<>();
    private final ArrayDeque<byte[]> frameQ = new ArrayDeque<>();
    private byte[] cur;          /* the frame being sent */
    private int curOff;
    private long queuedBytes;    /* frames waiting, not yet on the air */
    private volatile boolean streaming;
    private long sent;           /* stream bytes written since start_streaming */
    private long creditAvail, creditRecv;   /* from the latest buffer_update */
    private volatile long maxAhead = 16384;
    /* Live sources (the camera): at most this much video the device hasn't
     * decoded yet (in the air plus waiting in its ring) before the next frame
     * starts; 0: maxAhead, sized for clips. A live frame that can't start
     * waits on the phone, where the camera skips frames instead of queueing
     * them: every frame's worth held anywhere is a frame period of latency. */
    private long lastPriorityMs;

    /* Audio playback beside the video (stream_audio), interleaved on Data with
     * change_data_type: SBC frames looped from a file, paced in real time. */
    private static final int TYPE_SBC = 1, TYPE_LC3 = 19;
    /* The video stream's data type: H.264, or VP8 (startStreaming's vp8). */
    private volatile int videoType = Mg.DATA_TYPE_VIDEO_H264;
    private volatile int audioType = TYPE_SBC, audioFrameUs = 8000;
    /* Audio goes in writes of 5-8 frames (40-64 ms): each switch between the
     * streams costs a change_data_type write. */
    private static final int AUDIO_PREBUFFER_FRAMES = 40, AUDIO_FRAMES_PER_WRITE = 8,
            AUDIO_MIN_FRAMES_PER_WRITE = 5;
    private volatile boolean audioOn;
    private byte[] sbc;
    private int sbcFrameBytes, sbcPos;
    private long audioStartNs, audioFramesSent;
    private long aAvail, aRecv, aSent;
    private int txType = videoType;     /* what the device takes our Data writes as */
    volatile long statAudioBytes, statTypeSwitches;
    /* Time the audio loop had frames due but not the gadget's credit for them (playback credit stalls). */
    volatile long statAudioStarveUs;
    private long audioStarveNs;
    private long lkBytes, lkAudio, lkWrites, lkStall, lkGate, lkStarve, lkSwitch, lkMs;

    /** One line of the link's last interval, for benches: rates, writes, stalls, and the link parameters. */
    String linkLine() {
        long now = SystemClock.elapsedRealtime(), dt = Math.max(1, now - lkMs);
        String s = String.format(java.util.Locale.US,
                "link: %.0f kb/s total (audio %.0f), %.0f writes/s, credit stall video %d ms/s, audio starved %d ms/s, gate %d ms/s,"
                        + " %d type switches/s | interval %.2f ms latency %d, PHY %d, MTU %d, write %d B",
                (statBytes - lkBytes) * 8.0 / dt, (statAudioBytes - lkAudio) * 8.0 / dt, (statWrites - lkWrites) * 1000.0 / dt,
                (statStallUs - lkStall) / dt, (statAudioStarveUs - lkStarve) / dt, (statGateUs - lkGate) / dt,
                (statTypeSwitches - lkSwitch) * 1000 / dt, connInterval * 1.25, connLatency, txPhy, mtu,
                Math.min(writeMax, Math.max(20, mtu - 3)));
        lkBytes = statBytes;
        lkAudio = statAudioBytes;
        lkWrites = statWrites;
        lkStall = statStallUs;
        lkGate = statGateUs;
        lkStarve = statAudioStarveUs;
        lkSwitch = statTypeSwitches;
        lkMs = now;
        return s;
    }
    /* The stream on an L2CAP channel when the gadget offers one (--ez l2cap false: GATT only). */
    volatile boolean useL2cap = false;
    private volatile android.bluetooth.BluetoothSocket coc;
    private volatile java.io.OutputStream cocOut;
    private long lastRecv = -1, lastRecvMs;
    volatile long statLost;
    /* Bytes written but not yet at the device: enough to keep the radio busy
     * across the credit round trip (~30 ms), no more. Anything past that waits
     * in Android's queue and is pure latency. Tunable with --ei air. */
    volatile long airCap = 3072;
    /* Largest Data write (bytes); --ei wmax for link experiments. */
    volatile int writeMax = 495;
    private long bufferSize = 65536;   /* the device's ring (report_capabilities) */
    /* Bench (--ei vbuf N): treat the gadget's ring as N bytes (frame caps, credits), to try a small gadget's limits on a big one. */
    volatile long bufferOverride;
    volatile int fpsOverride;
    volatile int audioParams = 3;   /* --ei vfps N: the same for its frame rate */
    private Thread sender;

    /* Stats. */
    volatile long statDataIn;
    /* --es mic: the gadget's mic stream as received, for a host to check. */
    volatile java.io.OutputStream micOut;
    /* The gadget's speaker_volume setting (get_settings / set_settings), -1 until it answers. */
    static final int CMD_GET_SETTINGS = 63, CMD_SET_SETTINGS = 65, SETTING_SPEAKER_VOLUME = 10;
    volatile int speakerVolume = -1;

    void askSpeakerVolume() {
        sendControl(new byte[] {(byte) CMD_GET_SETTINGS, (byte) SETTING_SPEAKER_VOLUME});
    }

    /** Asks the gadget for its speaker_volume (the answer updates speakerVolume). */
    void requestSpeakerVolume() {
        if (ready()) {
            sendControl(new byte[] {(byte) CMD_GET_SETTINGS, (byte) SETTING_SPEAKER_VOLUME});
        }
    }

    void setSpeakerVolume(int v) {
        speakerVolume = v;
        sendControl(new byte[] {(byte) CMD_SET_SETTINGS, (byte) SETTING_SPEAKER_VOLUME, (byte) v});
    }

    GadgetLink(Context ctx, String target, Listener l) {
        this.ctx = ctx;
        this.l = l;
        this.target = target;
        BluetoothManager bm = ctx.getSystemService(BluetoothManager.class);
        adapter = bm.getAdapter();
    }

    @Override
    String describe() {
        return String.format(java.util.Locale.US, "BLE  MTU %d  interval %.2f ms", mtu, connInterval * 1.25);
    }

    int mtu() {
        return mtu;
    }

    /** Asks for the fast connection (15 ms, latency 0) and 2M PHY now. */
    @SuppressWarnings("MissingPermission")
    void requestHighPriority() {
        BluetoothGatt g = gatt;
        if (g == null) return;
        try {
            lastPriorityMs = SystemClock.elapsedRealtime();
            g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
            g.setPreferredPhy(BluetoothDevice.PHY_LE_2M_MASK, BluetoothDevice.PHY_LE_2M_MASK, BluetoothDevice.PHY_OPTION_NO_PREFERRED);
        } catch (SecurityException e) {
            Log.w(TAG, "priority: " + e);
        }
    }

    @Override
    boolean ready() {
        return ready;
    }

    @Override
    Caps caps() {
        return caps;
    }

    /* ---- scan and connect ---------------------------------------------- */

    @Override
    void start() {
        closed = false;
        sender = new Thread(() -> {
            try {
                sendLoop();
            } catch (RuntimeException e) {
                if (!(e.getCause() instanceof InterruptedException)) {
                    throw e;
                }
                /* close() interrupted the wait: the link is done (switching to Wi-Fi, say) */
            }
        }, "mg-send");
        sender.setPriority(Thread.MAX_PRIORITY);
        sender.start();
        scan();
    }

    @Override
    void close() {
        closed = true;
        closeChannel();
        stopScan();
        BluetoothGatt g = gatt;
        if (g != null) {
            g.disconnect();
            g.close();
        }
        gatt = null;
        ready = false;
        resetProof(null);
        if (sender != null) {
            sender.interrupt();
        }
    }

    private final ScanCallback scanCb = new ScanCallback() {
        @Override
        public void onScanResult(int type, ScanResult r) {
            String name = r.getScanRecord() != null ? r.getScanRecord().getDeviceName() : null;
            if (name == null) {
                name = r.getDevice().getName();
            }
            if (name == null) {
                return;
            }
            String addr = r.getDevice().getAddress();
            if (looksLikeGadget(r, name)) {
                l.onSeen(name, addr, r.getRssi());
            }
            boolean match = targetAddr != null ? targetAddr.equalsIgnoreCase(addr)
                                               : target != null && name.contains(target);
            if (!match || gatt != null || connectPaused) {
                return;
            }
            Log.i(TAG, "found " + name + " " + r.getDevice().getAddress() + " rssi " + r.getRssi());
            stopScan();
            connect(r.getDevice(), name);
        }
    };

    private static boolean looksLikeGadget(ScanResult r, String name) {
        java.util.List<android.os.ParcelUuid> u = r.getScanRecord() != null ? r.getScanRecord().getServiceUuids() : null;
        if (u != null) {
            for (android.os.ParcelUuid p : u) {
                java.util.UUID x = p.getUuid();
                if (x.equals(Mg.SERVICE)) {
                    return true;
                }
            }
        }
        return name.startsWith("MuseGadget") || name.startsWith("Muse");
    }

    /* Picker open: keep scanning and listing, but don't auto-connect. */
    volatile boolean connectPaused;

    /** Scan for the picker without dropping the current link. */
    @Override
    void scanForPicker() {
        connectPaused = true;
        stopScan();
        scan();
    }

    /** Picker closed without a choice: back to normal. */
    @Override
    void pickerClosed() {
        connectPaused = false;
        if (gatt != null) {
            stopScan();
        }
    }

    /** Connect to this device (from the picker), dropping any other link. */
    @Override
    void select(String address, String name) {
        connectPaused = false;
        targetAddr = address;
        target = name;
        BluetoothGatt g = gatt;
        if (g != null && address.equalsIgnoreCase(g.getDevice().getAddress())) {
            stopScan();
            return;   /* already this one */
        }
        stopScan();
        if (g != null) {
            g.disconnect();   /* the disconnect callback rescans and finds the new target */
        } else {
            scan();
        }
    }

    /** Before start(): what to look for (address wins when given). */
    @Override
    void setTarget(String address, String name) {
        targetAddr = address;
        target = name;
    }

    /** The connected device's address, or null. */
    @Override
    String connectedAddress() {
        BluetoothGatt g = gatt;
        return g != null && ready ? g.getDevice().getAddress() : null;
    }

    String targetLabel() {
        return target != null ? target : targetAddr;
    }

    @Override
    String kind() {
        return "BLE";
    }

    @Override
    String connectedName() {
        return ready ? targetLabel() : null;
    }

    private void scan() {
        if (closed) {
            return;
        }
        if (!permitted(ctx)) {
            l.onStatus("Needs Nearby devices permission");
            return;
        }
        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            l.onStatus("Bluetooth is off");
            return;
        }
        if (!connectPaused) {
            l.onStatus(target == null && targetAddr == null ? "Pick a gadget (Gadget tab)"
                                                            : "Looking for " + targetLabel() + "...");
        }
        ScanSettings s = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        if (!connectPaused && gatt == null && connectBonded()) {
            return;
        }
        scanner.startScan(null, s, scanCb);
        if (hasBondedTarget()) {
            /* Not seen advertising within a few seconds: the OS may hold its link; try by address again. */
            retry.removeCallbacksAndMessages(null);
            retry.postDelayed(() -> {
                if (!closed && gatt == null && !connectPaused) {
                    stopScan();
                    scan();
                }
            }, 5000);
        }
    }

    /*
     * A bonded gadget (its HID needs a bond) is often already connected by
     * the phone's own HID host, and then it doesn't advertise: connect to it
     * by address, which shares the OS's link, instead of waiting for a scan.
     */
    private final android.os.Handler retry = new android.os.Handler(android.os.Looper.getMainLooper());

    private boolean hasBondedTarget() {
        try {
            for (BluetoothDevice d : adapter.getBondedDevices()) {
                String name = d.getName();
                if (targetAddr != null ? targetAddr.equalsIgnoreCase(d.getAddress())
                                       : target != null && name != null && name.contains(target)) {
                    return true;
                }
            }
        } catch (SecurityException e) {
            // no permission yet
        }
        return false;
    }

    private boolean bondedTried;   /* the last attempt was by address and never got ready: scan next */

    private boolean connectBonded() {
        if (bondedTried) {
            bondedTried = false;
            return false;
        }
        java.util.Set<BluetoothDevice> bonded;
        try {
            bonded = adapter.getBondedDevices();
        } catch (SecurityException e) {
            return false;
        }
        if (bonded == null) {
            return false;
        }
        for (BluetoothDevice d : bonded) {
            String name = d.getName();
            boolean match = targetAddr != null ? targetAddr.equalsIgnoreCase(d.getAddress())
                                               : target != null && name != null && name.contains(target);
            if (match && d.getType() != BluetoothDevice.DEVICE_TYPE_CLASSIC) {
                Log.i(TAG, "bonded " + name + " " + d.getAddress() + ": connecting by address");
                bondedTried = true;
                connect(d, name != null ? name : d.getAddress());
                return true;
            }
        }
        return false;
    }

    static final String[] PERMISSIONS = {android.Manifest.permission.BLUETOOTH_SCAN,
            android.Manifest.permission.BLUETOOTH_CONNECT};

    /** Nearby devices (scan and connect) is granted. */
    static boolean permitted(Context ctx) {
        for (String p : PERMISSIONS) {
            if (ctx.checkSelfPermission(p) != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                return false;
            }
        }
        return true;
    }

    private void stopScan() {
        if (scanner != null) {
            try {
                scanner.stopScan(scanCb);
            } catch (Exception e) {
                // Bluetooth turned off meanwhile.
            }
        }
    }

    private void connect(BluetoothDevice d, String name) {
        connName = name;
        l.onStatus("Connecting to " + name + "...");
        gatt = d.connectGatt(ctx, false, gattCb, BluetoothDevice.TRANSPORT_LE, BluetoothDevice.PHY_LE_2M_MASK);
    }

    private final BluetoothGattCallback gattCb = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int state) {
            Log.i(TAG, "connection state " + state + " status " + status);
            if (state == BluetoothProfile.STATE_CONNECTED) {
                g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
                g.setPreferredPhy(BluetoothDevice.PHY_LE_2M_MASK, BluetoothDevice.PHY_LE_2M_MASK,
                        BluetoothDevice.PHY_OPTION_NO_PREFERRED);
                new Thread(() -> setUp(g), "mg-setup").start();
            } else if (state == BluetoothProfile.STATE_DISCONNECTED) {
                closeChannel();
                g.close();
                if (gatt == g) {
                    gatt = null;
                }
                boolean wasReady = ready;
                ready = false;
                resetProof(null);
                synchronized (lock) {
                    streaming = false;
                    frameQ.clear();
                    controlQ.clear();
                    cur = null;
                    queuedBytes = 0;
                }
                op.release();
                l.onDisconnected(wasReady);
                if (!closed) {
                    failedConnects = wasReady ? 0 : failedConnects + 1;
                    long wait = Math.min(30_000L, 1000L << Math.min(5, failedConnects));
                    if (failedConnects > 1) {
                        l.onStatus("Couldn't set up the gadget; trying again in " + wait / 1000 + " s");
                    }
                    new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
                        if (!closed && gatt == null) {
                            scan();
                        }
                    }, wait);
                }
            }
        }

        @Override
        public void onMtuChanged(BluetoothGatt g, int m, int status) {
            mtu = m;
            Log.i(TAG, "MTU " + m);
            done(status);
        }

        @Override
        public void onPhyUpdate(BluetoothGatt g, int tx, int rx, int status) {
            Log.i(TAG, "PHY tx " + tx + " rx " + rx + " status " + status);
            txPhy = tx;
        }

        /* Hidden in the SDK, but the stack calls it: the parameters in effect. */
        public void onConnectionUpdated(BluetoothGatt g, int interval, int latency, int timeout, int status) {
            Log.i(TAG, "connection: interval " + interval * 1.25 + " ms, latency " + latency + ", timeout "
                    + timeout * 10 + " ms, status " + status);
            connInterval = interval;
            connLatency = latency;
            if (streaming && latency > 0 && SystemClock.elapsedRealtime() - lastPriorityMs > 3000) {
                lastPriorityMs = SystemClock.elapsedRealtime();
                Log.i(TAG, "connection: peripheral latency " + latency + " during a stream, asking for high priority again");
                g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
                return;
            }
            /* Another app on the same connection (the Muse app's gadget
             * service) can drop it to a 30 ms interval, which halves what the
             * link carries: while streaming, ask for the fast one again. */
            long now = SystemClock.elapsedRealtime();
            if (streaming && interval * 1.25 > 16 && now - lastPriorityMs > 3000) {
                lastPriorityMs = now;
                Log.i(TAG, "connection: interval too long for video, asking for high priority again");
                g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            done(status);
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            done(status);
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            done(status);
        }

        @Override
        public void onCharacteristicRead(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] v, int status) {
            if (status == BluetoothGatt.GATT_SUCCESS && v != null) {
                model = new String(v, StandardCharsets.UTF_8).trim();
            }
            done(status);
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] v) {
            if (c.getUuid().equals(Mg.DATA)) {
                statDataIn += v.length;   /* mic audio (start_mic) */
                java.io.OutputStream mo = micOut;
                if (mo != null) {
                    try {
                        mo.write(v);
                    } catch (java.io.IOException e) {
                        micOut = null;
                    }
                }
            } else if (c.getUuid().equals(Mg.CONTROL)) {
                onControl(v);
            } else if (c.getUuid().equals(Mg.NUS_TX)) {
                /* The log mirror comes in small notifications that cut lines
                 * anywhere: hand on whole lines only. */
                logLine.append(new String(v, StandardCharsets.UTF_8));
                int nl;
                while ((nl = logLine.indexOf("\n")) >= 0) {
                    String line = logLine.substring(0, nl).replace("\r", "");
                    logLine.delete(0, nl + 1);
                    if (!line.isEmpty()) {
                        l.onDeviceLog(line);
                    }
                }
                if (logLine.length() > 4096) {
                    logLine.setLength(0);
                }
            }
        }
    };

    private void done(int status) {
        opStatus = status;
        op.release();
    }

    private boolean await(String what) {
        try {
            if (!op.tryAcquire(5, TimeUnit.SECONDS)) {
                Log.w(TAG, what + ": timed out");
                return false;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();   /* close() ends the sender: its loops see the interrupt */
            return false;
        }
        if (opStatus != BluetoothGatt.GATT_SUCCESS) {
            Log.w(TAG, what + ": status " + opStatus);
            return false;
        }
        return true;
    }

    private static String serviceList(BluetoothGatt g) {
        StringBuilder b = new StringBuilder();
        for (BluetoothGattService x : g.getServices()) {
            b.append(b.length() > 0 ? " " : "").append(x.getUuid().toString(), 0, 8);
        }
        return "[" + b + "]";
    }

    private void setUp(BluetoothGatt g) {
        op.drainPermits();
        if (!g.requestMtu(517) || !await("MTU")) {
            Log.w(TAG, "MTU request failed; staying at " + mtu);
        }
        /* On a link the OS already holds (a bonded HID host) the MTU callback
         * can come twice: drop the extra one so discovery waits for itself. */
        SystemClock.sleep(100);
        op.drainPermits();
        if (!g.discoverServices() || !await("discovery")) {
            g.disconnect();
            return;
        }
        BluetoothGattService s = g.getService(Mg.SERVICE);
        if (s == null) {
            SystemClock.sleep(500);   /* discovery reported early: once more */
            /* ...from the device, not Android's GATT cache: a gadget whose firmware changed
             * (or that another app connected to as something else) can be cached without it. */
            try {
                java.lang.reflect.Method refresh = g.getClass().getMethod("refresh");
                Log.i(TAG, "no musegadgets service in " + serviceList(g) + ": GATT cache dropped (" + refresh.invoke(g) + "), discovering again");
            } catch (ReflectiveOperationException | RuntimeException e) {
                Log.w(TAG, "couldn't drop the GATT cache: " + e);
            }
            op.drainPermits();
            if (g.discoverServices()) {
                await("discovery");
            }
            s = g.getService(Mg.SERVICE);
        }
        if (s == null) {
            Log.i(TAG, "services: " + serviceList(g));
            l.onStatus("No musegadgets service");
            g.disconnect();
            return;
        }
        control = s.getCharacteristic(Mg.CONTROL);
        data = s.getCharacteristic(Mg.DATA);
        if (control == null || data == null) {
            l.onStatus("No Control/Data characteristics");
            g.disconnect();
            return;
        }
        Log.i(TAG, "musegadgets service");
        data.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
        BluetoothGattService nus = g.getService(Mg.NUS_SERVICE);
        nusTx = nus != null ? nus.getCharacteristic(Mg.NUS_TX) : null;
        if (!subscribe(g, control) || !subscribe(g, data)) {
            g.disconnect();
            return;
        }
        if (nusTx != null) {
            subscribe(g, nusTx);   /* the device's log mirror: its decode stats */
        }
        /* Device Information's model number ("M5Stack StopWatch"), logged: which board it is */
        model = "";
        BluetoothGattService dis = g.getService(java.util.UUID.fromString("0000180a-0000-1000-8000-00805f9b34fb"));
        BluetoothGattCharacteristic mc = dis != null
                ? dis.getCharacteristic(java.util.UUID.fromString("00002a24-0000-1000-8000-00805f9b34fb")) : null;
        if (mc != null && g.readCharacteristic(mc) && await("model")) {
            Log.i(TAG, "model: " + model);
        }
        gotFeatures = false;
        caps.set(new Caps());   /* this connection's gadget reports its own: nothing kept from another */
        uiInput = false;
        hidMode = hidFlags = 0;
        armProof();
        ready = true;
        failedConnects = 0;
        bondedTried = false;
        sendControl(new byte[] {(byte) Mg.CMD_REQUEST_STATUS});
        sendControl(new byte[] {(byte) Mg.CMD_STREAM_VIDEO, (byte) Mg.V_REQUEST_CAPABILITIES});
        l.onStatus("Connected, MTU " + mtu);
    }

    private boolean subscribe(BluetoothGatt g, BluetoothGattCharacteristic c) {
        g.setCharacteristicNotification(c, true);
        BluetoothGattDescriptor d = c.getDescriptor(Mg.CCC);
        if (d == null) {
            return false;
        }
        int rc = g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
        return rc == BluetoothStatusCodes.SUCCESS && await("subscribe " + c.getUuid());
    }

    /* ---- incoming ------------------------------------------------------ */

    private void onControl(byte[] v) {
        if (v.length == 0) {
            return;
        }
        int cmd = v[0] & 0xff;
        boolean finished = false, taken = false;
        synchronized (proofLock) {
            if (proof != null && proof.onControl(v, SystemClock.elapsedRealtime())) {
                taken = true;
                TokenProof.Outcome o = proof.outcome();
                finished = o != null && finishProof(o, proof.refusedCode());
            }
        }
        if (finished) {
            proofReport();
        }
        if (taken || cmd == TokenProof.CMD) {
            return;   /* the proof's, or a late or extra proof message with none running */
        }
        if (cmd == Mg.CMD_SUPPORTED_FEATURES && v.length > 2 && (v[1] & 0xff) == Mg.SUB_FEATURE
                && (v[2] & 0xff) == Mg.CMD_REQUEST_STATUS) {
            Log.i(TAG, "the gadget is in a recovery mode");
            boolean done;
            synchronized (proofLock) {
                done = finishProof(TokenProof.Outcome.RECOVERY, -1);   /* trust nothing from it, with or without a key, and run no proof */
                proven = false;
            }
            if (done) {
                proofReport();
            }
            return;
        }
        if (cmd == CMD_GET_SETTINGS) {
            /* (setting, value) pairs; only speaker_volume (one byte) is asked for */
            for (int i = 1; i + 1 < v.length && (v[i] & 0xff) == SETTING_SPEAKER_VOLUME; i += 2) {
                speakerVolume = v[i + 1] & 0xff;
            }
            return;
        }
        if (cmd == Mg.CMD_SUPPORTED_FEATURES && v.length > 2 && (v[1] & 0xff) == Mg.SUB_FEATURE
                && (v[2] & 0xff) == Mg.CMD_UI_INPUT) {
            for (int i = 3; i < v.length; i++) {
                int f = v[i] & 0xff;
                caps.touch |= f == Mg.UI_FEATURE_TOUCH;
                caps.hidDigitizer |= f == Mg.UI_FEATURE_HID_DIGITIZER;
                caps.hidMouseAbs |= f == Mg.UI_FEATURE_HID_MOUSE_ABS;
                caps.hidMouseRel |= f == Mg.UI_FEATURE_HID_MOUSE_REL;
                caps.uiControls |= f == Mg.UI_FEATURE_CONTROLS;
            }
            Log.i(TAG, "UI input: touch " + caps.touch + ", HID digitizer " + caps.hidDigitizer + " abs "
                    + caps.hidMouseAbs + " rel " + caps.hidMouseRel);
            return;
        }
        if (cmd == Mg.CMD_SUPPORTED_FEATURES && v.length > 1 && (v[1] & 0xff) != Mg.SUB_FEATURE) {
            for (int i = 1; i < v.length; i++) {
                int f = v[i] & 0xff;
                if (f == Mg.CMD_STREAM_VIDEO) {
                    caps.video = true;
                }
                if ((v[i] & 0xff) == Mg.CMD_UI_INPUT) {
                    uiInput = true;
                }
            }
            gotFeatures = true;
            boolean offered = false;
            for (int i = 1; i < v.length; i++) {
                offered |= (v[i] & 0xff) == TokenProof.CMD;
            }
            boolean report = false;
            synchronized (proofLock) {
                if (proofKey != null && proof == null && !proofDone) {
                    if (offered) {
                        startProof();
                    } else {
                        Log.i(TAG, "token proof: the gadget doesn't offer it");
                        report = finishProof(null, -1);
                    }
                }
            }
            if (report) {
                proofReport();
            }
            return;
        }
        if (cmd == Mg.CMD_UI_INPUT && v.length >= 2) {
            int sub = v[1] & 0xff;
            if (sub == Mg.UI_TOUCH && v.length >= 9) {
                l.onTouch(v[2] & 0xff, Mg.u16(v, 3), Mg.u16(v, 5), Mg.u16(v, 7), v.length >= 10 ? v[9] & 0xff : 0);
            } else if (sub == Mg.UI_CONTROL && v.length >= 4) {
                Log.i(TAG, "ui: gadget control " + (v[2] & 0xff) + " = " + (v[3] & 0xff));
                l.onUiControl(v[2] & 0xff, v[3] & 0xff);
            } else if (sub == Mg.UI_HID_STATUS && v.length >= 4) {
                hidMode = v[2] & 0xff;
                hidFlags = v[3] & 0xff;
                Log.i(TAG, "HID status: mode " + hidMode + ", flags " + hidFlags);
                l.onHidStatus(hidMode, hidFlags);
            }
            return;
        }
        if (cmd == Mg.CMD_ERROR) {
            int forCmd = v.length > 1 ? (v[1] & 0xff) : -1, code = TokenProof.errorCode(v);
            Log.w(TAG, "device error for command " + forCmd + " code " + code);
            if (forCmd == Mg.CMD_STREAM_AUDIO || forCmd == Mg.CMD_CHANGE_DATA_TYPE) {
                Log.w(TAG, "audio: refused (" + forCmd + " code " + code + ")");
                synchronized (lock) {
                    if (forCmd == Mg.CMD_STREAM_AUDIO) {
                        audioOn = false;
                        txType = videoType;
                    }
                }
            }
            if (forCmd == CMD_SET_SETTINGS) {
                askSpeakerVolume();   /* not set (a set-up gadget wants the token proof first): show what it has */
            }
            l.onRefused(forCmd, code);
            if (forCmd == Mg.CMD_STREAM_VIDEO && streaming) {
                /* start_streaming refused (busy: the gadget is still booting, or
                 * it has the audio path): the stream isn't on. */
                synchronized (lock) {
                    streaming = false;
                    frameQ.clear();
                    cur = null;
                    queuedBytes = 0;
                }
                l.onStartRefused(Mg.errorName(code));
            }
            return;
        }
        if (cmd == 30 && v.length >= 19 && (v[1] & 0xff) == 11) {
            long bytes = Mg.u32(v, 3), packets = Mg.u32(v, 7), ms = Mg.u32(v, 11), gaps = Mg.u32(v, 15);
            Log.i(TAG, "bench: gadget got " + bytes + " bytes, " + packets + " packets in " + ms + " ms = "
                    + (bytes * 8 / Math.max(1, ms)) + " kb/s, gaps " + gaps);
        }
        if (cmd == Mg.CMD_STREAM_AUDIO && v.length >= 2) {
            int sub = v[1] & 0xff;
            if (sub == 1) {
                StringBuilder hx = new StringBuilder();
                for (int k = 2; k < v.length; k++) {
                    hx.append(String.format(java.util.Locale.US, "%02x ", v[k] & 0xff));
                }
                Log.i(TAG, "audio capabilities: " + hx.toString().trim());
            } else if (sub == 5 && v.length >= 10) {
                synchronized (lock) {
                    aAvail = Mg.u32(v, 2);
                    aRecv = Mg.u32(v, 6);
                    lock.notifyAll();
                }
            } else if (sub == 3) {
                boolean kept = v.length >= 3 && v[2] == 0;
                Log.i(TAG, "audio: the gadget stopped playback (" + (kept ? "played out" : "dropped") + ")");
                synchronized (lock) {
                    audioOn = false;
                    txType = videoType;
                }
            }
            return;
        }
        if (cmd != Mg.CMD_STREAM_VIDEO || v.length < 2) {
            return;
        }
        switch (v[1] & 0xff) {
            case Mg.V_REPORT_CAPABILITIES: {
                int p = applyCapsReport(v, caps, bufferOverride, fpsOverride);
                if (p == CAPS_MALFORMED) {
                    Log.w(TAG, "capabilities: malformed report (" + v.length + " bytes), ignored");
                    l.onStatus("The gadget sent a malformed capability report");
                    break;
                }
                if (p != CAPS_NO_PSM) {
                    psm = p;
                }
                if (psm != 0 && useL2cap && coc == null) {
                    openChannel(psm);
                }
                synchronized (lock) {
                    bufferSize = caps.buffer;
                }
                Log.i(TAG, "capabilities: " + caps + (bufferOverride > 0 ? " (buffer overridden: --ei vbuf)" : ""));
                l.onReady(caps);
                break;
            }
            case Mg.V_BUFFER_UPDATE:
                if (v.length >= 10) {
                    long avail = Mg.u32(v, 2), recv = Mg.u32(v, 6);
                    synchronized (lock) {
                        creditAvail = avail;
                        creditRecv = recv;
                        /* Self-heal: the device's ring is empty and it has
                         * reported the same count for a while, yet we think
                         * bytes are still on their way: they were lost (or
                         * counted against an earlier stream). Stop waiting
                         * for them. */
                        long now = SystemClock.elapsedRealtime();
                        if (recv != lastRecv) {
                            lastRecv = recv;
                            lastRecvMs = now;
                        } else if (avail == bufferSize && sent > recv && now - lastRecvMs > 400) {
                            Log.w(TAG, "credits: " + (sent - recv) + " bytes sent never arrived; resyncing");
                            statLost += sent - recv;
                            sent = recv;
                            lastRecvMs = now;
                        }
                        lock.notifyAll();
                    }
                    int shown = v.length >= 12 ? Mg.u16(v, 10) : -1;
                    int dropped = v.length >= 14 ? Mg.u16(v, 12) : -1;
                    int ms = v.length >= 15 ? (v[14] & 0xff) : -1;
                    l.onBufferUpdate(avail, recv, shown, dropped, ms);
                    if (v.length >= 19) {
                        int num = Mg.u16(v, 15), age = Mg.u16(v, 17);
                        LatencyMeter m = meter;
                        if (m != null) {
                            m.shown(num, age, SystemClock.elapsedRealtimeNanos());
                        }
                        l.onShown(num, age, SystemClock.elapsedRealtimeNanos());
                    }
                }
                break;
            case Mg.V_REQUEST_KEYFRAME:
                l.onKeyframeRequest();
                break;
            case Mg.V_REQUEST_STREAMING:
                l.onRequestStreaming(v.length >= 3 && v[2] != 0);
                break;
            case Mg.V_STOP_STREAMING:
                synchronized (lock) {
                    streaming = false;
                    frameQ.clear();
                    cur = null;
                    queuedBytes = 0;
                }
                l.onStreamStopped();
                break;
            default:
                break;
        }
    }

    /**
     * Parses a report into a copy of caps, clamps it and only then publishes
     * it: the PSM or CAPS_NO_PSM, or CAPS_MALFORMED with caps untouched.
     */
    static int applyCapsReport(byte[] v, Caps caps, long bufferOverride, int fpsOverride) {
        Caps c = caps.copy();
        int p = parseCaps(v, c);
        if (p == CAPS_MALFORMED) {
            return p;
        }
        c.video = true;
        if (bufferOverride > 0) {
            c.buffer = Math.min(c.buffer, bufferOverride);   /* bench: act as a smaller gadget */
        }
        if (fpsOverride > 0) {
            c.maxFps = Math.min(c.maxFps, fpsOverride);   /* bench: a gadget that shows fewer frames */
        }
        c.clamp();
        caps.set(c);
        return p;
    }

    static final int CAPS_MALFORMED = -1, CAPS_NO_PSM = -2;

    /**
     * Reads a report_capabilities message into c: the L2CAP PSM it carries,
     * CAPS_NO_PSM when none, CAPS_MALFORMED when the count is missing, a field
     * runs past the end or the report has fewer entries than its count (as the
     * iPhone app). An unknown id up to 16 ends the report there; one from 17
     * on is skipped by its length. Fields the report doesn't carry keep c's
     * values, except the codecs, ui and ui_still ones, which it clears.
     */
    static int parseCaps(byte[] v, Caps c) {
        if (v.length <= 2) {
            return CAPS_MALFORMED;
        }
        int n = v[2] & 0xff, i = 3, psm = CAPS_NO_PSM, e;
        c.vp8 = false;
        c.uiW = c.uiH = 0;
        c.uiStill = 0;
        for (e = 0; e < n && i < v.length; e++) {
            int cap = v[i++] & 0xff, width;
            switch (cap) {
                case Mg.VCAP_CODECS: width = i < v.length ? 1 + (v[i] & 0xff) : 1; break;
                case Mg.VCAP_DISPLAY: width = 5; break;
                case Mg.VCAP_MAX_SIZE: case Mg.VCAP_BUFFER_SIZE: width = 4; break;
                case Mg.VCAP_MAX_FPS: case Mg.VCAP_TILES: case Mg.VCAP_TILE_SPLIT: case Mg.VCAP_UI_STILL: width = 1; break;
                case Mg.VCAP_L2CAP_PSM: case Mg.VCAP_PLAYOUT: width = 2; break;
                case Mg.VCAP_UI: width = 10; break;
                case Mg.VCAP_SUGGESTED: width = 7; break;
                default:
                    if (cap < 17) {
                        return psm;   /* no length up to 16: the rest can't be found */
                    }
                    width = i < v.length ? 1 + (v[i] & 0xff) : 1;   /* from 17 on: a length, skipped */
                    break;
            }
            if (i + width > v.length) {
                return CAPS_MALFORMED;
            }
            switch (cap) {
                case Mg.VCAP_CODECS:
                    for (int k = 1; k < width; k++) {
                        c.vp8 |= (v[i + k] & 0xff) == Mg.DATA_TYPE_VIDEO_VP8;
                    }
                    break;
                case Mg.VCAP_DISPLAY:
                    c.dispW = Mg.u16(v, i);
                    c.dispH = Mg.u16(v, i + 2);
                    c.round = v[i + 4] != 0;
                    break;
                case Mg.VCAP_MAX_SIZE:
                    c.maxW = Mg.u16(v, i);
                    c.maxH = Mg.u16(v, i + 2);
                    break;
                case Mg.VCAP_MAX_FPS:
                    c.maxFps = v[i] & 0xff;
                    break;
                case Mg.VCAP_BUFFER_SIZE:
                    c.buffer = Mg.u32(v, i);
                    break;
                case Mg.VCAP_L2CAP_PSM:
                    psm = Mg.u16(v, i);
                    break;
                case Mg.VCAP_PLAYOUT:
                    c.maxPlayoutMs = Mg.u16(v, i);
                    break;
                case Mg.VCAP_TILES:
                    c.maxTiles = Math.max(1, v[i] & 0xff);
                    break;
                case Mg.VCAP_TILE_SPLIT:
                    c.tileSplitPct = v[i] & 0xff;
                    break;
                case Mg.VCAP_UI:
                    c.uiW = Mg.u16(v, i);
                    c.uiH = Mg.u16(v, i + 2);
                    c.uiFps = v[i + 4] & 0xff;
                    c.uiMaxFrame = Mg.u32(v, i + 5);
                    c.uiBands = Math.max(1, v[i + 9] & 0xff);
                    break;
                case Mg.VCAP_UI_STILL:
                    c.uiStill = v[i] & 0xff;
                    break;
                case Mg.VCAP_SUGGESTED:
                    c.sugW = Mg.u16(v, i);
                    c.sugH = Mg.u16(v, i + 2);
                    c.sugFps = v[i + 4] & 0xff;
                    c.sugKbps = Mg.u16(v, i + 5);
                    break;
                default:
                    break;
            }
            i += width;
        }
        return e < n ? CAPS_MALFORMED : psm;
    }

    /* ---- outgoing ------------------------------------------------------ */

    /* closeChannel bumps chanGen: an opener started for an older connection closes its socket instead of publishing it. */
    private final Object chanLock = new Object();
    private int chanGen, chanOpening = -1;
    private android.bluetooth.BluetoothSocket chanPending;

    private void openChannel(int psm) {
        BluetoothGatt g = gatt;
        if (g == null) {
            return;
        }
        BluetoothDevice dev = g.getDevice();
        int gen;
        synchronized (chanLock) {
            if (chanOpening == chanGen || coc != null) {
                return;   /* one opener per connection */
            }
            gen = chanOpening = chanGen;
        }
        new Thread(() -> {
            android.bluetooth.BluetoothSocket s = null;
            try {
                long t0 = SystemClock.elapsedRealtime();
                s = dev.createInsecureL2capChannel(psm);
                synchronized (chanLock) {
                    if (gen != chanGen) {
                        closeSocket(s);
                        return;
                    }
                    chanPending = s;   /* closeChannel closes it while connect() waits */
                }
                s.connect();
                synchronized (chanLock) {
                    if (gen != chanGen || closed || gatt != g) {
                        Log.i(TAG, "L2CAP channel opened for an older connection: closed");
                        closeSocket(s);
                        return;
                    }
                    cocOut = s.getOutputStream();
                    coc = s;
                }
                Log.i(TAG, "L2CAP channel open on PSM " + psm + " in " + (SystemClock.elapsedRealtime() - t0)
                        + " ms, max tx " + s.getMaxTransmitPacketSize());
                synchronized (lock) {
                    lock.notifyAll();
                }
            } catch (Exception e) {
                closeSocket(s);
                if (gen == chanGen) {
                    Log.w(TAG, "L2CAP channel failed; staying on GATT", e);
                }
            } finally {
                synchronized (chanLock) {
                    if (chanPending == s) {
                        chanPending = null;
                    }
                    if (chanOpening == gen) {
                        chanOpening = -1;
                    }
                }
            }
        }, "l2cap-open").start();
    }

    private void closeChannel() {
        android.bluetooth.BluetoothSocket s, p;
        synchronized (chanLock) {
            chanGen++;
            s = coc;
            p = chanPending;
            coc = null;
            cocOut = null;
            chanPending = null;
        }
        closeSocket(s);
        closeSocket(p);
    }

    private static void closeSocket(android.bluetooth.BluetoothSocket s) {
        if (s != null) {
            try {
                s.close();
            } catch (java.io.IOException e) {
                // closing
            }
        }
    }

    /* ---- token proof ---------------------------------------------------- */

    private final android.os.Handler main = new android.os.Handler(android.os.Looper.getMainLooper());

    /* A new connection (key: its gadget's, null: none), or none. */
    private void resetProof(javax.crypto.SecretKey key) {
        synchronized (proofLock) {
            proofConn++;
            proof = null;
            proofKey = key;
            proofFp = key != null ? ProofKeys.fingerprint(ctx, connName) : null;
            proven = false;
            proofDone = false;
            proofOutcome = null;
            proofCode = -1;
        }
    }

    /* This connection's proof, with the key kept for its gadget: it starts once the gadget lists its commands. */
    private void armProof() {
        String gadget = connName;
        javax.crypto.SecretKey k = ProofKeys.get(gadget);
        resetProof(k);
        if (k == null) {
            Log.i(TAG, "token proof: no proof key for " + gadget + ": no screen mirror to it");
            return;
        }
        int n;
        synchronized (proofLock) {
            n = proofConn;
        }
        /* A gadget that never lists its commands never proves anything: a timeout. */
        main.postDelayed(() -> {
            boolean report;
            synchronized (proofLock) {
                report = n == proofConn && proof == null && !proofDone && finishProof(TokenProof.Outcome.TIMEOUT, -1);
            }
            if (report) {
                proofReport();
            }
        }, TokenProof.SILENCE_MS);
    }

    /** The connected gadget's proof key changed (ProofKeys): prove it again with the new one, from the start. */
    void proofKeyChanged() {
        if (!ready) {
            return;
        }
        armProof();
        sendControl(new byte[] {(byte) Mg.CMD_REQUEST_STATUS});   /* its command list starts the proof */
    }

    /* Under proofLock. */
    private void startProof() {
        int n = proofConn;
        proof = new TokenProof(proofKey, this::sendControl);
        Log.i(TAG, "token proof: key " + proofFp);
        proof.start(SystemClock.elapsedRealtime());
        main.postDelayed(new Runnable() {
            @Override
            public void run() {
                boolean done;
                synchronized (proofLock) {
                    if (n != proofConn || proof == null || !proof.pending()) {
                        return;
                    }
                    proof.tick(SystemClock.elapsedRealtime());
                    TokenProof.Outcome o = proof.outcome();
                    done = o != null && finishProof(o, -1);
                }
                if (done) {
                    proofReport();
                } else {
                    main.postDelayed(this, 100);
                }
            }
        }, 100);
    }

    /* Under proofLock. Once per connection: the first outcome stands. True: report it. */
    private boolean finishProof(TokenProof.Outcome o, int code) {
        if (proofDone) {
            return false;
        }
        proofDone = true;
        proofOutcome = o;
        proofCode = code;
        proven = o == TokenProof.Outcome.MATCH;
        proof = null;
        return true;
    }

    private void proofReport() {
        TokenProof.Outcome o = proofOutcome;
        Log.i(TAG, "token proof: " + (o != null ? o + (o == TokenProof.Outcome.REFUSED ? " (error " + proofCode + ")" : "")
                : "not offered"));
        l.onProof(o);
    }

    /** This connection's token proof matched. */
    @Override
    boolean proven() {
        return proven;
    }

    /** A proof key is kept for the connected gadget. */
    boolean hasProofKey() {
        synchronized (proofLock) {
            return ready && proofKey != null;
        }
    }

    /** A proof is running, or due once the gadget lists its commands. */
    boolean proofPending() {
        synchronized (proofLock) {
            return ready && proofKey != null && !proofDone;
        }
    }

    /** Where the token proof stands on this connection, for the UI (no key material). */
    String proofLine() {
        String name = connName != null ? connName : targetLabel();
        if (!ready) {
            return "Not connected";
        }
        synchronized (proofLock) {
            if (proofKey == null) {
                return "No proof key for " + name + ": no screen mirror to it. Set it up with mg_ble_client, then paste its key";
            }
            if (!proofDone) {
                return "Proving key " + proofFp + "...";
            }
            if (proofOutcome == null) {
                return name + " doesn't offer the token proof: no screen mirror (a firmware update may add it)";
            }
            switch (proofOutcome) {
                case MATCH: return name + " proved key " + proofFp;
                case MISMATCH: return name + " didn't prove this phone's key: another device may be using its name, or it was set up again";
                case NOT_FOUND: return name + " isn't set up (it holds no token)";
                case REFUSED: return name + " refused the token proof (error " + proofCode + ")";
                case RECOVERY: return name + " is in a recovery mode";
                default: return name + " didn't answer the token proof (reconnect to try again)";
            }
        }
    }

    /** The advertised name of the gadget connected to, null if none. */
    String gadgetName() {
        return ready ? connName : null;
    }

    /* ---- UI input (protocols/UI.md) ----------------------------------- */

    private volatile boolean uiInput;
    private volatile int hidMode, hidFlags;

    /** The gadget lists mg_command_ui_input. */
    boolean uiInput() {
        return uiInput;
    }

    int hidMode() {
        return hidMode;
    }

    int hidFlags() {
        return hidFlags;
    }

    /** Tells the gadget a UI control's value (Mg.CTL_*), at the start of a UI stream and on every change. */
    void sendUiControl(int id, int value) {
        if (caps.uiControls) {
            sendControl(new byte[] {(byte) Mg.CMD_UI_INPUT, (byte) Mg.UI_CONTROL, (byte) id, (byte) value});
        }
    }

    /** The gadget's HID presses a consumer-control key (0x224 AC Back, 0x223 AC Home). */
    void sendHidKey(int usage) {
        sendControl(new byte[] {(byte) Mg.CMD_UI_INPUT, (byte) Mg.UI_HID_KEY, (byte) usage, (byte) (usage >> 8)});
    }

    /** Touch events on (the gadget notifies onTouch) or off. */
    void enableTouch(boolean on) {
        sendControl(new byte[] {(byte) Mg.CMD_UI_INPUT, (byte) Mg.UI_TOUCH_ENABLE, (byte) (on ? 1 : 0)});
    }

    /**
     * The gadget's HID mode and where the streamed square sits on this
     * phone's screen (fractions of its width and height); fisheye: the square
     * is warped onto the disc; countsW/H: the relative pointer's calibration.
     */
    void sendHidMap(int mode, double x, double y, double w, double h, boolean fisheye, int countsW, int countsH) {
        if (mode == Mg.HID_OFF) {
            sendControl(new byte[] {(byte) Mg.CMD_UI_INPUT, (byte) Mg.UI_HID_MAP, 0});
            return;
        }
        byte[] m = new byte[mode == Mg.HID_MOUSE_REL ? 16 : 12];
        m[0] = (byte) Mg.CMD_UI_INPUT;
        m[1] = (byte) Mg.UI_HID_MAP;
        m[2] = (byte) mode;
        /* A window panned past the screen's edges: x and y as (f + 0.5) / 2 (mg_hid_map_offset). */
        boolean offset = x < 0 || y < 0 || x + w > 1 || y + h > 1;
        double[] f = {offset ? (x + 0.5) / 2 : x, offset ? (y + 0.5) / 2 : y, w, h};
        for (int i = 0; i < 4; i++) {
            int q = (int) Math.round(Math.max(0, Math.min(1, f[i])) * 65535);
            m[3 + 2 * i] = (byte) q;
            m[4 + 2 * i] = (byte) (q >> 8);
        }
        m[11] = (byte) ((fisheye ? Mg.HID_MAP_FISHEYE : 0) | (offset ? Mg.HID_MAP_OFFSET : 0));
        if (mode == Mg.HID_MOUSE_REL) {
            m[12] = (byte) countsW;
            m[13] = (byte) (countsW >> 8);
            m[14] = (byte) countsH;
            m[15] = (byte) (countsH >> 8);
        }
        sendControl(m);
    }

    /** Bonds with the gadget (its HID reports need an encrypted, bonded link). */
    boolean bond() {
        BluetoothGatt g = gatt;
        if (g == null) {
            return false;
        }
        BluetoothDevice d = g.getDevice();
        if (d.getBondState() == BluetoothDevice.BOND_BONDED) {
            return true;
        }
        /* Android's bond-time discovery (the device's UUIDs, which decide whether its HID host
         * attaches) reads its GATT cache. A HID service the gadget showed after this connection's
         * discovery (hid_map turned HID on) isn't in it: the gadget's Service Changed doesn't
         * reach an unbonded phone. So drop the cache first; bonding then discovers afresh. */
        try {
            java.lang.reflect.Method refresh = g.getClass().getMethod("refresh");
            Log.i(TAG, "GATT cache dropped before bonding: " + refresh.invoke(g));
        } catch (ReflectiveOperationException | RuntimeException e) {
            Log.w(TAG, "couldn't drop the GATT cache before bonding: " + e);
        }
        Log.i(TAG, "bonding with " + d.getAddress());
        return d.createBond();
    }

    /** BluetoothDevice.BOND_* of the connected gadget, or -1. */
    int bondState() {
        BluetoothGatt g = gatt;
        return g != null ? g.getDevice().getBondState() : -1;
    }

    /** Bench: device_action throughput_test receive for secs; writes as fast as Android takes them. */
    void benchReceive(int secs) {
        new Thread(() -> {
            sendControl(new byte[] {30, 11, 2, (byte) secs, (byte) (secs >> 8)});
            sendControl(new byte[] {(byte) Mg.CMD_CHANGE_DATA_TYPE, 20});
            SystemClock.sleep(1500);   /* the L2CAP channel opens meanwhile */
            BluetoothGatt g = gatt;
            int n = cocOut != null ? 2048 : Math.min(writeMax, mtu - 3);
            Log.i(TAG, "bench over " + (cocOut != null ? "L2CAP" : "GATT") + ", " + n + " B writes");
            long end = SystemClock.elapsedRealtime() + secs * 1000L, t0 = SystemClock.elapsedRealtime();
            long bytes = 0;
            int seq = 0;
            while (SystemClock.elapsedRealtime() < end && g != null) {
                byte[] b = new byte[n];
                b[0] = (byte) seq; b[1] = (byte) (seq >> 8); b[2] = (byte) (seq >> 16); b[3] = (byte) (seq >> 24);
                for (int i = 4; i < n; i++) b[i] = (byte) (seq + i - 4);
                java.io.OutputStream out = cocOut;
                if (out != null) {
                    try {
                        out.write(b);
                    } catch (java.io.IOException e) {
                        break;
                    }
                } else {
                    synchronized (lock) {
                        if (!write(g, data, b)) break;
                    }
                }
                bytes += n;
                seq++;
            }
            long ms = SystemClock.elapsedRealtime() - t0;
            Log.i(TAG, "bench: phone wrote " + bytes + " bytes in " + ms + " ms = " + (bytes * 8 / Math.max(1, ms)) + " kb/s");
            sendControl(new byte[] {30, 11, 0});
        }, "bench").start();
    }

    /** Plays SBC (16 kHz mono, frameBytes per frame) on the gadget, looped, beside any video. */
    void startAudio(byte[] sbcFrames, int frameBytes) {
        startAudio(sbcFrames, frameBytes, false);
    }

    /** lc3: 10 ms LC3 frames of frameBytes (16 kHz mono) instead of SBC. */
    void startAudio(byte[] sbcFrames, int frameBytes, boolean lc3) {
        synchronized (lock) {
            audioType = lc3 ? TYPE_LC3 : TYPE_SBC;
            audioFrameUs = lc3 ? 10000 : 8000;
            sbc = sbcFrames;
            sbcFrameBytes = frameBytes;
            sbcPos = 0;
            audioFramesSent = 0;
            aAvail = aRecv = aSent = 0;
            audioStartNs = 0;
            audioOn = true;
            txType = streaming ? videoType : audioType;   /* a second stream: Data stays with the first */
            controlQ.add(new byte[] {(byte) Mg.CMD_STREAM_AUDIO, 0});   /* request_capabilities: logged */
            /* audioParams (bench, --ei aparams): 3 codec + rate + frame size (default), 2 codec + rate, 1 codec only */
            /* SBC: no frame_size (each SBC header carries its length; some gadgets refuse one). */
            controlQ.add(audioParams >= 3 && lc3 ? new byte[] {(byte) Mg.CMD_STREAM_AUDIO, 2, 3, 1, (byte) audioType, 2,
                                     (byte) 0x80, (byte) 0x3e, 0, 0, 5, (byte) frameBytes, (byte) (frameBytes >> 8)}
                    : audioParams >= 2 ? new byte[] {(byte) Mg.CMD_STREAM_AUDIO, 2, 2, 1, (byte) audioType, 2, (byte) 0x80, (byte) 0x3e, 0, 0}
                    : new byte[] {(byte) Mg.CMD_STREAM_AUDIO, 2, 1, 1, (byte) audioType});
            lock.notifyAll();
        }
        Log.i(TAG, "audio: start_streaming " + (lc3 ? "LC3" : "SBC") + " 16 kHz (" + frameBytes + " B frames)");
    }

    void stopAudio() {
        synchronized (lock) {
            if (!audioOn) {
                return;
            }
            audioOn = false;
            txType = videoType;
            controlQ.add(new byte[] {(byte) Mg.CMD_STREAM_AUDIO, 3, (byte) Mg.BUFFER_DROP});
            lock.notifyAll();
        }
    }

    boolean audioOn() {
        return audioOn;
    }

    /* Under lock: how many audio frames are due and credited for the next write (0: none yet). */
    /* Audio frames per Data write: whole frames only (a gadget refuses a write that splits
     * one), as many as fit one ATT write (MTU - 3) up to AUDIO_FRAMES_PER_WRITE: on an MTU of
     * 247, 6 LC3 frames of 40 B or 4 SBC frames of 60 B. */
    private int audioPerWrite() {
        int room = cocOut != null ? 2048 : Math.min(writeMax, Math.max(20, mtu - 3));
        return Math.max(1, Math.min(AUDIO_FRAMES_PER_WRITE, room / Math.max(1, sbcFrameBytes)));
    }

    private int audioMinPerWrite() {
        return Math.min(AUDIO_MIN_FRAMES_PER_WRITE, audioPerWrite());
    }

    private int audioFramesDue() {
        if (!audioOn || sbc == null || aAvail == 0) {
            return 0;
        }
        long now = SystemClock.elapsedRealtimeNanos();
        if (audioStartNs == 0) {
            audioStartNs = now;
        }
        long due = (now - audioStartNs) / 1000 / audioFrameUs + AUDIO_PREBUFFER_FRAMES - audioFramesSent;
        long credit = (aAvail - (aSent - aRecv)) / sbcFrameBytes;
        boolean starved = due >= audioMinPerWrite() && credit < audioMinPerWrite();
        if (starved && audioStarveNs == 0) {
            audioStarveNs = now;
        } else if (!starved && audioStarveNs != 0) {
            statAudioStarveUs += (now - audioStarveNs) / 1000;
            audioStarveNs = 0;
        }
        int k = (int) Math.min(Math.min(due, credit), audioPerWrite());
        return k < audioMinPerWrite() ? 0 : k;
    }

    /* Under lock: takes the next k frames of the loop. */
    private byte[] takeAudio(int k) {
        byte[] out = new byte[k * sbcFrameBytes];
        int frames = sbc.length / sbcFrameBytes;
        for (int i = 0; i < k; i++) {
            System.arraycopy(sbc, sbcPos * sbcFrameBytes, out, i * sbcFrameBytes, sbcFrameBytes);
            sbcPos = (sbcPos + 1) % frames;
        }
        return out;
    }

    void sendControl(byte[] m) {
        synchronized (lock) {
            controlQ.add(m);
            lock.notifyAll();
        }
    }

    /** start_streaming; credits start empty until the device's buffer_update. */
    void startStreaming(int w, int h, int fps, int kbps) {
        startStreaming(w, h, fps, kbps, 0);
    }

    /** A UI stream (content ui): H.264 in that many bands. */
    @Override
    void startUiStreaming(int w, int h, int fps, int kbps, int bands) {
        startStreaming(w, h, fps, kbps, 0, bands, false, true);
    }

    /**
     * playoutMs: ask the device to show the frames evenly this long after they start (pre-recorded clips);
     * bands: the picture as that many horizontal bands, each its own stream (Mg.VP_TILES); vp8: VP8 (data type 25), else H.264.
     */
    @Override
    void startStreaming(int w, int h, int fps, int kbps, int playoutMs, int bands, boolean vp8) {
        startStreaming(w, h, fps, kbps, playoutMs, bands, vp8, false);
    }

    private void startStreaming(int w, int h, int fps, int kbps, int playoutMs, int bands, boolean vp8, boolean ui) {
        if (ui && proofRequired && !proven) {
            Log.i(TAG, "start_streaming: the gadget hasn't proved its key; not sent");
            return;
        }
        int codec = vp8 ? Mg.DATA_TYPE_VIDEO_VP8 : Mg.DATA_TYPE_VIDEO_H264;
        videoType = codec;
        playoutMs = Math.min(playoutMs, caps.maxPlayoutMs);
        Log.i(TAG, "start_streaming " + (ui ? "UI " : "") + (vp8 ? "VP8 " : "H.264 ") + w + "x" + h + "@" + fps + " " + kbps + " kb/s, playout " + playoutMs
                + " ms (was streaming: " + streaming + ")");
        /* The device holds the playout delay's worth unread on top. */
        maxAhead = Math.max(12288, kbps * 1000L / 8 * (150 + playoutMs) / 1000);
        BluetoothGatt g = gatt;
        if (g != null && connInterval * 1.25 > 16) {
            lastPriorityMs = SystemClock.elapsedRealtime();
            g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
        }
        byte[] m = new byte[] {
            (byte) Mg.CMD_STREAM_VIDEO, (byte) Mg.V_START_STREAMING, (byte) (playoutMs > 0 ? 6 : 5),
            (byte) Mg.VP_CODEC, (byte) codec,
            (byte) Mg.VP_WIDTH, (byte) w, (byte) (w >> 8),
            (byte) Mg.VP_HEIGHT, (byte) h, (byte) (h >> 8),
            (byte) Mg.VP_FPS, (byte) fps,
            (byte) Mg.VP_BITRATE, (byte) kbps, (byte) (kbps >> 8),
            (byte) Mg.VP_PLAYOUT, (byte) playoutMs, (byte) (playoutMs >> 8),
        };
        if (playoutMs <= 0) {
            m = java.util.Arrays.copyOf(m, m.length - 3);
        }
        if (bands > 1) {
            m = java.util.Arrays.copyOf(m, m.length + 2);
            m[2]++;
            m[m.length - 2] = (byte) Mg.VP_TILES;
            m[m.length - 1] = (byte) bands;
        }
        if (ui) {
            m = java.util.Arrays.copyOf(m, m.length + 2);
            m[2]++;
            m[m.length - 2] = (byte) Mg.VP_CONTENT;
            m[m.length - 1] = (byte) Mg.CONTENT_UI;
            /* A UI keyframe is far bigger than a camera frame: let the whole
             * frame be on its way (the device's ring holds it). */
            maxAhead = Math.max(maxAhead, caps.uiMaxFrame + 4096);
        }
        synchronized (lock) {
            frameQ.clear();
            cur = null;
            queuedBytes = 0;
            sent = 0;
            creditAvail = 0;   /* nothing until the device's first buffer_update: Data could overtake start_streaming */
            creditRecv = 0;
            streaming = true;
            txType = audioOn ? audioType : videoType;   /* a second stream: Data stays with the first */
            controlQ.add(m);
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
            controlQ.add(new byte[] {(byte) Mg.CMD_STREAM_VIDEO, (byte) Mg.V_STOP_STREAMING, (byte) Mg.BUFFER_DROP});
            lock.notifyAll();
        }
    }

    @Override
    boolean streaming() {
        return streaming;
    }

    /** Queues one frame (header included). */
    @Override
    void sendFrame(byte[] frame) {
        if (proofRequired && !proven) {
            return;
        }
        synchronized (lock) {
            if (!streaming) {
                return;
            }
            frameQ.add(frame);
            queuedBytes += frame.length;
            lock.notifyAll();
        }
    }

    /** Frame bytes waiting to go (not counting what's in flight). */
    @Override
    long queuedBytes() {
        synchronized (lock) {
            return queuedBytes;
        }
    }

    /** Bytes the device hasn't taken out of its ring yet, as far as we know. */
    @Override
    long inFlight() {
        synchronized (lock) {
            return sent - creditRecv;
        }
    }

    private void sendLoop() {
        while (!Thread.currentThread().isInterrupted()) {
            byte[] ctl = null, chunk = null;
            boolean audioChunk = false;
            int frameDone = -1;   /* the chunk ends this frame (its number) */
            BluetoothGattCharacteristic ch;
            synchronized (lock) {
                while (true) {
                    if (!ready) {
                        waitLock(50);
                        continue;
                    }
                    if (!controlQ.isEmpty()) {
                        ctl = controlQ.poll();
                        break;
                    }
                    int k = audioFramesDue();
                    if (k > 0) {
                        /* The switch goes first and the frames stay put: they
                         * go out on the next pass (taking them here and
                         * sending the switch instead lost them, a skip and a
                         * click in the tone at every switch). */
                        if (txType != audioType) {
                            txType = audioType;
                            statTypeSwitches++;
                            ctl = new byte[] {(byte) Mg.CMD_CHANGE_DATA_TYPE, (byte) audioType};
                            break;
                        }
                        byte[] a = takeAudio(k);
                        chunk = a;
                        audioFramesSent += a.length / sbcFrameBytes;
                        aSent += a.length;
                        statAudioBytes += a.length;
                        audioChunk = true;
                        break;
                    }
                    if (streaming) {
                        if (cur == null && !frameQ.isEmpty()) {
                            /* A new frame starts only while the device holds
                             * little: what's in the air plus what waits in its
                             * ring, unread. That bounds the latency when the
                             * decoder is the slow end; the camera then skips
                             * frames here instead of the watch falling behind.
                             * (A frame once started goes whole: the device can't
                             * free a frame it hasn't all of.) */
                            long backlog = sent - creditRecv + (bufferSize - Math.min(creditAvail, bufferSize));
                            if (backlog > (liveAhead > 0 ? liveAhead : maxAhead)) {
                                long t0 = SystemClock.elapsedRealtimeNanos();
                                waitLock(10);
                                statGateUs += (SystemClock.elapsedRealtimeNanos() - t0) / 1000;
                                continue;
                            }
                            cur = frameQ.poll();
                            curOff = 0;
                        }
                        if (cur != null) {
                            /* ATT caps an attribute value at 512 bytes, whatever the MTU;
                             * the channel takes bigger pieces. */
                            int max = cocOut != null ? 2048 : Math.min(writeMax, Math.max(20, mtu - 3));
                            int n = Math.min(max, cur.length - curOff);
                            /* Inside the device's credits, and no more than
                             * maxAhead in the air: Android completes a Write
                             * Without Response once its own stack has queued it,
                             * so anything more is latency. */
                            /* airCap is tuned for a 15 ms interval; a longer one
                             * needs more in the air to carry the same rate. */
                            long cap = airCap * Math.max(12, connInterval) / 12;
                            if (sent - creditRecv + n <= Math.min(Math.min(creditAvail, bufferSize), cap)) {
                                if (txType != videoType && cocOut == null) {
                                    txType = videoType;
                                    statTypeSwitches++;
                                    ctl = new byte[] {(byte) Mg.CMD_CHANGE_DATA_TYPE, (byte) videoType};
                                    break;
                                }
                                chunk = new byte[n];
                                System.arraycopy(cur, curOff, chunk, 0, n);
                                curOff += n;
                                sent += n;
                                queuedBytes -= n;
                                if (curOff >= cur.length) {
                                    frameDone = (cur[2] & 0xff) | (cur[3] & 0xff) << 8;
                                    cur = null;
                                }
                                break;
                            }
                            long t0 = SystemClock.elapsedRealtimeNanos();
                            waitLock(20);   /* out of credits: wait for buffer_update */
                            statStallUs += (SystemClock.elapsedRealtimeNanos() - t0) / 1000;
                            continue;
                        }
                    }
                    waitLock(20);
                }
            }
            BluetoothGatt g = gatt;
            if (g == null) {
                continue;
            }
            ch = ctl != null ? control : data;
            byte[] v = ctl != null ? ctl : chunk;
            java.io.OutputStream out = audioChunk ? null : cocOut;
            if (chunk != null && out != null) {
                try {
                    out.write(chunk);
                    statBytes += chunk.length;
                    statWrites++;
                } catch (java.io.IOException e) {
                    Log.w(TAG, "L2CAP write failed; back to GATT", e);
                    closeChannel();
                }
                continue;
            }
            if (!write(g, ch, v)) {
                Log.w(TAG, "write failed");
            } else if (chunk != null) {
                statBytes += v.length;
                statWrites++;
            }
            LatencyMeter m = meter;
            if (frameDone >= 0 && m != null) {
                m.sent(frameDone, SystemClock.elapsedRealtimeNanos());
            }
        }
    }

    private void waitLock(long ms) {
        try {
            lock.wait(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new RuntimeException(e);
        }
    }

    private boolean write(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] v) {
        for (int tries = 0; tries < 500; tries++) {
            int rc;
            try {
                rc = g.writeCharacteristic(c, v, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
            } catch (Exception e) {
                return false;
            }
            if (rc == BluetoothStatusCodes.SUCCESS) {
                return await("write");
            }
            if (rc != BluetoothStatusCodes.ERROR_GATT_WRITE_REQUEST_BUSY) {
                Log.w(TAG, "writeCharacteristic rc " + rc);
                return false;
            }
            SystemClock.sleep(1);
        }
        return false;
    }
}
