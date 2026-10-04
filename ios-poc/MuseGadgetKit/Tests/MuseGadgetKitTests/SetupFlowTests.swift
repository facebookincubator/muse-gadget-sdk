import CryptoKit
import XCTest
@testable import MuseGadgetKit

/// Records notifications the gadget sends and reassembles them like the app.
final class FakeTransport: SetupTransport {
    let mtu = 185
    private var assembler = ChunkAssembler()
    private let lock = NSLock()
    private(set) var received: [Data] = []
    private(set) var disconnects = 0

    func send(packets: [Data]) {
        lock.lock(); defer { lock.unlock() }
        for packet in packets {
            XCTAssertLessThanOrEqual(packet.count, mtu - 3)
            if let message = assembler.feed(packet) { received.append(message) }
        }
    }

    func disconnect(after delay: TimeInterval) { lock.lock(); disconnects += 1; lock.unlock() }

    func take() -> [Data] {
        lock.lock(); defer { lock.unlock() }
        defer { received = [] }
        return received
    }
}

/// The phone side of the record layer, written from the contract, not the device code.
struct Mobile {
    let tx: SymmetricKey
    let rx: SymmetricKey
    let sessionID: String
    var txCounter: UInt64 = 0
    var rxCounter: UInt64 = 0

    mutating func seal(_ command: [String: Any]) -> Data {
        let aad = Data("hatch-link ble setup v1|\(sessionID)|m2d|\(txCounter)".utf8)
        var nonce = Data([0, 0, 0, 0]); withUnsafeBytes(of: txCounter.bigEndian) { nonce.append(contentsOf: $0) }
        let box = try! AES.GCM.seal(try! CompactJSON.data(command), using: tx, nonce: .init(data: nonce), authenticating: aad)
        defer { txCounter += 1 }
        return try! CompactJSON.data(["action": "pairing_encrypted", "session_id": sessionID,
                                      "counter": String(txCounter), "ciphertext": Base64URL.encode(box.ciphertext),
                                      "tag": Base64URL.encode(box.tag)])
    }

    mutating func open(_ data: Data) -> [String: Any] {
        let envelope = CompactJSON.object(data)!
        XCTAssertEqual(envelope["type"] as? String, "pairing_encrypted")
        XCTAssertEqual(envelope["counter"] as? String, String(rxCounter))
        let aad = Data("hatch-link ble setup v1|\(sessionID)|d2m|\(rxCounter)".utf8)
        var nonce = Data([1, 0, 0, 0]); withUnsafeBytes(of: rxCounter.bigEndian) { nonce.append(contentsOf: $0) }
        rxCounter += 1
        let box = try! AES.GCM.SealedBox(nonce: .init(data: nonce), ciphertext: Base64URL.decode(envelope["ciphertext"])!,
                                         tag: Base64URL.decode(envelope["tag"])!)
        return CompactJSON.object(try! AES.GCM.open(box, using: rx, authenticating: aad))!
    }
}

