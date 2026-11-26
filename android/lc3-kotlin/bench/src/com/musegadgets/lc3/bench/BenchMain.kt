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
package com.musegadgets.lc3.bench

import com.musegadgets.lc3.Lc3Decoder
import com.musegadgets.lc3.Lc3Encoder
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * File to file encoder and decoder, for tools/compare_liblc3.sh (16 kHz mono, s16le raw PCM, concatenated
 * fixed-size frames, as tools/lc3ref.c):
 *   enc IN.raw OUT.lc3 [-d dt_us] [-b nbytes]
 *   dec IN.lc3 OUT.raw [-d dt_us] [-b nbytes] [-P n]      -P n: every n-th frame lost (concealed)
 */
object BenchMain {
    @JvmStatic
    fun main(args: Array<String>) {
        val mode = args[0]
        var dtUs = 10000
        var nbytes = 40
        var lossP = 0
        var i = 3
        while (i < args.size) {
            when (args[i]) {
                "-d" -> dtUs = args[++i].toInt()
                "-b" -> nbytes = args[++i].toInt()
                "-P" -> lossP = args[++i].toInt()
            }
            i++
        }
        require(mode == "enc" || mode == "dec") { "mode is enc or dec" }
        val data = File(args[1]).readBytes()
        val ns = dtUs * 16 / 1000
        val enc = mode == "enc"
        val nframes = if (enc) data.size / 2 / ns else data.size / nbytes
        val pcm = ShortArray(nframes * ns)
        val out = ByteArray(if (enc) nframes * nbytes else nframes * ns * 2)
        if (enc) {
            ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().get(pcm)
            val e = Lc3Encoder(dtUs)
            for (f in 0 until nframes) e.encode(pcm, f * ns, out, f * nbytes, nbytes)
        } else {
            val d = Lc3Decoder(dtUs)
            for (f in 0 until nframes) {
                val lost = lossP != 0 && f % lossP == lossP - 1
                if (lost) d.conceal(pcm, f * ns) else d.decode(data, f * nbytes, nbytes, pcm, f * ns)
            }
            ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().put(pcm)
        }
        File(args[2]).writeBytes(out)
    }
}
