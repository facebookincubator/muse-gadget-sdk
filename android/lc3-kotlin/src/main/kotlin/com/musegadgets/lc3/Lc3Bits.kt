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
 * Kotlin translation of liblc3 v1.1.3 bits.c / bits.h (bitstream and arithmetic coder), with the
 * spectrum coding loops of spec.c folded in so the coder state stays in locals. Modified by Meta.
 */
package com.musegadgets.lc3

/**
 * LC3 bitstream: arithmetic-coded symbols grow forward from the start of the frame,
 * plain bits grow backward from the end. One instance is reused for every frame.
 * Models are flattened arrays of (low, range) pairs, 17 symbols per model.
 */
@Suppress("MagicNumber", "TooManyFunctions")
internal class Lc3Bits {
    private val models: CharArray = AC_MODELS
    private var read = false
    private var buf: ByteArray = EMPTY
    private var start = 0
    private var end = 0
    private var pFw = 0
    private var pBw = 0

    // Arithmetic coder
    private var acLow = 0
    private var acRange = 0
    private var acCache = 0
    private var acCarry = 0
    private var acCarryCount = 0
    private var acError = false

    // Bit accumulator (32-bit unsigned held in an Int)
    private var accuV = 0
    private var accuN = 0
    private var accuNover = 0

    /** Drops the reference to the caller's frame array, so an idle codec doesn't keep it alive. */
    fun release() {
        buf = EMPTY
    }

    fun setup(read: Boolean, buffer: ByteArray, offset: Int, len: Int) {
        this.read = read
        buf = buffer
        start = offset
        end = offset + len
        pFw = offset
        pBw = offset + len
        acLow = 0
        acRange = 0xffffff
        acCache = -1
        acCarry = 0
        acCarryCount = 0
        acError = false
        accuV = 0
        accuN = if (read) ACCU_BITS else 0
        accuNover = 0
        if (read) {
            acLow = acGet() shl 16
            acLow = acLow or (acGet() shl 8)
            acLow = acLow or acGet()
            accuLoad()
        }
    }

    // ---------------------------------------------------------------- Common

    private fun acRangeBits(): Int = 32 - Integer.numberOfLeadingZeros(acRange)

    private fun acPendingBits(): Int =
        26 - acRangeBits() + ((if (acCache >= 0) 1 else 0) + acCarryCount) * 8

    private fun bitsLeftRaw(): Int {
        val e = pBw + if (read) ACCU_BITS / 8 else 0
        val s = pFw - if (read) AC_BITS / 8 else 0
        val n = e - s
        return 8 * n - (accuN + accuNover + acPendingBits())
    }

    fun bitsLeft(): Int = maxOf(bitsLeftRaw(), 0)

    /** 0 when the frame was consumed consistently, -1 on overrun or coder error. */
    fun check(): Int = if (bitsLeftRaw() < 0 || acError) -1 else 0

    // ---------------------------------------------------------------- Writing

    fun putBit(v: Int) = putBits(v, 1)

    fun putBits(v: Int, n: Int) {
        if (accuN + n <= ACCU_BITS) {
            accuV = accuV or (v shl accuN)
            accuN += n
        } else {
            putBitsGeneric(v, n)
        }
    }

    private fun accuFlush() {
        var nbytes = minOf(accuN shr 3, maxOf(pBw - pFw, 0))
        accuN -= 8 * nbytes
        while (nbytes != 0) {
            buf[--pBw] = (accuV and 0xff).toByte()
            accuV = accuV ushr 8
            nbytes--
        }
        if (accuN >= 8) accuN = 0
    }

    private fun putBitsGeneric(v: Int, n: Int) {
        val n1 = minOf(ACCU_BITS - accuN, n)
        if (n1 != 0) {
            accuV = accuV or (v shl accuN)
            accuN = ACCU_BITS
        }
        accuFlush()
        accuV = if (n1 >= 32) 0 else v ushr n1
        accuN = n - n1
    }

    /** [model] is a model base offset: one of the *_M constants plus index * 34. */
    fun putSymbol(model: Int, s: Int) {
        val models = this.models
        val base = model + 2 * s
        val range = acRange ushr 10
        acLow += range * models[base].code
        acRange = range * models[base + 1].code
        acCarry = acCarry or (acLow ushr 24)
        acLow = acLow and 0xffffff
        if (acRange < 0x10000) {
            while (acRange < 0x10000) {
                acShift()
                acRange = acRange shl 8
            }
        }
    }

