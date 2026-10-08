import Foundation

/// Muse device API (`linux/src/musegadget/muse_api.py`).
public struct MuseAPI {
    public static let defaultBase = "https://api.muse.ai"
    public static let defaultNoiseHost = "hatch.metaaivm.com"

    public struct VM: Equatable {
        public let url: String
        public let authToken: String
        public let name: String
        public let id: String
        public let isDefault: Bool
    }

    public let root: String
    public let session: URLSession
    public let userAgent: String

    public init(apiURLv2: String = "", session: URLSession = .shared,
                userAgent: String = "musegadget-ios/0.1.0 (iPhone)") {
        root = (apiURLv2.isEmpty ? Self.defaultBase : apiURLv2)
            .trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        self.session = session
        self.userAgent = userAgent
    }

    /// Leased VMs for the device token, plus the HTTP status (nil = transport failure).
    public func fetchVMs(accessToken: String) async -> (vms: [VM], status: Int?) {
        var request = URLRequest(url: URL(string: root + "/fetch_vms")!, timeoutInterval: 15)
        request.setValue("Bearer \(accessToken)", forHTTPHeaderField: "Authorization")
        request.setValue("1.0.0", forHTTPHeaderField: "X-API-Version")
        request.setValue(userAgent, forHTTPHeaderField: "User-Agent")
        guard let (data, response) = try? await session.data(for: request),
              let status = (response as? HTTPURLResponse)?.statusCode
        else { return ([], nil) }
        guard (200..<300).contains(status), let body = CompactJSON.object(data),
              body["error_title"] == nil, body["backend_error_code"] == nil,
              let list = body["vm_list"] as? [[String: Any]]
        else { return ([], status) }
        let vms = list.compactMap { entry -> VM? in
            guard let url = (entry["vm_ws_url"] ?? entry["vm_url"]) as? String, !url.isEmpty,
                  let token = entry["vm_auth_token"] as? String, !token.isEmpty
            else { return nil }
            return VM(url: url, authToken: token, name: entry["vm_name"] as? String ?? "",
                      id: entry["vm_id"] as? String ?? "", isDefault: entry["default"] as? Bool ?? false)
        }
        return (vms, status)
    }

    /// Rotates the device token pair. Never presents the access token.
    public func refreshDeviceToken(refreshToken: String, nodeID: String, sdkToken: String?) async
        -> (tokens: (access: String, refresh: String)?, status: Int?)
    {
        // Apps hand over refresh tokens that may already carry the prefix.
        let raw = refreshToken.split(separator: ":").last.map(String.init) ?? refreshToken
        var body: [String: Any] = ["device_id": nodeID]
        if let sdkToken { body["sdk_token"] = sdkToken }
        var request = URLRequest(url: URL(string: root + "/device_token/refresh")!, timeoutInterval: 15)
        request.httpMethod = "POST"
        request.httpBody = try? CompactJSON.data(body)
        request.setValue("Bearer hatch_refresh:\(raw)", forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue(userAgent, forHTTPHeaderField: "User-Agent")
        guard let (data, response) = try? await session.data(for: request),
              let status = (response as? HTTPURLResponse)?.statusCode
        else { return (nil, nil) }
        guard (200..<300).contains(status), var object = CompactJSON.object(data) else { return (nil, status) }
        if let payload = object["payload"] as? [String: Any] { object = payload }
        guard let access = object["access_token"] as? String, !access.isEmpty,
              let refresh = object["refresh_token"] as? String, !refresh.isEmpty
        else { return (nil, status) }
        return ((access, refresh), status)
    }
}
