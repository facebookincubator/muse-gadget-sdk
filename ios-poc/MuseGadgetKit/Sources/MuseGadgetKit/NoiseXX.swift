import CryptoKit
import Foundation

/// `Noise_XX_25519_AESGCM_SHA256` initiator, empty prologue
/// (`linux/src/musegadget/noise/noise_xx.py`). The initiator's static key is
/// fresh per session: the VM bearer token authenticates the device.
public struct NoiseError: Error, CustomStringConvertible {
    public let description: String
    init(_ description: String) { self.description = description }
}

public final class NoiseCipherState {
    private var key: SymmetricKey?
    private var nonce: UInt64 = 0
    private var poisoned = false
    private let lock = NSLock()
    static let maxSafeNonce: UInt64 = (1 << 53) - 1

    init(key: Data? = nil) {
        if let key { self.key = SymmetricKey(data: key) }
    }

    var hasKey: Bool { key != nil }

    static func iv(_ n: UInt64) -> Data {
        var iv = Data(count: 4)
        withUnsafeBytes(of: n.bigEndian) { iv.append(contentsOf: $0) }
        return iv
    }

    public func encrypt(ad: Data, plaintext: Data) throws -> Data {
        lock.lock(); defer { lock.unlock() }
        guard !poisoned else { throw NoiseError("cipher poisoned") }
        guard let key else { return plaintext }
        let n = try nextNonce()
        do {
            let box = try AES.GCM.seal(plaintext, using: key, nonce: AES.GCM.Nonce(data: Self.iv(n)),
                                       authenticating: ad)
            return box.ciphertext + box.tag
        } catch {
            poisoned = true
            throw error
        }
    }

    public func decrypt(ad: Data, ciphertext: Data) throws -> Data {
        lock.lock(); defer { lock.unlock() }
        guard !poisoned else { throw NoiseError("cipher poisoned") }
        guard let key else { return ciphertext }
        let n = try nextNonce()
        do {
            guard ciphertext.count >= 16 else { throw NoiseError("ciphertext too short") }
            let box = try AES.GCM.SealedBox(nonce: AES.GCM.Nonce(data: Self.iv(n)),
                                            ciphertext: ciphertext.dropLast(16),
                                            tag: ciphertext.suffix(16))
            return try AES.GCM.open(box, using: key, authenticating: ad)
        } catch {
            poisoned = true
            throw NoiseError("decrypt failed")
        }
    }

    private func nextNonce() throws -> UInt64 {
        guard nonce < Self.maxSafeNonce else {
            poisoned = true
            throw NoiseError("nonce exhausted")
        }
        defer { nonce += 1 }
        return nonce
    }
}

enum NoiseCrypto {
    static let protocolName = Data("Noise_XX_25519_AESGCM_SHA256".utf8)

    static func hmac(_ key: Data, _ data: Data) -> Data {
        Data(HMAC<SHA256>.authenticationCode(for: data, using: SymmetricKey(data: key)))
    }

    /// Noise's HKDF (two outputs).
    static func hkdf2(_ chainingKey: Data, _ ikm: Data) -> (Data, Data) {
        let temp = hmac(chainingKey, ikm)
        let out1 = hmac(temp, Data([1]))
        let out2 = hmac(temp, out1 + Data([2]))
        return (out1, out2)
    }

    static let lowOrderPoints: [Data] = [
        Data(count: 32),
        Data([1] + [UInt8](repeating: 0, count: 31)),
        Data(hex: "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800")!,
        Data(hex: "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157")!,
        Data(hex: "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f")!,
        Data(hex: "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f")!,
        Data(hex: "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f")!,
    ]

    static func dh(_ priv: Curve25519.KeyAgreement.PrivateKey, _ pub: Data) throws -> Data {
        guard pub.count == 32 else { throw NoiseError("x25519: invalid public key length") }
        guard !lowOrderPoints.contains(pub) else { throw NoiseError("x25519: low-order public key") }
        let shared = try priv.sharedSecretFromKeyAgreement(
            with: Curve25519.KeyAgreement.PublicKey(rawRepresentation: pub))
        let bytes = shared.withUnsafeBytes { Data($0) }
        guard bytes != Data(count: 32) else { throw NoiseError("x25519: all-zero output") }
        return bytes
    }
}

