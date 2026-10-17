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
 * libx264 through jni/mgx264.c (libmgx264.so, built by jni/build.sh). One
 * picture in, one access unit out: no lookahead, no B frames, one reference,
 * intra refresh, constrained baseline, CBR-like VBV.
 */
final class X264 {
    private static Boolean loaded;

    static synchronized boolean available() {
        if (loaded == null) {
            try {
                System.loadLibrary("mgx264");
                loaded = true;
            } catch (Throwable t) {
                loaded = false;
            }
        }
        return loaded;
    }

    /** params: x264 options "name=value:name=value" (plus vbv-ms=N). 0 on failure. */
    static native long open(int w, int h, int fps, int kbps, String preset, String params);

    /** i420: w*h luma then the two quarter planes (null: flush, a frame the lookahead held, or null when none is left). info[0]: IDR, info[1]: QP. Null: no output. */
    static native byte[] encode(long h, byte[] i420, long pts, boolean idr, int[] info);

    static native void setBitrate(long h, int kbps);

    static native void close(long h);

    /** PanelScaler.scale in C (bit-identical), when this library is present. */
    static native void scale(byte[] src, int rowStride, int pixStride, int x0, int y0, int nIn, int nOut, int[] start,
            int[] taps, int len, byte[] dst);
}
