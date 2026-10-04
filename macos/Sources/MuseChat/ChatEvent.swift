// Copyright (c) Meta Platforms, Inc. and affiliates.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
import Foundation

public struct ChatEvent: Decodable {
    public let type: String?
    public let messageID: String?
    public let text: String?
    public let complete: Bool?
    public let busy: Bool?
    public let error: String?
    public let ok: Bool?
    public let paired: Bool?
    public let online: Bool?
    public let pairing: Bool?
    public let bleName: String?
    public let sdkTokenSaved: Bool?

    enum CodingKeys: String, CodingKey {
        case type, text, complete, busy, error, ok, paired, online, pairing
        case bleName = "ble_name"
        case sdkTokenSaved = "sdk_token_saved"
        case messageID = "message_id"
    }
}

/// Buffer bytes, not String fragments: a pipe read can split a UTF-8 character.
public struct EventDecoder {
    private var pending = Data()
    public init() {}
    public mutating func feed(_ data: Data) throws -> [ChatEvent] {
        pending.append(data)
        var events: [ChatEvent] = []
        while let newline = pending.firstIndex(of: 10) {
            let line = Data(pending[..<newline])
            pending.removeSubrange(...newline)
            guard line.count <= 8 * 1024 * 1024 else { throw DecodeError.tooLarge }
            if !line.isEmpty { events.append(try JSONDecoder().decode(ChatEvent.self, from: line)) }
        }
        guard pending.count <= 8 * 1024 * 1024 else { throw DecodeError.tooLarge }
        return events
    }
    public func finish() throws {
        if !pending.isEmpty { throw DecodeError.truncated }
    }
    public enum DecodeError: Error { case tooLarge, truncated }
}
