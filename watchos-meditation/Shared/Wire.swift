import Foundation

enum MuseWire {
    // This is the watch relay service, separate from Meta's enrollment service.
    static let service = "D7581200-24B5-457E-9D30-55987A68CDA2"
    static let rx = "D7581201-24B5-457E-9D30-55987A68CDA2"
    static let tx = "D7581202-24B5-457E-9D30-55987A68CDA2"
    static let maximumFrame = 256 * 1024

    static func encode(_ value: [String: Any]) throws -> Data {
        var data = try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
        guard data.count <= maximumFrame else { throw WireError.tooLarge }
        data.append(10)
        return data
    }

    static func command(_ value: [String: Any]) throws -> [String: Any] {
        switch value["op"] as? String {
        case "check", "cancel": return ["op": value["op"]!]
        case "watch_result":
            guard let id = value["request_id"] as? String,
                  UUID(uuidString: id)?.uuidString.lowercased() == id,
                  let result = value["result"] as? [String: Any], result["ok"] is Bool,
                  JSONSerialization.isValidJSONObject(result),
                  try JSONSerialization.data(withJSONObject: result).count <= 16384 else { throw WireError.invalidCommand }
            return ["op": "watch_result", "request_id": id, "result": result]
        case "chat":
            guard let message = value["message"] as? String,
                  !message.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
                  message.utf8.count <= 32000,
                  let session = value["session_id"] as? String,
                  UUID(uuidString: session)?.uuidString.lowercased() == session else {
                throw WireError.invalidCommand
            }
            return ["op": "chat", "message": message, "session_id": session]
        default: throw WireError.invalidCommand
        }
    }
}

enum WireError: Error { case tooLarge, invalidJSON, invalidCommand }

struct FrameDecoder {
    private var pending = Data()
    mutating func reset() { pending.removeAll(keepingCapacity: false) }
    mutating func feed(_ data: Data) throws -> [[String: Any]] {
        pending.append(data)
        var result: [[String: Any]] = []
        while let newline = pending.firstIndex(of: 10) {
            let line = Data(pending[..<newline])
            pending.removeSubrange(...newline)
            guard line.count <= MuseWire.maximumFrame else { reset(); throw WireError.tooLarge }
            if line.isEmpty { continue }
            guard let value = try JSONSerialization.jsonObject(with: line) as? [String: Any] else {
                reset(); throw WireError.invalidJSON
            }
            result.append(value)
        }
        guard pending.count <= MuseWire.maximumFrame else { reset(); throw WireError.tooLarge }
        return result
    }
    var hasPartialFrame: Bool { !pending.isEmpty }
}

struct ChatLine: Identifiable {
    let id: String
    let isUser: Bool
    var text: String
    var complete = false
}

struct Conversation {
    var lines: [ChatLine] = []
    var busy = false
    var error: String?
    var status = "Connect to your Mac"
    private(set) var currentReplyIDs: [String] = []

    mutating func begin(_ text: String) {
        error = nil; busy = true; status = "Sending…"; currentReplyIDs = []
        lines.append(ChatLine(id: UUID().uuidString, isUser: true, text: text, complete: true))
        if lines.count > 100 { lines.removeFirst(lines.count - 100) }
    }

    // SDK reply events are full snapshots, not deltas. The speech buffer commits sentences separately.
    mutating func receive(_ event: [String: Any]) -> String? {
        switch event["type"] as? String {
        case "ack": status = "Muse is thinking…"
        case "status": status = (event["busy"] as? Bool == true) ? "Muse is working…" : "Finishing…"
        case "reply":
            guard busy, let id = event["message_id"] as? String, let text = event["text"] as? String else { return nil }
            if !currentReplyIDs.contains(id) { currentReplyIDs.append(id) }
            if let index = lines.firstIndex(where: { !$0.isUser && $0.id == id }) {
                lines[index].text = text
                lines[index].complete = event["complete"] as? Bool ?? false
            } else {
                lines.append(ChatLine(id: id, isUser: false, text: text, complete: event["complete"] as? Bool ?? false))
            }
            status = "Muse is replying…"
        case "error", "command_error":
            error = event["error"] as? String ?? "The conversation failed."
            status = "Needs attention"
            if event["type"] as? String == "command_error" { busy = false }
        case "turn_finished":
            guard busy else { return nil }
            busy = false; status = error == nil ? "Ready" : "Needs attention"
            guard error == nil else { return nil }
            return lines.filter { currentReplyIDs.contains($0.id) && !$0.isUser }.map(\.text).joined(separator: "\n")
        default: break
        }
        return nil
    }

    mutating func disconnected(_ reason: String) {
        if busy { error = "Connection lost. Your message may already have reached Muse; it was not resent." }
        busy = false; status = reason
    }
}
