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
 * LC3 decoder: Kotlin translation of liblc3 v1.1.3 (lc3.c, spec.c, tns.c, sns.c, ltpf.c, plc.c) for
 * 16 kHz mono. Modified by Meta.
 */
package com.musegadgets.lc3

import kotlin.math.abs

/**
 * LC3 decoder for 16 kHz mono, to 16-bit PCM: the complete decoder (noise filling, TNS, SNS, long-term
 * postfilter, packet-loss concealment), so it plays any LC3 stream at that rate.
 *
 * Bit-exact with liblc3 v1.1.3 built without FMA contraction, including concealment of lost and corrupt frames.
 *
 * Allocates nothing per frame. Not thread-safe: use one instance per stream.
 *
 * @param frameDurationUs [Lc3.FRAME_10MS] (160 samples a frame) or [Lc3.FRAME_7_5MS] (120 samples).
 * @param sampleRateHz [Lc3.SAMPLE_RATE_16K], the only rate supported.
 */
@Suppress("MagicNumber", "LargeClass", "LongMethod", "ComplexMethod", "TooManyFunctions", "NestedBlockDepth")
class Lc3Decoder(val frameDurationUs: Int, val sampleRateHz: Int) {
    /** 16 kHz, frames of [frameDurationUs]. */
    constructor(frameDurationUs: Int) : this(frameDurationUs, Lc3.SAMPLE_RATE_16K)

    /** 16 kHz, 10 ms frames. */
    constructor() : this(Lc3.FRAME_10MS, Lc3.SAMPLE_RATE_16K)

    private val enableLtpf = true
    private val cfg = Lc3Config(frameDurationUs, sampleRateHz)
    private val dt = cfg.dt
    private val ns = cfg.ns
    private val ne = cfg.ne
    private val nh = cfg.nh
    private val nd = cfg.nd
    private val mdct = Lc3Mdct(cfg)
    private val bits = Lc3Bits()

    init {
        // Initialize the shared tables here rather than in the first frame (up to milliseconds on a little core).
        check(Lc3Shared.VQ_GAINS.size + Lc3Consts.GE_18.size + Lc3Math.dbQ16(1f) != 0)
    }

    /** PCM samples per frame: 160 at 10 ms, 120 at 7.5 ms. */
    val frameSamples: Int get() = ns

    // Buffer: [history + current ring (nh + ns)] [delay nd] [last good spectrum ns]
    private val x = FloatArray(nh + ns + nd + ns)
    private var xsOff = nh
    private val xdOff = nh + ns
    private val xgOff = nh + ns + nd

    // PLC
    private var plcSeed = 24607
    private var plcCount = 1
    private var plcAlpha = 1f

    // LTPF synthesis state
    private var ltpfActive = false
    private var ltpfPitch = 0
    private val ltpfC = FloatArray(8)
    private val ltpfX = FloatArray(3)
    private val cNew = FloatArray(8)
    private val x0a = FloatArray(3)
    private val x0b = FloatArray(3)
    private val u = FloatArray(4)

    // Side data
    private var bw = 0
    private var nq = 0
    private var lsbMode = false
    private var gIdx = 0
    private var tnsLpcW = false
    private var tnsOrder = 0
    private val tnsRc = IntArray(8)
    private var pitchPresent = false
    private var ltpfDataActive = false
    private var ltpfPitchIndex = 0
    private var snsLfcb = 0
    private var snsHfcb = 0
    private var snsShape = 0
    private var snsGain = 0
    private var snsIdxA = 0
    private var snsLsA = 0
    private var snsIdxB = 0
    private var snsLsB = 0
    private var nfSeed = 0

    // Scratch
    private val c = IntArray(16)
    private val cn = FloatArray(16)
    private val scf = FloatArray(16)
    private val scf64 = FloatArray(64)
    private val rc = FloatArray(8)
    private val s = FloatArray(8)

