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
 * LC3 encoder: Kotlin translation of liblc3 v1.1.3 (lc3.c, energy.c, bwdet.c, sns.c, tns.c, spec.c)
 * for 16 kHz mono. Modified by Meta.
 */
package com.musegadgets.lc3

import kotlin.math.abs
import kotlin.math.sqrt

/**
 * LC3 encoder for 16 kHz mono 16-bit PCM.
 *
 * Bit-exact with liblc3 v1.1.3 built without FMA contraction, with the long-term postfilter analysis disabled
 * (`lc3_encoder_disable_ltpf`): every frame signals "no pitch", a valid LC3 frame for any decoder.
 *
 * Allocates nothing per frame. Not thread-safe: use one instance per stream.
 *
 * @param frameDurationUs [Lc3.FRAME_10MS] (160 samples a frame) or [Lc3.FRAME_7_5MS] (120 samples).
 * @param sampleRateHz [Lc3.SAMPLE_RATE_16K], the only rate supported.
 */
@Suppress("MagicNumber", "LargeClass", "LongMethod", "ComplexMethod", "TooManyFunctions", "NestedBlockDepth")
class Lc3Encoder(val frameDurationUs: Int, val sampleRateHz: Int) {
    /** 16 kHz, frames of [frameDurationUs]. */
    constructor(frameDurationUs: Int) : this(frameDurationUs, Lc3.SAMPLE_RATE_16K)

    /** 16 kHz, 10 ms frames. */
    constructor() : this(Lc3.FRAME_10MS, Lc3.SAMPLE_RATE_16K)

    private val cfg = Lc3Config(frameDurationUs, sampleRateHz)
    private val dt = cfg.dt
    private val ns = cfg.ns
    private val ne = cfg.ne
    private val mdct = Lc3Mdct(cfg)
    private val bits = Lc3Bits()

    init {
        // Initialize the shared tables here rather than in the first frame (up to milliseconds on a little core).
        check(LFCB_T.size + SUB_10M_WB.size + Lc3Shared.VQ_GAINS.size + Lc3Consts.GE_18.size + Lc3Math.dbQ16(1f) != 0)
    }

    /** PCM samples per frame: 160 at 10 ms, 120 at 7.5 ms. */
    val frameSamples: Int get() = ns

    // Buffers
    private val xs = FloatArray(ns)
    private val xd = FloatArray(cfg.nd)
    private val xf = FloatArray(ns)
    private val e = FloatArray(64)

    // Spectral analysis state
    private var nbitsOffState = 0f
    private var nbitsSpareState = 0

    // Side data
    private var bw = 0
    private var snsLfcb = 0
    private var snsHfcb = 0
    private var snsShape = 0
    private var snsGain = 0
    private var snsIdxA = 0
    private var snsLsA = 0
    private var snsIdxB = 0
    private var snsLsB = 0
    private var tnsLpcW = false
    private var tnsOrder = 0
    private val tnsRc = IntArray(8)
    private var gIdx = 0
    private var nq = 0
    private var lsbMode = false

    // SNS scratch
    private val scf = FloatArray(16)
    private val scf64 = FloatArray(64)
    private val sc = IntArray(64)
    private val scn = FloatArray(64)
    private val sr = FloatArray(16)
    private val sx = FloatArray(16)
    private val sxm = FloatArray(16)
    private var pCorr = 0f
    private val cbDl = FloatArray(32)
    private val cbDh = FloatArray(32)
    private val mseAcc = FloatArray(8)
    private var pEnergy = 0f

    // TNS scratch
    private val tc = FloatArray(27)
    private val tr = FloatArray(9)
    private val ta0 = FloatArray(9)
    private val ta1 = FloatArray(9)
    private val trc = FloatArray(8)
    private val tb = FloatArray(14)
    private val ts = FloatArray(8)

    // Spectrum scratch
    private val eq = IntArray(64)
    /** Quantized magnitudes (int)(|x| + 6/16), valid after quantize(). */
    private val qa = IntArray(ns)
    private val eqf = FloatArray(64)

    /**
     * Encodes one frame: [frameSamples] samples from `pcm[pcmOffset]` into [frameBytes] bytes at `out[outOffset]`.
     *
     * @param frameBytes [Lc3.MIN_FRAME_BYTES]..[Lc3.MAX_FRAME_BYTES]; 40 is 32 kb/s at 10 ms (see [Lc3.frameBytes]).
     *   It may change from frame to frame.
     */
    fun encode(pcm: ShortArray, pcmOffset: Int, out: ByteArray, outOffset: Int, frameBytes: Int) {
        if (frameBytes < Lc3.MIN_FRAME_BYTES || frameBytes > Lc3.MAX_FRAME_BYTES) {
            throw IllegalArgumentException("frameBytes $frameBytes outside ${Lc3.MIN_FRAME_BYTES}..${Lc3.MAX_FRAME_BYTES}")
        }
        if (pcmOffset < 0 || pcmOffset > pcm.size - ns || outOffset < 0 || outOffset > out.size - frameBytes) {
            throw IndexOutOfBoundsException("pcm or out too short for one frame")
        }
        val xs = xs
        for (i in 0 until ns) xs[i] = pcm[pcmOffset + i].toFloat()
        analyze(frameBytes)
        write(frameBytes, out, outOffset)
    }

