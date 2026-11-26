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
 * Kotlin translation of liblc3 v1.1.3 mdct.c (scalar path). Modified by Meta.
 */
package com.musegadgets.lc3

@Suppress("MagicNumber", "LongMethod", "NestedBlockDepth", "LongParameterList")
internal class Lc3Mdct(cfg: Lc3Config) {
    private val ns = cfg.ns
    private val nd = cfg.nd
    private val n4 = ns / 2 // complex FFT size (80 or 60)
    private val win = cfg.win

    // Rotation twiddles, split
    private val rotRe = FloatArray(n4) { cfg.rot[2 * it] }
    private val rotIm = FloatArray(n4) { cfg.rot[2 * it + 1] }

    // FFT stage plan: 5 x (3 x)? 2^k
    private val n5 = n4 / 5 // number of interleaved 5-point transforms
    private val hasBf3 = (n5 and (n5 - 1)) != 0
    private val bf3N3 = cfg.bf3N3
    private val bf3W0Re: FloatArray
    private val bf3W0Im: FloatArray
    private val bf3W1Re: FloatArray
    private val bf3W1Im: FloatArray
    private val bf2Re: Array<FloatArray>
    private val bf2Im: Array<FloatArray>
    private val bf2N2: IntArray

    // Ping-pong FFT buffers (split complex) and the window scratch
    private val aRe = FloatArray(n4)
    private val aIm = FloatArray(n4)
    private val bRe = FloatArray(n4)
    private val bIm = FloatArray(n4)
    private val u = FloatArray(ns)
    private var resultInA = false

    init {
        // bf3 twiddles: t[(r * n3 + j) * 2 + e] complex, r = output row (0..2), e = which input (x1 / x2)
        val t = cfg.bf3T
        val n3 = bf3N3
        bf3W0Re = FloatArray(3 * n3) { t[4 * it] }
        bf3W0Im = FloatArray(3 * n3) { t[4 * it + 1] }
        bf3W1Re = FloatArray(3 * n3) { t[4 * it + 2] }
        bf3W1Im = FloatArray(3 * n3) { t[4 * it + 3] }
        bf2N2 = cfg.bf2N2
        bf2Re = Array(cfg.bf2T.size) { s -> FloatArray(bf2N2[s]) { cfg.bf2T[s][2 * it] } }
        bf2Im = Array(cfg.bf2T.size) { s -> FloatArray(bf2N2[s]) { cfg.bf2T[s][2 * it + 1] } }
    }

    // ------------------------------------------------------------------ FFT

    private fun fft5(xr: FloatArray, xi: FloatArray, yr: FloatArray, yi: FloatArray, n: Int) {
        val cos1 = 0.3090169944f
        val cos2 = -0.8090169944f
        val sin1 = -0.9510565163f
        val sin2 = -0.5877852523f
        var o = 0
        for (i in 0 until n) {
            val x0re = xr[i]
            val x0im = xi[i]
            val a1r = xr[i + n]; val a1i = xi[i + n]
            val a2r = xr[i + 2 * n]; val a2i = xi[i + 2 * n]
            val a3r = xr[i + 3 * n]; val a3i = xi[i + 3 * n]
            val a4r = xr[i + 4 * n]; val a4i = xi[i + 4 * n]
            val s14re = a1r + a4r
            val s14im = a1i + a4i
            val d14re = a1r - a4r
            val d14im = a1i - a4i
            val s23re = a2r + a3r
            val s23im = a2i + a3i
            val d23re = a2r - a3r
            val d23im = a2i - a3i

            yr[o + 4] = x0re + s14re * cos1 + d14im * sin1 + s23re * cos2 + d23im * sin2
            yi[o + 4] = x0im + s14im * cos1 - d14re * sin1 + s23im * cos2 - d23re * sin2
            yr[o] = x0re + s14re + s23re
            yi[o] = x0im + s14im + s23im
            yr[o + 1] = x0re + s14re * cos1 - d14im * sin1 + s23re * cos2 - d23im * sin2
            yi[o + 1] = x0im + s14im * cos1 + d14re * sin1 + s23im * cos2 + d23re * sin2
            yr[o + 2] = x0re + s14re * cos2 - d14im * sin2 + s23re * cos1 + d23im * sin1
            yi[o + 2] = x0im + s14im * cos2 + d14re * sin2 + s23im * cos1 - d23re * sin1
            yr[o + 3] = x0re + s14re * cos2 + d14im * sin2 + s23re * cos1 - d23im * sin1
            yi[o + 3] = x0im + s14im * cos2 - d14re * sin2 + s23im * cos1 + d23re * sin1
            o += 5
        }
    }

