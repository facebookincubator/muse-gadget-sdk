import Foundation

enum CloudProtocol {
    static func endpoint(_ address: String) throws -> URL {
        guard let parts = URLComponents(string: address.trimmingCharacters(in: .whitespacesAndNewlines)),
              parts.scheme == "https", let host = parts.host, !host.isEmpty,
              parts.user == nil, parts.password == nil, parts.query == nil, parts.fragment == nil,
              parts.path.isEmpty || parts.path == "/", let url = parts.url else {
            throw NSError(domain: "Muse", code: 1, userInfo: [NSLocalizedDescriptionKey: "Enter the HTTPS address of your Muse backend, without a path."])
        }
        return url.appendingPathComponent("v1/muse")
    }

    static func request(_ command: [String: Any]) throws -> Data {
        var value = try MuseWire.command(command)
        guard ["check", "chat"].contains(value["op"] as? String ?? "") else { throw WireError.invalidCommand }
        if value["op"] as? String == "chat", let news = command["walking_news"] {
            guard let enabled = news as? Bool else { throw WireError.invalidCommand }
            value["walking_news"] = enabled
        }
        value["request_id"] = UUID().uuidString.lowercased()
        return try MuseWire.encode(value)
    }
}