    /** Encodes the first [frameSamples] samples of [pcm] into a new array of [frameBytes] bytes. */
    @JvmOverloads
    fun encode(pcm: ShortArray, frameBytes: Int = Lc3.DEFAULT_FRAME_BYTES): ByteArray {
        if (frameBytes < Lc3.MIN_FRAME_BYTES || frameBytes > Lc3.MAX_FRAME_BYTES) {
            throw IllegalArgumentException("frameBytes $frameBytes outside ${Lc3.MIN_FRAME_BYTES}..${Lc3.MAX_FRAME_BYTES}")
        }
        val out = ByteArray(frameBytes)
        encode(pcm, 0, out, 0, frameBytes)
        return out
    }

    /** Forgets the stream: the next frame encodes as the first of a new stream. */
    fun reset() {
        java.util.Arrays.fill(xd, 0f)
        nbitsOffState = 0f
        nbitsSpareState = 0
    }

    // ================================================================= Analysis

    private fun analyze(nbytes: Int) {
        mdct.forward(xs, 0, xd, 0, xf, 0)
        val nnFlag = energyCompute()
        bw = bwdet()
        snsAnalyze(nbytes)
        tnsAnalyze(nnFlag, nbytes)
        specAnalyze(nbytes)
    }

    private fun energyCompute(): Boolean {
        val lim = cfg.bandLim
        val nb = cfg.nb
        val ibandH = nb - (if (dt == 2) 4 else 2)
        var eLo = 0f
        var eHi = 0f
        var i = lim[0]
        for (iband in 0 until nb) {
            val ie = lim[iband + 1]
            val n = ie - i
            var sx2 = xf[i] * xf[i]
            i++
            while (i < ie) { sx2 += xf[i] * xf[i]; i++ }
            val v = sx2 / n
            e[iband] = v
            if (iband >= ibandH) eHi += v else eLo += v
        }
        return eHi > 30 * eLo
    }

    /** Bandwidth detector for 16 kHz: NB (0) or WB (1). */
    private fun bwdet(): Int {
        val is0 = if (dt == 2) 51 else 53
        val ie0 = 64
        var bw0 = 0
        run {
            var i = is0
            val n = ie0 - i
            var se = e[i]
            i++
            while (i < ie0) { se += e[i]; i++ }
            if (se >= ((10 shl 1) * n).toFloat()) bw0 = 1
        }
        var hold = bw0 >= 1
        if (!hold) {
            val l = 4
            val tcv = 31.62277660f
            var i = is0 - l + 1
            while (!hold && i <= is0 + 1) {
                hold = e[i - l] > tcv * e[i]
                i++
            }
        }
        return if (hold) bw0 else 1
    }

    // ================================================================= SNS

    private fun snsAnalyze(nbytes: Int) {
        computeScaleFactors()
        resolveCodebooks()
        snsQuantize()
        Lc3Shared.snsUnquantize(snsLfcb, snsHfcb, scn, snsShape * 16, snsShape, snsGain, scf)
        enumerate()
        Lc3Shared.spectralShaping(cfg, scf, false, scf64, xf, 0, xf, 0)
    }

