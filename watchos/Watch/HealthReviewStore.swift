import Foundation
import HealthKit
import Combine

final class HealthReviewStore: ObservableObject {
    @Published var rows: [HealthReviewRow] = []
    @Published var selected: Set<String> = []
    @Published var status = "Choose a period, then authorize the Health categories you want to review."
    @Published var loading = false
    @Published var days = 30
    @Published var context = ""
    private(set) var loadedDays = 30
    private(set) var generatedAt = Date()
    private let store = HKHealthStore()
    private var generation = UUID()
    private var queries: [UUID: HKQuery] = [:]
    private var jobs: [(HKSampleType, HealthMetric?)] = []
    private var completed = 0
    private var unavailable = 0
    private var total = 0
    private let limit = 128

    var prompt: String? {
        HealthReview.prompt(rows: rows.filter { selected.contains($0.id) }, days: loadedDays,
                            generatedAt: generatedAt, context: context)
    }

    func load() {
        guard !loading else { return }
        guard HKHealthStore.isHealthDataAvailable() else { status = "Apple Health is unavailable on this device."; return }
        clear()
        loading = true; loadedDays = days; generatedAt = Date()
        status = "Choose read permissions in Apple Health…"
        jobs = HealthMetric.catalog.compactMap { metric in metric.type.map { ($0, metric) } }
        jobs.append((HKObjectType.workoutType(), nil))
        total = jobs.count
        let token = generation
        store.requestAuthorization(toShare: [], read: Set(jobs.map { $0.0 })) { [weak self] success, error in
            DispatchQueue.main.async {
                guard let self, self.generation == token else { return }
                guard success else {
                    self.loading = false; self.status = error?.localizedDescription ?? "Health authorization did not complete."; return
                }
                for _ in 0..<4 { self.next(token) }
            }
        }
    }

    private func next(_ token: UUID) {
        guard generation == token else { return }
        guard !jobs.isEmpty else {
            if queries.isEmpty {
                loading = false
                rows.sort { $0.title < $1.title }
                status = "\(rows.count) categories available. \(unavailable) queries could not complete. Other categories had no accessible records; this does not reveal whether access was denied. Review your selections before sharing."
            }
            return
        }
        let (type, metric) = jobs.removeFirst()
        let id = UUID()
        let start = generatedAt.addingTimeInterval(-Double(loadedDays) * 86400)
        let predicate = HKQuery.predicateForSamples(withStart: start, end: generatedAt, options: [.strictStartDate, .strictEndDate])
        let query = HKSampleQuery(sampleType: type, predicate: predicate, limit: limit,
                                  sortDescriptors: [NSSortDescriptor(key: HKSampleSortIdentifierStartDate, ascending: false)]) { [weak self] _, samples, error in
            DispatchQueue.main.async {
                guard let self, self.generation == token, self.queries[id] != nil else { return }
                if error != nil { self.finish(id, token, row: nil, failed: true); return }
                let samples = samples ?? []
                guard !samples.isEmpty else { self.finish(id, token, row: nil); return }
                if let quantity = type as? HKQuantityType {
                    self.store.preferredUnits(for: [quantity]) { [weak self] units, _ in
                        DispatchQueue.main.async {
                            guard let self, self.generation == token, self.queries[id] != nil else { return }
                            guard let unit = units[quantity] else { self.finish(id, token, row: nil, failed: true); return }
                            let values = (samples as? [HKQuantitySample] ?? []).filter { $0.quantity.is(compatibleWith: unit) }
                                .map { $0.quantity.doubleValue(for: unit) }
                            let summary = HealthReview.quantity(values: values, unit: unit.unitString, cumulative: quantity.aggregationStyle == .cumulative)
                            self.finish(id, token, row: summary.map {
                                HealthReviewRow(id: type.identifier, title: metric?.title ?? type.identifier,
                                                detail: self.coverage(samples) + "; " + $0)
                            })
                        }
                    }
                } else {
                    let detail: String
                    if let categories = samples as? [HKCategorySample] {
                        let counts = Dictionary(grouping: categories, by: \.value)
                        detail = counts.keys.sorted().map { value in
                            let label = metric?.categoryValues[value] ?? "unknown category code \(value) (do not interpret)"
                            let records = counts[value]!
                            var summary = "\(label): \(records.count) records"
                            if metric?.suffix == "SleepAnalysis" || metric?.suffix == "MindfulSession" {
                                let intervals = records.filter { $0.endDate >= $0.startDate }.map { DateInterval(start: $0.startDate, end: $0.endDate) }
                                summary += ", \(HealthReview.number(HealthReview.coveredSeconds(intervals) / 3600)) recorded hours (overlap merged within this label only; not a nightly average)"
                            }
                            return summary
                        }.joined(separator: ", ")
                    } else if let workouts = samples as? [HKWorkout] {
                        detail = "latest workout duration \(HealthReview.number(workouts[0].duration / 60)) min; \(workouts.count) workout records (may overlap)"
                    } else { self.finish(id, token, row: nil, failed: true); return }
                    self.finish(id, token, row: HealthReviewRow(id: type.identifier, title: metric?.title ?? "Workouts",
                                                               detail: self.coverage(samples) + "; " + detail))
                }
            }
        }
        queries[id] = query; store.execute(query)
        DispatchQueue.main.asyncAfter(deadline: .now() + 20) { [weak self] in
            guard let self, self.generation == token, let query = self.queries[id] else { return }
            self.store.stop(query); self.finish(id, token, row: nil, failed: true)
        }
    }

    private func coverage(_ samples: [HKSample]) -> String {
        let date = ISO8601DateFormatter()
        let first = samples.map(\.startDate).min()!
        let last = samples.map(\.endDate).max()!
        let sources = Set(samples.map { $0.sourceRevision.source.bundleIdentifier }).count
        return "\(samples.count) records\(samples.count == limit ? " (limit reached; partial)" : ""), \(sources) sources; \(date.string(from: first)) to \(date.string(from: last))"
    }

    private func finish(_ id: UUID, _ token: UUID, row: HealthReviewRow?, failed: Bool = false) {
        guard generation == token, queries.removeValue(forKey: id) != nil else { return }
        completed += 1
        if failed { unavailable += 1 }
        if let row { rows.append(row); selected.insert(row.id) }
        status = "Reading categories: \(completed)/\(total)…"
        next(token)
    }

    func clear() {
        generation = UUID()
        for query in queries.values { store.stop(query) }
        queries = [:]; jobs = []; rows = []; selected = []; context = ""
        loading = false; completed = 0; unavailable = 0
        status = "Choose a period, then read Apple Health."
    }
}
