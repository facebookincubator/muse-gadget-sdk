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

/// Executes an argument array directly, without a shell or credentials.
private final class DockerChat {
    private let process = Process()
    private let errors = Pipe()
    private let lock = NSLock()
    private var diagnostics = Data()

    static var executable: URL? {
        ["/usr/local/bin/docker", "/opt/homebrew/bin/docker",
         "/Applications/Docker.app/Contents/Resources/bin/docker"]
            .first(where: { FileManager.default.isExecutableFile(atPath: $0) }).map(URL.init(fileURLWithPath:))
    }

    func start(container: String, session: String, prompt: String,
               event: @escaping (ChatEvent) -> Void, finished: @escaping (String?) -> Void) throws {
        guard let docker = Self.executable else {
            throw NSError(domain: "Muse", code: 1, userInfo: [NSLocalizedDescriptionKey: "Install and start Docker Desktop to connect to Muse."])
        }
        process.executableURL = docker
        process.arguments = ["exec", "-i", container, "musegadget", "chat", "--json", "--session-id", session, "-"]
        let input = Pipe(), output = Pipe()
        process.standardInput = input; process.standardOutput = output; process.standardError = errors
        errors.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard let self = self, !data.isEmpty else { return }
            self.lock.lock()
            self.diagnostics.append(data.prefix(max(0, 16384 - self.diagnostics.count)))
            self.lock.unlock()
        }
        try process.run()
        try input.fileHandleForWriting.write(contentsOf: Data(prompt.utf8))
        try input.fileHandleForWriting.close()
        DispatchQueue.global(qos: .userInitiated).async { [self] in
            var decoder = EventDecoder()
            var failure: String?
            var receivedDone = false
            do {
                while true {
                    let data = output.fileHandleForReading.availableData
                    if data.isEmpty { break }
                    for update in try decoder.feed(data) {
                        if update.type == "done" { receivedDone = true }
                        if update.type == "error" || update.ok == false { failure = update.error ?? "Muse could not answer." }
                        DispatchQueue.main.async { event(update) }
                    }
                }
                try decoder.finish()
            } catch {
                failure = "Could not read Muse's response: \(error.localizedDescription)"
                if process.isRunning { process.terminate() }
            }
            process.waitUntilExit()
            errors.fileHandleForReading.readabilityHandler = nil
            lock.lock(); let details = String(data: diagnostics, encoding: .utf8) ?? ""; lock.unlock()
            if failure == nil && (!receivedDone || process.terminationStatus != 0) {
                failure = details.trimmingCharacters(in: .whitespacesAndNewlines)
                if failure?.isEmpty != false { failure = "The connection ended before Muse finished. Check that the paired gadget is online." }
            }
            let result = failure
            DispatchQueue.main.async { finished(result) }
        }
    }
    func terminate() { if process.isRunning { process.terminate() } }
}

@MainActor
final class ChatModel: ObservableObject {
    @Published var messages: [ChatMessage] = []
    @Published var prompt = ""
    @Published var busy = false
    @Published var answering = false
    @Published var status = "Checking connection…"
    @Published var online = false
    @Published var error: String?
    @Published var container: String {
        didSet { UserDefaults.standard.set(container, forKey: "container") }
    }
    private var session: String
    private var active: DockerChat?
    private var checking = false
    private var probed = false

    init() {
        container = UserDefaults.standard.string(forKey: "container") ?? "muse-gadget-linux"
        session = UserDefaults.standard.string(forKey: "chatSession") ?? UUID().uuidString.lowercased()
        UserDefaults.standard.set(session, forKey: "chatSession")
    }
    var validContainer: Bool {
        container.range(of: "^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$", options: .regularExpression) != nil
    }
    var canSend: Bool { !busy && validContainer && !prompt.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }

    func send() {
        guard canSend else { return }
        let text = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        guard text.utf8.count <= 32000 else { error = "Please keep your prompt under 32 KB."; return }
        error = nil; busy = true; status = "Connecting…"
        messages.append(ChatMessage(id: UUID().uuidString, user: true, text: text, complete: true))
        prompt = ""
        let chat = DockerChat(); active = chat
        do {
            try chat.start(container: container, session: session, prompt: text, event: { [weak self] event in
                self?.receive(event)
            }, finished: { [weak self] failure in
                guard let self else { return }
                self.active = nil; self.busy = false; self.answering = false
                if let failure { self.error = failure; self.status = "Connection needs attention" }
                else { self.online = true; self.status = "Ready to chat" }
                DebugProbe.reply(self.messages, failure: failure)
            })
        } catch {
            active?.terminate(); active = nil; busy = false
            self.error = error.localizedDescription; status = "Connection needs attention"
        }
    }
    func runDebugProbe() {
        #if DEBUG
        guard !probed else { return }
        probed = true
        if let text = ProcessInfo.processInfo.environment["MUSE_TEST_PROMPT"] {
            newChat(); prompt = text; send()
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { DebugProbe.snapshot() }
        #endif
    }
    private func receive(_ event: ChatEvent) {
        switch event.type {
        case "ack": online = true; status = "Muse is thinking…"
        case "status": status = event.busy == true ? "Muse is working…" : "Finishing the reply…"
        case "reply":
            guard let id = event.messageID, let text = event.text else { return }
            status = "Muse is replying…"
            if !text.isEmpty { answering = true }
            if let index = messages.firstIndex(where: { $0.id == id && !$0.user }) {
                messages[index].text = text; messages[index].complete = event.complete ?? false
            } else { messages.append(ChatMessage(id: id, user: false, text: text, complete: event.complete ?? false)) }
        case "error": error = event.error
        default: break
        }
    }
    func newChat() {
        guard !busy else { return }
        session = UUID().uuidString.lowercased()
        UserDefaults.standard.set(session, forKey: "chatSession")
        messages = []; error = nil
    }
    func checkConnection() {
        guard !checking && !busy else { return }
        guard validContainer, let docker = DockerChat.executable else {
            online = false; status = "Start Docker Desktop and the paired gadget"; return
        }
        checking = true
        let name = container
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let process = Process(), output = Pipe()
            process.executableURL = docker
            // Check both the running container and presence of the reply-streaming CLI.
            process.arguments = ["exec", name, "musegadget", "chat", "--help"]
            process.standardOutput = output; process.standardError = output
            var available = false
            do {
                try process.run()
                _ = output.fileHandleForReading.readDataToEndOfFile()
                process.waitUntilExit(); available = process.terminationStatus == 0
            } catch { }
            let result = available
            DispatchQueue.main.async {
                guard let self else { return }
                self.checking = false; self.online = result
                if !self.busy && self.error == nil {
                    self.status = result ? "Gadget running • ready to connect" : "Start or update the paired gadget"
                }
            }
        }
    }
}