    private fun computeScaleFactors() {
        val ge = Lc3Consts.GE_18
        val ev = scf64
        System.arraycopy(e, 0, ev, 0, 64)
        val scf = scf

        var e0 = ev[0]
        var e1 = ev[0]
        var e2: Float
        var eSum = 0f
        var i = 0
        while (i < 63) {
            e2 = ev[i + 1]
            ev[i] = (e0 * 0.25f + e1 * 0.5f + e2 * 0.25f) * ge[i]
            eSum += ev[i++]
            e0 = ev[i + 1]
            ev[i] = (e1 * 0.25f + e2 * 0.5f + e0 * 0.25f) * ge[i]
            eSum += ev[i++]
            e1 = ev[i + 1]
            ev[i] = (e2 * 0.25f + e0 * 0.5f + e1 * 0.25f) * ge[i]
            eSum += ev[i++]
        }
        ev[63] = (e0 * 0.25f + e1 * 0.75f) * ge[63]
        eSum += ev[63]

        val nf0 = eSum * (1e-4f / 64)
        val noiseFloor = if (nf0 >= 2.3283064e-10f) nf0 else 2.3283064e-10f /* fmaxf(., 0x1p-32f) */
        // lc3_log2f(fmaxf(e, noiseFloor)) * 0.5f, inlined, four bands per step
        var k = 0
        while (k < 64) {
            val u0 = java.lang.Float.floatToRawIntBits(ev[k + 0].let { if (it >= noiseFloor) it else noiseFloor })
            val e0 = ((u0 and 0x7f800000) ushr 23) - 126
            val f0 = java.lang.Float.intBitsToFloat((u0 and 0x7f800000.inv()) or (126 shl 23))
            val u1 = java.lang.Float.floatToRawIntBits(ev[k + 1].let { if (it >= noiseFloor) it else noiseFloor })
            val e1 = ((u1 and 0x7f800000) ushr 23) - 126
            val f1 = java.lang.Float.intBitsToFloat((u1 and 0x7f800000.inv()) or (126 shl 23))
            val u2 = java.lang.Float.floatToRawIntBits(ev[k + 2].let { if (it >= noiseFloor) it else noiseFloor })
            val e2 = ((u2 and 0x7f800000) ushr 23) - 126
            val f2 = java.lang.Float.intBitsToFloat((u2 and 0x7f800000.inv()) or (126 shl 23))
            val u3 = java.lang.Float.floatToRawIntBits(ev[k + 3].let { if (it >= noiseFloor) it else noiseFloor })
            val e3 = ((u3 and 0x7f800000) ushr 23) - 126
            val f3 = java.lang.Float.intBitsToFloat((u3 and 0x7f800000.inv()) or (126 shl 23))
            var y0 = -1.29479677f * f0
            var y1 = -1.29479677f * f1
            var y2 = -1.29479677f * f2
            var y3 = -1.29479677f * f3
            y0 = (y0 + 5.11769018f) * f0
            y1 = (y1 + 5.11769018f) * f1
            y2 = (y2 + 5.11769018f) * f2
            y3 = (y3 + 5.11769018f) * f3
            y0 = (y0 + -8.42295281f) * f0
            y1 = (y1 + -8.42295281f) * f1
            y2 = (y2 + -8.42295281f) * f2
            y3 = (y3 + -8.42295281f) * f3
            y0 = (y0 + 8.10557963f) * f0
            y1 = (y1 + 8.10557963f) * f1
            y2 = (y2 + 8.10557963f) * f2
            y3 = (y3 + 8.10557963f) * f3
            ev[k + 0] = (e0 + (y0 + -3.50567360f)) * 0.5f
            ev[k + 1] = (e1 + (y1 + -3.50567360f)) * 0.5f
            ev[k + 2] = (e2 + (y2 + -3.50567360f)) * 0.5f
            ev[k + 3] = (e3 + (y3 + -3.50567360f)) * 0.5f
            k += 4
        }

        var sum: Float
        scf[0] = (ev[0] + ev[4]) * 1f / 12 + (ev[0] + ev[3]) * 2f / 12 + (ev[1] + ev[2]) * 3f / 12
        sum = scf[0]
        for (k in 1 until 15) {
            scf[k] = (ev[4 * k - 1] + ev[4 * k + 4]) * 1f / 12 +
                (ev[4 * k] + ev[4 * k + 3]) * 2f / 12 +
                (ev[4 * k + 1] + ev[4 * k + 2]) * 3f / 12
            sum += scf[k]
        }
        scf[15] = (ev[59] + ev[63]) * 1f / 12 + (ev[60] + ev[63]) * 2f / 12 + (ev[61] + ev[62]) * 3f / 12
        sum += scf[15]

        val cf = 0.85f
        for (k in 0 until 16) scf[k] = cf * (scf[k] - sum * 1f / 16)
        // Attack handling: the attack detector is only active at 32 kHz and above.
    }

    /** Squared distance to all 32 codebook vectors at once (transposed tables; each sum in liblc3's order). */
    private fun resolveCodebooks() {
        val lf = LFCB_T
        val hf = HFCB_T
        val dl = cbDl
        val dh = cbDh
        val scf = scf
        for (k in 0 until 32) { dl[k] = 0f; dh[k] = 0f }
        for (i in 0 until 8) {
            val sl = scf[i]
            val sh = scf[8 + i]
            val b = i * 32
            for (k in 0 until 32) {
                val a = sl - lf[b + k]
                dl[k] += a * a
            }
            for (k in 0 until 32) {
                val a = sh - hf[b + k]
                dh[k] += a * a
            }
        }
        var li = 0
        var hi = 0
        var dlMax = dl[0]
        var dhMax = dh[0]
        for (k in 1 until 32) {
            if (dl[k] < dlMax) { li = k; dlMax = dl[k] }
            if (dh[k] < dhMax) { hi = k; dhMax = dh[k] }
        }
        snsLfcb = li
        snsHfcb = hi
    }

    private fun addPulse(x: FloatArray, xo: Int, y: IntArray, yo: Int, n: Int, start: Int, end: Int) {
        for (k in start until end) {
            var bestC2 = (pCorr + x[xo]) * (pCorr + x[xo])
            var bestE = pEnergy + 2 * y[yo] + 1
            var nbest = 0
            for (i in 1 until n) {
                val c2 = (pCorr + x[xo + i]) * (pCorr + x[xo + i])
                val ee = pEnergy + 2 * y[yo + i] + 1
                if (c2 * bestE > ee * bestC2) { bestC2 = c2; bestE = ee; nbest = i }
            }
            pCorr += x[xo + nbest]
            pEnergy += 2 * y[yo + nbest] + 1
            y[yo + nbest]++
        }
    }