    /**
     * Decodes the LC3 frame `frame[offset until offset + length]` into [frameSamples] samples at `pcm[pcmOffset]`.
     *
     * A corrupt frame is concealed (as liblc3 does) and the call returns false.
     *
     * @param length the frame's size in bytes, [Lc3.MIN_FRAME_BYTES]..[Lc3.MAX_FRAME_BYTES].
     * @return true when the frame decoded, false when it was corrupt and concealed instead.
     */
    fun decode(frame: ByteArray, offset: Int, length: Int, pcm: ShortArray, pcmOffset: Int): Boolean {
        if (length < Lc3.MIN_FRAME_BYTES || length > Lc3.MAX_FRAME_BYTES) {
            throw IllegalArgumentException("frame length $length outside ${Lc3.MIN_FRAME_BYTES}..${Lc3.MAX_FRAME_BYTES}")
        }
        if (offset < 0 || offset > frame.size - length) throw IndexOutOfBoundsException("frame too short")
        checkPcm(pcm, pcmOffset)
        return run(frame, offset, length, pcm, pcmOffset)
    }

    /** Decodes a whole [frame] into a new array of [frameSamples] samples (concealed if corrupt). */
    fun decode(frame: ByteArray): ShortArray {
        val pcm = ShortArray(ns)
        decode(frame, 0, frame.size, pcm, 0)
        return pcm
    }

    /** Packet-loss concealment: synthesizes [frameSamples] samples at `pcm[pcmOffset]` for a lost frame. */
    fun conceal(pcm: ShortArray, pcmOffset: Int) {
        checkPcm(pcm, pcmOffset)
        run(null, 0, 0, pcm, pcmOffset)
    }

    /** Forgets the stream: the next frame decodes as the first of a new stream. */
    fun reset() {
        java.util.Arrays.fill(x, 0f)
        xsOff = nh
        plcSeed = 24607
        plcCount = 1
        plcAlpha = 1f
        ltpfActive = false
        ltpfPitch = 0
        java.util.Arrays.fill(ltpfC, 0f)
        java.util.Arrays.fill(ltpfX, 0f)
    }

    private fun checkPcm(pcm: ShortArray, pcmOffset: Int) {
        if (pcmOffset < 0 || pcmOffset > pcm.size - ns) throw IndexOutOfBoundsException("pcm too short for one frame")
    }

    private fun run(input: ByteArray?, off: Int, nbytes: Int, pcm: ShortArray, pcmOff: Int): Boolean {
        val ok = input != null && decodeFrame(input, off, nbytes) >= 0
        bits.release()
        synthesize(ok, nbytes)
        store(pcm, pcmOff)
        xsOff = if (xsOff < nh) xsOff + ns else 0
        return ok
    }

    // ================================================================= Bitstream

    private fun decodeFrame(input: ByteArray, off: Int, nbytes: Int): Int {
        val b = bits
        b.setup(true, input, off, nbytes)
        bw = b.getBits(1)
        // spectral side
        nq = (b.getBits(Lc3Shared.nbitsNq(ne)) + 1) shl 1
        lsbMode = b.getBit() != 0
        gIdx = b.getBits(8)
        if (nq > ne) { nq = ne; return -1 }
        // TNS
        tnsLpcW = Lc3Shared.lpcWeighting(dt, nbytes)
        tnsOrder = b.getBit()
        if (tnsOrder != 0) {
            tnsOrder += b.getSymbol(TNS_ORDER_M + (if (tnsLpcW) 34 else 0))
            for (i in 0 until tnsOrder) tnsRc[i] = b.getSymbol(TNS_COEF_M + i * 34) - 8
        }
        pitchPresent = b.getBit() != 0
        if (getSns(b) < 0) return -1
        if (pitchPresent) {
            ltpfDataActive = b.getBit() != 0
            ltpfPitchIndex = b.getBits(9)
        }
        // spectrum
        val nf = b.getBits(3)
        if (getQuantized(b, nbytes) < 0) return -1
        val left = b.bitsLeft()
        if (lsbMode) getLsb(b, left) else getResidual(b, left)
        val gInt = gIdx - Lc3Shared.gainOffset(nbytes)
        val g = unquantize(gInt)
        val xf = xsOff
        if (nq > 2 || x[xf] != 0f || x[xf + 1] != 0f || gIdx > 0 || nf < 7) fillNoise(nf, g)
        val r = b.check()
        return r
    }

