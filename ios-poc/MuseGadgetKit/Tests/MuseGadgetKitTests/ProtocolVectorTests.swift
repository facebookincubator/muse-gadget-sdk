import CryptoKit
import XCTest
@testable import MuseGadgetKit

func loadVectors(_ name: String) throws -> [String: Any] {
    let url = try XCTUnwrap(Bundle.module.url(forResource: name, withExtension: "json", subdirectory: "Vectors"))
    return try XCTUnwrap(CompactJSON.object(Data(contentsOf: url)))
}

func hex(_ any: Any?) -> Data { Data(hex: any as! String)! }

/// Pairing v5 against the published fixtures in linux/tests/vectors/link_pairing_v5.json.
final class PairingVectorTests: XCTestCase {
    var vectors: [[String: Any]] = []

    override func setUpWithError() throws {
        vectors = try loadVectors("link_pairing_v5")["vectors"] as! [[String: Any]]
    }

    func vector(_ name: String) -> [String: Any] { vectors.first { $0["name"] as? String == name }! }

    func testTranscriptMatchesEveryVector() {
        for v in vectors {
            let transcript = PairingCrypto.transcript(.init(
                community: v["community"] as! Bool, authEpoch: v["pairing_auth_epoch"] as! Int,
                policy: v["pairing_policy"] as! String, deviceID: v["device_id"] as! String,
                nodeID: v["node_id"] as! String, mac: v["mac"] as! String,
                firmwareVersion: v["firmware_version"] as! String, mobilePub: v["mobile_pub"] as! String,
                devicePub: v["device_pub"] as! String, mobileNonce: v["mobile_nonce"] as! String,
                deviceNonce: v["device_nonce"] as! String))
            XCTAssertEqual(transcript, v["transcript"] as? String, "\(v["name"]!)")
            let hash = Data(SHA256.hash(data: Data(transcript!.utf8)))
            XCTAssertEqual(Base64URL.encode(hash), v["transcript_hash"] as? String)
        }
    }

    func testECDHAndKeyScheduleMatchEveryVector() throws {
        for v in vectors {
            let mobile = try P256.KeyAgreement.PrivateKey(rawRepresentation: hex(v["mobile_private_scalar_hex"]))
            let device = try P256.KeyAgreement.PrivateKey(rawRepresentation: hex(v["device_private_scalar_hex"]))
            XCTAssertEqual(Base64URL.encode(mobile.publicKey.x963Representation), v["mobile_pub"] as? String)
            XCTAssertEqual(Base64URL.encode(device.publicKey.x963Representation), v["device_pub"] as? String)
            let ecdh = try device.sharedSecretFromKeyAgreement(with: mobile.publicKey).withUnsafeBytes { Data($0) }
            XCTAssertEqual(ecdh, hex(v["ecdh_secret_hex"]))

            let keys = PairingCrypto.deriveSessionKeys(
                ecdhSecret: ecdh, mobileNonce: Base64URL.decode(v["mobile_nonce"])!,
                deviceNonce: Base64URL.decode(v["device_nonce"])!,
                transcriptHash: Base64URL.decode(v["transcript_hash"])!)
            XCTAssertEqual(keys.sessionSecret, hex(v["session_secret_hex"]))
            XCTAssertEqual(keys.mobileTx.withUnsafeBytes { Data($0) }, hex(v["mobile_tx_key_hex"]))
            XCTAssertEqual(keys.mobileRx.withUnsafeBytes { Data($0) }, hex(v["mobile_rx_key_hex"]))
            XCTAssertEqual(Base64URL.encode(keys.sessionID), v["session_id"] as? String)

            // The client_finished record, sealed by the mobile with mobile_tx.
            let aad = PairingCrypto.recordAAD(sessionID: v["session_id"] as! String, direction: 0, counter: 0)
            XCTAssertEqual(String(decoding: aad, as: UTF8.self), v["client_finished_aad"] as? String)
            let sealed = try AES.GCM.seal(Data((v["client_finished_plaintext"] as! String).utf8), using: keys.mobileTx,
                                          nonce: AES.GCM.Nonce(data: PairingCrypto.recordNonce(direction: 0, counter: 0)),
                                          authenticating: aad)
            XCTAssertEqual(Base64URL.encode(sealed.ciphertext), v["client_finished_ciphertext"] as? String)
            XCTAssertEqual(Base64URL.encode(sealed.tag), v["client_finished_tag"] as? String)
        }
    }

    func testDeviceSessionProducesTheVectorPairingReadyAndConfirms() throws {
        let v = vector("community_app_v5")
        let session = PairingSession(
            nodeID: v["node_id"] as! String, deviceID: v["device_id"] as! String, mac: v["mac"] as! String,
            firmwareVersion: v["firmware_version"] as! String, sdkToken: "mgst_test",
            generateKey: { try! P256.KeyAgreement.PrivateKey(rawRepresentation: hex(v["device_private_scalar_hex"])) },
            randomBytes: { _ in Base64URL.decode(v["device_nonce"])! })
        let ready = try session.handleHello([
            "action": "pairing_client_hello", "version": 5, "pairing_auth": "none",
            "pairing_policy": "confirm_app", "mobile_pub": v["mobile_pub"]!, "mobile_nonce": v["mobile_nonce"]!,
        ])
        XCTAssertEqual(ready["type"] as? String, "pairing_ready")
        XCTAssertEqual(ready["device_pub"] as? String, v["device_pub"] as? String)
        XCTAssertEqual(ready["transcript_hash"] as? String, v["transcript_hash"] as? String)
        XCTAssertEqual(ready["session_id"] as? String, v["session_id"] as? String)

        let plaintext = try session.decrypt([
            "action": "pairing_encrypted", "session_id": v["session_id"]!, "counter": "0",
            "ciphertext": v["client_finished_ciphertext"]!, "tag": v["client_finished_tag"]!,
        ])
        XCTAssertEqual(plaintext, v["client_finished_plaintext"] as? String)
        XCTAssertNotEqual(session.handleClientFinished(CompactJSON.object(Data(plaintext.utf8))!), 0)
        XCTAssertTrue(session.confirmed)
    }

