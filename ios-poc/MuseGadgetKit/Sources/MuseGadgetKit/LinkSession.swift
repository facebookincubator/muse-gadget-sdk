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
import Foundation

/// `/link-control` messages are JSON, each prefixed with a u32-LE length.
public enum LinkMessage {
    public static let maxInbound = 4 * 1024 * 1024

    public static func encode(_ json: Data) -> Data {
        var length = UInt32(json.count).littleEndian
        return Data(bytes: &length, count: 4) + json
    }

    public static func encode(_ object: [String: Any]) throws -> Data {
        encode(try CompactJSON.data(object))
    }
}

public struct LinkMessageDecoder {
    private var buffer = Data()
    public init() {}

    public mutating func feed(_ data: Data) throws -> [[String: Any]] {
        buffer.append(data)
        var messages: [[String: Any]] = []
        while buffer.count >= 4 {
            let b = [UInt8](buffer.prefix(4))
            let length = Int(UInt32(b[0]) | UInt32(b[1]) << 8 | UInt32(b[2]) << 16 | UInt32(b[3]) << 24)
            guard length <= LinkMessage.maxInbound else { throw NoiseError("inbound message too large") }
            guard buffer.count >= 4 + length else { break }
            let raw = buffer.subdata(in: buffer.startIndex + 4 ..< buffer.startIndex + 4 + length)
            buffer = Data(buffer.dropFirst(4 + length))
            if raw.isEmpty { continue }  // keepalive
            if let message = CompactJSON.object(raw) { messages.append(message) }
        }
        return messages
    }
}

/// What the gadget tells the Muse about itself in `link.register`.
public struct DeviceDescription {
    public var nodeID: String
    public var displayName: String
    public var version: String
    /// `commands_v2`: `{name: {description, required: {p: {type, description}}, optional: {...}, timeout_ms?}}`.
    public var commands: [String: Any]
    /// The Linux SDK registers `linux`/`homehub`/`linux`. Never use family
    /// `link`: the server pushes ESP32 firmware updates to every `link` device.
    public var platform = "linux"
    public var deviceFamily = "homehub"
    public var modelID = "linux"

    public init(nodeID: String, displayName: String, version: String, commands: [String: Any]) {
        self.nodeID = nodeID
        self.displayName = displayName
        self.version = version
        self.commands = commands
    }

    public var registerParams: [String: Any] {
        [
            "node_id": nodeID,
            "display_name": displayName,
            "platform": platform,
            "version": version,
            "device_family": deviceFamily,
            "model_id": modelID,
            "is_wakeup_supported": false,
            "commands_v2": commands,
        ]
    }
}

/// Minimal binary WebSocket, so tests can substitute a fake VM.
public protocol GadgetWebSocket: AnyObject {
    func send(_ data: Data) async throws
    func receive() async throws -> Data
    func close()
}

public struct UpgradeRejected: Error { public let status: Int }

/// `URLSessionWebSocketTask` with the VM bearer in the upgrade request.
public final class URLSessionGadgetWebSocket: GadgetWebSocket {
    private let task: URLSessionWebSocketTask
    private var pingTimer: Task<Void, Never>?

    public init(url: URL, bearer: String, userAgent: String, session: URLSession = .shared) {
        var request = URLRequest(url: url, timeoutInterval: 20)
        request.setValue("Bearer \(bearer)", forHTTPHeaderField: "Authorization")
        request.setValue(userAgent, forHTTPHeaderField: "User-Agent")
        task = session.webSocketTask(with: request)
        task.maximumMessageSize = 16 * 1024 * 1024
        task.resume()
        pingTimer = Task { [task] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 20_000_000_000)
                task.sendPing { _ in }
            }
        }
    }

    public func send(_ data: Data) async throws {
        do { try await task.send(.data(data)) } catch { throw mapped(error) }
    }

    public func receive() async throws -> Data {
        do {
            switch try await task.receive() {
            case .data(let data): return data
            case .string: throw NoiseError("text frame on Noise connection")
            @unknown default: throw NoiseError("unknown frame")
            }
        } catch { throw mapped(error) }
    }

    public func close() {
        pingTimer?.cancel()
        task.cancel(with: .normalClosure, reason: nil)
    }

    /// A refused upgrade surfaces as a failed task whose response carries the status.
    private func mapped(_ error: Error) -> Error {
        if let status = (task.response as? HTTPURLResponse)?.statusCode, status == 401 || status == 403 {
            return UpgradeRejected(status: status)
        }
        return error
    }
}

/// One control session with a Muse VM (`linux/src/musegadget/link_client.py`).
public final class LinkSession {
    public enum Outcome: Equatable { case closed, authRejected, forbidden, unpaired, stopped }

    public typealias CommandHandler = (_ command: String, _ params: [String: Any]) async -> [String: Any]

    public static let noisePath = "/v1/noise"

