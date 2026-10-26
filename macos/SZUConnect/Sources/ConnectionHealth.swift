import Foundation

/// Monitoring intent is separate from permission to send credentials on the current path.
enum MonitoringMode: String, CaseIterable, Identifiable, Codable, Sendable {
    case manual, observe, recover

    var id: String { rawValue }
    var title: String {
        switch self {
        case .manual: SharedCopy.t("mode.manual.title")
        case .observe: SharedCopy.t("mode.observe.title")
        case .recover: SharedCopy.t("mode.recover.title")
        }
    }
    var symbol: String {
        switch self {
        case .manual: "hand.tap"
        case .observe: "eye"
        case .recover: "arrow.triangle.2.circlepath"
        }
    }
    /// One line for a choice card; `detail` is the longer explanation.
    var caption: String {
        switch self {
        case .manual: SharedCopy.t("mode.manual.detail")
        case .observe: SharedCopy.t("mode.observe.detail")
        case .recover: SharedCopy.t("mode.recover.detail")
        }
    }
    var detail: String {
        switch self {
        case .manual: SharedCopy.t("mode.manual.detail")
        case .observe: SharedCopy.t("mode.observe.detail")
        case .recover: SharedCopy.t("mode.recover.detail")
        }
    }
    static func migrated(raw: String?, legacyAutoReconnect: Bool) -> Self {
        if let raw, let mode = Self(rawValue: raw) { return mode }
        return legacyAutoReconnect ? .recover : .manual
    }
}

/// Contains only typed probe outcomes and a coarse interface label, never request metadata.
struct ConnectionSample: Identifiable, Codable, Sendable, Equatable {
    let id: UUID
    let date: Date
    let online: Bool
    let latencyMilliseconds: Double?
    let appleState: String
    let microsoftState: String
    let interface: String
    /// Optional for backwards compatibility with history written before selectable checks.
    let probeMode: String?
    /// Aggregate selected-site counts only; never URL, hostname or a user-defined label.
    let websiteTotal: Int?
    let websitePassed: Int?
    /// Actual endpoint duration, including failures. A skipped or cancelled item has no duration.
    let appleMilliseconds: Double?
    let microsoftMilliseconds: Double?
    /// Fixed at collection, never reconstructed from the current network on restore or export.
    let context: DiagnosticSampleContext?
    let completeResult: String?

    private enum CodingKeys: String, CodingKey {
        case id, date, online, latencyMilliseconds, appleState, microsoftState, interface, probeMode
        case websiteTotal, websitePassed, appleMilliseconds, microsoftMilliseconds, context, completeResult
    }

    init(report: ConnectivityReport, interface: String, date: Date = Date(), websiteTotal: Int? = nil,
         websitePassed: Int? = nil, context: DiagnosticSampleContext? = nil, completeResult: String? = nil) {
        let latencies = [report.appleResult, report.microsoftResult].compactMap { endpoint -> Double? in
            guard endpoint.succeeded, endpoint.elapsed.isFinite, endpoint.elapsed >= 0 else { return nil }
            let milliseconds = endpoint.elapsed * 1000
            return milliseconds.isFinite ? milliseconds : nil
        }
        self.init(id: UUID(), date: date, online: report.online, latencyMilliseconds: latencies.min(),
                  appleState: report.appleResult.state.rawValue,
                  microsoftState: report.microsoftResult.state.rawValue,
                  interface: Self.interfaceLabel(interface), probeMode: report.mode.rawValue,
                  websiteTotal: websiteTotal, websitePassed: websitePassed,
                  appleMilliseconds: report.appleResult.elapsed * 1000,
                  microsoftMilliseconds: report.microsoftResult.elapsed * 1000,
                  context: context, completeResult: completeResult)
    }

    fileprivate init(id: UUID, date: Date, online: Bool, latencyMilliseconds: Double?,
                     appleState: String, microsoftState: String, interface: String, probeMode: String? = nil,
                     websiteTotal: Int? = nil, websitePassed: Int? = nil, appleMilliseconds: Double? = nil,
                     microsoftMilliseconds: Double? = nil, context: DiagnosticSampleContext? = nil,
                     completeResult: String? = nil) {
        self.id = id
        self.date = date
        self.online = online
        self.latencyMilliseconds = latencyMilliseconds
        self.appleState = appleState
        self.microsoftState = microsoftState
        self.interface = interface
        self.probeMode = probeMode
        if let total = websiteTotal, let passed = websitePassed, (0...12).contains(total), (0...total).contains(passed) {
            self.websiteTotal = total; self.websitePassed = passed
        } else { self.websiteTotal = nil; self.websitePassed = nil }
        self.appleMilliseconds = Self.validDuration(appleMilliseconds, state: appleState)
        self.microsoftMilliseconds = Self.validDuration(microsoftMilliseconds, state: microsoftState)
        self.context = context.flatMap { $0.valid && $0.startedAt <= date ? $0 : nil }
        self.completeResult = completeResult.flatMap {
            ["basicPassed", "allPassed", "partial", "unavailable", "cancelled"].contains($0) ? $0 : nil
        }
    }

