import Foundation

// Hand-rolled protobuf for the Hatch Noise envelopes
// (`linux/src/musegadget/noise/{_proto,framing,envelope}.py`). Fields are
// written in the same order, and only when non-default, so the bytes match
// the Python and firmware encoders exactly.

struct ProtoWriter {
    private(set) var data = Data()

    mutating func varint(_ value: UInt64) {
        var v = value
        while v >= 0x80 {
            data.append(UInt8(v & 0x7F) | 0x80)
            v >>= 7
        }
        data.append(UInt8(v))
    }

    mutating func key(_ field: Int, _ wireType: Int) { varint(UInt64(field << 3 | wireType)) }
    mutating func uint(_ field: Int, _ value: UInt64) { key(field, 0); varint(value) }
    mutating func int64(_ field: Int, _ value: Int64) { uint(field, UInt64(bitPattern: value)) }
    mutating func int32(_ field: Int, _ value: Int32) { uint(field, UInt64(bitPattern: Int64(value))) }
    mutating func bool(_ field: Int) { uint(field, 1) }
    mutating func bytes(_ field: Int, _ value: Data) { key(field, 2); varint(UInt64(value.count)); data.append(value) }
    mutating func string(_ field: Int, _ value: String) { bytes(field, Data(value.utf8)) }
}

struct ProtoReader {
    enum Value { case varint(UInt64), bytes(Data), skipped }

    private let bytes: [UInt8]
    private var offset = 0

    init(_ data: Data) { bytes = [UInt8](data) }

    var atEnd: Bool { offset >= bytes.count }

    mutating func next() throws -> (field: Int, value: Value) {
        let key = try readVarint()
        let field = Int(key >> 3), wire = Int(key & 7)
        guard field != 0, field <= (1 << 29) - 1, !(19000...19999).contains(field) else {
            throw NoiseError("invalid field number")
        }
        switch wire {
        case 0: return (field, .varint(try readVarint()))
        case 1: try skip(8); return (field, .skipped)
        case 2:
            let length = Int(try readVarint())
            guard length <= bytes.count - offset else { throw NoiseError("truncated delimited field") }
            defer { offset += length }
            return (field, .bytes(Data(bytes[offset..<offset + length])))
        case 5: try skip(4); return (field, .skipped)
        default: throw NoiseError("invalid wire type")
        }
    }

    private mutating func skip(_ n: Int) throws {
        guard n <= bytes.count - offset else { throw NoiseError("truncated fixed field") }
        offset += n
    }

    private mutating func readVarint() throws -> UInt64 {
        var value: UInt64 = 0
        for i in 0..<10 {
            guard offset < bytes.count else { throw NoiseError("truncated varint") }
            let byte = bytes[offset]
            offset += 1
            if i == 9 && byte & 0xFE != 0 { throw NoiseError("malformed varint") }
            value |= UInt64(byte & 0x7F) << (7 * i)
            if byte & 0x80 == 0 { return value }
        }
        throw NoiseError("malformed varint")
    }
}

public struct NoiseHeader: Equatable {
    public var key: String
    public var value: String
    public init(_ key: String, _ value: String) { self.key = key; self.value = value }

    func encoded() -> Data {
        var w = ProtoWriter()
        if !key.isEmpty { w.string(1, key) }
        if !value.isEmpty { w.string(2, value) }
        return w.data
    }
}

public enum ServiceType: UInt64 {
    case daemon = 0, sentinel = 1, vault = 2, authd = 3
}

public enum ResetCode: Int32 {
    case unspecified = 0, cancelled, timeout, protocolError, refusedStream, internalError, serviceUnavailable
}

/// A device-to-VM frame (`ServiceFrame` with request, body_chunk or reset).
public enum OutboundFrame {
    case request(verb: String, path: String, headers: [NoiseHeader], body: Data, endBody: Bool)
    case bodyChunk(data: Data, endBody: Bool)
    case reset(code: ResetCode, reason: String)

    func encoded(streamID: Int64) -> Data {
        var w = ProtoWriter()
        if streamID != 0 { w.int64(1, streamID) }
        var inner = ProtoWriter()
        switch self {
        case let .request(verb, path, headers, body, endBody):
            if !verb.isEmpty { inner.string(1, verb) }
            if !path.isEmpty { inner.string(2, path) }
            for header in headers { inner.bytes(3, header.encoded()) }
            if !body.isEmpty { inner.bytes(4, body) }
            if endBody { inner.bool(5) }
            w.bytes(2, inner.data)
        case let .bodyChunk(data, endBody):
            if !data.isEmpty { inner.bytes(1, data) }
            if endBody { inner.bool(2) }
            w.bytes(4, inner.data)
        case let .reset(code, reason):
            if code != .unspecified { inner.int32(1, code.rawValue) }
            if !reason.isEmpty { inner.string(2, reason) }
            w.bytes(5, inner.data)
        }
        return w.data
    }
}