    private let socket: GadgetWebSocket
    private let device: DeviceDescription
    private let runCommand: CommandHandler
    private let makeInitiator: () -> NoiseXXInitiator
    private let sendLock = AsyncLock()
    private var sendFailed = false  // guarded by sendLock
    var transport: NoiseTransport!
    private var controlStream: Int64 = 0
    private var registerID = ""
    private var pending: [Int64: PendingRequest] = [:]
    private var requestsClosed = false  // guarded by pendingLock
    private let pendingLock = NSLock()
    private var registeredAtValue: Date?  // guarded by pendingLock
    public var registeredAt: Date? { withPendingLock { registeredAtValue } }
    /// How long `sendChat` waits for the whole response (the Linux client's 60 s).
    public var requestTimeout: TimeInterval = 60
    static let maxResponseBytes = 1024 * 1024
    public var log: (String) -> Void = { _ in }

    public static func noiseURL(host: String, vmID: String) -> URL {
        // encodeURIComponent, as the firmware does.
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-_.!~*'()")
        let escaped = vmID.addingPercentEncoding(withAllowedCharacters: allowed) ?? vmID
        return URL(string: "wss://\(host)\(noisePath)?vm_id=\(escaped)")!
    }

    public init(socket: GadgetWebSocket, device: DeviceDescription,
                makeInitiator: @escaping () -> NoiseXXInitiator = { NoiseXXInitiator() },
                runCommand: @escaping CommandHandler) {
        self.socket = socket
        self.device = device
        self.makeInitiator = makeInitiator
        self.runCommand = runCommand
    }

    public func run() async -> Outcome {
        defer {
            socket.close()
            failPending()
        }
        do {
            try await handshake()
            try await openControlStream()
            return try await readLoop()
        } catch let rejected as UpgradeRejected {
            log("VM refused connection: HTTP \(rejected.status)")
            return rejected.status == 401 ? .authRejected : .forbidden
        } catch is CancellationError {
            return .stopped
        } catch {
            log("session ended: \(error)")
            return Task.isCancelled ? .stopped : .closed
        }
    }

    func handshake() async throws {
        let initiator = makeInitiator()
        try await socket.send(try initiator.writeMessage1())
        try initiator.readMessage2(try await socket.receive())
        // The bearer already authenticated us at the upgrade; message 3 has an empty payload.
        try await socket.send(try initiator.writeMessage3())
        let (send, receive) = try initiator.split()
        transport = NoiseTransport(send: send, receive: receive)
        log("Noise session established")
    }

    func openControlStream(registerID: String = UUID().uuidString.lowercased()) async throws {
        controlStream = try await sendEncrypted { try transport.request("POST", "/link-control", endBody: false) }
        self.registerID = registerID
        try await send(["type": "req", "id": registerID, "method": "link.register",
                        "params": device.registerParams])
        log("sent link.register as \(device.nodeID)")
    }

    public func send(_ message: [String: Any]) async throws {
        let chunk = try LinkMessage.encode(message)
        try await sendEncrypted { ((), try transport.bodyChunk(streamID: controlStream, chunk)) }
    }

    /// Posts a user message to the Muse as coming from this device.
    public func sendChat(_ message: String, sessionID: String? = nil) async throws -> (status: Int, body: Data) {
        var body: [String: Any] = ["message": message, "output_modality": "text", "device_id": device.nodeID]
        if let sessionID { body["session_id"] = sessionID }
        let json = try CompactJSON.data(body)
        let headers = [NoiseHeader("Content-Type", "application/json"),
                       NoiseHeader("x-request-id", UUID().uuidString.lowercased()),
                       NoiseHeader("x-app-id", "musegadget")]
        return try await withCheckedThrowingContinuation { continuation in
            Task {
                var registered = false
                do {
                    let id: Int64 = try await sendEncrypted {
                        let (id, frames) = try transport.request("POST", "/chat/stream", headers: headers,
                                                                 body: json, endBody: true)
                        // Registered before sending, so the reply can't arrive first.
                        try register(id, continuation)
                        registered = true
                        return (id, frames)
                    }
                    let timeout = requestTimeout
                    Task { [weak self] in
                        try? await Task.sleep(nanoseconds: UInt64(timeout * 1_000_000_000))
                        await self?.abandon(id, NoiseError("request timed out"))
                    }
                } catch {
                    // Once registered, a failed send has already failed every pending request.
                    if !registered { continuation.resume(throwing: error) }
                }
            }
        }
    }

    /// Encryption advances the Noise nonce, so it runs under the send lock:
    /// ciphertext then reaches the socket in nonce order. A failure after
    /// that leaves delivery ambiguous, so the session is closed rather than
    /// reusing the cipher (`link_client.py` `_send_encrypted`).
    @discardableResult
    private func sendEncrypted<T>(_ encrypt: () throws -> (T, [Data])) async throws -> T {
        await sendLock.lock()
        guard !sendFailed else {
            await sendLock.unlock()
            throw NoiseError("session closed")
        }
        do {
            let (value, frames) = try encrypt()
            for frame in frames { try await socket.send(frame) }
            await sendLock.unlock()
            return value
        } catch {
            sendFailed = true
            await sendLock.unlock()
            socket.close()
            failPending()
            throw error
        }
    }

