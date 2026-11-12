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
import MuseGadgetKit
import Network
import SwiftUI

/// Wires BLE setup and the cloud link together for the UI.
@MainActor
final class GadgetModel: ObservableObject {
    static let version = "0.1.0-ios"
    static let sdkTokenAccount = "sdk_token"

    @Published var logLines: [String] = []
    @Published var shownText: String?
    @Published var pairingOpen = false
    @Published var sdkToken: String
    @Published private(set) var paired = false

    let service: GadgetService
    private let commands: PhoneCommands
    private let pathMonitor = NWPathMonitor()
    private var peripheral: BLEPeripheral?
    private var controller: SetupController?

    var identity: GadgetIdentity { service.identity }

    init() {
        let store = KeychainStore()
        // A pairing done on the Mac (MusePairMac), copied into Documents by devicectl.
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        var imported: GadgetIdentity?
        var importFailed = false
        do { imported = try PairingExport.importIfPresent(from: documents, into: store) } catch { importFailed = true }
        let token = store.load(String.self, Self.sdkTokenAccount) ?? ""
        sdkToken = token
        let commands = PhoneCommands()
        self.commands = commands
        service = GadgetService(
            store: store,
            device: { identity in
                DeviceDescription(nodeID: identity.nodeID, displayName: UIDevice.current.name,
                                  version: GadgetModel.version, commands: PhoneCommands.specs)
            },
            sdkToken: token.isEmpty ? nil : token,
            runCommand: { command, params in await MainActor.run { commands.run(command, params) } })
        paired = service.credentials != nil
        service.log = { [weak self] line in Task { @MainActor in self?.log(line) } }
        service.onUnpaired = { [weak self] in Task { @MainActor in self?.paired = false } }
        commands.showText = { [weak self] text in self?.shownText = text }
        pathMonitor.start(queue: DispatchQueue(label: "musegadget.path"))
        if let imported { log("imported pairing for \(imported.nodeID) from the Mac") }
        if importFailed { log("couldn't save the pairing from the Mac; will retry on the next launch") }
        if paired { service.start() }
    }

    func saveSDKToken() {
        let token = sdkToken.trimmingCharacters(in: .whitespacesAndNewlines)
        service.store.save(token, Self.sdkTokenAccount)
        service.sdkToken = token.isEmpty ? nil : token
        log(token.isEmpty ? "SDK token cleared" : "SDK token saved")
    }

    /// Opens BLE setup for the Muse app on another phone.
    func startPairing() {
        stopPairing()
        let identity = service.identity
        let peripheral = BLEPeripheral(localName: identity.bleName)
        let pairing = PairingSession(nodeID: identity.nodeID, deviceID: identity.deviceID, mac: identity.mac,
                                     firmwareVersion: Self.version, sdkToken: service.sdkToken)
        let service = self.service
        let controller = SetupController(
            pairing: pairing, identity: identity, version: Self.version, transport: peripheral,
            isOnline: { [pathMonitor] in pathMonitor.currentPath.status == .satisfied },
            provision: { credentials, commit in try await service.verifyAndSave(credentials, commit: commit) },
            onComplete: { [weak self] in
                Task { @MainActor in
                    guard let self else { return }
                    self.log("paired")
                    self.paired = true
                    self.service.restart()  // drops a session from an earlier pairing
                    // Let the last status reach the app, then close this
                    // window unless it was stopped or replaced meanwhile.
                    try? await Task.sleep(nanoseconds: 1_500_000_000)
                    if self.peripheral === peripheral { self.stopPairing() }
                }
            })
        let log: (String) -> Void = { [weak self] line in Task { @MainActor in self?.log(line) } }
        controller.log = log
        peripheral.log = log
        peripheral.onWrite = { [weak controller] in controller?.onWrite($0) }
        peripheral.onDisconnect = { [weak controller] in controller?.onDisconnect() }
        peripheral.start()
        self.peripheral = peripheral
        self.controller = controller
        pairingOpen = true
        log("Setup open. In the Muse app on another phone: Settings > Devices > Developer mode, then Add Device > \(identity.bleName)")
    }

    func stopPairing() {
        controller?.stop()
        peripheral?.stop()
        peripheral = nil
        controller = nil
        pairingOpen = false
    }

    func unpair() {
        service.stop()
        service.store.delete(GadgetService.credentialsAccount)
        paired = false
        log("pairing removed")
    }

    func resume() {
        if paired { service.start() }
    }

    func sendToMuse(_ text: String) {
        guard let session = service.current, session.registeredAt != nil else {
            log("not connected to the Muse")
            return
        }
        Task {
            do {
                let reply = try await session.sendChat(text)
                log("sent to Muse: HTTP \(reply.status)")
            } catch {
                log("send failed: \(error)")
            }
        }
    }

    func log(_ line: String) {
        let stamp = Date().formatted(date: .omitted, time: .standard)
        logLines.append("\(stamp) \(line)")
        if logLines.count > 300 { logLines.removeFirst(logLines.count - 300) }
    }
}
