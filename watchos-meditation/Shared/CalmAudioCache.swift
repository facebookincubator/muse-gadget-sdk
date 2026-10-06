import Foundation

struct CalmSavedAudio: Codable {
    let id: String
    let duration: Double
    let label: String
    let scene: String?
    let voice: String?
    let text: String?
    let savedAt: Date
    var lastStarted: Date?
    var isSound: Bool { text == nil && voice == nil }
}

// Only relative UUID filenames are persisted, so app updates keep the same library.
final class CalmAudioCache {
    let directory: URL
    private(set) var entries: [CalmSavedAudio] = []
    private var indexURL: URL { directory.appendingPathComponent("index.json") }

    init(directory: URL) {
        self.directory = directory
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        if let data = try? Data(contentsOf: indexURL),
           let saved = try? JSONDecoder().decode([CalmSavedAudio].self, from: data) {
            var ids = Set<String>()
            entries = saved.filter { Self.valid($0) && ids.insert($0.id).inserted }
        }
        entries.removeAll { !exists($0) }
    }

    func file(_ id: String) -> URL { directory.appendingPathComponent(id + ".wav") }
    func contains(_ id: String) -> Bool { entries.contains { $0.id == id } }

    func save(_ entry: CalmSavedAudio) {
        guard Self.valid(entry), exists(entry) else { return }
        entries.removeAll { $0.id == entry.id }
        entries.append(entry); persist()
    }

    func starters(scene: CalmScene? = nil, voice: String? = nil, limit: Int) -> [CalmSavedAudio] {
        let candidates = entries.filter { entry in
            guard exists(entry) else { return false }
            if let scene { return entry.isSound && (scene == .mix || entry.scene == scene.rawValue) }
            if let voice { return !entry.isSound && entry.voice == voice }
            return false
        }.sorted {
            // Prefer recordings not previously used to open a session; rotate the rest.
            let lhs = $0.lastStarted ?? .distantPast, rhs = $1.lastStarted ?? .distantPast
            return lhs == rhs ? $0.savedAt > $1.savedAt : lhs < rhs
        }
        return Array(candidates.prefix(max(0, limit)))
    }

    func markStarted(_ id: String) {
        guard let index = entries.firstIndex(where: { $0.id == id }) else { return }
        entries[index].lastStarted = Date(); persist()
    }

    func remove(_ id: String) {
        guard UUID(uuidString: id)?.uuidString.lowercased() == id else { return }
        try? FileManager.default.removeItem(at: file(id))
        entries.removeAll { $0.id == id }; persist()
    }

    func trim(to limit: Int64) {
        entries.removeAll { !exists($0) }
        let files = ((try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: [.fileSizeKey, .contentModificationDateKey])) ?? [])
            .filter { $0.pathExtension == "wav" }
            .sorted { modification($0) < modification($1) }
        var bytes = files.reduce(Int64(0)) { $0 + size($1) }
        // Protect the newest recording for each known scene/voice before pruning history.
        var protected = Set<String>(), groups = Set<String>()
        for entry in entries.sorted(by: { $0.savedAt > $1.savedAt }) {
            let group = entry.isSound ? "sound:" + (entry.scene ?? "mix") : "voice:" + (entry.voice ?? "")
            if groups.insert(group).inserted { protected.insert(entry.id) }
        }
        let ordered = files.filter { !protected.contains($0.deletingPathExtension().lastPathComponent) }
            + files.filter { protected.contains($0.deletingPathExtension().lastPathComponent) }
        for url in ordered where bytes > limit {
            let length = size(url)
            if (try? FileManager.default.removeItem(at: url)) != nil {
                bytes -= length
                entries.removeAll { $0.id == url.deletingPathExtension().lastPathComponent }
            }
        }
        persist()
    }

    private static func valid(_ entry: CalmSavedAudio) -> Bool {
        UUID(uuidString: entry.id)?.uuidString.lowercased() == entry.id &&
        entry.duration.isFinite && (1...90).contains(entry.duration) && !entry.label.isEmpty &&
        (entry.isSound || (entry.text != nil && ["af_heart", "af_nicole"].contains(entry.voice ?? "")))
    }
    private func exists(_ entry: CalmSavedAudio) -> Bool { size(file(entry.id)) > 44 }
    private func size(_ url: URL) -> Int64 { Int64((try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0) }
    private func modification(_ url: URL) -> Date { (try? url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast }
    private func persist() {
        if let data = try? JSONEncoder().encode(entries) { try? data.write(to: indexURL, options: .atomic) }
    }
}
