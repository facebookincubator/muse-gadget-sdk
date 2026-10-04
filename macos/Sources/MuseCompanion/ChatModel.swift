// Copyright (c) Meta Platforms, Inc. and affiliates.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
import Foundation
import SwiftUI
import MuseChat

struct ChatMessage: Identifiable {
    let id: String
    let user: Bool
    var text: String
    var complete: Bool = false
}

/// The SDK and CoreBluetooth run locally over private stdio, without a shell.
private final class DirectConnection {
    let process = Process()
    private let input = Pipe(), output = Pipe(), errors = Pipe()

    func start(event: @escaping (ChatEvent) -> Void, finished: @escaping (String) -> Void) throws {
        guard let resources = Bundle.main.resourceURL else { throw failure("Rebuild the app to install its local runtime.") }
        let runtime = resources.appendingPathComponent("Runtime")
        let python = runtime.appendingPathComponent("python/bin/python3")
        guard FileManager.default.isExecutableFile(atPath: python.path) else {
            throw failure("The local Python runtime is missing. Run macos/build.sh to rebuild Muse Companion.")
        }
        let state = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Muse Companion/Device", isDirectory: true)
        process.executableURL = python
        process.arguments = ["-B", "-u", runtime.appendingPathComponent("native_backend.py").path, "--state-dir", state.path]
        var environment = ProcessInfo.processInfo.environment
        environment.removeValue(forKey: "MUSEGADGET_SDK_TOKEN")
        process.environment = environment
        process.standardInput = input; process.standardOutput = output; process.standardError = errors
        errors.fileHandleForReading.readabilityHandler = { handle in _ = handle.availableData }
        try process.run()
        DispatchQueue.global(qos: .userInitiated).async { [self] in
            var decoder = EventDecoder()
            var message = "Muse connection stopped. Click reconnect to try again."
            do {
                while true {
                    let data = output.fileHandleForReading.availableData
                    if data.isEmpty { break }
                    for update in try decoder.feed(data) { DispatchQueue.main.async { event(update) } }
                }
                try decoder.finish()
            } catch {
                message = "Could not read the local Muse connection: \(error.localizedDescription)"
                stop()
            }
            process.waitUntilExit()
            errors.fileHandleForReading.readabilityHandler = nil
            let result = message
            DispatchQueue.main.async { finished(result) }
        }
    }
    func send(_ request: [String: Any]) throws {
        guard process.isRunning else { throw failure("Reconnect to Muse first.") }
        var data = try JSONSerialization.data(withJSONObject: request); data.append(10)
        try input.fileHandleForWriting.write(contentsOf: data)
    }
    func stop() {
        if process.isRunning { process.terminate() }
        try? input.fileHandleForWriting.close()
    }
    private func failure(_ text: String) -> NSError {
        NSError(domain: "Muse", code: 1, userInfo: [NSLocalizedDescriptionKey: text])
    }
}

@MainActor
final class ChatModel: ObservableObject {
    @Published var messages: [ChatMessage] = []
    @Published var prompt = ""
    @Published var busy = false
    @Published var answering = false
    @Published var status = "Connecting on this Mac…"
    @Published var online = false
    @Published var paired = false
    @Published var pairing = false
    @Published var pairingStatus = "Pair this Mac with your Muse phone app."
    @Published var deviceName = "MuseGadget"
    @Published var sdkToken = ""
    @Published var sdkTokenSaved = false
    @Published var showPairing = false
    @Published var error: String?
    private var session: String
    private var connection: DirectConnection?
    private var probed = false
    private var checkedPairing = false

