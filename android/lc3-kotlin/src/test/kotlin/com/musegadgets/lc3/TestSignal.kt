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
package com.musegadgets.lc3

/**
 * A deterministic 3 s, 16 kHz test signal built with integer arithmetic only (identical on every JVM), so the
 * conformance vectors need no audio files. It exercises what the codec's paths care about:
 *
 * - 0.0-1.2 s: voiced speech-like sound: a glottal pulse train gliding 110 -> 230 Hz through three formant
 *   resonators, with breath noise and a syllable envelope (pitch for the long-term postfilter, TNS, SNS).
 * - 1.2-1.8 s: noise bursts with sharp attacks and digital silence in between (attack/TNS, zero frames).
 * - 1.8-2.2 s: low-pass-only content (narrowband detection), then a full-scale square wave (saturation).
 * - 2.2-3.0 s: a triangle-wave chirp sweeping 100 Hz -> 7.6 kHz (all bands, high spectral peaks).
 */
object TestSignal {
    const val SAMPLE_RATE = 16000
    const val SAMPLES = 3 * SAMPLE_RATE

    private var seed = 0x2545F491

    private fun noise(): Int { // 15-bit signed noise, LCG (Numerical Recipes constants)
        seed = seed * 1664525 + 1013904223
        return ((seed shr 16) and 0x7fff) - 0x4000
    }

    private fun sat(v: Long): Short = (if (v > 32767) 32767 else if (v < -32768) -32768 else v).toShort()

    /** Second-order resonator in Q14: y = x*g + a1*y1 - a2*y2. */
    private class Resonator(private val a1: Long, private val a2: Long, private val g: Long) {
        private var y1 = 0L
        private var y2 = 0L
        fun step(x: Long): Long {
            val y = (x * g + a1 * y1 - a2 * y2) shr 14
            y2 = y1
            y1 = y
            return y
        }
    }

    @JvmStatic
    fun generate(): ShortArray {
        seed = 0x2545F491
        val out = ShortArray(SAMPLES)
        // Formants ~700 Hz (r 0.96), ~1200 Hz (r 0.94), ~2500 Hz (r 0.90), constants precomputed in Q14.
        val f1 = Resonator(30278, 15099, 900)
        val f2 = Resonator(27443, 14477, 1200)
        val f3 = Resonator(16384, 13271, 1600)
        var phase = 0L // pulse phase, Q16 periods
        var n = 0
        // 0.0 - 1.2 s: voiced
        while (n < 19200) {
            val f0 = 110 + 120 * n / 19200 // Hz
            phase += (f0.toLong() shl 16) / SAMPLE_RATE
            var x = 0L
            if (phase >= 65536) {
                phase -= 65536
                x = 24000
            }
            x += noise() / 16
            val env = run { // syllables: 4 per second, raised-triangle envelope in Q8
                val p = n % 4000
                if (p < 2000) 32 + p * 224 / 2000 else 256 - (p - 2000) * 224 / 2000
            }
            val v = f1.step(x) + f2.step(x) / 2 + f3.step(x) / 4
            out[n] = sat(v * env shr 8)
            n++
        }
        // 1.2 - 1.8 s: noise bursts (40 ms on, 60 ms off, rising level), silence between
        while (n < 28800) {
            val p = (n - 19200) % 1600
            out[n] = if (p < 640) sat(noise().toLong() * (1 + (n - 19200) / 1600) / 3) else 0
            n++
        }
        // 1.8 - 2.0 s: low-pass content only (2-pole lowpass of noise, ~1 kHz)
        var l1 = 0L
        var l2 = 0L
        while (n < 32000) {
            l1 += (noise() * 4L - l1) shr 3
            l2 += (l1 - l2) shr 3
            out[n] = sat(l2)
            n++
        }
        // 2.0 - 2.2 s: full-scale square wave, 250 Hz
        while (n < 35200) {
            out[n] = if ((n / 32) % 2 == 0) 32767 else -32768
            n++
        }
        // 2.2 - 3.0 s: triangle chirp 100 Hz -> 7.6 kHz
        var ph = 0L // Q32 phase in a Long
        while (n < SAMPLES) {
            val k = (n - 35200).toLong()
            val f = 100 + 7500 * k / 12800
            ph = (ph + (f shl 32) / SAMPLE_RATE) and 0xffffffffL
            val tri = if (ph < 0x80000000L) (ph shr 15) - 32768 else 98303 - (ph shr 15)
            out[n] = sat(tri * 3 / 4)
            n++
        }
        return out
    }
}
