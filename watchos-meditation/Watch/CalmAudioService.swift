import Foundation
import CryptoKit
import AVFoundation

struct GeneratedClip {
    let url: URL
    let duration: Double
    let label: String
    let id: String
    let text: String?
}

final class CalmAudioService: NSObject, URLSessionDownloadDelegate {
    private struct Pending {
        let id: String
        let label: String
        let text: String?
        let scene: String?
        let voice: String?
        let completion: (Result<GeneratedClip, Error>) -> Void
    }
    private var pending: [Int: Pending] = [:]
    private var session: URLSession!
    private let cache = CalmAudioCache(directory: FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("CalmAudioLibrary", isDirectory: true))
    private var directory: URL { cache.directory }
    override init() {
        super.init()
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 110
        config.timeoutIntervalForResource = 115
        config.urlCache = nil; config.httpCookieStorage = nil
        session = URLSession(configuration: config, delegate: self, delegateQueue: .main)
        var folder = directory
        var values = URLResourceValues(); values.isExcludedFromBackup = true
        try? folder.setResourceValues(values)
        importPreviousAudio()
    }

    func generate(kind: String, scene: CalmScene? = nil, text: String? = nil, voice: String = "af_heart", recipe: CalmSoundRecipe? = nil,
                  completion: @escaping (Result<GeneratedClip, Error>) -> Void) {
        do {
            guard let credentials = CloudCredentials.load() else { throw failure("The Muse connection isn't configured.") }
            let origin = try CloudProtocol.endpoint(credentials.address).deletingLastPathComponent().deletingLastPathComponent()
            var request = URLRequest(url: origin.appendingPathComponent("v1/calm/audio"))
            let id = UUID().uuidString.lowercased()
            var body: [String: Any] = ["kind": kind, "request_id": id]
            if let scene { body["scene"] = scene.rawValue }
            if let recipe, recipe.isValid, recipe.scene == scene?.rawValue { body["recipe"] = recipe.parameters }
            if let text { body["text"] = text; body["voice"] = voice }
            request.httpMethod = "POST"
            request.setValue("Bearer " + credentials.token, forHTTPHeaderField: "Authorization")
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.httpBody = try JSONSerialization.data(withJSONObject: body)
            let task = session.downloadTask(with: request)
            pending[task.taskIdentifier] = Pending(id: id, label: scene?.title ?? "Muse voice", text: text, scene: scene?.rawValue, voice: kind == "voice" ? voice : nil, completion: completion)
            task.resume()
        } catch { completion(.failure(error)) }
    }

    func cancelAll() {
        pending.removeAll()
        session.invalidateAndCancel()
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 110; config.timeoutIntervalForResource = 115
        config.urlCache = nil; config.httpCookieStorage = nil
        session = URLSession(configuration: config, delegate: self, delegateQueue: .main)
    }

    func trimCache() { cache.trim(to: CalmBufferPolicy.cacheLimitBytes) }

    func savedSounds(for scene: CalmScene) -> [GeneratedClip] {
        validated(cache.starters(scene: scene, limit: CalmBufferPolicy.maximumSoundClips))
    }
    func savedVoice(for voice: String) -> [GeneratedClip] {
        validated(cache.starters(voice: voice, limit: CalmBufferPolicy.maximumVoiceClips))
    }
    func markPlayed(_ clip: GeneratedClip) { cache.markStarted(clip.id) }

    private func validated(_ entries: [CalmSavedAudio]) -> [GeneratedClip] {
        entries.compactMap { entry in
            let url = cache.file(entry.id)
            guard let player = try? AVAudioPlayer(contentsOf: url), player.duration >= 1 else {
                cache.remove(entry.id); return nil
            }
            return GeneratedClip(url: url, duration: player.duration, label: entry.label, id: entry.id, text: entry.text)
        }
    }