    private fun snsQuantize() {
        val lf = Lc3Tables.SNS_LFCB
        val hf = Lc3Tables.SNS_HFCB
        val c = sc
        for (i in 0 until 8) {
            sr[i] = scf[i] - lf[snsLfcb * 8 + i]
            sr[8 + i] = scf[8 + i] - hf[snsHfcb * 8 + i]
        }
        Lc3Shared.dct16Forward(sr, sx)

        var xmSum = 0f
        for (i in 0 until 16) { sxm[i] = abs(sx[i]); xmSum += sxm[i] }

        // Shape 3 (row 48)
        val proj = (6 - 1) / Lc3Math.fmaxf(xmSum, 1e-31f)
        pCorr = 0f
        pEnergy = 0f
        var npulses = 0
        for (i in 0 until 16) {
            val v = Math.floor((sxm[i] * proj).toDouble()).toInt()
            c[48 + i] = v
            npulses += v
            pCorr += v * sxm[i]
            pEnergy += (v * v).toFloat()
        }
        addPulse(sxm, 0, c, 48, 16, npulses, 6)
        npulses = 6

        // Shape 2 (row 32)
        System.arraycopy(c, 48, c, 32, 16)
        addPulse(sxm, 0, c, 32, 16, npulses, 8)
        npulses = 8

        // Shape 1 (row 16)
        System.arraycopy(c, 32, c, 16, 16)
        for (i in 10 until 16) {
            c[16 + i] = 0
            npulses -= c[32 + i]
            pCorr -= c[32 + i] * sxm[i]
            pEnergy -= (c[32 + i] * c[32 + i]).toFloat()
        }
        addPulse(sxm, 0, c, 16, 10, npulses, 10)
        npulses = 10

        // Shape 0 (row 0)
        System.arraycopy(c, 16, c, 0, 16)
        addPulse(sxm, 10, c, 10, 6, 0, 1)

        for (j in 0 until 16) for (i in 0 until 4) if (sx[j] < 0) c[i * 16 + j] = -c[i * 16 + j]
        for (i in 0 until 4) Lc3Shared.normalize(c, i * 16, scn, i * 16)

        // MSE for every (shape, gain): the inner loop runs over a shape's gains (vectorizable), each sum over i in order
        val cn = scn
        val acc = mseAcc
        var mseMin = Float.MAX_VALUE
        var bestShape = 0
        var bestGain = 0
        for (ic in 0 until 4) {
            val gains = Lc3Shared.VQ_GAINS[ic]
            val count = gains.size
            val b = ic * 16
            // gains four at a time, accumulators in registers (counts are 2, 4, 4, 8)
            var k0 = 0
            while (k0 < count) {
                val g0 = gains[k0]
                val g1 = gains[k0 + 1]
                val four = k0 + 3 < count
                val g2 = if (four) gains[k0 + 2] else 0f
                val g3 = if (four) gains[k0 + 3] else 0f
                var m0 = 0f
                var m1 = 0f
                var m2 = 0f
                var m3 = 0f
                for (i in 0 until 16) {
                    val xi = sx[i]
                    val ci = cn[b + i]
                    val d0 = xi - g0 * ci
                    val d1 = xi - g1 * ci
                    val d2 = xi - g2 * ci
                    val d3 = xi - g3 * ci
                    m0 += d0 * d0
                    m1 += d1 * d1
                    m2 += d2 * d2
                    m3 += d3 * d3
                }
                acc[k0] = m0
                acc[k0 + 1] = m1
                if (four) {
                    acc[k0 + 2] = m2
                    acc[k0 + 3] = m3
                }
                k0 += if (four) 4 else 2
            }
            var cmseMin = Float.MAX_VALUE
            var cgain = 0
            for (k in 0 until count) if (acc[k] < cmseMin) { cgain = k; cmseMin = acc[k] }
            if (cmseMin < mseMin) { bestShape = ic; bestGain = cgain; mseMin = cmseMin }
        }
        snsShape = bestShape
        snsGain = bestGain
    }

    private var mvpqIdx = 0
    private var mvpqLs = 0

    private fun enumMvpq(c: IntArray, co: Int, n: Int) {
        val off = Lc3Tables.SNS_MPVQ_OFFSETS
        var i = 0
        var p = co + n
        var ci = c[--p]
        while (ci == 0 && i < 15) { i++; ci = c[--p] }
        var idx = 0
        var ls = if (ci < 0) 1 else 0
        var j = if (ci < 0) -ci else ci
        i++
        while (i < n) {
            ci = c[--p]
            if (ci != 0) {
                idx = (idx shl 1) or ls
                ls = if (ci < 0) 1 else 0
            }
            idx += off[i * 11 + minOf(j, 10)]
            j += if (ci < 0) -ci else ci
            i++
        }
        mvpqIdx = idx
        mvpqLs = ls
    }

    private fun enumerate() {
        val co = snsShape * 16
        enumMvpq(sc, co, if (snsShape < 2) 10 else 16)
        snsIdxA = mvpqIdx; snsLsA = mvpqLs
        if (snsShape == 0) {
            enumMvpq(sc, co + 10, 6)
            snsIdxB = mvpqIdx; snsLsB = mvpqLs
        }
    }

    // ================================================================= TNS

