import SwiftUI
import CoreBluetooth

final class RelayModel: NSObject, ObservableObject, CBPeripheralManagerDelegate {
    @Published var sdkStatus = "Starting Muse SDK…"
    @Published var bluetoothStatus = "Starting Bluetooth…"
    @Published var paired = false
    @Published var online = false
    @Published var pairing = false
    @Published var deviceName = "MuseGadget"
    @Published var sdkToken = ""
    @Published var tokenSaved = false
    @Published var needsApproval = false
    @Published var watchConnected = false
    @Published var error: String?
    private let sdk = SDKProcess()
    private var manager: CBPeripheralManager!
    private var tx: CBMutableCharacteristic?
    private var central: CBCentral?
    private var approved = false
    private var decoder = FrameDecoder()
    private var packets: [Data] = []
    private var queuedBytes = 0
    private var lastConnection: [String: Any] = [:]
    private var activeTurn = false
    private var turnWatchID: UUID?

    override init() {
        super.init()
        sdk.onEvent = { [weak self] in self?.receive($0) }
        sdk.onExit = { [weak self] in
            guard let self else { return }
            online = false; pairing = false; sdkStatus = "Muse SDK stopped. Restart this app."
            notify(["type": "error", "error": sdkStatus])
            notify(["type": "turn_finished"])
            notify(["type": "connection", "online": false, "paired": paired])
        }
        do { try sdk.start() } catch { self.error = error.localizedDescription }
        manager = CBPeripheralManager(delegate: self, queue: .main)
    }

    func pair() {
        error = nil
        do {
            var request: [String: Any] = ["op": "pair"]
            if !sdkToken.isEmpty { request["sdk_token"] = sdkToken }
            try sdk.send(request); sdkToken = ""
        } catch { self.error = error.localizedDescription }
    }
    func cancelPair() { try? sdk.send(["op": "cancel_pair"]) }
    func stop() { manager.stopAdvertising(); manager.removeAllServices(); sdk.stop() }

    func approveWatch() {
        guard let central else { return }
        approved = true; needsApproval = false; watchConnected = true
        UserDefaults.standard.set(central.identifier.uuidString, forKey: "approvedWatch")
        notify(["type": "approved"])
        notify(lastConnection)
        bluetoothStatus = "Watch connected"
    }

    func forgetWatch() {
        UserDefaults.standard.removeObject(forKey: "approvedWatch")
        approved = false; watchConnected = false; needsApproval = central != nil
        if activeTurn { try? sdk.send(["op": "cancel"]) }
        packets.removeAll(); queuedBytes = 0; decoder.reset()
        notify(["type": "approval_required"], allowUnapproved: true)
        bluetoothStatus = "Watch access revoked"
    }

    private func receive(_ event: [String: Any]) {
        switch event["type"] as? String {
        case "connection":
            paired = event["paired"] as? Bool ?? false
            online = event["online"] as? Bool ?? false
            pairing = event["pairing"] as? Bool ?? false
            deviceName = event["ble_name"] as? String ?? deviceName
            tokenSaved = event["sdk_token_saved"] as? Bool ?? false
            sdkStatus = online ? "Muse connected" : pairing ? "Pairing with Muse…" : paired ? "Connecting to Muse…" : "Pair with the Muse phone app"
            lastConnection = ["type": "connection", "online": online, "paired": paired]
            notify(lastConnection)
        case "pairing_status": sdkStatus = event["text"] as? String ?? "Pairing…"
        case "pairing_error": error = event["error"] as? String; pairing = false
        case "paired": paired = true; pairing = false
        case "turn_finished":
            if central?.identifier == turnWatchID { notify(event) }
            activeTurn = false; turnWatchID = nil
        case "ack", "status", "reply", "done", "error", "command_error":
            if activeTurn, central?.identifier == turnWatchID { notify(event) }
        default: break
        }
    }

