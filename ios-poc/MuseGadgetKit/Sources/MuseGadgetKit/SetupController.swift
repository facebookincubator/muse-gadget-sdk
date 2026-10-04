import Foundation

/// Stable gadget identity (`linux/src/musegadget/identity.py`): a random,
/// locally administered MAC-shaped value; node id and BLE name share its
/// last six hex digits, with no hyphen in the BLE name.
public struct GadgetIdentity: Codable, Equatable {
    public let mac: String

    public init(mac: String) { self.mac = mac }

    public static func generate() -> GadgetIdentity {
        var octets = [UInt8](PairingSession.secureRandom(6))
        octets[0] = (octets[0] & 0xFC) | 0x02  // unicast, locally administered
        return GadgetIdentity(mac: octets.map { String(format: "%02x", $0) }.joined(separator: ":"))
    }

    public var suffix: String { String(mac.replacingOccurrences(of: ":", with: "").suffix(6)) }
    public var nodeID: String { "homelink-" + suffix }
    public var deviceID: String { "hatch-link:" + mac }
    public var bleName: String { "MuseGadget" + suffix.uppercased() }
}

/// What the BLE layer must provide (`ble_setup.Transport`).
public protocol SetupTransport: AnyObject {
    var mtu: Int { get }
    /// Notifies each packet in order.
    func send(packets: [Data])
    /// Ends the connection after `delay`; best effort on iOS (see BLEPeripheral).
    func disconnect(after delay: TimeInterval)
}

/// Credentials delivered by `provision_v2`. The Wi-Fi fields are dropped:
/// the gadget only sets up while it is already online.
public struct DeviceCredentials: Codable, Equatable {
    public var accessToken: String
    public var refreshToken: String
    public var username: String
    public var apiURLv2: String
    public var noiseHost: String
    public var accessTokenSavedAt: Date

    public init(accessToken: String, refreshToken: String, username: String, apiURLv2: String,
                noiseHost: String, accessTokenSavedAt: Date = Date()) {
        self.accessToken = accessToken
        self.refreshToken = refreshToken
        self.username = username
        self.apiURLv2 = apiURLv2
        self.noiseHost = noiseHost
        self.accessTokenSavedAt = accessTokenSavedAt
    }
}

public struct ProvisionFailed: Error {
    public let status: String
    public init(_ status: String) { self.status = status }
}

/// The setup protocol behind the GATT characteristics
/// (`linux/src/musegadget/ble_setup.py`), independent of CoreBluetooth.
/// Messages are handled one at a time on a serial queue, so encrypted record
/// counters reach the phone in order.
public final class SetupController {
    static let sensitiveActions: Set<String> = [
        "provision", "provision_v2", "wifi_scan", "ota", "device.ota",
        "unpair", "set_wifi", "set_auth",
    ]
    static let plaintextStatuses: Set<String> = [
        "error_encryption_required", "error_pairing_invalid_hello",
        "error_pairing_unavailable", "error_pairing_decrypt",
    ]

    public typealias Provision = (DeviceCredentials, _ commit: (() -> Bool) -> Bool) async throws -> Void

    private let pairing: PairingSession
    private let identity: GadgetIdentity
    private let version: String
    private weak var transport: SetupTransport?
    private let isOnline: () -> Bool
    private let networkLabel: () -> String
    private let provision: Provision
    private let onComplete: () -> Void
    private let queue = DispatchQueue(label: "musegadget.setup")
    private let stateLock = NSLock()
    private var assembler = ChunkAssembler()
    private var plaintextBlocked = false
    private var provisioning = false
    public var log: (String) -> Void = { _ in }

    public init(pairing: PairingSession, identity: GadgetIdentity, version: String,
                transport: SetupTransport, isOnline: @escaping () -> Bool,
                networkLabel: @escaping () -> String = { "Use current connection" },
                provision: @escaping Provision, onComplete: @escaping () -> Void = {}) {
        self.pairing = pairing
        self.identity = identity
        self.version = version
        self.transport = transport
        self.isOnline = isOnline
        self.networkLabel = networkLabel
        self.provision = provision
        self.onComplete = onComplete
    }

    // MARK: - Transport callbacks

    public func onWrite(_ packet: Data) {
        queue.async { [self] in
            if let message = assembler.feed(packet) { handleMessage(message) }
        }
    }

    public func onDisconnect() {
        queue.async { [self] in
            log("BLE client gone; clearing pairing session")
            assembler.reset()
            locked { plaintextBlocked = false }
            pairing.reset()
        }
    }

    /// Waits for queued messages; for tests.
    public func drain() { queue.sync {} }

    // MARK: - Dispatch (on `queue`)

    func handleMessage(_ raw: Data, decrypted: Bool = false) {
        guard let command = CompactJSON.object(raw) else {
            sendStatus("error_invalid_command")
            return
        }
        let action = command["action"] as? String ?? ""
        log("RX \(action.isEmpty ? "?" : action)\(decrypted ? " (encrypted)" : "")")
        let blocked = locked { plaintextBlocked }

        switch action {
        case "pairing_client_hello" where !decrypted: handleHello(command)
        case "pairing_encrypted" where !decrypted: handleRecord(command)
        case "get_device_info": sendJSON(deviceInfo())
        case _ where !decrypted && blocked: log("plaintext ignored after pairing started: \(action)")
        case _ where !decrypted && Self.sensitiveActions.contains(action):
            sendStatus("error_encryption_required")
        case "pairing_client_finished" where decrypted: handleClientFinished(command)
        case _ where decrypted && Self.sensitiveActions.contains(action) && !pairing.confirmed:
            sendStatus("error_pairing_confirm_required")
        case "wifi_scan" where decrypted:
            let networks: [[String: Any]] = isOnline()
                ? [["ssid": networkLabel(), "rssi": -40, "secure": false]] : []
            sendEncryptedJSON(["type": "wifi_scan_result", "networks": networks])
        case "provision_v2" where decrypted: handleProvision(command)
        default: sendStatus("error_unknown_action")
        }
    }

