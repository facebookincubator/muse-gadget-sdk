#if canImport(CoreBluetooth)
import CoreBluetooth
import Foundation

/// The gadget's setup GATT service on `CBPeripheralManager`
/// (`linux/src/musegadget/ble_server.py`).
///
/// iOS differences from BlueZ / NimBLE:
/// - Only the local name and service UUIDs can be advertised. The
///   `0xFFFF` manufacturer-data "paired flag" the other SDKs send is dropped.
/// - The name and UUID are advertised only while the app is in the foreground.
/// - The GAP Device Name is the iPhone's own name, not `MuseGadgetXXXXXX`.
/// - A peripheral can't drop a central; `disconnect(after:)` resets state
///   and re-advertises instead. There is no connect/disconnect callback, so
///   unsubscribing from TX stands in for a disconnect.
public final class BLEPeripheral: NSObject, SetupTransport, CBPeripheralManagerDelegate {
    public static let serviceUUID = CBUUID(string: "7fdd3d1c-38ea-46cf-8b46-314ecf5f240c")
    public static let rxUUID = CBUUID(string: "4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01")
    public static let txUUID = CBUUID(string: "d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c")

    public var onWrite: (Data) -> Void = { _ in }
    public var onDisconnect: () -> Void = {}
    public var log: (String) -> Void = { _ in }

    private let localName: String
    private var manager: CBPeripheralManager!
    private let queue = DispatchQueue(label: "musegadget.ble")
    private var tx: CBMutableCharacteristic!
    private var central: CBCentral?
    private var outbox: [Data] = []
    private var pumping = false
    private var started = false

    public init(localName: String) {
        self.localName = localName
        super.init()
        manager = CBPeripheralManager(delegate: self, queue: queue)
    }

    // MARK: - SetupTransport

    /// ATT MTU; CoreBluetooth reports the usable payload (MTU - 3).
    public var mtu: Int {
        queue.sync { central.map { $0.maximumUpdateValueLength + 3 } ?? BLEFraming.maxPacketBytes + 3 }
    }

    public func send(packets: [Data]) {
        queue.async { [self] in
            outbox += packets
            pump()
        }
    }

    public func disconnect(after delay: TimeInterval) {
        queue.asyncAfter(deadline: .now() + delay) { [self] in
            outbox.removeAll()
            central = nil
            onDisconnect()
        }
    }

    // MARK: - Lifecycle

    public func start() {
        queue.async { [self] in
            started = true
            if manager.state == .poweredOn { publish() }
        }
    }

    public func stop() {
        queue.async { [self] in
            started = false
            manager.stopAdvertising()
            manager.removeAllServices()
        }
    }

    private func publish() {
        let rx = CBMutableCharacteristic(
            type: Self.rxUUID, properties: [.write, .writeWithoutResponse], value: nil,
            permissions: [.writeable])
        tx = CBMutableCharacteristic(
            type: Self.txUUID, properties: [.read, .notify], value: nil, permissions: [.readable])
        let service = CBMutableService(type: Self.serviceUUID, primary: true)
        service.characteristics = [rx, tx]
        manager.removeAllServices()
        manager.add(service)
    }

    // MARK: - CBPeripheralManagerDelegate (on `queue`)

    public func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        log("Bluetooth state \(peripheral.state.rawValue)")
        if peripheral.state == .poweredOn, started { publish() }
    }

    public func peripheralManager(_ peripheral: CBPeripheralManager, didAdd service: CBService, error: Error?) {
        if let error {
            log("adding service failed: \(error)")
            return
        }
        peripheral.startAdvertising([
            CBAdvertisementDataLocalNameKey: localName,
            CBAdvertisementDataServiceUUIDsKey: [Self.serviceUUID],
        ])
    }

    public func peripheralManagerDidStartAdvertising(_ peripheral: CBPeripheralManager, error: Error?) {
        log(error.map { "advertising failed: \($0)" } ?? "advertising as \(localName)")
    }

    public func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral,
                                  didSubscribeTo characteristic: CBCharacteristic) {
        self.central = central
        log("central subscribed, max update \(central.maximumUpdateValueLength)")
    }

    public func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral,
                                  didUnsubscribeFrom characteristic: CBCharacteristic) {
        guard central.identifier == self.central?.identifier else { return }
        self.central = nil
        outbox.removeAll()
        log("central unsubscribed")
        onDisconnect()
    }

    public func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveWrite requests: [CBATTRequest]) {
        for request in requests {
            if central == nil { central = request.central }
            if let value = request.value { onWrite(value) }
        }
        // One response covers the whole batch of write-with-response requests.
        if let first = requests.first { peripheral.respond(to: first, withResult: .success) }
    }

    public func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveRead request: CBATTRequest) {
        request.value = tx.value ?? Data()
        peripheral.respond(to: request, withResult: .success)
    }

    public func peripheralManagerIsReady(toUpdateSubscribers peripheral: CBPeripheralManager) {
        pump()
    }

    /// Sends queued notifications, `chunkStagger` apart, retrying when the
    /// transmit queue is full.
    private func pump() {
        guard !pumping, let packet = outbox.first else { return }
        guard manager.updateValue(packet, for: tx, onSubscribedCentrals: nil) else { return }
        outbox.removeFirst()
        pumping = true
        queue.asyncAfter(deadline: .now() + BLEFraming.chunkStagger) { [self] in
            pumping = false
            pump()
        }
    }
}
#endif