    private fun tnsAnalyze(nnFlag: Boolean, nbytes: Int) {
        tnsLpcW = Lc3Shared.lpcWeighting(dt, nbytes)
        val maxorder = 8
        val predGain = computeLpcCoeffs(maxorder)
        tnsOrder = 0
        if (!(nnFlag || predGain <= 1.5f)) {
            if (tnsLpcW && predGain < 2f) {
                val gamma = 1f - (1f - 0.85f) * (2f - predGain) / (2f - 1.5f)
                var g = 1f
                for (i in 1 until 9) { g *= gamma; ta0[i] *= g }
            }
            lpcReflection(maxorder)
            quantizeRc(maxorder)
            Lc3Shared.tnsUnquantizeRc(tnsRc, 0, tnsOrder, trc, 0)
        }
        forwardFiltering()
    }

    /** Returns the prediction gain; LPC coefficients in ta0. */
    private fun computeLpcCoeffs(maxorder: Int): Float {
        val sub = if (dt == 2) (if (bw == 0) SUB_7M5_NB else SUB_7M5_WB) else (if (bw == 0) SUB_10M_NB else SUB_10M_WB)
        val lag = Lc3Consts.TNS_LAG_WINDOW
        val x = xf
        // All 9 lags in one pass: each lag keeps its own accumulator, summed in the same order as liblc3's dot().
        val tcv = tc
        for (s in 0 until 3) {
            val a0 = sub[s]
            val len = sub[s + 1] - a0
            var c0 = 0f; var c1 = 0f; var c2 = 0f; var c3 = 0f; var c4 = 0f
            var c5 = 0f; var c6 = 0f; var c7 = 0f; var c8 = 0f
            val jEnd = len - 8
            for (j in a0 until a0 + jEnd) {
                val xj = x[j]
                c8 += xj * x[j + 8]
                c0 += xj * xj
                c1 += xj * x[j + 1]
                c2 += xj * x[j + 2]
                c3 += xj * x[j + 3]
                c4 += xj * x[j + 4]
                c5 += xj * x[j + 5]
                c6 += xj * x[j + 6]
                c7 += xj * x[j + 7]
            }
            // tail: the last 8 samples contribute only to the lags that still fit
            val e = a0 + len
            var j = a0 + jEnd
            while (j < e) {
                val xj = x[j]
                val r = e - j // lags 0 until r are in range
                c0 += xj * xj
                if (r > 1) c1 += xj * x[j + 1]
                if (r > 2) c2 += xj * x[j + 2]
                if (r > 3) c3 += xj * x[j + 3]
                if (r > 4) c4 += xj * x[j + 4]
                if (r > 5) c5 += xj * x[j + 5]
                if (r > 6) c6 += xj * x[j + 6]
                if (r > 7) c7 += xj * x[j + 7]
                j++
            }
            tcv[s] = c0; tcv[3 + s] = c1; tcv[6 + s] = c2; tcv[9 + s] = c3; tcv[12 + s] = c4
            tcv[15 + s] = c5; tcv[18 + s] = c6; tcv[21 + s] = c7; tcv[24 + s] = c8
        }
        tr[0] = 3f
        val e0 = tc[0]; val e1 = tc[1]; val e2 = tc[2]
        for (k in 1..maxorder) {
            tr[k] = if (e0 == 0f || e1 == 0f || e2 == 0f) 0f
            else (tc[k * 3] / e0 + tc[k * 3 + 1] / e1 + tc[k * 3 + 2] / e2) * lag[k]
        }

        // Levinson-Durbin
        val a0 = ta0
        val a1 = ta1
        var err = tr[0]
        var rc: Float
        var gain = err
        a0[0] = 1f
        var k = 1
        while (k <= maxorder) {
            rc = -tr[k]
            for (i in 1 until k) rc -= a0[i] * tr[k - i]
            rc /= err
            err *= 1 - rc * rc
            for (i in 1 until k) a1[i] = a0[i] + rc * a0[k - i]
            a1[k++] = rc

            rc = -tr[k]
            for (i in 1 until k) rc -= a1[i] * tr[k - i]
            rc /= err
            err *= 1 - rc * rc
            for (i in 1 until k) a0[i] = a1[i] + rc * a1[k - i]
            a0[k++] = rc
        }
        gain /= err
        return gain
    }

    private fun lpcReflection(maxorder: Int) {
        val a = ta0
        val rc = trc
        val b = tb // rows of 7: b[0] = 0..6, b[1] = 7..13
        rc[maxorder - 1] = a[maxorder]
        var e = 1 - rc[maxorder - 1] * rc[maxorder - 1]
        var b1 = 7
        for (i in 0 until maxorder - 1) b[b1 + i] = (a[1 + i] - rc[maxorder - 1] * a[(maxorder - 1) - i]) / e
        for (k in maxorder - 2 downTo 1) {
            val b0 = b1
            b1 = (k and 1) * 7
            rc[k] = b[b0 + k]
            e = 1 - rc[k] * rc[k]
            for (i in 0 until k) b[b1 + i] = (b[b0 + i] - rc[k] * b[b0 + k - 1 - i]) / e
        }
        rc[0] = b[b1]
    }

