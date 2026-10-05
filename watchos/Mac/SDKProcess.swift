import Foundation

final class SDKProcess {
    private let process = Process()
    private let input = Pipe(), output = Pipe(), errors = Pipe()
    var onEvent: (([String: Any]) -> Void)?
    var onExit: (() -> Void)?

    func start() throws {
        let runtime = Bundle.main.resourceURL!.appendingPathComponent("Runtime")
        let python = runtime.appendingPathComponent("python/bin/python3")
        guard FileManager.default.isExecutableFile(atPath: python.path) else {
            throw NSError(domain: "Muse", code: 1, userInfo: [NSLocalizedDescriptionKey: "Build the relay with scripts/build.sh to include the Muse SDK."])
        }
        let state = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Muse Watch/Device")
        process.executableURL = python
        process.arguments = ["-B", "-u", runtime.appendingPathComponent("native_backend.py").path, "--state-dir", state.path]
        var env = ProcessInfo.processInfo.environment
        env.removeValue(forKey: "MUSEGADGET_SDK_TOKEN")
        env.removeValue(forKey: "PYTHONPATH")
        env.removeValue(forKey: "PYTHONHOME")
        process.environment = env
        process.standardInput = input; process.standardOutput = output; process.standardError = errors
        errors.fileHandleForReading.readabilityHandler = { _ = $0.availableData }
        try process.run()
        DispatchQueue.global(qos: .userInitiated).async { [self] in
            var decoder = FrameDecoder()
            do {
                while true {
                    let data = output.fileHandleForReading.availableData
                    if data.isEmpty { break }
                    for event in try decoder.feed(data) {
                        DispatchQueue.main.async { self.onEvent?(event) }
                    }
                }
            } catch {
                DispatchQueue.main.async { self.onEvent?(["type": "error", "error": "Invalid SDK response."]) }
                if process.isRunning { process.terminate() }
            }
            process.waitUntilExit()
            errors.fileHandleForReading.readabilityHandler = nil
            DispatchQueue.main.async { self.onExit?() }
        }
    }

    func send(_ command: [String: Any]) throws {
        guard process.isRunning else {
            throw NSError(domain: "Muse", code: 2, userInfo: [NSLocalizedDescriptionKey: "Restart the relay to reconnect to the Muse SDK."])
        }
        try input.fileHandleForWriting.write(contentsOf: MuseWire.encode(command))
    }
    func stop() {
        try? send(["op": "stop"])
        try? input.fileHandleForWriting.close()
    }
}
