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
// Pair a Muse gadget identity from this Mac, then hand it to the iPhone app.
//
//   swift run MusePairMac --sdk-token mgst_… [--out pairing-export.json]
//
// The Mac advertises as MuseGadgetXXXXXX; add it from the Muse app on a phone
// (Settings > Devices > Developer mode, then Add Device). On success the
// identity and device tokens are written to --out, readable only by you.

import Foundation
import MuseGadgetKit
import Network

setvbuf(stdout, nil, _IOLBF, 0)

let arguments = CommandLine.arguments
func option(_ flag: String) -> String? {
    guard let index = arguments.firstIndex(of: flag), index + 1 < arguments.count else { return nil }
    return arguments[index + 1]
}

let output = URL(fileURLWithPath: option("--out") ?? "pairing-export.json")
let sdkToken = option("--sdk-token") ?? ProcessInfo.processInfo.environment["MUSE_SDK_TOKEN"]
let windowMinutes = Double(option("--minutes") ?? "") ?? 10
let identity = option("--mac").map(GadgetIdentity.init) ?? .generate()
let version = "0.1.0-ios"

let monitor = NWPathMonitor()
monitor.start(queue: DispatchQueue(label: "path"))

func log(_ line: String) {
    print("\(Date().formatted(date: .omitted, time: .standard)) \(line)")
}

let peripheral = BLEPeripheral(localName: identity.bleName)
let pairing = PairingSession(nodeID: identity.nodeID, deviceID: identity.deviceID, mac: identity.mac,
                             firmwareVersion: version, sdkToken: sdkToken)
let controller = SetupController(
    pairing: pairing, identity: identity, version: version, transport: peripheral,
    isOnline: { monitor.currentPath.status == .satisfied },
    provision: { credentials, commit in
        let (vms, status) = await MuseAPI(apiURLv2: credentials.apiURLv2)
            .fetchVMs(accessToken: credentials.accessToken)
        guard !vms.isEmpty else {
            log("device token check failed (HTTP \(status.map(String.init) ?? "-"))")
            throw ProvisionFailed("auth_failed")
        }
        let saved = commit {
            guard let data = try? JSONEncoder().encode(PairingExport(identity: identity, credentials: credentials)),
                  (try? data.write(to: output, options: .atomic)) != nil
            else { return false }
            try? FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: output.path)
            return true
        }
        guard saved else { throw ProvisionFailed("error_storage") }
    },
    onComplete: {
        log("Paired. Saved the pairing to \(output.path)")
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { exit(0) }
    })
controller.log = log
peripheral.log = log
peripheral.onWrite = { controller.onWrite($0) }
peripheral.onDisconnect = { controller.onDisconnect() }

if sdkToken == nil { log("warning: no SDK token (--sdk-token or MUSE_SDK_TOKEN); pairing may be refused") }
log("Node id \(identity.nodeID). Setup open for \(Int(windowMinutes)) minutes.")
log("In the Muse app: Settings > Devices > Developer mode, then Add Device > \(identity.bleName)")
peripheral.start()
DispatchQueue.main.asyncAfter(deadline: .now() + windowMinutes * 60) {
    log("Setup window closed without pairing.")
    exit(1)
}
dispatchMain()