    private fun quantizeRc(maxorder: Int) {
        val thr = Lc3Consts.TNS_Q_THR
        var order = maxorder
        for (i in 0 until maxorder) {
            val m = abs(trc[i])
            var q = if (m >= thr[4]) 4 else 0
            var j = 0
            while (j < 4 && m >= thr[q]) { j++; q++ }
            if (trc[i] < 0) q = -q
            tnsRc[i] = q
            order = if (q != 0) maxorder else order - 1
        }
        tnsOrder = order
    }

    private fun forwardFiltering() {
        val nf = 20 * (1 + bw) * (1 + dt) // ne(dt, bw)
        val i0 = 3 * (1 + dt)
        val order = tnsOrder
        if (order == 0) return
        val x = xf
        val rc = trc
        for (k in order until 8) rc[k] = 0f
        val r0 = rc[0]; val r1 = rc[1]; val r2 = rc[2]; val r3 = rc[3]
        val r4 = rc[4]; val r5 = rc[5]; val r6 = rc[6]; val r7 = rc[7]
        var s0 = 0f; var s1 = 0f; var s2 = 0f; var s3 = 0f
        var s4 = 0f; var s5 = 0f; var s6 = 0f; var s7 = 0f
        for (i in i0 until nf) {
            var xi = x[i]
            var t = xi
            var u: Float
            u = s0; s0 = t; t = r0 * xi + u; xi += r0 * u
            u = s1; s1 = t; t = r1 * xi + u; xi += r1 * u
            u = s2; s2 = t; t = r2 * xi + u; xi += r2 * u
            u = s3; s3 = t; t = r3 * xi + u; xi += r3 * u
            u = s4; s4 = t; t = r4 * xi + u; xi += r4 * u
            u = s5; s5 = t; t = r5 * xi + u; xi += r5 * u
            u = s6; s6 = t; t = r6 * xi + u; xi += r6 * u
            u = s7; s7 = t; xi += r7 * u
            x[i] = xi
        }
    }

    private fun tnsNbits(): Int {
        var n2048 = 2048
        if (tnsOrder > 0) n2048 += Lc3Tables.TNS_ORDER_BITS[(if (tnsLpcW) 1 else 0) * 8 + tnsOrder - 1]
        for (i in 0 until tnsOrder) n2048 += Lc3Tables.TNS_COEFFS_BITS[i * 17 + 8 + tnsRc[i]]
        return (n2048 + (1 shl 11) - 1) shr 11
    }

    // ================================================================= Spectrum

    private fun specAnalyze(nbytes: Int) {
        val nbitsAc = Lc3Shared.nbitsNq(ne) + 3 + minOf((nbytes - 1) / 160, 2)
        val nbitsBudget = 8 * nbytes - nbitsAc - 1 /* bw */ - 1 /* ltpf */ - 38 /* sns */ - tnsNbits() - 8 - 3

        var nbitsOff = nbitsOffState + nbitsSpareState
        nbitsOff = Lc3Math.fminf(Lc3Math.fmaxf(nbitsOff, -40f), 40f)
        nbitsOff = 0.8f * nbitsOffState + 0.2f * nbitsOff

        val gOff = Lc3Shared.gainOffset(nbytes)
        val gInt = estimateGain(nbitsBudget, nbitsOff, gOff)
        val gMin = egMin
        val resetOff = egReset

        quantize(gInt)
        val nbits = computeNbits(nbytes, 0, false)

        nbitsOffState = if (resetOff) 0f else nbitsOff
        nbitsSpareState = if (resetOff) 0 else nbitsBudget - nbits

        val gAdj = adjustGain(gOff + gInt, nbits, nbitsBudget, gOff + gMin)
        if (gAdj != 0) quantize(gAdj)
        gIdx = gInt + gAdj + gOff
        computeNbits(nbytes, nbitsBudget, true)
    }

    private var egMin = 0
    private var egReset = false

    private fun estimateGain(nbitsBudget: Int, nbitsOff: Float, gOff: Int): Int {
        val x = xf
        val n4 = ne / 4
        val eqv = eq
        var x2Max = 0f
        for (i in 0 until n4) {
            val x0 = x[4 * i] * x[4 * i]
            val x1 = x[4 * i + 1] * x[4 * i + 1]
            val x2 = x[4 * i + 2] * x[4 * i + 2]
            val x3 = x[4 * i + 3] * x[4 * i + 3]
            x2Max = Lc3Math.fmaxf(x2Max, x0)
            x2Max = Lc3Math.fmaxf(x2Max, x1)
            x2Max = Lc3Math.fmaxf(x2Max, x2)
            x2Max = Lc3Math.fmaxf(x2Max, x3)
            eqf[i] = x0 + x1 + x2 + x3
        }
        val xMax = sqrt(x2Max)
        val nf = 0f
        for (i in 0 until n4) eqv[i] = Lc3Math.dbQ16(Lc3Math.fmaxf(eqf[i] + nf, 1e-10f))

        val nbits = (nbitsBudget + nbitsOff + 0.5f).toInt()
        var gInt = 255 - gOff
        var i = 128
        var j0 = n4 - 1
        while (i > 0) {
            val gn = (gInt - i) * K_20_28
            var v = 0
            var j = j0
            while (j >= 0 && eqv[j] < gn) j--
            val j1 = j
            while (j >= 0) {
                val d = eqv[j] - gn
                v += if (d < 0) K_2U7 else if (d < (43 shl 16)) d + (7 shl 16) else 2 * d - (36 shl 16)
                j--
            }
            if (v > nbits * K_1U4) j0 = j1 else gInt -= i
            i = i shr 1
        }

        val xLim = 32767f
        var gMin = 255 - gOff
        i = 128
        while (i > 0) {
            if (xLim * Lc3Shared.unquantizeGain(gMin - i) > xMax) gMin -= i
            i = i shr 1
        }
        egMin = gMin
        egReset = gInt < gMin || xMax == 0f
        if (egReset) gInt = gMin
        return gInt
    }

