// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// lc3-kotlin: LC3 encoder and decoder in pure Kotlin (JVM). No Android dependency and, as built here, no Kotlin
// runtime dependency either: the jar only uses the JDK.

plugins {
    kotlin("jvm") version "2.1.21"
    `java-library`
}

group = "com.musegadgets"
version = "1.0.0"

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
        // Without these the jar would call kotlin.jvm.internal.Intrinsics on every public entry point.
        freeCompilerArgs.addAll("-Xno-param-assertions", "-Xno-call-assertions", "-Xno-receiver-assertions")
    }
}

dependencies {
    // Needed to compile, not at runtime (gradle.properties turns off the default stdlib dependency).
    compileOnly(kotlin("stdlib"))
    testImplementation(kotlin("stdlib"))
    testImplementation("junit:junit:4.13.2")
}

// The jar carries the license and the attribution notice (Apache-2.0, section 4).
tasks.jar {
    metaInf { from("LICENSE", "NOTICE") }
}

tasks.test {
    testLogging { events("passed", "failed") }
}
