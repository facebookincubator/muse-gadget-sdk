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
import CryptoKit
import Foundation
import Security

/*
 * The token proof from the client's side (protocols/mgcommands.h, Token
 * proof): challenge, check the gadget's MAC, confirm, result. A match says
 * the gadget holds the proof key K this phone has for it. busy answers
 * restart the exchange about 200 ms later; 5 s of silence, or 20 s in all,
 * is a timeout. Not thread-safe: one queue drives it.
 */
final class TokenProof {
    enum Outcome: Equatable {
        case match
        case mismatch        /* the gadget's MAC is wrong, or it said 0: another key */
        case notFound        /* the gadget holds no token */
        case refused(Int)    /* another error (e.g. recovery mode, unsupported) */
        case recovery        /* the gadget declared a recovery mode: no proof with it (mgcommands.h) */
        case timeout
    }

    static let cmd: UInt8 = 34
    static let challenge: UInt8 = 0, response: UInt8 = 1, confirm: UInt8 = 2, result: UInt8 = 3
    static let nonceLen = 16, macLen = 32
    static let errorCmd: UInt8 = 25, errorBusy = 4, errorNotFound = 5
    static let busyRetryMs: Int64 = 200, silenceMs: Int64 = 5000, totalMs: Int64 = 20000

    private let key: SymmetricKey
    private let send: ([UInt8]) -> Void
    private let nonce: () -> [UInt8]
    private var clientNonce: [UInt8] = []
    private var started = false, awaitingResult = false
    private var firstMs: Int64 = 0, lastMs: Int64 = 0, retryAtMs: Int64 = -1
    private(set) var outcome: Outcome?

    /* send: one Control message. nonce: a fresh client nonce (tests pass a fixed one). */
    init(key: [UInt8], send: @escaping ([UInt8]) -> Void, nonce: @escaping () -> [UInt8] = TokenProof.randomNonce) {
        self.key = SymmetricKey(data: key)
        self.send = send
        self.nonce = nonce
    }

    static func randomNonce() -> [UInt8] {
        var n = [UInt8](repeating: 0, count: nonceLen)
        guard SecRandomCopyBytes(kSecRandomDefault, n.count, &n) == errSecSuccess else {
            fatalError("no random source for the token proof")
        }
        return n
    }

    /* HMAC-SHA256(K, "mg token proof v1 device|client" || client_nonce || device_nonce). */
    static func mac(_ key: SymmetricKey, device: Bool, _ cn: [UInt8], _ dn: [UInt8]) -> [UInt8] {
        var m = Array(("mg token proof v1 " + (device ? "device" : "client")).utf8)
        m += cn
        m += dn
        return Array(HMAC<SHA256>.authenticationCode(for: m, using: key))
    }

    var pending: Bool { started && outcome == nil }

    func start(nowMs: Int64) {
        started = true
        firstMs = nowMs
        sendChallenge(nowMs)
    }

    private func sendChallenge(_ nowMs: Int64) {
        clientNonce = nonce()
        awaitingResult = false
        retryAtMs = -1
        lastMs = nowMs
        send([Self.cmd, Self.challenge] + clientNonce)
    }

    /* A Control notification: true when it was the proof's (token_proof, or an error for it). */
    @discardableResult
    func onControl(_ v: [UInt8], nowMs: Int64) -> Bool {
        guard pending, let c = v.first else { return false }
        if c == Self.errorCmd && v.count > 1 && v[1] == Self.cmd {
            let code = Self.errorCode(v)
            if code == Self.errorBusy {
                lastMs = nowMs
                retryAtMs = nowMs + Self.busyRetryMs
            } else if code == Self.errorNotFound {
                outcome = .notFound
            } else {
                outcome = .refused(code)
            }
            return true
        }
        guard c == Self.cmd, v.count >= 2 else { return false }
        if v[1] == Self.response && !awaitingResult && !clientNonce.isEmpty {
            guard v.count >= 2 + Self.nonceLen + Self.macLen else {
                outcome = .mismatch
                return true
            }
            let dn = Array(v[2..<(2 + Self.nonceLen)])
            let got = Array(v[(2 + Self.nonceLen)..<(2 + Self.nonceLen + Self.macLen)])
            guard Self.equal(got, Self.mac(key, device: true, clientNonce, dn)) else {
                outcome = .mismatch   /* the gadget doesn't hold this key: no confirm */
                return true
            }
            awaitingResult = true
            retryAtMs = -1
            lastMs = nowMs
            send([Self.cmd, Self.confirm] + Self.mac(key, device: false, clientNonce, dn))
        } else if v[1] == Self.result && awaitingResult {
            outcome = v.count >= 3 && v[2] == 1 ? .match : .mismatch
        }
        return true
    }

    /* Call every 100 ms or so while pending: busy retries and timeouts. */
    func tick(nowMs: Int64) {
        guard pending else { return }
        if nowMs - firstMs > Self.totalMs || (retryAtMs < 0 && nowMs - lastMs > Self.silenceMs) {
            outcome = .timeout
        } else if retryAtMs >= 0 && nowMs >= retryAtMs {
            sendChallenge(nowMs)
        }
    }

    /* The code in an error's mg_error_data_t list ([error, command, (type, value)...]), -1 if none. */
    static func errorCode(_ v: [UInt8]) -> Int {
        var i = 2
        while i < v.count {
            switch v[i] {
            case 1:   /* code: uint16 */
                return i + 2 < v.count ? Int(v[i + 1]) | Int(v[i + 2]) << 8 : -1
            case 2:   /* log: uint16 size, then the string */
                guard i + 2 < v.count else { return -1 }
                i += 3 + (Int(v[i + 1]) | Int(v[i + 2]) << 8)
            default:
                return -1
            }
        }
        return -1
    }

    private static func equal(_ a: [UInt8], _ b: [UInt8]) -> Bool {
        guard a.count == b.count else { return false }
        var d: UInt8 = 0
        for i in 0..<a.count {
            d |= a[i] ^ b[i]
        }
        return d == 0
    }

    /* A 64-hex-digit key, as mg_ble_client keeps it (proof_key). */
    static func parseKey(_ s: String) -> [UInt8]? {
        let h = s.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard h.count == 64, h.allSatisfy(\.isHexDigit) else { return nil }
        var out: [UInt8] = []
        var i = h.startIndex
        while i < h.endIndex {
            let j = h.index(i, offsetBy: 2)
            guard let b = UInt8(h[i..<j], radix: 16) else { return nil }
            out.append(b)
            i = j
        }
        return out
    }

    /* The first 4 bytes of SHA-256(K), to tell keys apart in logs and the UI without showing one. */
    static func fingerprint(_ k: [UInt8]) -> String {
        SHA256.hash(data: k).prefix(4).map { String(format: "%02x", $0) }.joined()
    }
}