    private fun acPut(byte: Int) {
        if (pFw < end) buf[pFw++] = byte.toByte()
    }

    private fun acShift() {
        if (acLow < 0xff0000 || acCarry != 0) {
            if (acCache >= 0) acPut(acCache + acCarry)
            while (acCarryCount > 0) {
                acPut(if (acCarry != 0) 0x00 else 0xff)
                acCarryCount--
            }
            acCache = acLow ushr 16
            acCarry = 0
        } else {
            acCarryCount++
        }
        acLow = (acLow shl 8) and 0xffffff
    }

    private fun acTerminate() {
        var nbits = 25 - acRangeBits()
        var mask = 0xffffff ushr nbits
        var v = acLow + mask
        var high = acLow + acRange
        val overVal = (v ushr 24) != 0
        val overHigh = (high ushr 24) != 0
        v = (v and 0xffffff) and mask.inv()
        high = high and 0xffffff
        if (overVal == overHigh) {
            if (v + mask >= high) {
                nbits++
                mask = mask ushr 1
                v = ((acLow + mask) and 0xffffff) and mask.inv()
            }
            if (v < acLow) acCarry = acCarry or 1
        }
        acLow = v
        while (nbits > 8) {
            acShift()
            nbits -= 8
        }
        acShift()
        var endVal = acCache shr (8 - nbits)
        if (acCarryCount != 0) {
            acPut(acCache)
            while (acCarryCount > 1) {
                acPut(0xff)
                acCarryCount--
            }
            endVal = if (nbits < 8) 0 else 0xff
        }
        if (pFw < end) {
            var b = buf[pFw].toInt() and 0xff
            b = b and (0xff ushr nbits)
            b = b or (endVal shl (8 - nbits))
            buf[pFw] = b.toByte()
        }
    }

    fun flush() {
        val nleft = pBw - pFw
        var n = 8 * nleft - accuN
        while (n > 0) {
            putBits(0, minOf(n, 32))
            n -= 32
        }
        accuFlush()
        acTerminate()
    }


    /**
     * liblc3 put_quantized: arithmetic-codes the quantized spectrum. qa[i] = |x[i]| quantized magnitude (precomputed),
     * x supplies the signs. Coder state is held in locals.
     */
    fun putSpectrum(qa: IntArray, x: FloatArray, nq: Int, ne: Int, lsbMode: Boolean, highRate: Boolean) {
        val m = models
        val lut = SPEC_LUT
        var low = acLow
        var range = acRange
        var carry = acCarry
        var state = 0
        var i = 0
        val lsbI = if (lsbMode) 1 else 0
        val hr = if (highRate) 2 else 0
        for (h in 0 until 2) {
            val lutBase = (hr + h) * 256
            val lim = minOf(nq, (ne + 2) shr (1 - h))
            while (i < lim) {
                var a = qa[i]
                var b = qa[i + 1]
                val lb = (lutBase + state) * 4
                var mm = (a or b) ushr 2
                var k = 0
                var shr = 0
                if (mm != 0) {
                    if (lsbMode) {
                        run {
                            val base = lut[lb + k] * 34
                            val r = range ushr 10
                            low += r * m[base + 2 * (16)].code
                            range = r * m[base + 2 * (16) + 1].code
                            carry = carry or (low ushr 24)
                            low = low and 0xffffff
                            if (range < 0x10000) {
                                acLow = low
                                acCarry = carry
                                do {
                                    acShift()
                                    range = range shl 8
                                } while (range < 0x10000)
                                low = acLow
                                carry = acCarry
                            }
                        }
                        k++
                    }
                    mm = mm ushr lsbI
                    while (mm != 0) {
                        putBits((a ushr k) and 1, 1)
                        putBits((b ushr k) and 1, 1)
                        run {
                            val base = lut[lb + minOf(k, 3)] * 34
                            val r = range ushr 10
                            low += r * m[base + 2 * (16)].code
                            range = r * m[base + 2 * (16) + 1].code
                            carry = carry or (low ushr 24)
                            low = low and 0xffffff
                            if (range < 0x10000) {
                                acLow = low
                                acCarry = carry
                                do {
                                    acShift()
                                    range = range shl 8
                                } while (range < 0x10000)
                                low = acLow
                                carry = acCarry
                            }
                        }
                        mm = mm ushr 1
                        k++
                    }
                    a = a ushr lsbI
                    b = b ushr lsbI
                    shr = k - lsbI
                    k = minOf(k, 3)
                }
                if (a != 0) putBits(if (x[i] < 0) 1 else 0, 1)
                if (b != 0) putBits(if (x[i + 1] < 0) 1 else 0, 1)
                a = a ushr shr
                b = b ushr shr
                run {
                    val base = lut[lb + k] * 34
                    val r = range ushr 10
                    low += r * m[base + 2 * (a + 4 * b)].code
                    range = r * m[base + 2 * (a + 4 * b) + 1].code
                    carry = carry or (low ushr 24)
                    low = low and 0xffffff
                    if (range < 0x10000) {
                        acLow = low
                        acCarry = carry
                        do {
                            acShift()
                            range = range shl 8
                        } while (range < 0x10000)
                        low = acLow
                        carry = acCarry
                    }
                }
                state = ((state shl 4) + (if (k > 1) 12 + k else 1 + (a + b) * (k + 1))) and 0xff
                i += 2
            }
        }
        acLow = low
        acRange = range
        acCarry = carry
    }

