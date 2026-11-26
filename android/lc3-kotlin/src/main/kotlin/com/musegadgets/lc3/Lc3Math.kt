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
 * Kotlin translation of liblc3 v1.1.3 fastmath.h. Modified by Meta.
 */
package com.musegadgets.lc3

import java.lang.Float.floatToRawIntBits

/** Bit-exact equivalents of liblc3 fastmath.h (lc3_exp2f and lc3_log2f are inlined where they are used). */
@Suppress("MagicNumber")
internal object Lc3Math {
    /** [n][0], [n][1] interleaved (Q15). */
    private val DB_T = intArrayOf(
        0, 4379, 4379, 4248, 8627, 4125, 12753, 4009,
        16762, 3899, 20661, 3795, 24456, 3697, 28153, 3603,
        31755, 3514, 35269, 3429, 38699, 3349, 42047, 3272,
        45319, 3198, 48517, 3128, 51645, 3061, 54705, 2996,
        8381, 2934, 11315, 2875, 14190, 2818, 17008, 2763,
        19772, 2711, 22482, 2660, 25142, 2611, 27754, 2564,
        30318, 2519, 32837, 2475, 35312, 2433, 37744, 2392,
        40136, 2352, 42489, 2314, 44803, 2277, 47080, 2241,
    )

    @JvmStatic
    fun dbQ16(x: Float): Int {
        val u = floatToRawIntBits(x * x)
        val e2 = (u ushr 22) - 2 * 127
        val hi = (u ushr 18) and 0x1f
        val lo = (u ushr 2) and 0xffff
        return e2 * 49321 + DB_T[2 * hi] + ((DB_T[2 * hi + 1] * lo) shr 16)
    }

    @JvmStatic
    fun fmaxf(a: Float, b: Float): Float = if (a >= b) a else b

    @JvmStatic
    fun fminf(a: Float, b: Float): Float = if (a <= b) a else b
}