    private fun fftBf3(xr: FloatArray, xi: FloatArray, yr: FloatArray, yi: FloatArray, n: Int) {
        val n3 = bf3N3
        val w0r = bf3W0Re
        val w0i = bf3W0Im
        val w1r = bf3W1Re
        val w1i = bf3W1Im
        var x = 0
        var yb = 0
        val s1 = n * n3
        for (i in 0 until n) {
            for (j in 0 until n3) {
                val x0re = xr[x]; val x0im = xi[x]
                val x1re = xr[x + s1]; val x1im = xi[x + s1]
                val x2re = xr[x + 2 * s1]; val x2im = xi[x + 2 * s1]
                // row 0
                var a = w0r[j]; var b = w0i[j]; var c = w1r[j]; var d = w1i[j]
                yr[yb + j] = x0re + x1re * a - x1im * b + x2re * c - x2im * d
                yi[yb + j] = x0im + x1im * a + x1re * b + x2im * c + x2re * d
                // row 1
                a = w0r[n3 + j]; b = w0i[n3 + j]; c = w1r[n3 + j]; d = w1i[n3 + j]
                yr[yb + n3 + j] = x0re + x1re * a - x1im * b + x2re * c - x2im * d
                yi[yb + n3 + j] = x0im + x1im * a + x1re * b + x2im * c + x2re * d
                // row 2
                a = w0r[2 * n3 + j]; b = w0i[2 * n3 + j]; c = w1r[2 * n3 + j]; d = w1i[2 * n3 + j]
                yr[yb + 2 * n3 + j] = x0re + x1re * a - x1im * b + x2re * c - x2im * d
                yi[yb + 2 * n3 + j] = x0im + x1im * a + x1re * b + x2im * c + x2re * d
                x++
            }
            yb += 3 * n3
        }
    }

    private fun fftBf2(wr: FloatArray, wi: FloatArray, n2: Int, xr: FloatArray, xi: FloatArray,
                       yr: FloatArray, yi: FloatArray, n: Int) {
        val half = n * n2
        var x0 = 0
        var y0 = 0
        for (i in 0 until n) {
            val y1 = y0 + n2
            val x1 = x0 + half
            for (j in 0 until n2) {
                val ar = xr[x0 + j]
                val ai = xi[x0 + j]
                val br = xr[x1 + j]
                val bi = xi[x1 + j]
                val c = wr[j]
                val s = wi[j]
                yr[y0 + j] = ar + br * c - bi * s
                yi[y0 + j] = ai + bi * c + br * s
                yr[y1 + j] = ar - br * c + bi * s
                yi[y1 + j] = ai - bi * c - br * s
            }
            x0 += n2
            y0 += 2 * n2
        }
    }

    /** FFT of n4 points; input in (aRe, aIm). Sets [resultInA] to say which buffer holds the result. */
    private fun fft() {
        var n = n5
        fft5(aRe, aIm, bRe, bIm, n)
        var inA = false
        if (hasBf3) {
            n /= 3
            fftBf3(bRe, bIm, aRe, aIm, n)
            inA = true
        }
        var s = 0
        while (n > 1) {
            n = n shr 1
            if (inA) fftBf2(bf2Re[s], bf2Im[s], bf2N2[s], aRe, aIm, bRe, bIm, n)
            else fftBf2(bf2Re[s], bf2Im[s], bf2N2[s], bRe, bIm, aRe, aIm, n)
            inA = !inA
            s++
        }
        resultInA = inA
    }

    // ------------------------------------------------------------------ Forward