    private func importPreviousAudio() {
        // Builds 2/3 retained WAVs but no metadata. Recover their ambient recordings
        // for ASMR mix; never guess the voice or scene of an unlabelled recording.
        let old = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("FreshCalmAudio")
        let files = (try? FileManager.default.contentsOfDirectory(at: old, includingPropertiesForKeys: nil)) ?? []
        for source in files where source.pathExtension == "wav" {
            let id = source.deletingPathExtension().lastPathComponent
            guard UUID(uuidString: id)?.uuidString.lowercased() == id,
                  let player = try? AVAudioPlayer(contentsOf: source),
                  player.numberOfChannels == 2, (30...90).contains(player.duration) else { continue }
            let duration = player.duration
            let target = cache.file(id)
            if !FileManager.default.fileExists(atPath: target.path) {
                do { try FileManager.default.moveItem(at: source, to: target) } catch { continue }
            }
            if !cache.contains(id) {
                cache.save(CalmSavedAudio(id: id, duration: duration, label: "Saved soundscape", scene: nil,
                                          voice: nil, text: nil, savedAt: Date(), lastStarted: nil))
            }
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) { completionHandler(nil) }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        guard session === self.session else { return }
        if totalBytesWritten > CalmBufferPolicy.maximumDownloadBytes {
            downloadTask.cancel()
            pending.removeValue(forKey: downloadTask.taskIdentifier)?.completion(.failure(failure("Generated audio was too large.")))
        }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        guard session === self.session else { return }
        guard let item = pending.removeValue(forKey: downloadTask.taskIdentifier) else { return }
        do {
            guard let response = downloadTask.response as? HTTPURLResponse else { throw failure("Audio download was interrupted.") }
            guard response.statusCode == 200, response.mimeType == "audio/wav" else {
                var message = "Fresh audio is unavailable. Tap Retry audio."
                if let handle = try? FileHandle(forReadingFrom: location) {
                    defer { try? handle.close() }
                    if let data = try? handle.read(upToCount: 4096),
                       let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any], let reason = json["error"] as? String { message = reason }
                }
                let delay = response.value(forHTTPHeaderField: "Retry-After").flatMap(Double.init)
                var info: [String: Any] = [NSLocalizedDescriptionKey: message]
                if let delay, delay.isFinite, delay > 0 { info["retryAfter"] = delay }
                throw NSError(domain: "MuseCalm", code: response.statusCode, userInfo: info)
            }
            let values = try location.resourceValues(forKeys: [.fileSizeKey])
            let count = Int64(values.fileSize ?? 0)
            guard count > 44, count <= CalmBufferPolicy.maximumDownloadBytes,
                  let expected = response.value(forHTTPHeaderField: "X-Audio-Sha256"),
                  let rawDuration = response.value(forHTTPHeaderField: "X-Audio-Duration"),
                  let duration = Double(rawDuration), duration.isFinite, duration > 0, duration <= 90 else { throw failure("Invalid audio response.") }
            let file = try FileHandle(forReadingFrom: location)
            defer { try? file.close() }
            var hash = SHA256()
            while let data = try file.read(upToCount: 64 * 1024), !data.isEmpty { hash.update(data: data) }
            guard hash.finalize().map({ String(format: "%02x", $0) }).joined() == expected else { throw failure("Audio download was incomplete.") }
            let destination = directory.appendingPathComponent(item.id + ".wav")
            try FileManager.default.moveItem(at: location, to: destination)
            cache.save(CalmSavedAudio(id: item.id, duration: duration, label: item.label, scene: item.scene,
                                      voice: item.voice, text: item.text, savedAt: Date(), lastStarted: nil))
            item.completion(.success(GeneratedClip(url: destination, duration: duration, label: item.label, id: item.id, text: item.text)))
        } catch { item.completion(.failure(error)) }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard session === self.session else { return }
        guard let item = pending.removeValue(forKey: task.taskIdentifier) else { return }
        item.completion(.failure(error ?? failure("Audio download did not finish.")))
    }
    private func failure(_ message: String) -> NSError { NSError(domain: "MuseCalm", code: 1, userInfo: [NSLocalizedDescriptionKey: message]) }
}