    init(from decoder: any Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        self.init(id: try container.decode(UUID.self, forKey: .id),
            date: try container.decode(Date.self, forKey: .date),
            online: try container.decode(Bool.self, forKey: .online),
            latencyMilliseconds: try container.decodeIfPresent(Double.self, forKey: .latencyMilliseconds),
            appleState: try container.decode(String.self, forKey: .appleState),
            microsoftState: try container.decode(String.self, forKey: .microsoftState),
            interface: try container.decode(String.self, forKey: .interface),
            probeMode: try container.decodeIfPresent(String.self, forKey: .probeMode),
            websiteTotal: try? container.decodeIfPresent(Int.self, forKey: .websiteTotal),
            websitePassed: try? container.decodeIfPresent(Int.self, forKey: .websitePassed),
            appleMilliseconds: try? container.decodeIfPresent(Double.self, forKey: .appleMilliseconds),
            microsoftMilliseconds: try? container.decodeIfPresent(Double.self, forKey: .microsoftMilliseconds),
            context: try? container.decodeIfPresent(DiagnosticSampleContext.self, forKey: .context),
            completeResult: try? container.decodeIfPresent(String.self, forKey: .completeResult))
    }

    private static func validDuration(_ value: Double?, state: String) -> Double? {
        guard let value, value.isFinite, (0...120_000).contains(value),
              let state = ConnectivityEndpointState(rawValue: state),
              state != .cancelled, state != .notCompleted else { return nil }
        return value
    }
    var allSelectedPassed: Bool {
        appleState == "success" && microsoftState == "success"
            && (websiteTotal == nil || websitePassed == websiteTotal)
    }
    var hasAnySelectedPass: Bool { online || (websitePassed ?? 0) > 0 }

    var resultTitle: String {
        if probeMode == ConnectivityProbeMode.complete.rawValue {
            if allSelectedPassed { return SharedCopy.t("records.sampleResult.all") }
            return hasAnySelectedPass ? SharedCopy.t("records.sampleResult.partial") : SharedCopy.t("records.sampleResult.failed")
        }
        return online ? SharedCopy.t("records.sampleResult.quick") : diagnosis.title
    }

    var isPartialCompleteResult: Bool {
        probeMode == ConnectivityProbeMode.complete.rawValue && hasAnySelectedPass && !allSelectedPassed
    }

    /// A partial complete check is useful failure evidence even if one base endpoint passed.
    /// Cancellation and a quick check's unawaited endpoint do not invent an outage.
    var isHistoricalFailure: Bool {
        let states = [appleState, microsoftState].compactMap(ConnectivityEndpointState.init(rawValue:))
        guard completeResult != "cancelled", !states.contains(.cancelled) else { return false }
        if states.contains(where: { $0 != .success && $0 != .notCompleted }) { return true }
        return probeMode == ConnectivityProbeMode.complete.rawValue && states.contains(.success)
            && websiteTotal.map { $0 > (websitePassed ?? $0) } == true
    }

    /// Derived on read, so stored history keeps its original format.
    var diagnosis: ConnectivityDiagnosis {
        ConnectivityDiagnosis(states: [appleState, microsoftState].compactMap(ConnectivityEndpointState.init(rawValue:)))
    }

    /// Interface names such as en0 are safe to retain; arbitrary labels may contain an SSID or IP.
    var anonymizedForExport: ConnectionSample {
        ConnectionSample(id: id, date: date, online: online, latencyMilliseconds: latencyMilliseconds,
            appleState: appleState, microsoftState: microsoftState, interface: Self.anonymousInterfaceLabel(interface),
            probeMode: probeMode, websiteTotal: websiteTotal, websitePassed: websitePassed,
            appleMilliseconds: appleMilliseconds, microsoftMilliseconds: microsoftMilliseconds,
            context: context, completeResult: completeResult)
    }
    static func anonymousInterfaceLabel(_ raw: String) -> String {
        let labels = interfaceLabel(raw).components(separatedBy: " · ").map { String($0.split(separator: " ").first ?? "") }
        var unique: [String] = []
        for label in labels where ["Wi-Fi", "有线"].contains(label) && !unique.contains(label) { unique.append(label) }
        return unique.isEmpty ? "未知接口" : unique.joined(separator: " · ")
    }

    static func interfaceLabel(_ raw: String) -> String {
        let labels = raw.components(separatedBy: "·").compactMap { component -> String? in
            let label = component.trimmingCharacters(in: .whitespacesAndNewlines)
            let pattern = #"^(Wi-Fi|有线)( (en|bridge|eth)[0-9]{1,3})?$"#
            return label.range(of: pattern, options: .regularExpression) != nil ? label : nil
        }
        var unique: [String] = []
        for label in labels where !unique.contains(label) { unique.append(label) }
        return unique.isEmpty ? "未知接口" : unique.prefix(4).joined(separator: " · ")
    }
}

