import Foundation

enum CalmScene: String, CaseIterable, Identifiable {
    case mix, brush, fabric, rain, leaves, wood, water
    var id: String { rawValue }
    var title: String {
        switch self {
        case .mix: return "ASMR mix"
        case .brush: return "Soft brushing"
        case .fabric: return "Silk & cotton"
        case .rain: return "Gentle rain"
        case .leaves: return "Rustling leaves"
        case .wood: return "Soft wooden taps"
        case .water: return "Quiet water"
        }
    }
    func segment(_ index: Int) -> CalmScene {
        guard self == .mix else { return self }
        let sequence: [CalmScene] = [.brush, .rain, .fabric, .leaves, .water, .wood]
        return sequence[max(0, index) % sequence.count]
    }
}

struct CalmBufferPolicy {
    static let maximumSoundClips = 3
    static let maximumVoiceClips = 3
    static let crossfadeSeconds = 4.0
    static let maximumDownloadBytes: Int64 = 20 * 1024 * 1024
    static let cacheLimitBytes: Int64 = 1024 * 1024 * 1024
}


struct CalmAudioRetry {
    private(set) var attempts = 0
    mutating func reset() { attempts = 0 }
    mutating func nextDelay(status: Int, retryAfter: Double? = nil) -> Double? {
        guard [408, 409, 429, 500, 502, 503, 504, -1001, -1005, -1009, -1004].contains(status),
              attempts < 3 else { return nil }
        let delays = [5.0, 15.0, 30.0]
        let minimum = delays[attempts]; attempts += 1
        guard let retryAfter, retryAfter.isFinite, retryAfter > 0 else { return minimum }
        return max(minimum, retryAfter)
    }
}
