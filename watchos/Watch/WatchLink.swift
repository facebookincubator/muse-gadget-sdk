import Foundation
import CoreBluetooth
import SwiftUI

struct NearbyRelay: Identifiable {
    let peripheral: CBPeripheral
    let name: String
    var id: UUID { peripheral.identifier }
}

final class WatchLink: NSObject, ObservableObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    @Published var status = "Connect to your Mac"
    @Published var relays: [NearbyRelay] = []
    @Published var connected = false
    @Published var online = false
    @Published var approved = false
    @Published var scanning = false
    var onEvent: (([String: Any]) -> Void)?
    var onDisconnect: ((String) -> Void)?
    private var manager: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var rx: CBCharacteristic?
    private var decoder = FrameDecoder()
    private var outgoing: [Data] = []
    private var writing = false
    private var timer: Timer?

    override init() {
        super.init()
        manager = CBCentralManager(delegate: self, queue: .main)
    }
    func scan() {
        guard manager.state == .poweredOn else { status = "Turn on Bluetooth and allow Muse access"; return }
        disconnect(); relays = []; scanning = true; status = "Looking for Mac…"
        manager.scanForPeripherals(withServices: [CBUUID(string: MuseWire.service)])
        timer = Timer.scheduledTimer(withTimeInterval: 15, repeats: false) { [weak self] _ in
            guard let self else { return }
            manager.stopScan(); scanning = false
            if relays.isEmpty { status = "Open Muse Watch Relay on your nearby Mac, then retry." }
        }
    }
    func connect(_ relay: NearbyRelay) {
        manager.stopScan(); scanning = false; timer?.invalidate()
        peripheral = relay.peripheral; peripheral?.delegate = self; status = "Connecting…"
        manager.connect(relay.peripheral)
        timer = Timer.scheduledTimer(withTimeInterval: 30, repeats: false) { [weak self] _ in
            guard let self, !connected else { return }
            disconnect(); status = "Connection timed out. Check the Bluetooth prompt on your Mac."
        }
    }
    func disconnect() {
        timer?.invalidate(); manager.stopScan(); scanning = false
        if let peripheral { manager.cancelPeripheralConnection(peripheral) }
        peripheral = nil; reset()
    }
    private func reset() {
        connected = false; approved = false; online = false; rx = nil
        decoder.reset(); outgoing = []; writing = false
    }
    func send(_ command: [String: Any]) throws {
        guard connected, approved, rx != nil else { throw failure("Connect and approve this watch on your Mac first.") }
        let validated = try MuseWire.command(command)
        let data = try MuseWire.encode(validated)
        guard outgoing.isEmpty, !writing else { throw failure("Wait for the current message to finish sending.") }
        let size = max(1, min(512, peripheral?.maximumWriteValueLength(for: .withResponse) ?? 20))
        for start in stride(from: 0, to: data.count, by: size) {
            outgoing.append(data.subdata(in: start..<min(start + size, data.count)))
        }
        writeNext()
    }
    private func writeNext() {
        guard !writing, let packet = outgoing.first, let peripheral, let rx else { return }
        writing = true; peripheral.writeValue(packet, for: rx, type: .withResponse)
    }
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn { status = "Tap Connect to find your Mac" }
        else {
            reset(); status = central.state == .unauthorized ? "Allow Bluetooth in Watch Settings" : "Bluetooth unavailable"
            onDisconnect?(status)
        }
    }
    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard !relays.contains(where: { $0.id == peripheral.identifier }) else { return }
        relays.append(NearbyRelay(peripheral: peripheral, name: advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name ?? "Muse Watch Relay"))
    }
    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        guard self.peripheral?.identifier == peripheral.identifier else { return }
        status = "Opening chat service…"; peripheral.discoverServices([CBUUID(string: MuseWire.service)])
    }
    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) { lost(peripheral, error) }
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) { lost(peripheral, error) }
    private func lost(_ peer: CBPeripheral, _ error: Error?) {
        guard peripheral?.identifier == peer.identifier else { return }
        timer?.invalidate(); peripheral = nil; reset(); status = error?.localizedDescription ?? "Disconnected from Mac"
        onDisconnect?(status)
    }
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard self.peripheral === peripheral else { return }
        guard error == nil, let service = peripheral.services?.first(where: { $0.uuid == CBUUID(string: MuseWire.service) }) else {
            fail(error?.localizedDescription ?? "Muse relay service missing"); return
        }
        peripheral.discoverCharacteristics([CBUUID(string: MuseWire.rx), CBUUID(string: MuseWire.tx)], for: service)
    }
    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard self.peripheral === peripheral else { return }
        guard error == nil else { fail(error!.localizedDescription); return }
        rx = service.characteristics?.first(where: { $0.uuid == CBUUID(string: MuseWire.rx) })
        guard rx != nil, let tx = service.characteristics?.first(where: { $0.uuid == CBUUID(string: MuseWire.tx) }) else { fail("Muse relay channels missing"); return }
        status = "Accept Bluetooth pairing on your Mac"
        peripheral.setNotifyValue(true, for: tx)
    }
    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard self.peripheral === peripheral else { return }
        guard error == nil, characteristic.isNotifying else { fail(error?.localizedDescription ?? "Could not subscribe"); return }
        timer?.invalidate(); connected = true
        if !approved { status = "Click Allow my watch on your Mac" }
    }
    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard self.peripheral === peripheral else { return }
        guard error == nil, let data = characteristic.value else { fail(error?.localizedDescription ?? "Missing Bluetooth data"); return }
        do {
            for event in try decoder.feed(data) {
                switch event["type"] as? String {
                case "approved": approved = true; status = "Waiting for Muse…"
                case "approval_required": approved = false; online = false; status = "Click Allow my watch on your Mac"
                case "connection":
                    online = event["online"] as? Bool ?? false
                    status = online ? "Ready to talk" : event["paired"] as? Bool == true ? "Mac is connecting to Muse…" : "Pair the Mac with Muse first"
                default: break
                }
                onEvent?(event)
            }
        } catch { fail("Invalid relay response. Reconnect to your Mac.") }
    }
    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard self.peripheral === peripheral else { return }
        writing = false
        guard error == nil else { fail(error!.localizedDescription); return }
        if !outgoing.isEmpty { outgoing.removeFirst() }
        writeNext()
    }
    private func fail(_ message: String) {
        disconnect(); status = message; onDisconnect?(message)
    }
    private func failure(_ message: String) -> NSError { NSError(domain: "Muse", code: 1, userInfo: [NSLocalizedDescriptionKey: message]) }
}
