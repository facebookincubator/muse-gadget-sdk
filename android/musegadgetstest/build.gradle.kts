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

// Muse Gadgets Test, for Android Studio and Gradle. Plain Java on framework
// APIs, with no dependencies. The sources
// stay where build.sh (the Gradle-free build) finds them. The optional encoders' native libraries (jni/build.sh,
// jni/build_vpx.sh) are packed from LIB_DIR: the environment variable, or
// musegadgetstest.libDir in local.properties or gradle.properties, default lib/.

import java.util.Properties

plugins {
    id("com.android.application") version "8.13.2"
}

val localProps = Properties().apply {
    val f = file("local.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}
val libDir: String = System.getenv("LIB_DIR")
    ?: localProps.getProperty("musegadgetstest.libDir")
    ?: (findProperty("musegadgetstest.libDir") as String?)
    ?: "lib"

// An x264 library from before the UI streams lacks their JNI functions (the app would fail when one
// starts): the build stops and says to rebuild it.
val x264So = file("$libDir/arm64-v8a/libmgx264.so")
if (x264So.exists()) {
    val text = String(x264So.readBytes(), Charsets.ISO_8859_1)
    for (f in listOf("openUi", "encodeQp", "reconI420", "rgbaConvert")) {
        if (!text.contains("Java_com_musegadgets_test_X264_$f")) {
            throw GradleException("$x264So is an older build without X264.$f: rebuild it with jni/build.sh")
        }
    }
}

android {
    namespace = "com.musegadgets.test"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.musegadgets.test"
        minSdk = 33
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"
        ndk { abiFilters += "arm64-v8a" }
    }

    sourceSets["main"].apply {
        manifest.srcFile("AndroidManifest.xml")
        java.setSrcDirs(listOf("src"))
        res.setSrcDirs(listOf("res"))
        assets.setSrcDirs(listOf("assets"))
        jniLibs.setSrcDirs(listOf(libDir))
    }

    // Pre-encoded video in assets is read in place.
    androidResources { noCompress += listOf("mp4", "264", "ivf") }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        release {
            // A sample app: signed with the debug key so a release build installs.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    lint {
        checkReleaseBuilds = false
    }
}
