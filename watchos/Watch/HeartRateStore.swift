import Foundation
import HealthKit
import Combine

final class HeartRateStore: ObservableObject {
    @Published var readings: [HeartRateReading] = []
    @Published var status = "Read recent Apple Health heart-rate samples."
    @Published var loading = false
    @Published var context = ""
    private let store = HKHealthStore()
    private let type = HKQuantityType.quantityType(forIdentifier: .heartRate)!
    private var query: HKSampleQuery?

    func load() {
        guard !loading else { return }
        guard HKHealthStore.isHealthDataAvailable() else { status = "Apple Health is unavailable on this device."; return }
        loading = true; readings = []; status = "Waiting for Health permission…"
        store.requestAuthorization(toShare: [], read: [type]) { [weak self] success, error in
            DispatchQueue.main.async {
                guard let self else { return }
                guard success else { self.loading = false; self.status = error?.localizedDescription ?? "Health authorization did not complete."; return }
                self.fetch()
            }
        }
    }
    private func fetch() {
        status = "Reading recent samples…"
        let now = Date()
        let predicate = HKQuery.predicateForSamples(withStart: now.addingTimeInterval(-24 * 3600), end: now, options: [.strictStartDate, .strictEndDate])
        let query = HKSampleQuery(sampleType: type, predicate: predicate, limit: 12,
                                  sortDescriptors: [NSSortDescriptor(key: HKSampleSortIdentifierStartDate, ascending: false)]) { [weak self] _, samples, error in
            let values = (samples as? [HKQuantitySample] ?? []).map {
                HeartRateReading(id: $0.uuid, date: $0.startDate, bpm: $0.quantity.doubleValue(for: HKUnit.count().unitDivided(by: .minute())))
            }
            DispatchQueue.main.async {
                guard let self else { return }
                self.loading = false; self.query = nil
                if let error { self.status = error.localizedDescription; return }
                self.readings = values
                // HealthKit intentionally does not disclose whether read access was denied.
                self.status = values.isEmpty ? "No accessible readings in the past 24 hours. Check Health permissions or record a heart-rate reading first." : "\(values.count) recent samples. Review before sharing."
            }
        }
        self.query = query; store.execute(query)
    }
    func clear() { readings = []; context = ""; status = "Read recent Apple Health heart-rate samples." }
}
