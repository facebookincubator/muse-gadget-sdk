import Foundation

struct HealthReviewRow: Identifiable {
    let id: String
    let title: String
    let detail: String
}

enum HealthReview {
    static func coveredSeconds(_ intervals: [DateInterval]) -> TimeInterval {
        let sorted = intervals.filter { $0.duration.isFinite && $0.duration > 0 }.sorted { $0.start < $1.start }
        guard var current = sorted.first else { return 0 }
        var total: TimeInterval = 0
        for next in sorted.dropFirst() {
            if next.start <= current.end {
                current = DateInterval(start: current.start, end: max(current.end, next.end))
            } else { total += current.duration; current = next }
        }
        return total + current.duration
    }

    static func number(_ value: Double) -> String {
        String(format: "%.4g", locale: Locale(identifier: "en_US_POSIX"), value)
    }

    static func quantity(values: [Double], unit: String, cumulative: Bool) -> String? {
        // HealthKit represents percentages as fractions (0.97 means 97%).
        let valid = values.filter(\.isFinite).map { unit == "%" ? $0 * 100 : $0 }.filter(\.isFinite)
        guard let latest = valid.first, let low = valid.min(), let high = valid.max() else { return nil }
        // Do not sum overlapping records from multiple apps or interpret sampled increments as daily totals.
        return "latest \(number(latest)) \(unit); sample range \(number(low))–\(number(high)) \(unit)" +
            (cumulative ? "; individual recorded increments, NOT daily totals" : "; intermittent measurements")
    }

    static func prompt(rows: [HealthReviewRow], days: Int, generatedAt: Date, context: String) -> String? {
        guard !rows.isEmpty else { return nil }
        let formatter = ISO8601DateFormatter()
        var result = """
        Give me a conversational health check-in using the Apple Health summary below. You are an AI health-information assistant, not my clinician; do not imply that this is a medical appointment or an examination.
        Report generated \(formatter.string(from: generatedAt)). Review window: previous \(days) days ending then. Timestamps are UTC. Up to the latest 128 records per type were examined; capped types may cover only part of the window. This is NOT my complete medical record or continuous monitoring. Multiple sources may overlap. Do not add record counts or durations into daily totals, infer trends from min/max, assume heart rates were at rest, or pair separate blood-pressure samples. Missing types can mean no records, unavailable permission, or watch limitations—not normal results.
        First briefly explain the most useful observations with their dates and units, distinguish observations from possible explanations, and acknowledge uncertainty and missing context. Ask one focused follow-up question, then continue the check-in when I answer. Suggest useful questions to bring to a licensed clinician. Do not diagnose, rule out disease, prescribe, or change medications. Historical symptom entries do not establish symptoms now. If I describe current severe symptoms, advise urgent medical help without waiting for more watch readings. Use plain speech, no tables, and keep each response under about 180 words for spoken playback.
        My context (self-reported): \(String(context.prefix(1500)).isEmpty ? "Not provided; current symptoms, history and medications are unknown." : String(context.prefix(1500)))
        Selected health summaries:

        """
        var omitted = 0
        for row in rows {
            let line = "\(row.title): \(row.detail)\n"
            if result.utf8.count + line.utf8.count < 30500 { result += line }
            else { omitted += 1 }
        }
        if omitted > 0 { result += "\(omitted) selected summaries omitted because of the message size limit.\n" }
        return result
    }
}