    func testReplayedRecordClearsTheSession() throws {
        let v = vector("community_app_v5")
        let session = PairingSession(
            nodeID: v["node_id"] as! String, deviceID: v["device_id"] as! String, mac: v["mac"] as! String,
            firmwareVersion: "1.0.0",
            generateKey: { try! P256.KeyAgreement.PrivateKey(rawRepresentation: hex(v["device_private_scalar_hex"])) },
            randomBytes: { _ in Base64URL.decode(v["device_nonce"])! })
        _ = try session.handleHello(["version": 5, "pairing_auth": "none", "pairing_policy": "confirm_app",
                                     "mobile_pub": v["mobile_pub"]!, "mobile_nonce": v["mobile_nonce"]!])
        let record: [String: Any] = ["session_id": v["session_id"]!, "counter": "0",
                                     "ciphertext": v["client_finished_ciphertext"]!, "tag": v["client_finished_tag"]!]
        _ = try session.decrypt(record)
        XCTAssertThrowsError(try session.decrypt(record))
        XCTAssertEqual(session.currentState, .idle)
    }

    func testHelloRejectsOfficialAuthAndBooleanVersion() {
        let session = PairingSession(nodeID: "homelink-000001", deviceID: "d", mac: "m", firmwareVersion: "1")
        XCTAssertThrowsError(try session.handleHello(["version": true, "pairing_auth": "none", "pairing_policy": "confirm_app"]))
        XCTAssertThrowsError(try session.handleHello(["version": 5, "pairing_auth": "fleet_ecdsa_p256_v1", "pairing_policy": "confirm_app"]))
    }
}

/// Noise XX, envelopes and BLE framing against bytes produced by the Python SDK
/// (Vectors/gen_noise_vectors.py).
final class NoiseVectorTests: XCTestCase {
    var v: [String: Any] = [:]

    override func setUpWithError() throws { v = try loadVectors("noise_vectors") }

    func key(_ name: String) -> Curve25519.KeyAgreement.PrivateKey {
        let seed = (v["seeds"] as! [String: Int])[name]!
        return try! .init(rawRepresentation: Data(repeating: UInt8(seed), count: 32))
    }

    func testHandshakeAndTransportMatchPythonByteForByte() throws {
        var keys = [key("init_e"), key("init_s")]
        let initiator = NoiseXXInitiator(generateKey: { keys.removeFirst() })
        XCTAssertEqual(try initiator.writeMessage1().hex, v["msg1"] as? String)
        let payload = try initiator.readMessage2(hex(v["msg2"]))
        XCTAssertEqual(String(decoding: payload, as: UTF8.self), v["server_payload"] as? String)
        XCTAssertEqual(try initiator.writeMessage3().hex, v["msg3"] as? String)

        let (send, receive) = try initiator.split()
        let transport = NoiseTransport(send: send, receive: receive, chunkID: { 0x0123456789ABCDEF })
        let (stream, control) = try transport.request("POST", "/link-control", endBody: false)
        XCTAssertEqual(stream, 1)
        XCTAssertEqual(control.map(\.hex), v["control_request_ciphertexts"] as? [String])

        let register = LinkMessage.encode(Data((v["register_json"] as! String).utf8))
        XCTAssertEqual(try transport.bodyChunk(streamID: stream, register).map(\.hex),
                       v["register_ciphertexts"] as? [String])

        var decoder = LinkMessageDecoder()
        var received: [[String: Any]] = []
        var statuses: [Int] = []
        for ciphertext in v["server_ciphertexts"] as! [String] {
            let (id, frame) = try XCTUnwrap(try transport.decrypt(Data(hex: ciphertext)!))
            XCTAssertEqual(id, 1)
            if case let .response(status, _, _, _) = frame { statuses.append(status) }
            received += try decoder.feed(frame.data!.0)
        }
        XCTAssertEqual(statuses, [200])
        XCTAssertEqual(received.first?["method"] as? String, "link.invoke")
        XCTAssertEqual(received.first?["command"] as? String, "device.health")
    }

    func testTamperedFrameFailsAndPoisonsTheCipher() throws {
        var keys = [key("init_e"), key("init_s")]
        let initiator = NoiseXXInitiator(generateKey: { keys.removeFirst() })
        _ = try initiator.writeMessage1()
        try initiator.readMessage2(hex(v["msg2"]))
        _ = try initiator.writeMessage3()
        let (send, receive) = try initiator.split()
        let transport = NoiseTransport(send: send, receive: receive)
        var bad = Data(hex: (v["server_ciphertexts"] as! [String])[0])!
        bad[bad.startIndex] ^= 1
        XCTAssertThrowsError(try transport.decrypt(bad))
        XCTAssertThrowsError(try transport.decrypt(Data(hex: (v["server_ciphertexts"] as! [String])[0])!))
    }

    func testBLEChunksMatchPython() throws {
        let message = Data(#"{"type":"device_info","node_id":"homelink-abcdef","pad":""#.utf8)
            + Data(repeating: UInt8(ascii: "x"), count: 300) + Data(#""}"#.utf8)
        let chunks = try BLEFraming.encodeChunks(message, mtu: 185)
        XCTAssertEqual(chunks.map(\.hex), v["ble_chunks_mtu185"] as? [String])
        var assembler = ChunkAssembler()
        let out = chunks.compactMap { assembler.feed($0) }
        XCTAssertEqual(out, [message])
    }
}