    private fun getSns(b: Lc3Bits): Int {
        snsLfcb = b.getBits(5)
        snsHfcb = b.getBits(5)
        snsShape = 0; snsIdxA = 0; snsLsA = 0; snsIdxB = 0; snsLsB = 0
        val shapeMsb = b.getBit()
        snsGain = b.getBits(1 + shapeMsb)
        snsLsA = b.getBit()
        var mux = b.getBits(25 - shapeMsb)
        if (shapeMsb == 0) {
            val sizeA = 2390004
            if (mux >= sizeA * 14) return -1
            snsIdxA = mux % sizeA
            mux /= sizeA
            snsShape = if (mux < 2) 1 else 0
            if (snsShape == 0) {
                snsIdxB = (mux - 2) / 2
                snsLsB = (mux - 2) % 2
            } else {
                snsGain = (snsGain shl 1) + (mux % 2)
            }
        } else {
            val sizeA = 15158272
            if (mux >= sizeA + 1549824) return -1
            snsShape = 2 + (if (mux >= sizeA) 1 else 0)
            if (snsShape == 2) {
                snsIdxA = mux
            } else {
                mux -= sizeA
                snsIdxA = mux / 2
                snsGain = (snsGain shl 1) + (mux % 2)
            }
        }
        return 0
    }

    private fun getQuantized(b: Lc3Bits, nbytes: Int): Int {
        val seed = b.getSpectrum(x, xsOff, nq, ne, lsbMode, Lc3Shared.highRate(nbytes))
        if (seed < 0) return -1
        nfSeed = seed
        return 0
    }

    private fun getResidual(b: Lc3Bits, nbits0: Int) {
        var nbits = nbits0
        val off1 = 5f / 16
        val off2 = 3f / 16
        val xf = xsOff
        var i = 0
        while (i < nq && nbits > 0) {
            val v = x[xf + i]
            if (v != 0f) {
                if (b.getBit() == 0) x[xf + i] = v - (if (v < 0) off1 else off2)
                else x[xf + i] = v + (if (v > 0) off1 else off2)
                nbits--
            }
            i++
        }
    }

    private fun getLsb(b: Lc3Bits, nbits0: Int) {
        var nbits = nbits0
        val xf = xsOff
        var seed = nfSeed
        var i = 0
        while (i < nq && nbits > 0) {
            val a = abs(x[xf + i])
            val bb = abs(x[xf + i + 1])
            if (Lc3Math.fmaxf(a, bb) >= 4) {
                if (nbits-- > 0 && b.getBit() != 0) {
                    if (a != 0f) {
                        x[xf + i] += if (x[xf + i] < 0) -1 else 1
                        seed = (seed + i) and 0xffff
                    } else if (nbits-- > 0) {
                        x[xf + i] = if (b.getBit() != 0) -1f else 1f
                        seed = (seed + i) and 0xffff
                    }
                }
                if (nbits-- > 0 && b.getBit() != 0) {
                    if (bb != 0f) {
                        x[xf + i + 1] += if (x[xf + i + 1] < 0) -1 else 1
                        seed = (seed + i + 1) and 0xffff
                    } else if (nbits-- > 0) {
                        x[xf + i + 1] = if (b.getBit() != 0) -1f else 1f
                        seed = (seed + i + 1) and 0xffff
                    }
                }
            }
            i += 2
        }
        nfSeed = seed
    }

    private fun unquantize(gInt: Int): Float {
        val g = Lc3Shared.unquantizeGain(gInt)
        val xf = xsOff
        var i = 0
        while (i < nq) { x[xf + i] = x[xf + i] * g; i++ }
        while (i < ne) { x[xf + i] = 0f; i++ }
        return g
    }

    private fun fillNoise(nf: Int, g: Float) {
        val xf = xsOff
        val bwStop = 20 * (1 + bw) * (1 + dt)
        val w = 1 + (if (dt >= 2) 1 else 0) + (if (dt >= 3) 1 else 0)
        val sv = g * (8 - nf).toFloat() / 16
        var seed = nfSeed
        var z = 0
        var i = 6 * (1 + dt) - w
        val lim = minOf(nq, bwStop)
        while (i < lim) {
            z = if (x[xf + i] != 0f) 0 else z + 1
            if (z > 2 * w) {
                seed = (13849 + seed * 31821) and 0xffff
                x[xf + i - w] = if (seed and 0x8000 != 0) -sv else sv
            }
            i++
        }
        while (i < bwStop + w) {
            if (++z > 2 * w) {
                seed = (13849 + seed * 31821) and 0xffff
                x[xf + i - w] = if (seed and 0x8000 != 0) -sv else sv
            }
            i++
        }
    }

    // ================================================================= Synthesis