final class SetupFlowTests: XCTestCase {
    /// What the Muse app does: device info, hello, client_finished, Wi-Fi scan, provision_v2.
    func testFullAppSetupFlow() throws {
        let identity = GadgetIdentity(mac: "02:12:34:ab:cd:ef")
        XCTAssertEqual(identity.bleName, "MuseGadgetABCDEF")
        XCTAssertEqual(identity.nodeID, "homelink-abcdef")

        let pairing = PairingSession(nodeID: identity.nodeID, deviceID: identity.deviceID, mac: identity.mac,
                                     firmwareVersion: "0.1.0-ios", sdkToken: "mgst_test")
        let transport = FakeTransport()
        var saved: DeviceCredentials?
        let completed = expectation(description: "setup complete")
        let controller = SetupController(
            pairing: pairing, identity: identity, version: "0.1.0-ios", transport: transport,
            isOnline: { true }, networkLabel: { "HomeWiFi" },
            provision: { credentials, commit in
                XCTAssertTrue(commit { saved = credentials; return true })
            },
            onComplete: { completed.fulfill() })

        func write(_ object: [String: Any]) { write(try! CompactJSON.data(object)) }
        func write(_ data: Data) {
            for packet in try! BLEFraming.encodeChunks(data, mtu: 185) { controller.onWrite(packet) }
            controller.drain()
        }

        write(["action": "get_device_info"])
        let info = CompactJSON.object(transport.take()[0])!
        XCTAssertEqual(info["model"] as? String, "hatch_link")
        XCTAssertEqual(info["pairing_policy"] as? String, "confirm_app")
        XCTAssertEqual(info["node_id"] as? String, "homelink-abcdef")

        // Sensitive actions are refused in plaintext.
        write(["action": "wifi_scan"])
        XCTAssertEqual(transport.take(), [Data("error_encryption_required".utf8)])

        let phoneKey = P256.KeyAgreement.PrivateKey()
        let phoneNonce = PairingSession.secureRandom(16)
        write(["action": "pairing_client_hello", "version": 5, "pairing_auth": "none", "pairing_policy": "confirm_app",
               "mobile_pub": Base64URL.encode(phoneKey.publicKey.x963Representation),
               "mobile_nonce": Base64URL.encode(phoneNonce)])
        let ready = CompactJSON.object(transport.take()[0])!
        XCTAssertEqual(ready["type"] as? String, "pairing_ready")

        // Derive keys as the app would and check the device's transcript hash.
        let devicePub = try P256.KeyAgreement.PublicKey(x963Representation: Base64URL.decode(ready["device_pub"])!)
        let transcript = PairingCrypto.transcript(.init(
            community: true, authEpoch: 0, policy: "confirm_app", deviceID: identity.deviceID,
            nodeID: identity.nodeID, mac: identity.mac, firmwareVersion: "0.1.0-ios",
            mobilePub: Base64URL.encode(phoneKey.publicKey.x963Representation),
            devicePub: ready["device_pub"] as! String, mobileNonce: Base64URL.encode(phoneNonce),
            deviceNonce: ready["device_nonce"] as! String))!
        let transcriptHash = Data(SHA256.hash(data: Data(transcript.utf8)))
        XCTAssertEqual(Base64URL.encode(transcriptHash), ready["transcript_hash"] as? String)
        let ecdh = try phoneKey.sharedSecretFromKeyAgreement(with: devicePub).withUnsafeBytes { Data($0) }
        let keys = PairingCrypto.deriveSessionKeys(ecdhSecret: ecdh, mobileNonce: phoneNonce,
                                                   deviceNonce: Base64URL.decode(ready["device_nonce"])!,
                                                   transcriptHash: transcriptHash)
        var phone = Mobile(tx: keys.mobileTx, rx: keys.mobileRx, sessionID: ready["session_id"] as! String)

        write(phone.seal(["action": "pairing_client_finished"]))
        let confirmed = phone.open(transport.take()[0])
        XCTAssertEqual(confirmed["status"] as? String, "pairing_confirmed")
        XCTAssertEqual(confirmed["sdk_token"] as? String, "mgst_test")

        write(phone.seal(["action": "wifi_scan"]))
        let scan = phone.open(transport.take()[0])
        XCTAssertEqual((scan["networks"] as? [[String: Any]])?.first?["ssid"] as? String, "HomeWiFi")

        write(phone.seal(["action": "provision_v2", "ssid": "HomeWiFi", "password": "",
                          "access_token": "at", "refresh_token": "hatch_refresh:rt", "token_type": "device",
                          "username": "me", "api_url_v2": "https://api.example", "noise_host": "noise.example"]))
        wait(for: [completed], timeout: 5)
        let statuses = transport.take().map { phone.open($0)["status"] as? String }
        XCTAssertEqual(statuses, ["wifi_connecting", "wifi_connected", "auth_ok"])
        XCTAssertEqual(saved?.accessToken, "at")
        XCTAssertEqual(saved?.noiseHost, "noise.example")
        XCTAssertEqual(saved?.apiURLv2, "https://api.example")
    }

    func testNoiseURLEscapesLikeEncodeURIComponent() {
        XCTAssertEqual(LinkSession.noiseURL(host: "gw.example", vmID: "vm 1&x").absoluteString,
                       "wss://gw.example/v1/noise?vm_id=vm%201%26x")
    }
}

/// Swift LinkSession against Tests/fake_vm.py over a real WebSocket. Run via
/// Tests/run_interop.sh, which sets FAKE_VM_PORT; skipped otherwise.
final class LiveInteropTests: XCTestCase {
    func testAgainstPythonFakeVM() async throws {
        let port = ProcessInfo.processInfo.environment["FAKE_VM_PORT"].flatMap(Int.init)
        try XCTSkipIf(port == nil, "run Tests/run_interop.sh to start the fake VM")
        let url = URL(string: "ws://127.0.0.1:\(port!)/v1/noise?vm_id=vm%201%26x")!
        let device = DeviceDescription(
            nodeID: "homelink-abcdef", displayName: "iPhone", version: "0.1.0-ios",
            commands: ["device.health": ["description": "Battery and status", "required": [:], "optional": [:]]])

        // 1. A wrong bearer must surface as authRejected (HTTP 401 on upgrade).
        let refused = LinkSession(socket: URLSessionGadgetWebSocket(url: url, bearer: "wrong", userAgent: "test"),
                                  device: device) { _, _ in [:] }
        let refusedOutcome = await refused.run()
        XCTAssertEqual(refusedOutcome, .authRejected)

        // 2. The full session.
        let session = LinkSession(socket: URLSessionGadgetWebSocket(url: url, bearer: "vm-token", userAgent: "test"),
                                  device: device) { command, params in
            ["ok": true, "payload": ["command": command, "echo": params]]
        }
        session.log = { print("[swift]", $0) }
        let chat = Task { () -> (Int, Data)? in
            while session.registeredAt == nil { try await Task.sleep(nanoseconds: 50_000_000) }
            try await Task.sleep(nanoseconds: 300_000_000)  // let invoke/result go first
            let reply = try await session.sendChat("hello from iPhone")
            return (reply.status, reply.body)
        }
        let outcome = await session.run()
        XCTAssertEqual(outcome, .unpaired)
        let reply = try await chat.value
        XCTAssertEqual(reply?.0, 200)
        XCTAssertEqual(reply.flatMap { CompactJSON.object($0.1)?["message_id"] as? String }, "m-1")
    }
}
