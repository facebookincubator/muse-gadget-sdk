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
 * Frame geometry for the supported configurations (16 kHz, 7.5 / 10 ms), from liblc3 v1.1.3 tables.h.
 * Modified by Meta.
 */
package com.musegadgets.lc3

@Suppress("MagicNumber")
internal class Lc3Config(frameUs: Int, sampleRateHz: Int) {
    /** liblc3 enum lc3_dt: 2 = 7.5 ms, 3 = 10 ms. */
    val dt: Int
    val ns: Int
    val ne: Int
    val nd: Int
    val nh: Int
    val win: FloatArray
    val rot: FloatArray
    val bandLim: IntArray
    val nb = 64
    val bf3T: FloatArray = Lc3Tables.BF3_T_0
    val bf3N3: Int = Lc3Tables.BF3_N3_0
    val bf2T: Array<FloatArray>
    val bf2N2: IntArray

    init {
        when (frameUs) {
            10000 -> {
                dt = 3
                win = Lc3Tables.MDCT_WIN_10M
                rot = Lc3Tables.MDCT_ROT_10M
                bandLim = Lc3Tables.BAND_LIM_10M
                bf2T = arrayOf(Lc3Tables.BF2_T_0_0, Lc3Tables.BF2_T_1_0, Lc3Tables.BF2_T_2_0, Lc3Tables.BF2_T_3_0)
            }
            7500 -> {
                dt = 2
                win = Lc3Tables.MDCT_WIN_7M5
                rot = Lc3Tables.MDCT_ROT_7M5
                bandLim = Lc3Tables.BAND_LIM_7M5
                bf2T = arrayOf(Lc3Tables.BF2_T_0_1, Lc3Tables.BF2_T_1_1)
            }
            else -> throw IllegalArgumentException("frame duration $frameUs us: only 7500 and 10000 are supported")
        }
        if (sampleRateHz != Lc3.SAMPLE_RATE_16K) {
            throw IllegalArgumentException("sample rate $sampleRateHz Hz: only 16000 is supported")
        }
        ns = 40 * (1 + dt)
        ne = ns
        nd = (ns + (if (dt == 2) 64 else 40)) shr 1
        nh = (8 + (if (dt == 2) 1 else 0)) * 40
        bf2N2 = IntArray(bf2T.size) { bf2T[it].size / 2 }
        check(bandLim.size == nb + 1)
    }
}
