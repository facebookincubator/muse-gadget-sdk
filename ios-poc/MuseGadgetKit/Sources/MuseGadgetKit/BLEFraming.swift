import Foundation

/// Chunked framing for setup messages larger than one BLE packet
/// (`linux/src/musegadget/ble_framing.py`).
///
/// Both directions use `0xFE`, chunk index, total chunks, then a fragment. A
/// packet that does not start with `0xFE` is a complete, unchunked message.
public enum BLEFraming {
    public static let chunkMagic: UInt8 = 0xFE
    public static let headerBytes = 3
    public static let maxPacketBytes = 160
    public static let maxChunks = 255
    public static let maxMessageBytes = 8192
    public static let defaultATTMTU = 23
    /// Delay between notifications of one message, as the Linux SDK paces them.
    public static let chunkStagger: TimeInterval = 0.05

    public struct TooLarge: Error {}

    public static func encodeChunks(_ data: Data, mtu: Int = defaultATTMTU) throws -> [Data] {
        let notifyMax = min(mtu > 3 ? mtu - 3 : 20, maxPacketBytes)
        let usable = notifyMax - headerBytes
        let bytes = [UInt8](data)
        var fragments: [ArraySlice<UInt8>] = stride(from: 0, to: bytes.count, by: usable).map {
            bytes[$0..<Swift.min($0 + usable, bytes.count)]
        }
        if fragments.isEmpty { fragments = [[]] }
        guard fragments.count <= maxChunks else { throw TooLarge() }
        let total = UInt8(fragments.count)
        return fragments.enumerated().map { index, fragment in
            Data([chunkMagic, UInt8(index), total] + fragment)
        }
    }
}

/// Reassembles chunked writes strictly in order. Index 0, or a change in the
/// total, starts a new message; anything out of order discards the buffer.
public struct ChunkAssembler {
    private var buffer = [UInt8]()
    private var total = 0
    private var next = 0
    private let maxBytes: Int

    public init(maxBytes: Int = BLEFraming.maxMessageBytes) {
        self.maxBytes = maxBytes
    }

    public mutating func reset() {
        buffer = []
        total = 0
        next = 0
    }

    /// Adds one write; returns a complete message once one is available.
    public mutating func feed(_ packet: Data) -> Data? {
        let bytes = [UInt8](packet)
        guard bytes.count >= BLEFraming.headerBytes, bytes[0] == BLEFraming.chunkMagic else {
            return packet
        }
        let index = Int(bytes[1]), packetTotal = Int(bytes[2])
        let fragment = bytes[BLEFraming.headerBytes...]
        if packetTotal == 0 {
            reset()
            return nil
        }
        if index == 0 || packetTotal != total {
            reset()
            total = packetTotal
        }
        if index != next || index >= total {
            reset()
            return nil
        }
        if buffer.count + fragment.count > maxBytes {
            reset()
            return nil
        }
        buffer += fragment
        next = index + 1
        if next < total { return nil }
        let message = Data(buffer)
        reset()
        return message
    }
}
