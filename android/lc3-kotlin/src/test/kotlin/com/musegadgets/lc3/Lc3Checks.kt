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

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest

/**
 * The conformance and API checks, runnable without JUnit (RunTests, used by build.sh) and through JUnit
 * (Lc3ConformanceTest, used by Gradle). Each check throws AssertionError on failure.
 */
object Lc3Checks {
    private val DTS = intArrayOf(Lc3.FRAME_10MS, Lc3.FRAME_7_5MS)
    private val BYTES = intArrayOf(20, 30, 40, 60, 100, 160, 400)
    private val signal: ShortArray by lazy { TestSignal.generate() }

    val all: List<Pair<String, () -> Unit>> = listOf(
        "encoder matches liblc3 (14 configurations)" to ::encoderMatchesLiblc3,
        "decoder matches liblc3, with and without lost frames" to ::decoderMatchesLiblc3,
        "decoder matches liblc3 on LTPF streams, lost and corrupt frames" to ::ltpfStreamsMatchLiblc3,
        "random frames never throw" to ::randomFramesNeverThrow,
        "a corrupt frame returns false" to ::corruptFrameReturnsFalse,
        "reset restarts the stream" to ::resetRestartsTheStream,
        "invalid arguments throw" to ::invalidArgumentsThrow,
    )

    private fun check(ok: Boolean, msg: () -> String) {
        if (!ok) throw AssertionError(msg())
    }

    private fun sha(b: ByteArray): String = MessageDigest.getInstance("SHA-256").digest(b).joinToString("") { "%02x".format(it) }

    private fun sha(s: ShortArray): String {
        val b = ByteBuffer.allocate(s.size * 2).order(ByteOrder.LITTLE_ENDIAN)
        b.asShortBuffer().put(s)
        return sha(b.array())
    }

    private fun expect(key: String, actual: String) {
        val want = ReferenceVectors.SHA256[key] ?: throw AssertionError("no reference for $key")
        check(want == actual) { "$key: got $actual, liblc3 $want" }
    }

    fun encode(dt: Int, nbytes: Int, pcm: ShortArray = signal): ByteArray {
        val e = Lc3Encoder(dt)
        val ns = e.frameSamples
        val n = pcm.size / ns
        val out = ByteArray(n * nbytes)
        for (f in 0 until n) e.encode(pcm, f * ns, out, f * nbytes, nbytes)
        return out
    }

    /** lossPeriod: every n-th frame concealed; corruptSeed >= 0: frames corrupted like tools/lc3ref.c -X. */
    fun decode(dt: Int, nbytes: Int, stream: ByteArray, lossPeriod: Int = 0, corruptSeed: Int = -1): ShortArray {
        val d = Lc3Decoder(dt)
        val ns = d.frameSamples
        val n = stream.size / nbytes
        val pcm = ShortArray(n * ns)
        val fr = ByteArray(nbytes)
        val seed = intArrayOf(corruptSeed)
        for (f in 0 until n) {
            System.arraycopy(stream, f * nbytes, fr, 0, nbytes)
            if (corruptSeed >= 0) corrupt(fr, seed)
            if (lossPeriod != 0 && f % lossPeriod == lossPeriod - 1) d.conceal(pcm, f * ns)
            else d.decode(fr, 0, nbytes, pcm, f * ns)
        }
        return pcm
    }

    /** Mirror of corrupt() in tools/lc3ref.c (32-bit unsigned LCG). */
    fun corrupt(fr: ByteArray, s: IntArray) {
        fun next(): Long {
            s[0] = s[0] * 1664525 + 1013904223
            return (s[0].toLong() and 0xffffffffL) ushr 8
        }
        val r = next() % 16
        if (r < 12) return
        if (r < 15) {
            val n = 1 + (next() % 8).toInt()
            repeat(n) {
                val b = (next() % (fr.size * 8)).toInt()
                fr[b / 8] = (fr[b / 8].toInt() xor (1 shl (b % 8))).toByte()
            }
        } else {
            for (i in fr.indices) fr[i] = (next() and 0xff).toByte()
        }
    }

    fun encoderMatchesLiblc3() {
        for (dt in DTS) for (nb in BYTES) expect("enc_d${dt}_b$nb", sha(encode(dt, nb)))
    }