/// A bounded history of observations. It does not infer continuous uptime between samples.
struct ConnectionHealth: Codable, Sendable {
    private(set) var samples: [ConnectionSample] = []
    private static let retention: TimeInterval = 24 * 60 * 60
    private static let futureTolerance: TimeInterval = 60
    private static let maximumSamples = 360
    private enum CodingKeys: String, CodingKey { case samples }

    init() {}

    init(from decoder: any Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        restore(try container.decode([ConnectionSample].self, forKey: .samples))
    }

    mutating func append(_ sample: ConnectionSample, now: Date = Date()) {
        guard now.timeIntervalSinceReferenceDate.isFinite else { samples = []; return }
        let oldest = now.addingTimeInterval(-Self.retention)
        let newest = now.addingTimeInterval(Self.futureTolerance)
        // Existing samples entered through restore and are already normalized and ordered.
        // Only the new observation needs the full untrusted-input validation again.
        var incoming = ConnectionHealth()
        incoming.restore([sample], now: now)
        samples.removeAll { $0.date < oldest || $0.date > newest }
        guard let normalized = incoming.samples.first else { return }
        if let duplicate = samples.firstIndex(where: { $0.id == normalized.id }) {
            guard samples[duplicate].date <= normalized.date else { return }
            samples.remove(at: duplicate)
        }
        let insertion = samples.firstIndex { Self.precedes(normalized, $0) } ?? samples.endIndex
        samples.insert(normalized, at: insertion)
        if samples.count > Self.maximumSamples { samples.removeFirst(samples.count - Self.maximumSamples) }
    }

    private static func precedes(_ lhs: ConnectionSample, _ rhs: ConnectionSample) -> Bool {
        if lhs.date == rhs.date { return lhs.id.uuidString < rhs.id.uuidString }
        return lhs.date < rhs.date
    }

    mutating func restore(_ restoredSamples: [ConnectionSample], now: Date = Date()) {
        guard now.timeIntervalSinceReferenceDate.isFinite else { samples = []; return }
        let oldest = now.addingTimeInterval(-Self.retention)
        let newest = now.addingTimeInterval(Self.futureTolerance)
        var unique: [UUID: ConnectionSample] = [:]
        for sample in restoredSamples {
            guard sample.date.timeIntervalSinceReferenceDate.isFinite,
                  sample.date >= oldest, sample.date <= newest,
                  let apple = ConnectivityEndpointState(rawValue: sample.appleState),
                  let microsoft = ConnectivityEndpointState(rawValue: sample.microsoftState) else { continue }
            if let latency = sample.latencyMilliseconds, !latency.isFinite || latency < 0 { continue }
            let online = apple == .success || microsoft == .success
            let normalized = ConnectionSample(id: sample.id, date: sample.date, online: online,
                latencyMilliseconds: online ? sample.latencyMilliseconds : nil,
                appleState: apple.rawValue, microsoftState: microsoft.rawValue,
                interface: ConnectionSample.interfaceLabel(sample.interface),
                probeMode: sample.probeMode.flatMap(ConnectivityProbeMode.init(rawValue:))?.rawValue,
                websiteTotal: sample.websiteTotal, websitePassed: sample.websitePassed,
                appleMilliseconds: sample.appleMilliseconds, microsoftMilliseconds: sample.microsoftMilliseconds,
                context: sample.context, completeResult: sample.completeResult)
            if let existing = unique[sample.id], existing.date > normalized.date { continue }
            unique[sample.id] = normalized
        }
        samples = Array(unique.values.sorted(by: Self.precedes).suffix(Self.maximumSamples))
    }

    func snapshot(now: Date = Date()) -> ConnectionHealth {
        var current = ConnectionHealth()
        guard now.timeIntervalSinceReferenceDate.isFinite else { return current }
        let oldest = now.addingTimeInterval(-Self.retention)
        let newest = now.addingTimeInterval(Self.futureTolerance)
        // Every write and decode enters through restore. Samples are already sanitized,
        // deduplicated, sorted and bounded; reading only needs to apply the current time window.
        if let first = samples.first, let last = samples.last,
           first.date >= oldest, last.date <= newest { return self }
        current.samples = samples.filter { $0.date >= oldest && $0.date <= newest }
        return current
    }

    var successfulCount: Int { samples.reduce(0) { $0 + ($1.online ? 1 : 0) } }

    /// Share of recorded samples that passed; nil instead of a fabricated value when empty.
    var successRate: Double? {
        samples.isEmpty ? nil : Double(successfulCount) / Double(samples.count)
    }

    var medianLatencyMilliseconds: Double? {
        let values = samples.compactMap { $0.online ? $0.latencyMilliseconds : nil }.sorted()
        guard !values.isEmpty else { return nil }
        let middle = values.count / 2
        guard values.count.isMultiple(of: 2) else { return values[middle] }
        return values[middle - 1] / 2 + values[middle] / 2
    }

    var successSummary: String { SharedCopy.f("records.passedCount", ["passed": String(successfulCount), "total": String(samples.count)]) }
}