    func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        if peripheral.state != .poweredOn {
            clearConnection()
            bluetoothStatus = peripheral.state == .unauthorized ? "Allow Bluetooth in System Settings → Privacy & Security" : "Turn on Bluetooth"
            return
        }
        peripheral.removeAllServices()
        let service = CBMutableService(type: CBUUID(string: MuseWire.service), primary: true)
        let rx = CBMutableCharacteristic(type: CBUUID(string: MuseWire.rx), properties: [.write], value: nil,
                                         permissions: [.writeEncryptionRequired])
        let tx = CBMutableCharacteristic(type: CBUUID(string: MuseWire.tx), properties: [.notify, .notifyEncryptionRequired], value: nil, permissions: [])
        self.tx = tx; service.characteristics = [rx, tx]
        peripheral.add(service)
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didAdd service: CBService, error: Error?) {
        if let error { self.error = error.localizedDescription; return }
        peripheral.startAdvertising([CBAdvertisementDataLocalNameKey: "Muse Watch Relay",
                                     CBAdvertisementDataServiceUUIDsKey: [CBUUID(string: MuseWire.service)]])
    }
    func peripheralManagerDidStartAdvertising(_ peripheral: CBPeripheralManager, error: Error?) {
        bluetoothStatus = error == nil ? "Available to your watch" : "Bluetooth advertising failed"
        if let error { self.error = error.localizedDescription }
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral, didSubscribeTo characteristic: CBCharacteristic) {
        guard characteristic.uuid == tx?.uuid, self.central == nil || self.central?.identifier == central.identifier else { return }
        self.central = central; decoder.reset(); packets.removeAll(); queuedBytes = 0
        approved = UserDefaults.standard.string(forKey: "approvedWatch") == central.identifier.uuidString
        needsApproval = !approved; watchConnected = approved
        if approved {
            notify(["type": "approved"]); notify(lastConnection)
            bluetoothStatus = "Watch connected"
        } else {
            bluetoothStatus = "A watch is requesting access"
            notify(["type": "approval_required"], allowUnapproved: true)
        }
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral, didUnsubscribeFrom characteristic: CBCharacteristic) {
        if self.central?.identifier == central.identifier { clearConnection() }
    }

    private func clearConnection() {
        central = nil; approved = false; needsApproval = false; watchConnected = false
        decoder.reset(); packets.removeAll(); queuedBytes = 0
        if activeTurn { try? sdk.send(["op": "cancel"]) }
        bluetoothStatus = "Waiting for watch"
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveWrite requests: [CBATTRequest]) {
        guard let first = requests.first else { return }
        guard approved, requests.allSatisfy({ $0.central.identifier == central?.identifier && $0.offset == 0 && $0.characteristic.uuid == CBUUID(string: MuseWire.rx) && $0.value != nil }) else {
            peripheral.respond(to: first, withResult: .insufficientAuthorization); return
        }
        do {
            for request in requests {
                for value in try decoder.feed(request.value!) {
                    let command = try MuseWire.command(value)
                    if command["op"] as? String == "check" { notify(lastConnection); continue }
                    if command["op"] as? String == "chat" {
                        guard online, !activeTurn else {
                            notify(["type": "command_error", "error": online ? "Wait for the current reply." : "Connect the Mac relay to Muse first."])
                            continue
                        }
                        try sdk.send(command); activeTurn = true; turnWatchID = central?.identifier
                    } else { try sdk.send(command) }
                }
            }
            peripheral.respond(to: first, withResult: .success)
        } catch {
            decoder.reset()
            peripheral.respond(to: first, withResult: .unlikelyError)
            notify(["type": "command_error", "error": "The watch request could not be processed."])
        }
    }

    private func notify(_ event: [String: Any], allowUnapproved: Bool = false) {
        guard let central, approved || allowUnapproved, !event.isEmpty else { return }
        do {
            let data = try MuseWire.encode(event)
            guard queuedBytes + data.count < 1024 * 1024 else {
                self.error = "Watch Bluetooth link is too slow. Reconnect the watch."
                manager.stopAdvertising(); manager.removeAllServices(); clearConnection()
                peripheralManagerDidUpdateState(manager)
                return
            }
            let chunk = max(1, min(512, central.maximumUpdateValueLength))
            for start in stride(from: 0, to: data.count, by: chunk) {
                packets.append(data.subdata(in: start..<min(start + chunk, data.count)))
            }
            queuedBytes += data.count; flush()
        } catch {
            self.error = "Muse returned a response larger than the watch transport supports."
            if activeTurn { try? sdk.send(["op": "cancel"]) }
        }
    }
    func peripheralManagerIsReady(toUpdateSubscribers peripheral: CBPeripheralManager) { flush() }
    private func flush() {
        guard let tx, let central else { return }
        while let packet = packets.first {
            guard manager.updateValue(packet, for: tx, onSubscribedCentrals: [central]) else { return }
            queuedBytes -= packet.count; packets.removeFirst()
        }
    }
}
