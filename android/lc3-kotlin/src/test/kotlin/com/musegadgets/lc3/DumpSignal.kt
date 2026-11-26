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

import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Writes [TestSignal] as raw s16le (for tools/make_vectors.sh): DumpSignal OUT.raw */
object DumpSignal {
    @JvmStatic
    fun main(args: Array<String>) {
        val s = TestSignal.generate()
        val b = ByteBuffer.allocate(s.size * 2).order(ByteOrder.LITTLE_ENDIAN)
        b.asShortBuffer().put(s)
        File(args[0]).writeBytes(b.array())
    }
}