    private fun synthesize(ok: Boolean, nbytes: Int) {
        val xf = xsOff
        if (ok) {
            plcCount = 1
            plcAlpha = 1f
            tnsSynthesize(xf)
            snsSynthesize(xf)
            mdct.inverse(x, xgOff, x, xdOff, x, xsOff)
        } else {
            plcSynthesize(xf)
            mdct.inverse(x, xf, x, xdOff, x, xsOff)
        }
        if (enableLtpf) {
            val present = ok && pitchPresent
            ltpfSynthesize(nbytes, present, present && ltpfDataActive, if (present) ltpfPitchIndex else 0)
        }
    }

    private fun tnsSynthesize(xf: Int) {
        if (tnsOrder == 0) return
        val rc = rc
        java.util.Arrays.fill(rc, 0f)
        Lc3Shared.tnsUnquantizeRc(tnsRc, 0, tnsOrder, rc, 0)
        // inverse_filtering, one filter at 16 kHz, 8 taps in registers
        val nf = 20 * (1 + bw) * (1 + dt)
        val i0 = 3 * (1 + dt)
        val x = x
        val r0 = rc[0]; val r1 = rc[1]; val r2 = rc[2]; val r3 = rc[3]
        val r4 = rc[4]; val r5 = rc[5]; val r6 = rc[6]; val r7 = rc[7]
        var s0 = 0f; var s1 = 0f; var s2 = 0f; var s3 = 0f
        var s4 = 0f; var s5 = 0f; var s6 = 0f; var s7 = 0f
        for (i in xf + i0 until xf + nf) {
            var xi = x[i]
            xi -= s7 * r7
            xi -= s6 * r6; s7 = s6 + r6 * xi
            xi -= s5 * r5; s6 = s5 + r5 * xi
            xi -= s4 * r4; s5 = s4 + r4 * xi
            xi -= s3 * r3; s4 = s3 + r3 * xi
            xi -= s2 * r2; s3 = s2 + r2 * xi
            xi -= s1 * r1; s2 = s1 + r1 * xi
            xi -= s0 * r0; s1 = s0 + r0 * xi
            s0 = xi
            x[i] = xi
        }
    }

    private fun deenumMvpq(idx0: Int, ls0: Int, npulses0: Int, co: Int, n: Int) {
        val off = Lc3Tables.SNS_MPVQ_OFFSETS
        var idx = idx0
        var ls = ls0
        var npulses = npulses0
        var p = co
        var i = n - 1
        while (i >= 0 && idx != 0) {
            var ci = 0
            while (idx < off[i * 11 + npulses - ci]) ci++
            idx -= off[i * 11 + npulses - ci]
            c[p++] = if (ls != 0) -ci else ci
            npulses -= ci
            if (ci > 0) {
                ls = idx and 1
                idx = idx shr 1
            }
            i--
        }
        val ci = npulses
        if (i-- >= 0) c[p++] = if (ls != 0) -ci else ci
        while (i-- >= 0) c[p++] = 0
    }

    private fun snsSynthesize(xf: Int) {
        val npa = when (snsShape) { 0 -> 10; 1 -> 10; 2 -> 8; else -> 6 }
        deenumMvpq(snsIdxA, snsLsA, npa, 0, if (snsShape < 2) 10 else 16)
        if (snsShape == 0) deenumMvpq(snsIdxB, snsLsB, 1, 10, 6)
        else if (snsShape == 1) java.util.Arrays.fill(c, 10, 16, 0)
        Lc3Shared.normalize(c, 0, cn, 0)
        Lc3Shared.snsUnquantize(snsLfcb, snsHfcb, cn, 0, snsShape, snsGain, scf)
        Lc3Shared.spectralShaping(cfg, scf, true, scf64, x, xf, x, xgOff)
    }

    private fun plcSynthesize(xf: Int) {
        var seed = plcSeed
        var alpha = plcAlpha
        alpha *= if (plcCount < 4) 1.0f else if (plcCount < 8) 0.9f else 0.85f
        for (i in 0 until ne) {
            seed = (16831 + seed * 12821) and 0xffff
            val v = x[xgOff + i]
            x[xf + i] = alpha * (if (seed and 0x8000 != 0) -v else v)
        }
        plcSeed = seed
        plcAlpha = alpha
        plcCount++
    }