/// A VM-to-device frame.
public enum InboundFrame: Equatable {
    case response(status: Int, headers: [NoiseHeader], body: Data, endBody: Bool)
    case bodyChunk(data: Data, endBody: Bool)
    case reset(code: Int32, reason: String)

    /// Body bytes and end flag for response and body_chunk frames.
    public var data: (Data, Bool)? {
        switch self {
        case let .response(_, _, body, end): return (body, end)
        case let .bodyChunk(data, end): return (data, end)
        case .reset: return nil
        }
    }
}

enum Envelope {
    /// `ServiceRequest{service, payload: ServiceFrame}`.
    static func encodeRequest(service: ServiceType, streamID: Int64, frame: OutboundFrame) -> Data {
        var w = ProtoWriter()
        if service != .daemon { w.uint(1, service.rawValue) }
        let payload = frame.encoded(streamID: streamID)
        if !payload.isEmpty { w.bytes(2, payload) }
        return w.data
    }

    /// `ServiceResponse{payload: ServiceFrame}` -> stream id and frame.
    static func decodeResponse(_ data: Data) throws -> (Int64, InboundFrame?) {
        var payload = Data()
        var r = ProtoReader(data)
        while !r.atEnd {
            let (field, value) = try r.next()
            if field == 1, case let .bytes(b) = value { payload = b }
        }
        guard !payload.isEmpty else { throw NoiseError("empty ServiceResponse payload") }

        var streamID: Int64 = 0
        var frame: InboundFrame?
        var fr = ProtoReader(payload)
        while !fr.atEnd {
            let (field, value) = try fr.next()
            switch (field, value) {
            case let (1, .varint(v)): streamID = Int64(bitPattern: v)
            case (2, .bytes): throw NoiseError("unexpected request frame from server")
            case let (3, .bytes(b)): frame = try decodeApplicationResponse(b)
            case let (4, .bytes(b)): frame = try decodeBodyChunk(b)
            case let (5, .bytes(b)): frame = try decodeReset(b)
            default: break
            }
        }
        return (streamID, frame)
    }

    private static func decodeApplicationResponse(_ data: Data) throws -> InboundFrame {
        var status = 0, headers: [NoiseHeader] = [], body = Data(), end = false
        var r = ProtoReader(data)
        while !r.atEnd {
            switch try r.next() {
            case let (1, .varint(v)): status = Int(Int32(truncatingIfNeeded: Int64(bitPattern: v)))
            case let (2, .bytes(b)): headers.append(try decodeHeader(b))
            case let (3, .bytes(b)): body = b
            case let (4, .varint(v)): end = v != 0
            default: break
            }
        }
        return .response(status: status, headers: headers, body: body, endBody: end)
    }

    private static func decodeHeader(_ data: Data) throws -> NoiseHeader {
        var key = "", value = ""
        var r = ProtoReader(data)
        while !r.atEnd {
            switch try r.next() {
            case let (1, .bytes(b)): key = String(decoding: b, as: UTF8.self)
            case let (2, .bytes(b)): value = String(decoding: b, as: UTF8.self)
            default: break
            }
        }
        return NoiseHeader(key, value)
    }

    private static func decodeBodyChunk(_ data: Data) throws -> InboundFrame {
        var chunk = Data(), end = false
        var r = ProtoReader(data)
        while !r.atEnd {
            switch try r.next() {
            case let (1, .bytes(b)): chunk = b
            case let (2, .varint(v)): end = v != 0
            default: break
            }
        }
        return .bodyChunk(data: chunk, endBody: end)
    }

    private static func decodeReset(_ data: Data) throws -> InboundFrame {
        var code: Int32 = 0, reason = ""
        var r = ProtoReader(data)
        while !r.atEnd {
            switch try r.next() {
            case let (1, .varint(v)): code = Int32(truncatingIfNeeded: Int64(bitPattern: v))
            case let (2, .bytes(b)): reason = String(decoding: b, as: UTF8.self)
            default: break
            }
        }
        return .reset(code: code, reason: reason)
    }
}

/// `NoiseTransportFrame{chunk_id, chunk_index, total_chunks, payload}`.
enum NoiseFraming {
    static let maxChunkPayload = 65489
    static let maxTotalChunks = 256
    static let maxPendingAssemblies = 16
    static let maxAssemblyBytes = 16 * 1024 * 1024

    static func encode(_ payload: Data, chunkID: Int64) throws -> [Data] {
        let total = max(1, (payload.count + maxChunkPayload - 1) / maxChunkPayload)
        guard total <= maxTotalChunks else { throw NoiseError("payload too large for noise framing") }
        return (0..<total).map { index in
            let chunk = payload.isEmpty ? Data()
                : payload.subdata(in: (payload.startIndex + index * maxChunkPayload)..<min(
                    payload.startIndex + (index + 1) * maxChunkPayload, payload.endIndex))
            var w = ProtoWriter()
            if chunkID != 0 { w.int64(1, chunkID) }
            if index != 0 { w.uint(2, UInt64(index)) }
            w.uint(3, UInt64(total))
            if !chunk.isEmpty { w.bytes(4, chunk) }
            return w.data
        }
    }
}

