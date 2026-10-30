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
import XCTest
@testable import MuseGadgetKit

/// Records what reaches the socket; never receives.
final class RecordingSocket: GadgetWebSocket {
    private let lock = NSLock()
    private var sent: [Data] = []
    private var failing = false
    private(set) var closed = false

    var frames: [Data] { lock.lock(); defer { lock.unlock() }; return sent }

    func failSends() { lock.lock(); failing = true; lock.unlock() }

    func send(_ data: Data) async throws {
        await Task.yield()
        try record(data)
    }

    private func record(_ data: Data) throws {
        lock.lock(); defer { lock.unlock() }
        if failing { throw URLError(.networkConnectionLost) }
        sent.append(data)
    }

    func receive() async throws -> Data {
        try await Task.sleep(nanoseconds: 60_000_000_000)
        throw CancellationError()
    }

    func close() { lock.lock(); closed = true; lock.unlock() }
}

final class LinkSessionTests: XCTestCase {
    let key = Data(repeating: 7, count: 32)

    func makeSession(_ socket: RecordingSocket) -> LinkSession {
        let session = LinkSession(socket: socket, device: DeviceDescription(
            nodeID: "homelink-abcdef", displayName: "iPhone", version: "test", commands: [:])) { _, _ in [:] }
        session.transport = NoiseTransport(send: NoiseCipherState(key: key),
                                           receive: NoiseCipherState(key: Data(repeating: 8, count: 32)))
        return session
    }

    /// Decrypts like the VM, whose nonces are implicit: frames must arrive in encryption order.
    func assertInNonceOrder(_ frames: [Data], file: StaticString = #filePath, line: UInt = #line) {
        let vm = NoiseCipherState(key: key)
        for (index, frame) in frames.enumerated() {
            guard (try? vm.decrypt(ad: Data(), ciphertext: frame)) != nil else {
                return XCTFail("frame \(index) arrived out of nonce order", file: file, line: line)
            }
        }
    }

    func testConcurrentSendsReachTheSocketInNonceOrder() async throws {
        let socket = RecordingSocket()
        let session = makeSession(socket)
        session.requestTimeout = 0.5
        let rounds = 200
        await withTaskGroup(of: Void.self) { group in
            for i in 0..<rounds {
                group.addTask {
                    if i % 2 == 0 {
                        _ = try? await session.sendChat("chat \(i)")
                    } else {
                        try? await session.send(["method": "link.result", "id": "\(i)"])
                    }
                }
            }
        }
        // Each chat also timed out and reset its stream.
        let deadline = Date().addingTimeInterval(5)
        while socket.frames.count < rounds + rounds / 2 && Date() < deadline {
            try await Task.sleep(nanoseconds: 10_000_000)
        }
        XCTAssertEqual(socket.frames.count, rounds + rounds / 2)
        assertInNonceOrder(socket.frames)
    }

    func testChatTimesOutAndResetsItsStream() async throws {
        let socket = RecordingSocket()
        let session = makeSession(socket)
        session.requestTimeout = 0.1
        do {
            _ = try await session.sendChat("hello")
            XCTFail("expected a timeout")
        } catch {
            XCTAssertEqual("\(error)", "request timed out")
        }
        try await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertEqual(socket.frames.count, 2)  // request, then reset
        assertInNonceOrder(socket.frames)
    }

    /// After a failed send the cipher state is unknown to the VM: never reuse it.
    func testFailedSendClosesTheSession() async throws {
        let socket = RecordingSocket()
        let session = makeSession(socket)
        socket.failSends()
        do {
            try await session.send(["method": "link.heartbeat"])
            XCTFail("expected the send to fail")
        } catch {}
        XCTAssertTrue(socket.closed)
        do {
            _ = try await session.sendChat("after")
            XCTFail("expected a closed session")
        } catch {
            XCTAssertEqual("\(error)", "session closed")
        }
    }

    /// A peer-supplied length above Int.max must be rejected, not trap.
    func testOversizedProtobufLengthIsRejected() {
        var reader = ProtoReader(Data([0x0A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01]))
        XCTAssertThrowsError(try reader.next())
    }

    /// Both rejection forms end the session, as in link_client.py, so the
    /// service fetches the VM again instead of sitting on a dead session.
    func testRejectedRegistrationEndsTheSession() async throws {
        for reply: [String: Any] in [["ok": false], ["error": ["code": "denied"]]] {
            let session = makeSession(RecordingSocket())
            try await session.openControlStream(registerID: "r1")
            var message = reply
            message["id"] = "r1"
            XCTAssertEqual(session.handle(message), .forbidden)
            XCTAssertNil(session.registeredAt)
        }
        let session = makeSession(RecordingSocket())
        try await session.openControlStream(registerID: "r1")
        XCTAssertNil(session.handle(["id": "r1", "ok": true, "result": ["status": "registered"]]))
        XCTAssertNotNil(session.registeredAt)
    }
}

/// Uses a throwaway Keychain service name.
final class GadgetServiceTests: XCTestCase {
    func testOldLoopCannotRemoveOrOverwriteANewerPairing() throws {
        let store = KeychainStore(service: "musegadget-test-\(UUID().uuidString)")
        defer {
            store.delete(GadgetService.credentialsAccount)
            store.delete(GadgetService.identityAccount)
        }
        let service = GadgetService(store: store, device: { identity in
            DeviceDescription(nodeID: identity.nodeID, displayName: "test", version: "test", commands: [:])
        }, sdkToken: nil) { _, _ in [:] }
        var unpaired = 0
        service.onUnpaired = { unpaired += 1 }
        func creds(_ refresh: String) -> DeviceCredentials {
            DeviceCredentials(accessToken: "a", refreshToken: refresh, username: "u", apiURLv2: "", noiseHost: "")
        }
        try XCTSkipUnless(store.save(creds("new"), GadgetService.credentialsAccount), "no Keychain access")

        // A session from the earlier pairing ends with link.unpaired, or rotates its token.
        service.removeCredentials(ifStill: creds("old"))
        XCTAssertFalse(service.replaceCredentials(creds("old"), with: creds("old-rotated")))
        XCTAssertEqual(service.credentials?.refreshToken, "new")
        XCTAssertEqual(unpaired, 0)

        service.removeCredentials(ifStill: creds("new"))
        XCTAssertNil(service.credentials)
        XCTAssertEqual(unpaired, 1)
    }
}
