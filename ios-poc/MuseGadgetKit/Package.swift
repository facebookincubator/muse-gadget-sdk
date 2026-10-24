// swift-tools-version:5.9

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