struct NoiseFrameDecoder {
    private struct Assembly {
        var chunks: [Int: Data] = [:]
        let total: Int
        var bytes = 0
        let created: Date
    }

    private var pending: [Int64: Assembly] = [:]

    mutating func decode(_ frame: Data) throws -> Data? {
        var chunkID: Int64 = 0, index = 0, total = 1, payload = Data()
        var r = ProtoReader(frame)
        while !r.atEnd {
            switch try r.next() {
            case let (1, .varint(v)): chunkID = Int64(bitPattern: v)
            case let (2, .varint(v)): index = Int(truncatingIfNeeded: v)
            case let (3, .varint(v)): total = Int(truncatingIfNeeded: v)
            case let (4, .bytes(b)): payload = b
            default: break
            }
        }
        guard (1...NoiseFraming.maxTotalChunks).contains(total), (0..<total).contains(index),
              payload.count <= NoiseFraming.maxChunkPayload
        else { throw NoiseError("invalid noise frame") }

        let now = Date()
        pending = pending.filter { now.timeIntervalSince($0.value.created) <= 60 }
        var assembly = pending[chunkID] ?? {
            Assembly(total: total, created: now)
        }()
        if pending[chunkID] == nil, pending.count >= NoiseFraming.maxPendingAssemblies {
            throw NoiseError("too many pending noise frame assemblies")
        }
        guard assembly.total == total, assembly.chunks[index] == nil else {
            pending[chunkID] = nil
            throw NoiseError("inconsistent or duplicate noise chunk")
        }
        assembly.bytes += payload.count
        guard assembly.bytes <= NoiseFraming.maxAssemblyBytes else {
            pending[chunkID] = nil
            throw NoiseError("assembly exceeded byte budget")
        }
        assembly.chunks[index] = payload
        guard assembly.chunks.count == total else {
            pending[chunkID] = assembly
            return nil
        }
        pending[chunkID] = nil
        return (0..<total).reduce(into: Data()) { $0.append(assembly.chunks[$1]!) }
    }
}

/// Encrypted HTTP-like streams over a split Noise session
/// (`linux/src/musegadget/noise/transport.py`).
public final class NoiseTransport {
    private let send: NoiseCipherState
    private let receive: NoiseCipherState
    private var decoder = NoiseFrameDecoder()
    private var nextStreamID: Int64 = 1
    private let chunkID: () -> Int64
    private let lock = NSLock()

    public init(send: NoiseCipherState, receive: NoiseCipherState,
                chunkID: @escaping () -> Int64 = { Int64.random(in: .min ... .max) }) {
        self.send = send
        self.receive = receive
        self.chunkID = chunkID
    }

    /// Opens a request stream; returns its id and the WebSocket frames to send.
    public func request(_ verb: String, _ path: String, headers: [NoiseHeader] = [],
                        body: Data = Data(), endBody: Bool,
                        service: ServiceType = .daemon) throws -> (streamID: Int64, frames: [Data]) {
        lock.lock(); defer { lock.unlock() }
        let id = nextStreamID
        nextStreamID += 1
        let frames = try encrypt(service, id, .request(verb: verb, path: path, headers: headers,
                                                       body: body, endBody: endBody))
        return (id, frames)
    }

    public func bodyChunk(streamID: Int64, _ data: Data, endBody: Bool = false,
                          service: ServiceType = .daemon) throws -> [Data] {
        lock.lock(); defer { lock.unlock() }
        return try encrypt(service, streamID, .bodyChunk(data: data, endBody: endBody))
    }

    public func reset(streamID: Int64, reason: String = "", code: ResetCode = .cancelled) throws -> [Data] {
        lock.lock(); defer { lock.unlock() }
        return try encrypt(.daemon, streamID, .reset(code: code, reason: reason))
    }

    /// Decrypts one WebSocket binary frame; nil until a chunked frame completes.
    public func decrypt(_ ciphertext: Data) throws -> (streamID: Int64, frame: InboundFrame)? {
        let plain = try receive.decrypt(ad: Data(), ciphertext: ciphertext)
        guard let whole = try decoder.decode(plain) else { return nil }
        let (id, frame) = try Envelope.decodeResponse(whole)
        guard let frame else { return nil }
        return (id, frame)
    }

    private func encrypt(_ service: ServiceType, _ streamID: Int64, _ frame: OutboundFrame) throws -> [Data] {
        let request = Envelope.encodeRequest(service: service, streamID: streamID, frame: frame)
        return try NoiseFraming.encode(request, chunkID: chunkID()).map {
            try send.encrypt(ad: Data(), plaintext: $0)
        }
    }
}