    init() {
        session = UserDefaults.standard.string(forKey: "chatSession") ?? UUID().uuidString.lowercased()
        UserDefaults.standard.set(session, forKey: "chatSession")
    }
    var canSend: Bool { online && !busy && !pairing && !prompt.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
    func checkConnection() {
        if let connection { try? connection.send(["op": "check"]); return }
        let client = DirectConnection(); connection = client
        do {
            try client.start(event: { [weak self, weak client] event in
                guard let self, self.connection === client else { return }; self.receive(event)
            }, finished: { [weak self, weak client] failure in
                guard let self, self.connection === client else { return }
                self.connection = nil; self.busy = false; self.answering = false; self.online = false; self.pairing = false
                self.error = failure; self.status = "Connection needs attention"
            })
        } catch { connection = nil; self.error = error.localizedDescription; status = "Connection needs attention" }
    }
    func pair() {
        error = nil; checkConnection()
        guard let connection else { return }
        do {
            try connection.send(["op": "pair", "sdk_token": sdkToken])
            sdkToken = ""; pairing = true; pairingStatus = "Opening Bluetooth pairing…"
        } catch { self.error = error.localizedDescription; pairing = false }
    }
    func cancelPairing() { try? connection?.send(["op": "cancel_pair"]) }
    func stop() { connection?.stop(); connection = nil }
    func cancel() { try? connection?.send(["op": "cancel"]) }
    func send() {
        guard canSend, let connection else { return }
        let text = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        guard text.utf8.count <= 32000 else { error = "Please keep your prompt under 32 KB."; return }
        error = nil; busy = true; status = "Connecting…"
        messages.append(ChatMessage(id: UUID().uuidString, user: true, text: text, complete: true)); prompt = ""
        do { try connection.send(["op": "chat", "message": text, "session_id": session]) }
        catch { self.error = error.localizedDescription; busy = false; status = "Connection needs attention" }
    }
    private func receive(_ event: ChatEvent) {
        switch event.type {
        case "connection":
            paired = event.paired ?? false; online = event.online ?? false; pairing = event.pairing ?? false
            deviceName = event.bleName ?? deviceName
            sdkTokenSaved = event.sdkTokenSaved ?? false
            if !busy { status = online ? "Connected directly • ready to chat" : pairing ? "Pairing with your phone…" : paired ? "Connecting to your Muse…" : "Pair this Mac to start chatting" }
            if !checkedPairing {
                checkedPairing = true; showPairing = !paired
                if !paired && sdkTokenSaved && CommandLine.arguments.contains("--pair") { pair() }
            }
        case "pairing_status": pairingStatus = event.text ?? "Pairing…"
        case "paired": paired = true; pairing = false; showPairing = false; status = "Connecting to your Muse…"
        case "pairing_error": pairing = false; error = event.error; pairingStatus = event.error ?? "Pairing did not finish."
        case "command_error":
            error = event.error; busy = false; answering = false
            if !paired { pairing = false }
        case "ack": online = true; status = "Muse is thinking…"
        case "status": status = event.busy == true ? "Muse is working…" : "Finishing the reply…"
        case "reply":
            guard let id = event.messageID, let text = event.text else { return }
            status = "Muse is replying…"; answering = !text.isEmpty
            if let index = messages.firstIndex(where: { $0.id == id && !$0.user }) {
                messages[index].text = text; messages[index].complete = event.complete ?? false
            } else { messages.append(ChatMessage(id: id, user: false, text: text, complete: event.complete ?? false)) }
        case "error": error = event.error
        case "turn_finished":
            busy = false; answering = false; status = error == nil ? "Ready to chat" : "Connection needs attention"
            DebugProbe.reply(messages, failure: error)
        default: break
        }
    }
    func newChat() {
        guard !busy else { return }
        session = UUID().uuidString.lowercased(); UserDefaults.standard.set(session, forKey: "chatSession")
        messages = []; error = nil
    }
    func runDebugProbe() {
        #if DEBUG
        guard !probed else { return }; probed = true
        if let text = ProcessInfo.processInfo.environment["MUSE_TEST_PROMPT"] { newChat(); prompt = text; send() }
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { DebugProbe.snapshot() }
        #endif
    }
}
