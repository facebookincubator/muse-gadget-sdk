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
 * The header before each access unit in a stream (protocols/VIDEO.md):
 * magic, flags, uint16 frame number, uint32 access unit length, little-endian.
 */
final class VideoFrame {
    private VideoFrame() {}

    static final int MAGIC = 0x56;   /* 'V' */
    static final int HEADER_LEN = 8;
    static final int KEYFRAME = 1;

    /** The header and the access unit, as one frame to send. */
    static byte[] make(byte[] au, int len, int number, boolean key) {
        byte[] fr = new byte[HEADER_LEN + len];
        fr[0] = (byte) MAGIC;
        fr[1] = (byte) (key ? KEYFRAME : 0);
        fr[2] = (byte) number;
        fr[3] = (byte) (number >> 8);
        fr[4] = (byte) len;
        fr[5] = (byte) (len >> 8);
        fr[6] = (byte) (len >> 16);
        fr[7] = (byte) (len >> 24);
        System.arraycopy(au, 0, fr, HEADER_LEN, len);
        return fr;
    }

    static int number(byte[] frame) {
        return (frame[2] & 0xff) | (frame[3] & 0xff) << 8;
    }
}
