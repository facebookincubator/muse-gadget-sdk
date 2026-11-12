// swift-tools-version:5.9

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

import Foundation
import PackageDescription

let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().path

let package = Package(
    name: "MuseGadgetKit",
    platforms: [.iOS(.v17), .macOS(.v14)],
    products: [
        .library(name: "MuseGadgetKit", targets: ["MuseGadgetKit"]),
        .executable(name: "MusePairMac", targets: ["MusePairMac"]),
    ],
    targets: [
        .target(name: "MuseGadgetKit"),
        // Runs the gadget's BLE setup on a Mac, for pairing from a Muse app on
        // the iPhone, then exports the pairing for the iPhone gadget app.
        .executableTarget(
            name: "MusePairMac",
            dependencies: ["MuseGadgetKit"],
            exclude: ["Info.plist"],
            linkerSettings: [.unsafeFlags([
                "-Xlinker", "-sectcreate", "-Xlinker", "__TEXT", "-Xlinker", "__info_plist",
                "-Xlinker", root + "/Sources/MusePairMac/Info.plist",
            ])]
        ),
        .testTarget(
            name: "MuseGadgetKitTests",
            dependencies: ["MuseGadgetKit"],
            resources: [.copy("Vectors")]
        ),
    ]
)
