import CryptoKit
import Foundation

/// Device side of Muse Gadget BLE pairing, protocol version 5, community mode
/// (`linux/src/musegadget/pairing.py`): P-256 ECDH, HKDF-SHA256, AES-256-GCM
/// records, and `confirm_app` so the app's own consent confirms the session.
public enum PairingConstants {
    public static let version = 5
    public static let model = "hatch_link"
    public static let suite = "p256-hkdf-sha256-aes-gcm-v1"
    public static let policyButton = "confirm_press"
    public static let policyApp = "confirm_app"
    public static let authOfficial = "fleet_ecdsa_p256_v1"
    public static let authCommunity = "none"
    public static let buttonConfirmTimeout = 60
    public static let recordLabel = "hatch-link ble setup v1"
    public static let sessionIDLabel = Data("hatch-link session id v1".utf8)
    public static let clientFinishedTimeout: TimeInterval = 60
    public static let confirmedTimeout: TimeInterval = 120
    public static let provisioningTimeout: TimeInterval = 120
    public static let errorInvalidHello = "error_pairing_invalid_hello"
    public static let errorDecrypt = "error_pairing_decrypt"
}

public struct PairingError: Error, Equatable {
    public let status: String
}

public enum PairingState: Equatable {
    case idle, waitClientFinished, ready, provisioning
}

public enum PairingCrypto {
    static let toDevice: UInt8 = 0
    static let fromDevice: UInt8 = 1

    public struct TranscriptFields {
        public var community: Bool
        public var authEpoch: Int
        public var policy: String
        public var deviceID: String
        public var nodeID: String
        public var mac: String
        public var firmwareVersion: String
        public var mobilePub: String
        public var devicePub: String
        public var mobileNonce: String
        public var deviceNonce: String

        public init(community: Bool, authEpoch: Int, policy: String, deviceID: String,
                    nodeID: String, mac: String, firmwareVersion: String, mobilePub: String,
                    devicePub: String, mobileNonce: String, deviceNonce: String) {
            self.community = community
            self.authEpoch = authEpoch
            self.policy = policy
            self.deviceID = deviceID
            self.nodeID = nodeID
            self.mac = mac
            self.firmwareVersion = firmwareVersion
            self.mobilePub = mobilePub
            self.devicePub = devicePub
            self.mobileNonce = mobileNonce
            self.deviceNonce = deviceNonce
        }
    }

    /// Canonical v5 transcript; its SHA-256 is the `transcript_hash`.
    public static func transcript(_ f: TranscriptFields) -> String? {
        let button = f.policy == PairingConstants.policyButton
        guard button || (f.community && f.policy == PairingConstants.policyApp) else { return nil }
        guard f.community ? f.authEpoch == 0 : f.authEpoch > 0 else { return nil }
        let required = [f.deviceID, f.nodeID, f.mac, f.firmwareVersion,
                        f.mobilePub, f.devicePub, f.mobileNonce, f.deviceNonce]
        guard required.allSatisfy({ !$0.isEmpty }) else { return nil }
        let v = PairingConstants.version
        return [
            "hatch-link-pairing-v\(v)",
            "version=\(v)",
            "initiator_role=mobile",
            "responder_role=link",
            "device_id=\(f.deviceID)",
            "node_id=\(f.nodeID)",
            "mac=\(f.mac)",
            "model=\(PairingConstants.model)",
            "firmware_version=\(f.firmwareVersion)",
            "selected_cipher_suite=\(PairingConstants.suite)",
            "pairing_auth=\(f.community ? PairingConstants.authCommunity : PairingConstants.authOfficial)",
            "pairing_auth_epoch=\(f.authEpoch)",
            "pairing_policy=\(f.policy)",
            "confirm_timeout_seconds=\(button ? PairingConstants.buttonConfirmTimeout : 0)",
            "mobile_pub=\(f.mobilePub)",
            "device_pub=\(f.devicePub)",
            "mobile_nonce=\(f.mobileNonce)",
            "device_nonce=\(f.deviceNonce)",
        ].joined(separator: "\n")
    }

    public struct SessionKeys {
        public let sessionSecret: Data
        /// Decrypts records the device receives.
        public let mobileTx: SymmetricKey
        /// Encrypts records the device sends.
        public let mobileRx: SymmetricKey
        public let sessionID: Data
    }

