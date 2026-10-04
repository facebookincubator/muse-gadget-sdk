// Copyright (c) Meta Platforms, Inc. and affiliates.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
import Foundation
import CoreBluetooth
import AppKit

// This helper handles BLE only. The upstream Python SDK handles all encryption,
// SDK credentials and provisioning. JSON lines travel through private stdio pipes.
let serviceID = CBUUID(string: "7fdd3d1c-38ea-46cf-8b46-314ecf5f240c")
let rxID = CBUUID(string: "4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01")
let txID = CBUUID(string: "d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c")

func emit(_ event: [String: Any]) {
    guard let data = try? JSONSerialization.data(withJSONObject: event) else { return }
    FileHandle.standardOutput.write(data + Data([10]))
}

final class Bridge: NSObject, CBPeripheralManagerDelegate {
    var manager: CBPeripheralManager!
    var tx: CBMutableCharacteristic?
    var central: CBCentral?
    var pending: [Data] = []
    let name: String
    let probe: Bool
    // Combined name/UUID advertising was not discovered on this Mac. Name-only
    // advertising completed real encrypted pairing with the Muse iPhone app.
    // The standard GATT service remains published for connection.
    let nameOnly = ProcessInfo.processInfo.environment["MUSE_BLE_NAME_ONLY"] != "0"
    var resetting = false
    var lastValue = Data()
    var generation = 0

    init(name: String, probe: Bool) {
        self.name = name
        self.probe = probe
        super.init()
        manager = CBPeripheralManager(delegate: self, queue: .main)
        if probe {
            DispatchQueue.main.asyncAfter(deadline: .now() + 30) {
                emit(["event": "error", "message": "Bluetooth permission or state still pending"])
                exit(2)
            }
        }
    }

    func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        let labels: [CBManagerState: String] = [.unknown: "unknown", .resetting: "resetting",
            .unsupported: "unsupported", .unauthorized: "unauthorized", .poweredOff: "poweredOff",
            .poweredOn: "poweredOn"]
        emit(["event": "state", "state": labels[peripheral.state] ?? "unknown"])
        if peripheral.state == .poweredOn {
            if probe { exit(0) }
            installService()
        } else if [.unsupported, .unauthorized, .poweredOff].contains(peripheral.state) {
            exit(2)
        } else {
            resetting = true
            pending.removeAll()
            lastValue = Data()
            central = nil
            emit(["event": "disconnect"])
        }
    }

    func installService() {
        manager.stopAdvertising()
        manager.removeAllServices()
        let service = CBMutableService(type: serviceID, primary: true)
        let rx = CBMutableCharacteristic(type: rxID, properties: [.write, .writeWithoutResponse],
            value: nil, permissions: [.writeable])
        tx = CBMutableCharacteristic(type: txID, properties: [.read, .notify],
            value: nil, permissions: [.readable])
        service.characteristics = [rx, tx!]
        manager.add(service)
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didAdd service: CBService, error: Error?) {
        if error != nil {
            emit(["event": "error", "message": "Unable to publish Muse GATT service"])
            exit(2)
        }
        var advertisement: [String: Any] = [CBAdvertisementDataLocalNameKey: name]
        if !nameOnly { advertisement[CBAdvertisementDataServiceUUIDsKey] = [serviceID] }
        peripheral.startAdvertising(advertisement)
    }

    func peripheralManagerDidStartAdvertising(_ peripheral: CBPeripheralManager, error: Error?) {
        if error != nil {
            emit(["event": "error", "message": "Unable to advertise Muse setup"])
            exit(2)
        }
        resetting = false
        emit(["event": "ready", "name": name, "nameOnly": nameOnly])
    }

    func select(_ candidate: CBCentral) -> Bool {
        if let selected = central, selected.identifier != candidate.identifier { return false }
        central = candidate
        emit(["event": "mtu", "value": candidate.maximumUpdateValueLength + 3])
        return true
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveWrite requests: [CBATTRequest]) {
        // A batch is accepted or rejected together, as required by Core Bluetooth.
        guard let first = requests.first else { return }
        guard !resetting, requests.allSatisfy({ $0.characteristic.uuid == rxID && $0.offset == 0 &&
            $0.central.identifier == first.central.identifier &&
            (central == nil || central!.identifier == $0.central.identifier) && $0.value != nil }) else {
            peripheral.respond(to: first, withResult: .unlikelyError)
            return
        }
        for request in requests {
            guard select(request.central) else { break }
            emit(["event": "write", "data": request.value!.base64EncodedString()])
        }
        peripheral.respond(to: first, withResult: .success)
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveRead request: CBATTRequest) {
        guard request.characteristic.uuid == txID, request.offset <= lastValue.count else {
            peripheral.respond(to: request, withResult: .invalidOffset)
            return
        }
        request.value = lastValue.subdata(in: request.offset..<lastValue.count)
        peripheral.respond(to: request, withResult: .success)
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral,
                           didSubscribeTo characteristic: CBCharacteristic) {
        if characteristic.uuid == txID && select(central) { flush() }
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral,
                           didUnsubscribeFrom characteristic: CBCharacteristic) {
        if characteristic.uuid == txID && self.central?.identifier == central.identifier {
            self.central = nil
            pending.removeAll()
            lastValue = Data()
            emit(["event": "disconnect"])
        }
    }

    func peripheralManagerIsReady(toUpdateSubscribers peripheral: CBPeripheralManager) { flush() }

    func flush() {
        guard let tx = tx, let target = central else { return }
        while let packet = pending.first {
            if !manager.updateValue(packet, for: tx, onSubscribedCentrals: [target]) { return }
            pending.removeFirst()
        }
    }

    func command(_ command: [String: Any]) {
        switch command["op"] as? String {
        case "notify":
            guard let value = command["data"] as? String, let packet = Data(base64Encoded: value),
                packet.count <= 512, pending.count < 2048 else {
                emit(["event": "error", "message": "Invalid or excessive notification"])
                exit(2)
            }
            lastValue = packet
            pending.append(packet)
            flush()
        case "disconnect":
            // Core Bluetooth has no force-disconnect API for peripherals. Remove
            // the service after the requested grace period, then reopen setup.
            resetting = true
            generation += 1
            let requestedGeneration = generation
            let delay = min(5, max(0, command["delay"] as? Double ?? 0.3))
            // Let the final status notification reach the phone before removing
            // GATT. This matches the Linux transport's disconnect(delay) grace.
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                guard self.generation == requestedGeneration else { return }
                self.manager.stopAdvertising()
                self.manager.removeAllServices()
                self.pending.removeAll()
                self.lastValue = Data()
                self.central = nil
                emit(["event": "disconnect"])
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                    guard self.generation == requestedGeneration else { return }
                    self.installService()
                }
            }
        case "stop":
            manager.stopAdvertising()
            manager.removeAllServices()
            exit(0)
        default:
            emit(["event": "error", "message": "Unknown bridge operation"])
            exit(2)
        }
    }
}

