/*
 * Copyright 2022 Google LLC
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
 *
 * Configuration helpers, from liblc3 v1.1.3 lc3.c (lc3_frame_samples, lc3_frame_bytes, lc3_resolve_bitrate,
 * lc3_delay_samples). Modified by Meta.
 */
package com.musegadgets.lc3

/** LC3 constants and the configuration arithmetic of liblc3's `lc3.h`, for the configurations this library supports. */
object Lc3 {
    /** The only sample rate supported. */
    const val SAMPLE_RATE_16K = 16000

    /** 10 ms frames: 160 samples. */
    const val FRAME_10MS = 10000

    /** 7.5 ms frames: 120 samples. */
    const val FRAME_7_5MS = 7500

    /** 40 bytes a 10 ms frame = 32 kb/s, the Muse Gadgets protocol's LC3 setting. */
    const val DEFAULT_FRAME_BYTES = 40

    const val MIN_FRAME_BYTES = 20
    const val MAX_FRAME_BYTES = 400

    /** PCM samples per frame. */
    @JvmStatic
    @JvmOverloads
    fun frameSamples(frameDurationUs: Int, sampleRateHz: Int = SAMPLE_RATE_16K): Int {
        check(frameDurationUs, sampleRateHz)
        return sampleRateHz / 100 * frameDurationUs / 10000
    }

    /**
     * Frame size in bytes for a bitrate in bits/s, clamped to [MIN_FRAME_BYTES]..[MAX_FRAME_BYTES] (lc3_frame_bytes).
     * A negative bitrate throws [IllegalArgumentException].
     */
    @JvmStatic
    fun frameBytes(frameDurationUs: Int, bitrate: Int): Int {
        val dt = dt(frameDurationUs)
        if (bitrate < 0) throw IllegalArgumentException("bitrate $bitrate: negative")
        val b = (minOf(bitrate, 8 * 672000).toLong() * (1 + dt) / 3200).toInt()
        return minOf(maxOf(b, MIN_FRAME_BYTES), MAX_FRAME_BYTES)
    }

    /** Bitrate in bits/s of frames of [frameBytes] bytes (lc3_resolve_bitrate). A negative size throws [IllegalArgumentException]. */
    @JvmStatic
    fun bitrate(frameDurationUs: Int, frameBytes: Int): Int {
        val dt = dt(frameDurationUs)
        if (frameBytes < 0) throw IllegalArgumentException("frame size $frameBytes bytes: negative")
        return minOf((frameBytes.toLong() * 3200 + dt) / (1 + dt), Int.MAX_VALUE.toLong()).toInt()
    }

    /** Algorithmic delay in samples (lc3_delay_samples): decoded PCM lags the input by this much. */
    @JvmStatic
    @JvmOverloads
    fun delaySamples(frameDurationUs: Int, sampleRateHz: Int = SAMPLE_RATE_16K): Int {
        val c = Lc3Config(frameDurationUs, sampleRateHz)
        return 2 * c.nd - c.ns
    }

    /**
     * Runs [frames] frames of noise through a throwaway encoder and decoder of each frame duration, so a JIT-only
     * runtime compiles the codec before a call needs it (an app with a baseline profile is AOT-compiled already).
     * Call it off the audio thread, at call setup. Allocates; takes a few to tens of milliseconds.
     */
    @JvmStatic
    @JvmOverloads
    fun warmUp(frames: Int = 400) {
        for (dt in intArrayOf(FRAME_10MS, FRAME_7_5MS)) {
            val e = Lc3Encoder(dt)
            val d = Lc3Decoder(dt)
            val pcm = ShortArray(e.frameSamples)
            val out = ByteArray(MAX_FRAME_BYTES)
            var s = 0x1234567
            for (f in 0 until (if (dt == FRAME_10MS) frames else frames / 4)) {
                val level = 64 shl (f % 8) // quiet to loud
                for (i in pcm.indices) {
                    s = s * 1664525 + 1013904223
                    pcm[i] = ((s shr 16) % level).toShort()
                }
                val nbytes = if (f % 5 == 4) 100 else DEFAULT_FRAME_BYTES
                e.encode(pcm, 0, out, 0, nbytes)
                if (f % 16 == 15) d.conceal(pcm, 0) else d.decode(out, 0, nbytes, pcm, 0)
            }
        }
    }

    private fun dt(frameDurationUs: Int): Int = when (frameDurationUs) {
        FRAME_7_5MS -> 2
        FRAME_10MS -> 3
        else -> throw IllegalArgumentException("frame duration $frameDurationUs us: only 7500 and 10000 are supported")
    }

    private fun check(frameDurationUs: Int, sampleRateHz: Int) {
        dt(frameDurationUs)
        if (sampleRateHz != SAMPLE_RATE_16K) throw IllegalArgumentException("sample rate $sampleRateHz Hz: only 16000 is supported")
    }
}