    /** x[xo..xo+ns) input, d[dOff..dOff+nd) delay line (updated), y[yo..yo+ns) output spectrum. */
    fun forward(x: FloatArray, xo: Int, d: FloatArray, dOff: Int, y: FloatArray, yo: Int) {
        val win = win
        val u = u
        val ns = ns
        val nd = nd
        // --- Windowing (mdct_window) into u ---
        var w0 = 0
        var w1 = ns
        var w2 = ns
        var w3 = ns + nd
        var x0 = xo + ns - nd
        var x1 = x0
        var y0 = ns / 2
        var y1 = y0
        var d0 = dOff
        var d1 = dOff + nd
        while (x1 > xo) {
            u[--y0] = d[d0] * win[w0++] - x[--x1] * win[--w1]
            val v = x[x0++]
            d[d0++] = v
            u[y1++] = v * win[w2++]
        }
        x1 += ns
        while (x0 < x1) {
            u[--y0] = d[d0] * win[w0++] - d[--d1] * win[--w1]
            val v = x[x0++]
            val q = x[--x1]
            d[d0++] = v
            d[d1] = q
            u[y1++] = v * win[w2++] + q * win[--w3]
        }

        // --- Pre-rotation (mdct_pre_fft) u -> a ---
        val n4 = n4
        val rr = rotRe
        val ri = rotIm
        val ar = aRe
        val ai = aIm
        for (k in 0 until n4 / 2) {
            // pair k takes u[2k], u[2k+1] from the front and u[2n4-1-2k], u[2n4-2-2k] from the back
            val f0 = u[2 * k]
            val f1 = u[2 * k + 1]
            val b0 = u[2 * n4 - 1 - 2 * k]
            val b1 = u[2 * n4 - 2 - 2 * k]
            val uwRe = rr[k]
            val uwIm = ri[k]
            ar[k] = -b0 * uwRe + f0 * uwIm
            ai[k] = f0 * uwRe + b0 * uwIm
            val vwRe = rr[n4 - 1 - k]
            val vwIm = ri[n4 - 1 - k]
            ar[n4 - 1 - k] = -b1 * vwIm + f1 * vwRe
            ai[n4 - 1 - k] = -f1 * vwIm - b1 * vwRe
        }

        fft()
        val zr = if (resultInA) aRe else bRe
        val zi = if (resultInA) aIm else bIm

        // --- Post-rotation (mdct_post_fft) z -> y ---
        val n8 = n4 shr 1
        for (k in 0 until n8) {
            val a = n8 + k
            val b = n8 - 1 - k
            val xare = zr[a]; val xaim = zi[a]
            val xbre = zr[b]; val xbim = zi[b]
            val ware = rr[a]; val waim = ri[a]
            val wbre = rr[b]; val wbim = ri[b]
            y[yo + n4 + 2 * k] = xaim * waim + xare * ware
            y[yo + n4 + 2 * k + 1] = xbre * wbim - xbim * wbre
            y[yo + n4 - 1 - 2 * k] = xare * waim - xaim * ware
            y[yo + n4 - 2 - 2 * k] = xbim * wbim + xbre * wbre
        }
    }

    // ------------------------------------------------------------------ Inverse

    /** x[xo..xo+ns) spectrum, d[dOff..dOff+nd) overlap state (updated), y[yo..yo+ns) output. */
    fun inverse(x: FloatArray, xo: Int, d: FloatArray, dOff: Int, y: FloatArray, yo: Int) {
        val n4 = n4
        val rr = rotRe
        val ri = rotIm
        val ar = aRe
        val ai = aIm
        // --- imdct_pre_fft: x -> a ---
        for (k in 0 until n4 / 2) {
            val u0 = x[xo + 2 * k]
            val v0 = x[xo + 2 * k + 1]
            val u1 = x[xo + 2 * n4 - 1 - 2 * k]
            val v1 = x[xo + 2 * n4 - 2 - 2 * k]
            val uwRe = rr[k]; val uwIm = ri[k]
            val vwRe = rr[n4 - 1 - k]; val vwIm = ri[n4 - 1 - k]
            ar[k] = -u0 * uwRe - u1 * uwIm
            ai[k] = -u1 * uwRe + u0 * uwIm
            ar[n4 - 1 - k] = -v1 * vwRe - v0 * vwIm
            ai[n4 - 1 - k] = -v0 * vwRe + v1 * vwIm
        }

        fft()
        val zr = if (resultInA) aRe else bRe
        val zi = if (resultInA) aIm else bIm

        // --- imdct_post_fft: z -> u ---
        val u = u
        for (k in 0 until n4 / 2) {
            val a = k
            val b = n4 - 1 - k
            val uzRe = zr[a]; val uzIm = zi[a]
            val vzRe = zr[b]; val vzIm = zi[b]
            val uwRe = rr[a]; val uwIm = ri[a]
            val vwRe = rr[b]; val vwIm = ri[b]
            u[2 * k] = uzRe * uwIm - uzIm * uwRe
            u[2 * n4 - 1 - 2 * k] = uzRe * uwRe + uzIm * uwIm
            u[2 * n4 - 2 - 2 * k] = vzRe * vwIm - vzIm * vwRe
            u[2 * k + 1] = vzRe * vwRe + vzIm * vwIm
        }

        // --- imdct_window: u, d -> y, d ---
        val win = win
        val nd = nd
        var w2 = 0
        var w0 = 3 * n4
        var w1 = w0
        var x0 = dOff + nd - n4
        var x1 = x0
        var y0 = yo + nd - n4
        var y1 = y0
        var y2 = dOff + nd
        var y3 = dOff
        var xi = 0
        while (y0 > yo) {
            y[--y0] = d[--x0] - u[xi] * win[w1++]
            y[y1++] = d[x1++] + u[xi++] * win[--w0]
        }
        while (y1 < yo + nd) {
            y[y1++] = d[x1++] + u[xi++] * win[--w0]
        }
        while (y1 < yo + 2 * n4) {
            val v = u[xi++]
            y[y1++] = v * win[--w0]
            d[--y2] = v * win[w2++]
        }
        while (y2 > y3) {
            val v = u[xi++]
            d[y3++] = v * win[--w0]
            d[--y2] = v * win[w2++]
        }
    }
}
