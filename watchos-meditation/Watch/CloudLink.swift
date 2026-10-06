import Foundation
import Security

struct CloudCredentials: Codable {
    let address: String
    let token: String
    private static let service = "local.jt.muse.cloud"

    static func load() -> CloudCredentials? {
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service, kSecAttrAccount as String: "connection",
            kSecReturnData as String: true, kSecMatchLimit as String: kSecMatchLimitOne]
        var result: CFTypeRef?
        if SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
           let data = result as? Data, let saved = try? JSONDecoder().decode(Self.self, from: data) { return saved }
        // Optional private development provisioning file; never committed or deployed to Vercel.
        if let url = Bundle.main.url(forResource: "CloudBootstrap", withExtension: "json"),
           let data = try? Data(contentsOf: url), let initial = try? JSONDecoder().decode(Self.self, from: data),
           (try? initial.save()) != nil { return initial }
        return nil
    }

    func save() throws {
        _ = try CloudProtocol.endpoint(address)
        guard token.utf8.count >= 32, token.utf8.count <= 256,
              !token.contains(where: { $0.isWhitespace }) else { throw WireError.invalidCommand }
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: Self.service, kSecAttrAccount as String: "connection"]
        let values: [String: Any] = [kSecValueData as String: try JSONEncoder().encode(self),
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly]
        let status = SecItemUpdate(query as CFDictionary, values as CFDictionary)
        let result = status == errSecItemNotFound ? SecItemAdd(query.merging(values) { _, new in new } as CFDictionary, nil) : status
        guard result == errSecSuccess else {
            throw NSError(domain: NSOSStatusErrorDomain, code: Int(result), userInfo: [NSLocalizedDescriptionKey: "Could not save the Muse connection key."])
        }
    }
}

// Ordinary HTTPS data tasks let watchOS use the paired iPhone's internet proxy.
// No sockets/WebSockets, companion app, APNs, or certificate exceptions on watch.
final class CloudLink: NSObject, URLSessionDataDelegate {
    var onEvent: (([String: Any]) -> Void)?
    var onFailure: ((String) -> Void)?
    private var active: URLSessionDataTask?
    private var decoder = FrameDecoder()
    private var httpStatus = 0
    private var errorBody = Data()
    private var finished = false
    private var generation = UUID()
    private var session: URLSession!

    override init() {
        super.init()
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 105
        configuration.timeoutIntervalForResource = 115
        configuration.urlCache = nil
        configuration.httpCookieStorage = nil
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        session = URLSession(configuration: configuration, delegate: self, delegateQueue: .main)
    }

    func send(_ command: [String: Any]) throws {
        if command["op"] as? String == "cancel" {
            disconnect()
            let canceledGeneration = generation
            DispatchQueue.main.async { [weak self] in
                guard let self, self.generation == canceledGeneration else { return }
                self.onEvent?(["type": "error", "error": "Reply wait canceled. Muse may already have accepted the message."])
                self.onEvent?(["type": "turn_finished"])
            }
            return
        }
        guard active == nil else { throw failure("Wait for the current Muse request to finish.") }
        guard let credentials = CloudCredentials.load() else { throw failure("Set up the cloud connection first.") }
        var request = URLRequest(url: try CloudProtocol.endpoint(credentials.address))
        request.httpMethod = "POST"
        request.setValue("Bearer " + credentials.token, forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("application/x-ndjson", forHTTPHeaderField: "Accept")
        request.httpBody = try CloudProtocol.request(command)
        decoder.reset(); httpStatus = 0; errorBody = Data(); finished = false
        generation = UUID()
        active = session.dataTask(with: request)
        active?.resume()
    }

    func disconnect() {
        generation = UUID()
        let task = active; active = nil; task?.cancel()
        decoder.reset(); errorBody = Data(); finished = false
    }

    func urlSession(_ session: URLSession, task: URLSessionTask,
                    willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest,
                    completionHandler: @escaping (URLRequest?) -> Void) {
        // Never forward the watch credential to a redirected host or login page.
        completionHandler(nil)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                    completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        guard dataTask === active, let response = response as? HTTPURLResponse else { completionHandler(.cancel); return }
        httpStatus = response.statusCode
        if httpStatus == 200 && response.mimeType != "application/x-ndjson" {
            completionHandler(.cancel); fail("The server did not return a Muse stream. Check the backend address."); return
        }
        completionHandler(.allow)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        guard dataTask === active else { return }
        if httpStatus != 200 {
            if errorBody.count + data.count <= 8192 { errorBody.append(data) }
            else { fail("The cloud server rejected the request (HTTP \(httpStatus)).") }
            return
        }
        do {
            for event in try decoder.feed(data) {
                guard !finished else { throw WireError.invalidJSON }
                if event["type"] as? String == "turn_finished" { finished = true }
                else { onEvent?(event) }
            }
        } catch { fail("The Muse stream was invalid or exceeded its size limit. No message was resent.") }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard task === active else { return }
        active = nil
        if let error { fail("Connection interrupted: \(error.localizedDescription) Your message was not resent."); return }
        guard httpStatus == 200 else {
            let value = (try? JSONSerialization.jsonObject(with: errorBody)) as? [String: Any]
            fail(value?["error"] as? String ?? "Cloud request failed (HTTP \(httpStatus))."); return
        }
        guard finished, !decoder.hasPartialFrame else { fail("Muse disconnected before the reply finished. No message was resent."); return }
        onEvent?(["type": "turn_finished"])
    }

    private func fail(_ message: String) { disconnect(); onFailure?(message) }
    private func failure(_ message: String) -> NSError {
        NSError(domain: "Muse", code: 1, userInfo: [NSLocalizedDescriptionKey: message])
    }
}
