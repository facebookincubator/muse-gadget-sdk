// swift-tools-version: 6.0
// Copyright (c) Meta Platforms, Inc. and affiliates.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
import PackageDescription

let package = Package(
    name: "MuseCompanion",
    platforms: [.macOS(.v14)],
    products: [.executable(name: "MuseCompanion", targets: ["MuseCompanion"])],
    targets: [
        .target(name: "MuseChat"),
        .executableTarget(name: "MuseCompanion", dependencies: ["MuseChat"], resources: [.copy("Resources/jollybot.gif")]),
        .testTarget(name: "MuseChatTests", dependencies: ["MuseChat"])
    ],
    swiftLanguageModes: [.v5]
)