    fun decoderMatchesLiblc3() {
        for (dt in DTS) for (nb in BYTES) {
            val s = encode(dt, nb)
            expect("dec_d${dt}_b$nb", sha(decode(dt, nb, s)))
            expect("dec_d${dt}_b${nb}_P7", sha(decode(dt, nb, s, lossPeriod = 7)))
        }
    }

    fun ltpfStreamsMatchLiblc3() {
        for ((dt, nb) in listOf(10000 to 40, 7500 to 30)) {
            val t = "d${dt}_b${nb}_ltpf"
            val s = Lc3Checks::class.java.getResourceAsStream("/vectors/$t.lc3")!!.readBytes()
            expect("dec_$t", sha(decode(dt, nb, s)))
            expect("dec_${t}_P7", sha(decode(dt, nb, s, lossPeriod = 7)))
            expect("dec_${t}_X1", sha(decode(dt, nb, s, corruptSeed = 1)))
        }
    }

    fun randomFramesNeverThrow() {
        val rnd = java.util.Random(7)
        for (dt in DTS) for (nb in intArrayOf(20, 40, 100, 400)) {
            val d = Lc3Decoder(dt)
            val pcm = ShortArray(d.frameSamples)
            val fr = ByteArray(nb)
            repeat(500) {
                rnd.nextBytes(fr)
                d.decode(fr, 0, nb, pcm, 0)
            }
        }
    }

    /** decode() returns true for an intact frame and false for a corrupt one (concealed instead). */
    fun corruptFrameReturnsFalse() {
        val s = encode(10000, 40)
        val d = Lc3Decoder()
        val pcm = ShortArray(d.frameSamples)
        for (f in 0 until s.size / 40) check(d.decode(s, f * 40, 40, pcm, 0)) { "intact frame $f" }
        // Every byte set: the side information's 7-bit last-nonzero-pair field reads all ones, which
        // puts the coded spectrum's end at 256 coefficients, past the 160 of a 10 ms frame at 16 kHz.
        // liblc3 refuses that frame (lc3_spec_get_side: nq > ne), so the decoder must too.
        check(!d.decode(ByteArray(40) { 0xff.toByte() }, 0, 40, pcm, 0)) { "corrupt frame" }
        // Random frames: most are corrupt, and those must say so.
        val rnd = java.util.Random(7)
        val fr = ByteArray(40)
        var corrupt = 0
        repeat(200) { rnd.nextBytes(fr); if (!d.decode(fr, 0, 40, pcm, 0)) corrupt++ }
        check(corrupt > 0) { "random frames all decoded" }
    }

    fun resetRestartsTheStream() {
        val e = Lc3Encoder()
        val first = e.encode(signal.copyOfRange(0, 160))
        e.encode(signal.copyOfRange(160, 320))
        e.reset()
        check(first.contentEquals(e.encode(signal.copyOfRange(0, 160)))) { "encoder reset" }
        val d = Lc3Decoder()
        val a = d.decode(first)
        d.decode(first)
        d.reset()
        check(a.contentEquals(d.decode(first))) { "decoder reset" }
    }

    private fun throws(block: () -> Unit): Boolean = try { block(); false } catch (e: IllegalArgumentException) { true } catch (e: IndexOutOfBoundsException) { true }

    fun invalidArgumentsThrow() {
        check(throws { Lc3Encoder(5000) }) { "5 ms frames" }
        check(throws { Lc3Encoder(10000, 48000) }) { "48 kHz" }
        check(throws { Lc3Decoder(10000, 8000) }) { "8 kHz" }
        val e = Lc3Encoder()
        check(throws { e.encode(ShortArray(160), 0, ByteArray(40), 0, 19) }) { "19 bytes" }
        check(throws { e.encode(ShortArray(160), 0, ByteArray(400), 0, 401) }) { "401 bytes" }
        check(throws { e.encode(ShortArray(159), 0, ByteArray(40), 0, 40) }) { "short pcm" }
        check(throws { e.encode(ShortArray(160), 0, ByteArray(39), 0, 40) }) { "short out" }
        val d = Lc3Decoder()
        check(throws { d.decode(ByteArray(40), 0, 41, ShortArray(160), 0) }) { "frame overrun" }
        check(throws { d.decode(ByteArray(40), 0, 40, ShortArray(100), 0) }) { "short pcm" }
        check(throws { Lc3.frameBytes(Lc3.FRAME_10MS, -1) }) { "negative bitrate" }
        check(throws { Lc3.bitrate(Lc3.FRAME_10MS, -1) }) { "negative frame size" }
    }
}