    public static func deriveSessionKeys(ecdhSecret: Data, mobileNonce: Data, deviceNonce: Data,
                                         transcriptHash: Data) -> SessionKeys {
        let salt = Data(SHA256.hash(data: mobileNonce + deviceNonce + transcriptHash))
        let sessionSecret = HKDF<SHA256>.deriveKey(
            inputKeyMaterial: SymmetricKey(data: ecdhSecret), salt: salt,
            info: Data(PairingConstants.recordLabel.utf8), outputByteCount: 32)
        let secretBytes = sessionSecret.withUnsafeBytes { Data($0) }
        let mobileTx = HKDF<SHA256>.expand(pseudoRandomKey: secretBytes,
                                           info: Data("mobile->device".utf8), outputByteCount: 32)
        let mobileRx = HKDF<SHA256>.expand(pseudoRandomKey: secretBytes,
                                           info: Data("device->mobile".utf8), outputByteCount: 32)
        let sessionID = Data(SHA256.hash(data: PairingConstants.sessionIDLabel + transcriptHash + ecdhSecret)
            .prefix(16))
        return SessionKeys(sessionSecret: secretBytes, mobileTx: mobileTx, mobileRx: mobileRx,
                           sessionID: sessionID)
    }

    public static func recordNonce(direction: UInt8, counter: UInt64) -> Data {
        var nonce = Data([direction, 0, 0, 0])
        withUnsafeBytes(of: counter.bigEndian) { nonce.append(contentsOf: $0) }
        return nonce
    }

    public static func recordAAD(sessionID: String, direction: UInt8, counter: UInt64) -> Data {
        let arrow = direction == toDevice ? "m2d" : "d2m"
        return Data("\(PairingConstants.recordLabel)|\(sessionID)|\(arrow)|\(counter)".utf8)
    }

    static func parseCounter(_ value: Any?) -> UInt64? {
        guard let text = value as? String, !text.isEmpty,
              text.utf8.allSatisfy({ $0 >= 0x30 && $0 <= 0x39 })
        else { return nil }
        return UInt64(text)
    }
}

/// One device's pairing state. Methods that advance the handshake return a
/// nonzero generation; deferred work checks it so a stale attempt can't act
/// on a newer one. Thread-safe.
public final class PairingSession {
    private let nodeID: String
    private let deviceID: String
    private let mac: String
    private let firmwareVersion: String
    private let sdkToken: String?
    private let clock: () -> TimeInterval
    private let generateKey: () -> P256.KeyAgreement.PrivateKey
    private let randomBytes: (Int) -> Data
    private let lock = NSLock()

    private var generation = 0
    private var state = PairingState.idle
    private var deadline: TimeInterval = 0
    private var rxKey: SymmetricKey?
    private var txKey: SymmetricKey?
    private var sessionIDB64 = ""
    private var rxCounter: UInt64 = 0
    private var txCounter: UInt64 = 0