    // ---------------------------------------------------------------- Reading

    private fun acGet(): Int = if (pFw < end) buf[pFw++].toInt() and 0xff else 0

    private fun accuLoad() {
        var nbytes = minOf(accuN shr 3, pBw - start)
        accuN -= 8 * nbytes
        while (nbytes != 0) {
            accuV = accuV ushr 8
            accuV = accuV or ((buf[--pBw].toInt() and 0xff) shl (ACCU_BITS - 8))
            nbytes--
        }
        if (accuN >= 8) {
            accuNover = minOf(accuNover + accuN, ACCU_BITS)
            accuV = if (accuN >= 32) accuV else accuV ushr accuN
            accuN = 0
        }
    }

    fun getBit(): Int = getBits(1)

    fun getBits(n: Int): Int {
        if (accuN + n <= ACCU_BITS) {
            val v = (accuV ushr accuN) and ((1 shl n) - 1)
            accuN += n
            return v
        }
        return getBitsGeneric(n)
    }

    private fun getBitsGeneric(n: Int): Int {
        accuLoad()
        val n1 = minOf(ACCU_BITS - accuN, n)
        var v = (accuV ushr accuN) and ((1 shl n1) - 1)
        accuN += n1
        val n2 = n - n1
        if (n2 != 0) {
            accuLoad()
            v = v or (((accuV ushr accuN) and ((1 shl n2) - 1)) shl n1)
            accuN += n2
        }
        return v
    }

