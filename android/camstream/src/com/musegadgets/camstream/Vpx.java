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
 * libvpx's VP8 encoder through jni/mgvpx.c (libmgvpx.so, built by
 * jni/build_vpx.sh; libvpx is BSD). The same calls as X264: one picture in,
 * one VP8 frame out, no lookahead, CBR with a short buffer.
 *
 * params "name=value:..." (defaults in brackets): cpu [2] (libvpx's
 * cpu-used), deadline [good] (good or rt), profile [1] (1: bilinear
 * prediction, cheap for a gadget to decode), lf [0] (0: no loop filter,
 * the cheapest decode; -1: libvpx picks), mvcheap [1] (no motion vectors
 * with both components fractional: one interpolation pass in the decoder),
 * threads [1] (two cost 4-15% more bits at 232 px), vbv-ms [200], vbr [0]
 * (1: VBR, for clips encoded ahead of time). Keyframes come only when asked.
 */
final class Vpx {
    private static Boolean loaded;

    static synchronized boolean available() {
        if (loaded == null) {
            try {
                System.loadLibrary("mgvpx");
                loaded = true;
            } catch (Throwable t) {
                loaded = false;
            }
        }
        return loaded;
    }

    /** 0 on failure. preset is unused (params carry the settings), kept for X264's signature. */
    static native long open(int w, int h, int fps, int kbps, String preset, String params);

    /** i420: w*h luma then the two quarter planes. info[0]: keyframe, info[1]: quantizer (0-63). Null: no output. */
    static native byte[] encode(long h, byte[] i420, long pts, boolean key, int[] info);

    static native void setBitrate(long h, int kbps);

    static native void close(long h);
}
