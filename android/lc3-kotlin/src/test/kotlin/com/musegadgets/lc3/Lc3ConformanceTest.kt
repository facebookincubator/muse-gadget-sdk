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

import org.junit.Test

/** JUnit entry points for Lc3Checks (Gradle: ./gradlew test). */
class Lc3ConformanceTest {
    @Test fun encoderMatchesLiblc3() = Lc3Checks.encoderMatchesLiblc3()
    @Test fun decoderMatchesLiblc3() = Lc3Checks.decoderMatchesLiblc3()
    @Test fun ltpfStreamsMatchLiblc3() = Lc3Checks.ltpfStreamsMatchLiblc3()
    @Test fun randomFramesNeverThrow() = Lc3Checks.randomFramesNeverThrow()
    @Test fun corruptFrameReturnsFalse() = Lc3Checks.corruptFrameReturnsFalse()
    @Test fun resetRestartsTheStream() = Lc3Checks.resetRestartsTheStream()
    @Test fun invalidArgumentsThrow() = Lc3Checks.invalidArgumentsThrow()
}