final class NoiseSymmetricState {
    private(set) var ck = Data(count: 32)
    private(set) var h = Data(count: 32)
    private var cipher = NoiseCipherState()

    func initialize() {
        var padded = NoiseCrypto.protocolName
        padded.append(Data(count: 32 - padded.count))
        h = padded
        ck = padded
        mixHash(Data())  // empty prologue
    }

    func mixHash(_ data: Data) {
        h = Data(SHA256.hash(data: h + data))
    }

    func mixKey(_ ikm: Data) {
        let (newCK, tempK) = NoiseCrypto.hkdf2(ck, ikm)
        ck = newCK
        cipher = NoiseCipherState(key: tempK)
    }

    func encryptAndHash(_ plaintext: Data) throws -> Data {
        let ciphertext = try cipher.encrypt(ad: h, plaintext: plaintext)
        mixHash(ciphertext)
        return ciphertext
    }

    func decryptAndHash(_ ciphertext: Data) throws -> Data {
        let plaintext = try cipher.decrypt(ad: h, ciphertext: ciphertext)
        mixHash(ciphertext)
        return plaintext
    }

    func split() -> (NoiseCipherState, NoiseCipherState) {
        let (k1, k2) = NoiseCrypto.hkdf2(ck, Data())
        ck = Data(count: 32)
        h = Data(count: 32)
        return (NoiseCipherState(key: k1), NoiseCipherState(key: k2))
    }
}

public final class NoiseXXInitiator {
    private enum Phase { case initialized, msg1Sent, msg2Read, msg3Sent, split, dead }

    private let ss = NoiseSymmetricState()
    private let generateKey: () -> Curve25519.KeyAgreement.PrivateKey
    private var e: Curve25519.KeyAgreement.PrivateKey?
    private var re: Data?
    private var phase = Phase.initialized

    public init(generateKey: @escaping () -> Curve25519.KeyAgreement.PrivateKey = { .init() }) {
        self.generateKey = generateKey
        ss.initialize()
    }

    /// `-> e`
    public func writeMessage1() throws -> Data {
        guard phase == .initialized else { throw NoiseError("writeMessage1 in wrong phase") }
        let e = generateKey()
        self.e = e
        let pub = e.publicKey.rawRepresentation
        ss.mixHash(pub)
        _ = try ss.encryptAndHash(Data())
        phase = .msg1Sent
        return pub
    }

    /// `<- e, ee, s, es`; returns the responder's payload.
    @discardableResult
    public func readMessage2(_ msg: Data) throws -> Data {
        guard phase == .msg1Sent, let e else { throw NoiseError("readMessage2 in wrong phase") }
        let bytes = Data(msg)
        guard bytes.count >= 32 + 48 + 16 else {
            phase = .dead
            throw NoiseError("message 2 too short (\(bytes.count))")
        }
        do {
            let re = bytes.prefix(32)
            self.re = Data(re)
            ss.mixHash(Data(re))
            ss.mixKey(try NoiseCrypto.dh(e, Data(re)))
            let rs = try ss.decryptAndHash(Data(bytes[32..<80]))
            ss.mixKey(try NoiseCrypto.dh(e, rs))
            let payload = try ss.decryptAndHash(Data(bytes[80...]))
            phase = .msg2Read
            return payload
        } catch {
            phase = .dead
            throw error
        }
    }

    /// `-> s, se` with an empty payload.
    public func writeMessage3() throws -> Data {
        guard phase == .msg2Read, let re else { throw NoiseError("writeMessage3 in wrong phase") }
        do {
            let s = generateKey()
            let encS = try ss.encryptAndHash(s.publicKey.rawRepresentation)
            ss.mixKey(try NoiseCrypto.dh(s, re))
            let encPayload = try ss.encryptAndHash(Data())
            phase = .msg3Sent
            return encS + encPayload
        } catch {
            phase = .dead
            throw error
        }
    }

    /// Returns (send, receive) cipher states for the initiator.
    public func split() throws -> (send: NoiseCipherState, receive: NoiseCipherState) {
        guard phase == .msg3Sent else { throw NoiseError("split in wrong phase") }
        phase = .split
        e = nil
        re = nil
        let (c1, c2) = ss.split()
        return (c1, c2)
    }
}