    private fun adjustGain(gIdx: Int, nbits: Int, nbitsBudget: Int, gIdxMin: Int): Int {
        val t0 = 230; val t1 = 1025; val t2 = 1700
        var delta: Int
        var den = 48
        if (nbits < t0) {
            delta = 3 * (nbits + 48)
        } else if (nbits < t1) {
            val n0 = 3 * (t0 + 48)
            val range = t1 - t0
            delta = n0 * range + (nbits - t0) * (t1 - n0)
            den *= range
        } else {
            delta = minOf(nbits, t2)
        }
        delta = (delta + den / 2) / den
        if (nbits < nbitsBudget - (delta + 2)) return -(if (gIdx > gIdxMin) 1 else 0)
        if (nbits > nbitsBudget) {
            return (if (gIdx < 255) 1 else 0) + (if (gIdx < 254 && nbits >= nbitsBudget + delta) 1 else 0)
        }
        return 0
    }

    private fun quantize(gInt: Int) {
        val gInv = Lc3Shared.unquantizeGain(-gInt)
        val x = xf
        var n = ne
        val xqMin = 10f / 16
        var i = 0
        val q = qa
        while (i < ne) {
            val x0 = x[i] * gInv
            val x1 = x[i + 1] * gInv
            x[i] = x0
            x[i + 1] = x1
            val a0 = abs(x0)
            val a1 = abs(x1)
            q[i] = (a0 + 0.375f).toInt()
            q[i + 1] = (a1 + 0.375f).toInt()
            n = if (a0 >= xqMin || a1 >= xqMin) ne else n - 2
            i += 2
        }
        nq = n
    }

    /** liblc3 compute_nbits; updates nq, and lsbMode when [setLsb]. */
    private fun computeNbits(nbytes: Int, budget: Int, setLsb: Boolean): Int {
        val hr = if (Lc3Shared.highRate(nbytes)) 2 else 0
        val lsbAllowed = Lc3Shared.lsbModeAllowed(nbytes)
        val lsbI = if (lsbAllowed) 1 else 0
        val q = qa
        val lookup = SPEC_LUT
        val sbits = SPEC_BITS
        var nbits = 0
        var nbitsLsb = 0
        var state = 0
        var nbitsEnd = 0
        var nEnd = 0
        val nbitsBudget = if (budget != 0) budget * 2048 else Int.MAX_VALUE
        var i = 0
        for (h in 0 until 2) {
            val lutBase = (hr + h) * 256
            val lim = minOf(nq, (ne + 2) shr (1 - h))
            while (i < lim && nbits <= nbitsBudget) {
                var a = q[i]
                var b = q[i + 1]
                val lut = (lutBase + state) * 4
                val s = (if (a != 0) 1 else 0) + (if (b != 0) 1 else 0)
                nbits += s * 2048
                var m = (a or b) ushr 2
                var k = 0
                if (m != 0) {
                    if (lsbAllowed) {
                        nbits += sbits[lookup[lut + k++] * 17 + 16].code - 2 * 2048
                        nbitsLsb += 2 + (if (a == 1) 1 else 0) + (if (b == 1) 1 else 0)
                    }
                    m = m ushr lsbI
                    while (m != 0) {
                        nbits += sbits[lookup[lut + minOf(k, 3)] * 17 + 16].code
                        m = m ushr 1
                        k++
                    }
                    nbits += k * 2 * 2048
                    a = a ushr k
                    b = b ushr k
                    k = minOf(k, 3)
                }
                nbits += sbits[lookup[lut + k] * 17 + a + 4 * b].code
                if (s != 0 && nbits <= nbitsBudget) {
                    nEnd = i + 2
                    nbitsEnd = nbits
                }
                state = ((state shl 4) + (if (k > 1) 12 + k else 1 + (a + b) * (k + 1))) and 0xff
                i += 2
            }
        }
        nq = nEnd
        if (setLsb) lsbMode = lsbAllowed && nbitsEnd + nbitsLsb * 2048 > nbitsBudget
        if (nbitsBudget == Int.MAX_VALUE) nbitsEnd += nbitsLsb * 2048
        return (nbitsEnd + 2047) / 2048
    }

    // ================================================================= Bitstream

