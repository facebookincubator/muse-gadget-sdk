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

import android.util.Log;

import java.util.Arrays;
import java.util.Locale;

/**
 * Capture -> shown latency, from the gadget's echo of the last frame it drew
 * (its buffer message: frame number and how long ago). Each frame's capture time
 * (the camera's sensor timestamp, or the moment a test source fed it), when
 * the encoder put it out and when its last byte went to the link
 * are kept by frame number; when the gadget reports a frame drawn, the
 * latency is its draw time minus its capture time. The draw time is the
 * report's arrival minus the age the gadget gives, so it includes the
 * report's own way back (a connection interval or so).
 */
final class LatencyMeter {
    private static final String TAG = "MuseCam";
    private static final int N = 4096;   /* frame numbers kept, mod N */
    private final long[] capNs = new long[N], encNs = new long[N], sentNs = new long[N];
    private final int[] gen = new int[N];
    private int lastShown = -1;
    private final long[] lat = new long[512], enc = new long[512], queue = new long[512], air = new long[512];
    private int n;
    private long sumLat;
    volatile boolean perFrame;
    private int countLat;

    synchronized void captured(int frameNo, long captureNs, long encodedNs) {
        int i = frameNo & (N - 1);
        capNs[i] = captureNs;
        encNs[i] = encodedNs;
        sentNs[i] = 0;
        gen[i] = frameNo & 0xffff;
    }

    synchronized void sent(int frameNo, long ns) {
        int i = frameNo & (N - 1);
        if (gen[i] == (frameNo & 0xffff)) {
            sentNs[i] = ns;
        }
    }

    /** num: the gadget's last frame drawn; ageMs: how long before this report. */
    synchronized void shown(int num, int ageMs, long rxNs) {
        if (ageMs == 0xffff || num == lastShown) {
            return;
        }
        lastShown = num;
        int i = num & (N - 1);
        if (gen[i] != num || capNs[i] == 0 || n >= lat.length) {
            return;
        }
        long drawn = rxNs - ageMs * 1_000_000L;
        lat[n] = (drawn - capNs[i]) / 1000;
        enc[n] = (encNs[i] - capNs[i]) / 1000;
        queue[n] = sentNs[i] > 0 ? (sentNs[i] - encNs[i]) / 1000 : -1;
        air[n] = sentNs[i] > 0 ? (drawn - sentNs[i]) / 1000 : -1;
        sumLat += lat[n];
        countLat++;
        if (perFrame) {
            Log.i(TAG, String.format(Locale.US, "lf %d %d %d %d %d", num, lat[n] / 1000, enc[n] / 1000,
                    queue[n] / 1000, air[n] / 1000));
        }
        n++;
    }

    private static long pct(long[] a, int n, int p) {
        long[] c = Arrays.copyOf(a, n);
        Arrays.sort(c);
        int k = 0;
        while (k < n && c[k] < 0) {
            k++;
        }
        if (k >= n) {
            return -1;
        }
        return c[k + (n - 1 - k) * p / 100];
    }

    /** One line of the last interval's numbers (ms), and starts the next. */
    synchronized String take() {
        if (n == 0) {
            return "latency: no frames reported";
        }
        String s = String.format(Locale.US,
                "latency capture->shown med %d p90 %d max %d ms (n %d) | capture->encoded %d, encoded->sent %d, sent->shown %d",
                pct(lat, n, 50) / 1000, pct(lat, n, 90) / 1000, pct(lat, n, 100) / 1000, n,
                pct(enc, n, 50) / 1000, pct(queue, n, 50) / 1000, pct(air, n, 50) / 1000);
        n = 0;
        return s;
    }

    /** Mean capture->shown since the last reset (ms), for a run summary. */
    synchronized String total() {
        String s = String.format(Locale.US, "latency run mean %.1f ms over %d frames",
                countLat > 0 ? sumLat / 1000.0 / countLat : -1.0, countLat);
        sumLat = 0;
        countLat = 0;
        return s;
    }

    static void log(String s) {
        Log.i(TAG, s);
    }
}
