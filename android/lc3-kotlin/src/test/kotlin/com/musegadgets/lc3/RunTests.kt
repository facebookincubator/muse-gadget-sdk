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

import kotlin.system.exitProcess

/** Runs Lc3Checks without JUnit (build.sh test). */
object RunTests {
    @JvmStatic
    fun main(args: Array<String>) {
        var failed = 0
        for ((name, check) in Lc3Checks.all) {
            try {
                check()
                println("PASS  $name")
            } catch (e: Throwable) {
                failed++
                println("FAIL  $name: ${e.message}")
            }
        }
        println(if (failed == 0) "all ${Lc3Checks.all.size} checks passed" else "$failed of ${Lc3Checks.all.size} checks failed")
        exitProcess(if (failed == 0) 0 else 1)
    }
}