final class PairingWindowDelegate: NSObject, NSWindowDelegate {
    var onClose: (() -> Void)?
    func windowWillClose(_ notification: Notification) { onClose?() }
}

let probe = CommandLine.arguments.contains("--probe")
let name = probe ? "MuseGadgetProbe" : (CommandLine.arguments.dropFirst().first ?? "MuseGadget")
// Use a real foreground macOS application: a CLI-only process can have its
// name suppressed by Apple's background advertising policy.
var pairingWindow: NSWindow?
let windowDelegate = PairingWindowDelegate()
if !probe {
    NSApplication.shared.setActivationPolicy(.regular)
    pairingWindow = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 480, height: 150),
        styleMask: [.titled, .closable], backing: .buffered, defer: false)
    pairingWindow!.title = "Muse Bluetooth Pairing"
    pairingWindow!.isReleasedWhenClosed = false
    pairingWindow!.delegate = windowDelegate
    let label = NSTextField(wrappingLabelWithString:
        "In the Muse phone app, add:\n\n\(name)\n\nKeep this window open while pairing.")
    label.frame = NSRect(x: 24, y: 18, width: 430, height: 112)
    pairingWindow!.contentView!.addSubview(label)
    pairingWindow!.center()
    pairingWindow!.makeKeyAndOrderFront(nil)
    NSApplication.shared.activate(ignoringOtherApps: true)
}
let bridge = Bridge(name: name, probe: probe)
windowDelegate.onClose = { bridge.command(["op": "stop"]) }
if !probe {
    DispatchQueue.global().async {
        while let line = readLine() {
            guard line.utf8.count < 8192, let data = line.data(using: .utf8),
                let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                emit(["event": "error", "message": "Malformed bridge command"])
                exit(2)
            }
            DispatchQueue.main.async { bridge.command(object) }
        }
        DispatchQueue.main.async { bridge.command(["op": "stop"]) }
    }
}
if probe { dispatchMain() } else { NSApplication.shared.run() }