    private fun ltpfSynthesize(nbytes: Int, present: Boolean, dataActive: Boolean, pIdx: Int) {
        val nhTot = ns + nh
        val xs = xsOff
        var pitch = if (pIdx >= 440) ((pIdx - 283) shl 2)
        else if (pIdx >= 380) (((pIdx shr 1) - 63) shl 2) + ((pIdx and 1) shl 1)
        else (((pIdx shr 2) + 32) shl 2) + (pIdx and 3)
        pitch = (pitch * 160 + 64) / 128

        val nbits = (nbytes * 8 * (1 + 3)) / (1 + dt)
        val gIdx = maxOf(nbits / 80, 3 + 1) - (3 + 1)
        val active = present && dataActive && gIdx < 4

        val w = 4
        val cden = Lc3Tables.LTPF_CDEN
        val cnum = Lc3Tables.LTPF_CNUM
        for (i in 0 until w) {
            val g = if (active) 0.4f - 0.05f * gIdx else 0f
            cNew[i] = g * cden[(pitch and 3) * 4 + (w - 1) - i]
            cNew[w + i] = 0.85f * g * cnum[minOf(gIdx, 3) * 4 + (w - 1) - i]
        }

        val nt = ns / (1 + dt)
        System.arraycopy(ltpfX, 0, x0a, 0, w - 1)
        System.arraycopy(x, xs + ns - (w - 1), ltpfX, 0, w - 1)
        if (active) System.arraycopy(x, xs + nt - (w - 1), x0b, 0, w - 1)

        if (!ltpfActive && active) {
            synth(nhTot, pitch / 4, x0a, 0, xs, nt, cNew, 1)
        } else if (ltpfActive && !active) {
            synth(nhTot, ltpfPitch / 4, x0a, 0, xs, nt, ltpfC, -1)
        } else if (ltpfActive && active && ltpfPitch == pitch) {
            synth(nhTot, pitch / 4, x0a, 0, xs, nt, cNew, 0)
        } else if (ltpfActive && active) {
            synth(nhTot, ltpfPitch / 4, x0a, 0, xs, nt, ltpfC, -1)
            synth(nhTot, pitch / 4, x, (if (xs <= 0) xs + nhTot else xs) - (w - 1), xs, nt, cNew, 1)
        }
        if (active && ns > nt) synth(nhTot, pitch / 4, x0b, 0, xs + nt, ns - nt, cNew, 0)

        ltpfActive = active
        ltpfPitch = pitch
        System.arraycopy(cNew, 0, ltpfC, 0, 2 * w)
    }

    /** liblc3 synthesize_template with w = 4, over the ring x[0 until nhTot]. */
    private fun synth(nhTot: Int, lag0: Int, x0: FloatArray, x0o: Int, xStart: Int, n: Int, cc: FloatArray, fade: Int) {
        val w = 4
        var g = if (fade <= 0) 1f else 0f
        val gIncr = ((if (fade > 0) 1 else 0) - (if (fade < 0) 1 else 0)).toFloat() / n
        val lag = lag0 + (w shr 1)
        var xi0 = x0o
        var xp = xStart
        var y = if (xp < lag) xp + (nhTot - lag) else xp - lag
        val yEnd = nhTot - 1

        for (j in 0 until w - 1) {
            u[j] = 0f
            val yi = x[y]
            val xi = x0[xi0++]
            y = if (y < yEnd) y + 1 else 0
            for (k in 0..j) u[j - k] -= yi * cc[k]
            for (k in 0..j) u[j - k] += xi * cc[w + k]
        }
        u[w - 1] = 0f

        var i = 0
        while (i < n) {
            for (j in 0 until w) {
                val yi = x[y]
                val xi = x[xp]
                y = if (y < yEnd) y + 1 else 0
                for (k in 0 until w) u[(j + (w - 1) - k) % w] -= yi * cc[k]
                for (k in 0 until w) u[(j + (w - 1) - k) % w] += xi * cc[w + k]
                x[xp++] = xi - g * u[j]
                u[j] = 0f
                g += gIncr
            }
            i += w
        }
    }

    private fun store(pcm: ShortArray, off: Int) {
        val xs = xsOff
        for (i in 0 until ns) {
            val v = x[xs + i]
            var sv = if (v >= 0) (v + 0.5f).toInt() else (v - 0.5f).toInt()
            if (sv > 32767) sv = 32767 else if (sv < -32768) sv = -32768
            pcm[off + i] = sv.toShort()
        }
    }
}