    public init(nodeID: String, deviceID: String, mac: String, firmwareVersion: String,
                sdkToken: String? = nil,
                clock: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime },
                generateKey: @escaping () -> P256.KeyAgreement.PrivateKey = { P256.KeyAgreement.PrivateKey() },
                randomBytes: @escaping (Int) -> Data = PairingSession.secureRandom) {
        self.nodeID = nodeID
        self.deviceID = deviceID
        self.mac = mac
        self.firmwareVersion = firmwareVersion.isEmpty ? "unknown" : firmwareVersion
        self.sdkToken = sdkToken
        self.clock = clock
        self.generateKey = generateKey
        self.randomBytes = randomBytes
        resetLocked()
    }

    public static func secureRandom(_ count: Int) -> Data {
        var bytes = [UInt8](repeating: 0, count: count)
        precondition(SecRandomCopyBytes(kSecRandomDefault, count, &bytes) == errSecSuccess)
        return Data(bytes)
    }

    /// Pairing fields of the `get_device_info` response.
    public func deviceInfo() -> [String: Any] {
        [
            "device_id": deviceID,
            "mac": mac,
            "model": PairingConstants.model,
            "pairing_protocol": PairingConstants.version,
            "pairing_auth": PairingConstants.authCommunity,
            "pairing_auth_epoch": 0,
            "pairing_policy": PairingConstants.policyApp,
        ]
    }

    public var currentState: PairingState {
        withLock { _ = expireLocked(); return state }
    }

    public var confirmed: Bool {
        let s = currentState
        return s == .ready || s == .provisioning
    }

    public func isCurrent(_ gen: Int) -> Bool {
        withLock { gen != 0 && gen == generation }
    }

    public func reset() {
        withLock { resetLocked() }
    }

    /// Starts a session from `pairing_client_hello`; returns `pairing_ready`.
    public func handleHello(_ message: [String: Any]) throws -> [String: Any] {
        // JSONSerialization yields NSNumber; reject booleans as Python does.
        guard let version = message["version"] as? NSNumber,
              CFGetTypeID(version) != CFBooleanGetTypeID(),
              version.doubleValue == Double(PairingConstants.version),
              message["pairing_auth"] as? String == PairingConstants.authCommunity,
              message["pairing_policy"] as? String == PairingConstants.policyApp
        else { throw PairingError(status: PairingConstants.errorInvalidHello) }

        return try withLock {
            resetLocked()
            guard let mobilePub = Base64URL.decode(message["mobile_pub"]),
                  let mobileNonce = Base64URL.decode(message["mobile_nonce"]),
                  mobilePub.count == 65, mobilePub.first == 0x04, mobileNonce.count == 16,
                  let peer = try? P256.KeyAgreement.PublicKey(x963Representation: mobilePub)
            else {
                resetLocked()
                throw PairingError(status: PairingConstants.errorInvalidHello)
            }

            let deviceKey = generateKey()
            let devicePub = deviceKey.publicKey.x963Representation
            let deviceNonce = randomBytes(16)
            guard let transcript = PairingCrypto.transcript(.init(
                community: true, authEpoch: 0, policy: PairingConstants.policyApp,
                deviceID: deviceID, nodeID: nodeID, mac: mac, firmwareVersion: firmwareVersion,
                mobilePub: Base64URL.encode(mobilePub), devicePub: Base64URL.encode(devicePub),
                mobileNonce: Base64URL.encode(mobileNonce), deviceNonce: Base64URL.encode(deviceNonce)))
            else {
                resetLocked()
                throw PairingError(status: PairingConstants.errorInvalidHello)
            }
            let transcriptHash = Data(SHA256.hash(data: Data(transcript.utf8)))
            guard let shared = try? deviceKey.sharedSecretFromKeyAgreement(with: peer) else {
                resetLocked()
                throw PairingError(status: PairingConstants.errorInvalidHello)
            }
            let ecdh = shared.withUnsafeBytes { Data($0) }
            let keys = PairingCrypto.deriveSessionKeys(
                ecdhSecret: ecdh, mobileNonce: mobileNonce, deviceNonce: deviceNonce,
                transcriptHash: transcriptHash)

            rxKey = keys.mobileTx
            txKey = keys.mobileRx
            sessionIDB64 = Base64URL.encode(keys.sessionID)
            rxCounter = 0
            txCounter = 0
            state = .waitClientFinished
            deadline = clock() + PairingConstants.clientFinishedTimeout
            return [
                "type": "pairing_ready",
                "version": PairingConstants.version,
                "device_id": deviceID,
                "node_id": nodeID,
                "mac": mac,
                "model": PairingConstants.model,
                "firmware_version": firmwareVersion,
                "pairing_auth": PairingConstants.authCommunity,
                "pairing_auth_epoch": 0,
                "pairing_policy": PairingConstants.policyApp,
                "device_pub": Base64URL.encode(devicePub),
                "device_nonce": Base64URL.encode(deviceNonce),
                "transcript_hash": Base64URL.encode(transcriptHash),
                "session_id": sessionIDB64,
            ]
        }
    }

    /// Opens one mobile-to-device `pairing_encrypted` record. Any failure
    /// clears the session, as the firmware does.
    public func decrypt(_ envelope: [String: Any]) throws -> String {
        try withLock {
            if expireLocked() || state == .idle {
                resetLocked()
                throw PairingError(status: PairingConstants.errorDecrypt)
            }
            guard envelope["session_id"] as? String == sessionIDB64,
                  let counter = PairingCrypto.parseCounter(envelope["counter"]),
                  counter == rxCounter,
                  let ciphertext = Base64URL.decode(envelope["ciphertext"], maxChars: 16384),
                  let tag = Base64URL.decode(envelope["tag"]), tag.count == 16,
                  let key = rxKey,
                  let box = try? AES.GCM.SealedBox(
                      nonce: AES.GCM.Nonce(data: PairingCrypto.recordNonce(direction: PairingCrypto.toDevice, counter: counter)),
                      ciphertext: ciphertext, tag: tag),
                  let plain = try? AES.GCM.open(box, using: key, authenticating: PairingCrypto.recordAAD(
                      sessionID: sessionIDB64, direction: PairingCrypto.toDevice, counter: counter)),
                  let text = String(data: plain, encoding: .utf8)
            else {
                resetLocked()
                throw PairingError(status: PairingConstants.errorDecrypt)
            }
            rxCounter += 1
            return text
        }
    }

    /// Confirms the session if `command` is exactly
    /// `{"action":"pairing_client_finished"}` and was the first record.
    /// Returns the new generation, or 0 after clearing the session.
    public func handleClientFinished(_ command: [String: Any]) -> Int {
        withLock {
            let ok = command.count == 1
                && command["action"] as? String == "pairing_client_finished"
                && !expireLocked()
                && state == .waitClientFinished
                && rxCounter == 1
            guard ok else {
                resetLocked()
                return 0
            }
            generation += 1
            state = .ready
            deadline = clock() + PairingConstants.confirmedTimeout
            return generation
        }
    }

    public func markProvisioning() -> Int {
        withLock {
            if !expireLocked() && state == .ready {
                generation += 1
                state = .provisioning
                deadline = clock() + PairingConstants.provisioningTimeout
            }
            return state == .provisioning ? generation : 0
        }
    }

    @discardableResult
    public func extendProvisioning(_ gen: Int) -> Bool {
        withLock {
            let valid = provisioningLocked(gen)
            if valid { deadline = clock() + PairingConstants.provisioningTimeout }
            return valid
        }
    }

    /// Runs `commit` (local persistence only) if the session is still valid.
    public func commitProvisioning(_ gen: Int, _ commit: () -> Bool) -> Bool {
        withLock { provisioningLocked(gen) && commit() }
    }

    /// Seals a device-to-mobile record, or nil without a current session.
    public func encryptJSON(_ plaintext: String, generation gen: Int = 0) -> [String: Any]? {
        withLock {
            if (gen != 0 && gen != generation) || expireLocked() || state == .idle { return nil }
            guard let key = txKey,
                  let sealed = try? AES.GCM.seal(
                      Data(plaintext.utf8), using: key,
                      nonce: AES.GCM.Nonce(data: PairingCrypto.recordNonce(direction: PairingCrypto.fromDevice, counter: txCounter)),
                      authenticating: PairingCrypto.recordAAD(
                          sessionID: sessionIDB64, direction: PairingCrypto.fromDevice, counter: txCounter))
            else { return nil }
            let counter = txCounter
            txCounter += 1
            return [
                "type": "pairing_encrypted",
                "session_id": sessionIDB64,
                "counter": String(counter),
                "ciphertext": Base64URL.encode(sealed.ciphertext),
                "tag": Base64URL.encode(sealed.tag),
            ]
        }
    }

    public func encryptStatus(_ status: String, generation gen: Int = 0) -> [String: Any]? {
        var message: [String: Any] = ["type": "status", "status": status]
        if let sdkToken, status == "pairing_confirmed" { message["sdk_token"] = sdkToken }
        guard let text = try? CompactJSON.string(message) else { return nil }
        return encryptJSON(text, generation: gen)
    }

    // MARK: - Locked helpers

    private func withLock<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }

    private func provisioningLocked(_ gen: Int) -> Bool {
        gen != 0 && gen == generation && !expireLocked() && state == .provisioning
    }

    private func resetLocked() {
        generation += 1
        clearLocked()
    }

    private func clearLocked() {
        state = .idle
        deadline = 0
        rxKey = nil
        txKey = nil
        sessionIDB64 = ""
        rxCounter = 0
        txCounter = 0
    }

    private func expireLocked() -> Bool {
        if state == .idle || clock() <= deadline { return false }
        // Keep the generation so the owner of the expired session can still
        // recognise it and close the connection.
        clearLocked()
        return true
    }
}
