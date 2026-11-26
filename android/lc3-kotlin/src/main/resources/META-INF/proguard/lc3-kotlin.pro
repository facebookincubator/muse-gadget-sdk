# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# lc3-kotlin: keep the codec's stages as separate methods. R8 otherwise inlines the single-caller stages into
# one very large method, which ART compiles markedly worse (up to 2x slower on Cortex-A510 in our measurements).
# Names may still be obfuscated and unused classes removed.
-keep,allowobfuscation,allowshrinking class com.musegadgets.lc3.** { *; }
# The classes carry Kotlin's class-retention annotations (and nullability annotations); an app without the Kotlin
# runtime doesn't need them.
-dontwarn kotlin.Metadata
-dontwarn kotlin.jvm.**
-dontwarn org.jetbrains.annotations.**
