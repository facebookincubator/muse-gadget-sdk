import Foundation

struct HeartRateReading: Identifiable {
    let id: UUID
    let date: Date
    let bpm: Double
}

enum HeartRateSummary {
    static func prompt(readings: [HeartRateReading], context: String, now: Date = Date()) -> String? {
        let valid = readings.filter { $0.bpm.isFinite && $0.bpm > 0 && $0.date <= now && $0.date >= now.addingTimeInterval(-24 * 3600) }
            .sorted { $0.date < $1.date }.suffix(12)
        guard !valid.isEmpty else { return nil }
        let date = ISO8601DateFormatter()
        let rows = valid.map { "\(date.string(from: $0.date)): \(String(format: "%.0f", $0.bpm)) bpm" }.joined(separator: "\n")
        let suppliedContext = context.trimmingCharacters(in: .whitespacesAndNewlines)
        return """
        Please help me understand these recent heart-rate samples from Apple Health.
        Report generated at \(date.string(from: now)); timestamps below are UTC.
        These are up to 12 intermittent samples from the past 24 hours, not a continuous recording or a live measurement.
        \(rows)
        My context: \(suppliedContext.isEmpty ? "Not provided. Activity, symptoms, medications, and measurement conditions are unknown." : suppliedContext)
        Explain the observed values and general patterns briefly in plain language. Distinguish observations from possibilities. Do not assume these are resting measurements or infer a diagnosis, arrhythmia, fitness score, or absence of illness from sparse heart-rate data. Ask for missing context when it matters. Do not give medication or treatment changes. If I report severe current symptoms, recommend urgent medical help rather than relying on these samples. Keep the reply concise enough to hear on a watch.
        """
    }
}