    private func readLoop() async throws -> Outcome {
        var decoder = LinkMessageDecoder()
        while true {
            try Task.checkCancellation()
            let raw = try await socket.receive()
            guard let (stream, frame) = try transport.decrypt(raw) else { continue }
            if stream != controlStream {
                onRequestFrame(stream, frame)
                continue
            }
            if case let .reset(_, reason) = frame {
                log("control stream reset: \(reason)")
                return .closed
            }
            if case let .response(status, _, _, _) = frame, status >= 400 {
                log("/link-control refused: HTTP \(status)")
                return status == 403 ? .forbidden : .closed
            }
            guard let (data, ended) = frame.data else { continue }
            for message in try decoder.feed(data) {
                if let outcome = handle(message) { return outcome }
            }
            if ended { return .closed }
        }
    }

    /// Ends the session from outside; `run` then returns.
    public func close() { socket.close() }

    func handle(_ message: [String: Any]) -> Outcome? {
        if message["id"] as? String == registerID, message["method"] == nil {
            // Rejected either way, as in link_client.py: fetch the VM again.
            if message["error"] != nil || message["ok"] as? Bool == false {
                log("link.register rejected")
                return .forbidden
            } else {
                withPendingLock { registeredAtValue = Date() }
                log("registered with the Muse")
                Task { try? await send(["method": "link.heartbeat"]) }
            }
            return nil
        }
        if let event = message["event"] as? String, event == "link.unpaired" || event == "node.unpaired" {
            return .unpaired
        }
        if message["method"] as? String == "link.invoke", let id = message["id"] as? String {
            let command = message["command"] as? String ?? ""
            let params = message["params"] as? [String: Any] ?? [:]
            log("invoke \(command)")
            Task {
                var result = await runCommand(command, params)
                result["method"] = "link.result"
                result["id"] = id
                try? await send(result)
            }
        }
        return nil
    }

    // MARK: - Request streams (e.g. /chat/stream)

    private struct PendingRequest {
        let continuation: CheckedContinuation<(status: Int, body: Data), Error>
        var status = 0
        var body = Data()
    }

    private func register(_ stream: Int64, _ continuation: CheckedContinuation<(status: Int, body: Data), Error>) throws {
        pendingLock.lock(); defer { pendingLock.unlock() }
        guard !requestsClosed else { throw NoiseError("session ended") }
        pending[stream] = PendingRequest(continuation: continuation)
    }

    /// Fails a request that is still waiting, and resets its stream.
    private func abandon(_ stream: Int64, _ error: Error) async {
        guard let request = takePending(stream) else { return }
        request.continuation.resume(throwing: error)
        _ = try? await sendEncrypted { ((), try transport.reset(streamID: stream, reason: "request ended")) }
    }

    private func onRequestFrame(_ stream: Int64, _ frame: InboundFrame) {
        pendingLock.lock()
        guard var request = pending[stream] else { pendingLock.unlock(); return }
        if case let .response(status, _, _, _) = frame { request.status = status }
        let chunk = frame.data
        if let (data, _) = chunk, request.body.count + data.count > Self.maxResponseBytes {
            pendingLock.unlock()
            Task { await abandon(stream, NoiseError("response too large")) }
            return
        }
        if let (data, _) = chunk { request.body.append(data) }
        pending[stream] = request
        pendingLock.unlock()
        if case let .reset(_, reason) = frame {
            complete(stream, .failure(NoiseError("stream reset: \(reason)")))
        } else if chunk?.1 == true {
            complete(stream, .success((request.status, request.body)))
        }
    }

    private func complete(_ stream: Int64, _ result: Result<(status: Int, body: Data), Error>) {
        takePending(stream)?.continuation.resume(with: result)
    }

    private func withPendingLock<T>(_ body: () -> T) -> T {
        pendingLock.lock(); defer { pendingLock.unlock() }
        return body()
    }

    private func takePending(_ stream: Int64) -> PendingRequest? {
        pendingLock.lock(); defer { pendingLock.unlock() }
        return pending.removeValue(forKey: stream)
    }

    private func failPending() {
        pendingLock.lock()
        requestsClosed = true
        let all = pending
        pending.removeAll()
        pendingLock.unlock()
        for request in all.values { request.continuation.resume(throwing: NoiseError("session ended")) }
    }
}

/// FIFO async mutex for `sendEncrypted`.
actor AsyncLock {
    private var locked = false
    private var waiters: [CheckedContinuation<Void, Never>] = []

    func lock() async {
        if !locked {
            locked = true
            return
        }
        await withCheckedContinuation { waiters.append($0) }
    }

    func unlock() {
        if waiters.isEmpty {
            locked = false
        } else {
            waiters.removeFirst().resume()
        }
    }
}