    private fun write(nbytes: Int, out: ByteArray, outOff: Int) {
        val b = bits
        b.setup(false, out, outOff, nbytes)
        b.putBits(bw, 1)
        // spectral side
        b.putBits(maxOf(nq shr 1, 1) - 1, Lc3Shared.nbitsNq(ne))
        b.putBits(if (lsbMode) 1 else 0, 1)
        b.putBits(gIdx, 8)
        // TNS
        b.putBits(if (tnsOrder > 0) 1 else 0, 1)
        if (tnsOrder > 0) {
            b.putSymbol(TNS_ORDER_M + (if (tnsLpcW) 34 else 0), tnsOrder - 1)
            for (i in 0 until tnsOrder) b.putSymbol(TNS_COEF_M + i * 34, 8 + tnsRc[i])
        }
        b.putBit(0) // pitch_present
        putSns(b)
        // spectrum
        b.putBits(estimateNoise(), 3)
        b.putSpectrum(qa, xf, nq, ne, lsbMode, Lc3Shared.highRate(nbytes))
        val left = b.bitsLeft()
        if (lsbMode) putLsb(b, left) else putResidual(b, left)
        b.flush()
        b.release()
    }

    private fun putSns(b: Lc3Bits) {
        b.putBits(snsLfcb, 5)
        b.putBits(snsHfcb, 5)
        val shapeMsb = snsShape shr 1
        b.putBit(shapeMsb)
        if (shapeMsb == 0) {
            val sizeA = 2390004
            val sub = snsShape and 1
            val muxHigh = if (sub == 0) 2 * (snsIdxB + 1) + snsLsB else snsGain and 1
            val muxCode = muxHigh * sizeA + snsIdxA
            b.putBits(snsGain shr sub, 1)
            b.putBits(snsLsA, 1)
            b.putBits(muxCode, 25)
        } else {
            val sizeA = 15158272
            val sub = snsShape and 1
            val muxCode = if (sub == 0) snsIdxA else sizeA + 2 * snsIdxA + (snsGain and 1)
            b.putBits(snsGain shr sub, 2)
            b.putBits(snsLsA, 1)
            b.putBits(muxCode, 24)
        }
    }

    private fun estimateNoise(): Int {
        val x = xf
        val bwStop = 20 * (1 + bw) * (1 + dt)
        val w = 1 + (if (dt >= 2) 1 else 0) + (if (dt >= 3) 1 else 0)
        val xqLim = 10f / 16
        var sum = 0f
        var nsum = 0
        var z = 0
        var i = 6 * (1 + dt) - w
        val lim = minOf(nq, bwStop)
        while (i < lim) {
            z = if (abs(x[i]) < xqLim) z + 1 else 0
            if (z > 2 * w) { sum += abs(x[i - w]); nsum++ }
            i++
        }
        while (i < bwStop + w) {
            if (++z > 2 * w) { sum += abs(x[i - w]); nsum++ }
            i++
        }
        val nf = if (nsum != 0) 8 - ((16 * sum) / nsum + 0.5f).toInt() else 8
        return if (nf < 0) 0 else if (nf > 7) 7 else nf
    }

    private fun putResidual(b: Lc3Bits, nbits0: Int) {
        var nbits = nbits0
        val x = xf
        val xqLim = 10f / 16
        val xqOff = xqLim / 2
        var i = 0
        while (i < nq && nbits > 0) {
            val xr = abs(x[i])
            if (xr >= xqLim) {
                val bit = (xr - Math.floor(xr.toDouble()).toFloat() < xqLim) xor (x[i] < 0) // truncf(xr), xr >= 0
                b.putBit(if (bit) 1 else 0)
                nbits--
                x[i] += if (bit) -xqOff else xqOff
            }
            i++
        }
    }

    private fun putLsb(b: Lc3Bits, nbits0: Int) {
        var nbits = nbits0
        val x = xf
        var i = 0
        while (i < nq && nbits > 0) {
            val a = qa[i]
            val bb = qa[i + 1]
            if ((a or bb) ushr 2 != 0) {
                if (nbits-- > 0) b.putBit(a and 1)
                if (a == 1 && nbits-- > 0) b.putBit(if (x[i] < 0) 1 else 0)
                if (nbits-- > 0) b.putBit(bb and 1)
                if (bb == 1 && nbits-- > 0) b.putBit(if (x[i + 1] < 0) 1 else 0)
            }
            i += 2
        }
    }
}

/** SNS codebooks transposed to [i][codebook] for the codebook search. */
private val LFCB_T = FloatArray(256) { Lc3Tables.SNS_LFCB[(it % 32) * 8 + it / 32] }
private val HFCB_T = FloatArray(256) { Lc3Tables.SNS_HFCB[(it % 32) * 8 + it / 32] }

/** TNS analysis subdivisions (liblc3 tns.c), 16 kHz: [7.5 / 10 ms][NB / WB]. */
private val SUB_7M5_NB = intArrayOf(9, 26, 43, 60)
private val SUB_7M5_WB = intArrayOf(9, 46, 83, 120)
private val SUB_10M_NB = intArrayOf(12, 34, 57, 80)
private val SUB_10M_WB = intArrayOf(12, 61, 110, 160)

private val K_20_28 = (20f / 28 * 65536f + 0.5f).toInt()
private val K_2U7 = (2.7f * 65536f + 0.5f).toInt()
private val K_1U4 = (1.4f * 65536f + 0.5f).toInt()
