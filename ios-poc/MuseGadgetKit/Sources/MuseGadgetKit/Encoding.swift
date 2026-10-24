import Foundation

/// Unpadded base64url, with the same strictness as the firmware and
/// `musegadget.pairing.b64url_decode`.
public enum Base64URL {
    public static func encode<D: DataProtocol>(_ data: D) -> String {
        Data(data).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }

    public static func decode(_ value: Any?, maxChars: Int = 4096) -> Data? {
        guard let text = value as? String, !text.isEmpty, text.count <= maxChars,
              text.count % 4 != 1,
              text.utf8.allSatisfy({ isBase64URLByte($0) })
        else { return nil }
        var standard = text
            .replacingOccurrences(of: "-", with: "+")
            .replacingOccurrences(of: "_", with: "/")
        standard += String(repeating: "=", count: (4 - standard.count % 4) % 4)
        return Data(base64Encoded: standard)
    }

    private static func isBase64URLByte(_ b: UInt8) -> Bool {
        (b >= 0x41 && b <= 0x5A) || (b >= 0x61 && b <= 0x7A) || (b >= 0x30 && b <= 0x39)
            || b == 0x2D || b == 0x5F
    }
}

/// Compact JSON, matching Python's `json.dumps(obj, separators=(",", ":"))`
/// closely enough for the apps and the VM (key order is not significant).
public enum CompactJSON {
    public static func data(_ object: [String: Any]) throws -> Data {
        try JSONSerialization.data(withJSONObject: object, options: [.withoutEscapingSlashes])
    }

    public static func string(_ object: [String: Any]) throws -> String {
        String(decoding: try data(object), as: UTF8.self)
    }

    public static func object(_ data: Data) -> [String: Any]? {
        (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
    }
}

extension Data {
    public init?(hex: String) {
        guard hex.count % 2 == 0 else { return nil }
        var bytes = [UInt8]()
        bytes.reserveCapacity(hex.count / 2)
        var index = hex.startIndex
        while index < hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            guard let byte = UInt8(hex[index..<next], radix: 16) else { return nil }
            bytes.append(byte)
            index = next
        }
        self.init(bytes)
    }

    public var hex: String { map { String(format: "%02x", $0) }.joined() }
}