    public func deviceInfo() -> [String: Any] {
        var info: [String: Any] = [
            "type": "device_info",
            "node_id": identity.nodeID,
            "version": version,
            "build_sha": "",
            "network_ready": isOnline(),
        ]
        info.merge(pairing.deviceInfo()) { _, new in new }
        return info
    }

    private func handleHello(_ command: [String: Any]) {
        do {
            let ready = try pairing.handleHello(command)
            locked { plaintextBlocked = true }
            sendJSON(ready)
        } catch let error as PairingError {
            sendStatus(error.status)
        } catch {
            sendStatus(PairingConstants.errorInvalidHello)
        }
    }

    private func handleRecord(_ envelope: [String: Any]) {
        do {
            let plaintext = try pairing.decrypt(envelope)
            handleMessage(Data(plaintext.utf8), decrypted: true)
        } catch {
            sendStatus((error as? PairingError)?.status ?? PairingConstants.errorDecrypt)
            transport?.disconnect(after: 0.3)
        }
    }

    private func handleClientFinished(_ command: [String: Any]) {
        let generation = pairing.handleClientFinished(command)
        guard generation != 0 else {
            sendStatus(PairingConstants.errorDecrypt)
            transport?.disconnect(after: 0.3)
            return
        }
        log("pairing confirmed (app consent)")
        sendStatus("pairing_confirmed", generation: generation)
    }

    private func handleProvision(_ command: [String: Any]) {
        func text(_ key: String) -> String { command[key] as? String ?? "" }
        guard command["ssid"] is String, command["password"] is String,
              !text("access_token").isEmpty, !text("refresh_token").isEmpty,
              text("token_type") == "device"
        else {
            sendStatus("error_missing_credentials")
            return
        }
        let generation: Int? = locked {
            if provisioning { return nil }
            let gen = pairing.markProvisioning()
            if gen != 0 { provisioning = true }
            return gen
        }
        guard let generation else {
            sendStatus("error_operation_in_progress")
            return
        }
        guard generation != 0 else {
            sendStatus("error_pairing_confirm_required")
            return
        }
        let apiV2 = text("api_url_v2")
        let credentials = DeviceCredentials(
            accessToken: text("access_token"), refreshToken: text("refresh_token"),
            username: text("username"), apiURLv2: apiV2.hasPrefix("https://") ? apiV2 : "",
            noiseHost: text("noise_host"))
        Task { await runProvision(credentials, generation) }
    }

    private func runProvision(_ credentials: DeviceCredentials, _ generation: Int) async {
        defer { locked { provisioning = false } }
        sendStatus("wifi_connecting", generation: generation)
        guard isOnline() else {
            pairing.extendProvisioning(generation)
            sendStatus("wifi_failed", generation: generation)
            return
        }
        sendStatus("wifi_connected", generation: generation)
        do {
            try await provision(credentials) { save in
                pairing.commitProvisioning(generation, save)
            }
        } catch {
            let status = (error as? ProvisionFailed)?.status ?? "auth_failed"
            log("provisioning failed: \(status)")
            sendStatus(status, generation: generation)
            transport?.disconnect(after: 0.5)
            return
        }
        sendStatus("auth_ok", generation: generation)
        log("setup complete")
        onComplete()
    }

    // MARK: - Sending (serialised by `txLock`)

    private let txLock = NSLock()

    public func sendStatus(_ status: String, generation: Int = 0) {
        txLock.lock(); defer { txLock.unlock() }
        if let envelope = pairing.encryptStatus(status, generation: generation) {
            sendLocked(envelope)
            log("TX status (encrypted #\(envelope["counter"] ?? "")): \(status)")
            return
        }
        let blocked = locked { plaintextBlocked }
        if generation != 0 || blocked || !Self.plaintextStatuses.contains(status) {
            log("TX status suppressed: \(status)")
            return
        }
        transport?.send(packets: [Data(status.utf8)])
        log("TX status: \(status)")
    }

    func sendJSON(_ object: [String: Any]) {
        txLock.lock(); defer { txLock.unlock() }
        sendLocked(object)
        log("TX \(object["type"] ?? "")")
    }

    func sendEncryptedJSON(_ object: [String: Any], generation: Int = 0) {
        txLock.lock(); defer { txLock.unlock() }
        guard let text = try? CompactJSON.string(object),
              let envelope = pairing.encryptJSON(text, generation: generation)
        else {
            log("TX \(object["type"] ?? "") suppressed: no session")
            return
        }
        sendLocked(envelope)
    }

    private func sendLocked(_ object: [String: Any]) {
        guard let transport, let data = try? CompactJSON.data(object),
              let packets = try? BLEFraming.encodeChunks(data, mtu: transport.mtu)
        else { return }
        transport.send(packets: packets)
    }

    private func locked<T>(_ body: () -> T) -> T {
        stateLock.lock(); defer { stateLock.unlock() }
        return body()
    }
}
