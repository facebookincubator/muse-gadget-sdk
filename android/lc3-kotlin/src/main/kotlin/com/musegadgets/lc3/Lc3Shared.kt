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
 * Pieces shared by the encoder and decoder, from liblc3 v1.1.3 sns.c, tns.c and spec.c. Modified by Meta.
 */
package com.musegadgets.lc3

@Suppress("MagicNumber")
internal object Lc3Shared {
    private val LFCB = Lc3Tables.SNS_LFCB
    private val HFCB = Lc3Tables.SNS_HFCB
    private val DCT16 = Lc3Consts.DCT16_M
    private val DCT16T = FloatArray(256) { Lc3Consts.DCT16_M[(it % 16) * 16 + it / 16] }
    private val IQ = Lc3Consts.IQ_TABLE
    private val Q_INV = Lc3Consts.TNS_Q_INV
    private val E8 = floatArrayOf(
        1.00000000e+00f, 1.09050773e+00f, 1.18920712e+00f, 1.29683955e+00f,
        1.41421356e+00f, 1.54221083e+00f, 1.68179283e+00f, 1.83400809e+00f,
    )
    @JvmField val VQ_GAINS = arrayOf(
        Lc3Tables.SNS_VQ_GAINS_0, Lc3Tables.SNS_VQ_GAINS_1, Lc3Tables.SNS_VQ_GAINS_2, Lc3Tables.SNS_VQ_GAINS_3,
    )

    /** liblc3 resolve_gain_offset, sr = 16 kHz (sr_ind = 1). */
    @JvmStatic
    fun gainOffset(nbytes: Int): Int {
        val gOff = (nbytes * 8) / (10 * (1 + 1))
        return minOf(255, 105 + 5 * (1 + 1) + minOf(gOff, 115))
    }

    /** liblc3 resolve_modes (high_rate), sr = 16 kHz. */
    @JvmStatic
    fun highRate(nbytes: Int): Boolean = nbytes > 20 * (1 + 1)

    @JvmStatic
    fun lsbModeAllowed(nbytes: Int): Boolean = nbytes >= 20 * (3 + 1)

    @JvmStatic
    fun unquantizeGain(gInt0: Int): Float {
        var gInt = gInt0
        var g = 1f
        while (gInt < 0) { gInt += 28; g *= 0.1f }
        while (gInt >= 28) { gInt -= 28; g *= 10f }
        return g * IQ[gInt]
    }

    @JvmStatic
    fun lpcWeighting(dt: Int, nbytes: Int): Boolean = nbytes * 8 < 120 * (1 + dt)

    /**
     * y[i] = sum_j x[j] * M[j][i], each output summed in j order as liblc3 does. Four outputs at a time in
     * registers: independent chains that in-order cores (Cortex-A510) can overlap.
     */
    @JvmStatic
    fun dct16Forward(x: FloatArray, y: FloatArray) = dct16(x, 0, DCT16, y)

    /** y[i] = sum_j x[j] * M[i][j], through the transposed matrix. */
    @JvmStatic
    fun dct16Inverse(x: FloatArray, xo: Int, y: FloatArray) = dct16(x, xo, DCT16T, y)

    @JvmStatic
    private fun dct16(x: FloatArray, xo: Int, m: FloatArray, y: FloatArray) {
        var i = 0
        while (i < 16) {
            var a0 = 0f
            var a1 = 0f
            var a2 = 0f
            var a3 = 0f
            var b = i
            for (j in 0 until 16) {
                val xj = x[xo + j]
                a0 += xj * m[b]
                a1 += xj * m[b + 1]
                a2 += xj * m[b + 2]
                a3 += xj * m[b + 3]
                b += 16
            }
            y[i] = a0
            y[i + 1] = a1
            y[i + 2] = a2
            y[i + 3] = a3
            i += 4
        }
    }

    @JvmStatic
    fun normalize(c: IntArray, co: Int, cn: FloatArray, cno: Int) {
        var c2 = 0
        for (i in 0 until 16) c2 += c[co + i] * c[co + i]
        val norm = 1f / kotlin.math.sqrt(c2.toFloat())
        for (i in 0 until 16) cn[cno + i] = c[co + i] * norm
    }

    @JvmStatic
    fun snsUnquantize(lfcb: Int, hfcb: Int, cn: FloatArray, cno: Int, shape: Int, gain: Int, scf: FloatArray) {
        val g = VQ_GAINS[shape][gain]
        dct16Inverse(cn, cno, scf)
        for (i in 0 until 8) scf[i] = LFCB[lfcb * 8 + i] + g * scf[i]
        for (i in 8 until 16) scf[i] = HFCB[hfcb * 8 + i - 8] + g * scf[i]
    }

    /** liblc3 spectral_shaping, nb = 64 (no band grouping). scfTmp: 64 floats scratch. */
    @JvmStatic
    fun spectralShaping(cfg: Lc3Config, scfQ: FloatArray, inv: Boolean, scf: FloatArray,
                        x: FloatArray, xo: Int, y: FloatArray, yo: Int) {
        var s0: Float
        var s1 = if (inv) -scfQ[0] else scfQ[0]
        scf[0] = s1
        scf[1] = s1
        s0 = s1
        for (i in 0 until 15) {
            s0 = s1
            s1 = if (inv) -scfQ[i + 1] else scfQ[i + 1]
            scf[4 * i + 2] = s0 + 0.125f * (s1 - s0)
            scf[4 * i + 3] = s0 + 0.375f * (s1 - s0)
            scf[4 * i + 4] = s0 + 0.625f * (s1 - s0)
            scf[4 * i + 5] = s0 + 0.875f * (s1 - s0)
        }
        scf[62] = s1 + 0.125f * (s1 - s0)
        scf[63] = s1 + 0.375f * (s1 - s0)

        // per-band gain (64 independent exp2f), stored over the interpolated scale factors
        for (ib in 0 until 64) {
            val xv = -scf[ib]
            val v = xv + 1572864f // 0x1.8p20f
            val k = java.lang.Float.floatToRawIntBits(v)
            val r = xv - (v - 1572864f)
            var q = 1.00448128e-02f * r
            q = (q + 5.54563260e-02f) * r
            q = (q + 2.40228756e-01f) * r
            q = (q + 6.93147140e-01f) * r
            q = (q + 1f) * E8[k and 7]
            scf[ib] = java.lang.Float.intBitsToFloat(java.lang.Float.floatToRawIntBits(q) + ((k shr 3) shl 23))
        }
        val lim = cfg.bandLim
        var i = lim[0]
        for (ib in 0 until 64) {
            val g = scf[ib]
            val e = lim[ib + 1]
            while (i < e) {
                y[yo + i] = x[xo + i] * g
                i++
            }
        }
    }

    @JvmStatic
    fun tnsUnquantizeRc(rcQ: IntArray, rcQo: Int, order: Int, rc: FloatArray, rco: Int) {
        for (i in 0 until order) {
            val q = rcQ[rcQo + i]
            val m = Q_INV[if (q < 0) -q else q]
            rc[rco + i] = if (q < 0) -m else m
        }
    }

    /** nbits of the spectral side info: nq field width (ne = ns at 16 kHz). */
    @JvmStatic
    fun nbitsNq(ne: Int): Int =
        4 + (if (ne > 32) 1 else 0) + (if (ne > 64) 1 else 0) + (if (ne > 128) 1 else 0) +
            (if (ne > 256) 1 else 0) + (if (ne > 512) 1 else 0)
}