    /**
     * liblc3 get_quantized: arithmetic-decodes the quantized spectrum into x[xo until xo + nq] with the coder state
     * held in locals. Returns the noise-filling seed, or -1 on a malformed frame.
     */
    fun getSpectrum(x: FloatArray, xo: Int, nq: Int, ne: Int, lsbMode: Boolean, highRate: Boolean): Int {
        val m = models
        val lut = SPEC_LUT
        val bf = buf
        val e = end
        var low = acLow
        var range = acRange
        var err = acError
        var p = pFw
        var seed = 0
        var state = 0
        var i = 0
        var ret = 0
        val hr = if (highRate) 2 else 0
        outer@ for (h in 0 until 2) {
            val lutBase = (hr + h) * 256
            val lim = minOf(nq, (ne + 2) shr (1 - h))
            while (i < lim) {
                val lb = (lutBase + state) * 4
                var u = 0
                var v = 0
                var k = 0
                var shl = 0
                var sym: Int
                var first = true
                while (true) {
                    run {
                        val base = lut[lb + k] * 34
                        val r = (range ushr 10) and 0xffff
                        if (low >= (r shl 10)) err = true
                        if (err) low = 0
                        var q = 16
                        if (low < r * m[base + 32].code) {
                            q = 8
                            q -= if (low < r * m[base + 16].code) 4 else -4
                            q -= if (low < r * m[base + 2 * q].code) 2 else -2
                            q -= if (low < r * m[base + 2 * q].code) 1 else -1
                            if (low < r * m[base + 2 * q].code) q -= 1
                        }
                        low -= r * m[base + 2 * q].code
                        range = r * m[base + 2 * q + 1].code
                        while (range < 0x10000) {
                            low = ((low shl 8) or (if (p < e) bf[p++].toInt() and 0xff else 0)) and 0xffffff
                            range = range shl 8
                        }
                        sym = q
                    }
                    if (sym < 16) break
                    if (first && lsbMode) {
                        first = false
                        k++
                        shl++
                        continue
                    }
                    first = false
                    if (shl >= 14) break
                    u = u or (getBit() shl shl)
                    v = v or (getBit() shl shl)
                    if (k < 3) k++
                    shl++
                }
                if (sym >= 16) {
                    ret = -1
                    break@outer
                }
                val a = sym and 3
                val b = sym shr 2
                u = u or (a shl shl)
                v = v or (b shl shl)
                x[xo + i] = (if (u != 0 && getBit() != 0) -u else u).toFloat()
                x[xo + i + 1] = (if (v != 0 && getBit() != 0) -v else v).toFloat()
                seed = (seed + (u and 0x7fff) * i + (v and 0x7fff) * (i + 1)) and 0xffff
                state = ((state shl 4) + (if (k > 1) 12 + k else 1 + (a + b) * (k + 1))) and 0xff
                i += 2
            }
        }
        acLow = low
        acRange = range
        acError = err
        pFw = p
        return if (ret < 0) -1 else seed
    }

    fun getSymbol(model: Int): Int {
        val models = this.models
        val base = model
        val range = (acRange ushr 10) and 0xffff
        if (acLow >= (range shl 10)) acError = true
        if (acError) acLow = 0
        var s = 16
        if (acLow < range * models[base + 2 * s].code) {
            s = s shr 1
            s -= if (acLow < range * models[base + 2 * s].code) 4 else -4
            s -= if (acLow < range * models[base + 2 * s].code) 2 else -2
            s -= if (acLow < range * models[base + 2 * s].code) 1 else -1
            if (acLow < range * models[base + 2 * s].code) s -= 1
        }
        acLow -= range * models[base + 2 * s].code
        acRange = range * models[base + 2 * s + 1].code
        if (acRange < 0x10000) {
            while (acRange < 0x10000) {
                acLow = ((acLow shl 8) or acGet()) and 0xffffff
                acRange = acRange shl 8
            }
        }
        return s
    }
}

private const val ACCU_BITS = 32
private const val AC_BITS = 24
private val EMPTY = ByteArray(0)

/** Arithmetic-coder model offsets into [AC_MODELS]: spectrum models 0..63, TNS order 64..65, TNS coefficients 66..73. */
internal const val TNS_ORDER_M = 64 * 34
internal const val TNS_COEF_M = 66 * 34

/** All arithmetic-coder models, 17 (low, range) pairs each. */
@JvmField internal val AC_MODELS: CharArray = chars(Lc3Tables.SPECTRUM_MODELS, Lc3Tables.TNS_ORDER_MODELS, Lc3Tables.TNS_COEFFS_MODELS)

/** Spectrum context lookup [highRate][half][state][k] -> model, as bytes. */
@JvmField internal val SPEC_LUT: ByteArray = ByteArray(Lc3Tables.SPECTRUM_LOOKUP.size) { Lc3Tables.SPECTRUM_LOOKUP[it].toByte() }

/** Spectrum symbol costs [model][symbol] in 1/2048 bit. */
@JvmField internal val SPEC_BITS: CharArray = CharArray(Lc3Tables.SPECTRUM_BITS.size) { Lc3Tables.SPECTRUM_BITS[it].toChar() }

private fun chars(a: IntArray, b: IntArray, c: IntArray): CharArray {
    val r = CharArray(a.size + b.size + c.size)
    for (i in a.indices) r[i] = a[i].toChar()
    for (i in b.indices) r[a.size + i] = b[i].toChar()
    for (i in c.indices) r[a.size + b.size + i] = c[i].toChar()
    return r
}
