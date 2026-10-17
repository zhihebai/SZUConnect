import Foundation

/// A user's optional targets. Persistence is explicitly owned by the caller;
/// these types are not Codable and must never enter connection logs or exports.
struct WebsiteTarget: Identifiable, Equatable, Sendable {
    static let maximumCount = 12

    let id: String
    var url: String
    var enabled: Bool

    init(id: String = UUID().uuidString, url: String, enabled: Bool = true) {
        self.id = id; self.url = url; self.enabled = enabled
    }

    var displayName: String {
        (try? WebsiteAddress.normalize(url).host) ?? SharedCopy.t("internet.targets.title")
    }

    enum ValidationError: Error, LocalizedError {
        case tooMany, duplicateAddress, invalidIdentity, duplicateIdentity
        var errorDescription: String? {
            switch self {
            case .tooMany: SharedCopy.f("internet.maximumCount", ["max": String(WebsiteTarget.maximumCount)])
            case .duplicateAddress: SharedCopy.t("internet.duplicateWebsite")
            case .invalidIdentity: SharedCopy.t("internet.invalidWebsite")
            case .duplicateIdentity: SharedCopy.t("internet.duplicateIdentity")
            }
        }
    }

    /// Validate the full list before saving, including currently disabled entries.
    /// Keep errors generic so credentials or URL components never reach diagnostics.
    static func validated(_ targets: [WebsiteTarget]) throws -> [WebsiteTarget] {
        guard targets.count <= maximumCount else { throw ValidationError.tooMany }
        var addresses = Set<String>()
        var identities = Set<String>()
        return try targets.map { target in
            guard validIdentity(target.id) else { throw ValidationError.invalidIdentity }
            guard identities.insert(target.id).inserted else { throw ValidationError.duplicateIdentity }
            let normalized = try normalizedAddress(target.url)
            guard addresses.insert(normalized).inserted else { throw ValidationError.duplicateAddress }
            return WebsiteTarget(id: target.id, url: normalized, enabled: target.enabled)
        }
    }

    /// A malformed imported list cannot expand the request budget, duplicate a
    /// request, or send sensitive components. The selected count in the report
    /// still includes skipped entries, so filtering cannot produce allPassed.
    fileprivate static func safeEnabledTargets(_ targets: [WebsiteTarget]) -> [WebsiteTarget] {
        var addresses = Set<String>()
        var identities = Set<String>()
        var selected: [WebsiteTarget] = []
        for target in targets where target.enabled {
            guard selected.count < maximumCount else { break }
            guard validIdentity(target.id), !identities.contains(target.id),
                  let normalized = try? normalizedAddress(target.url),
                  !addresses.contains(normalized) else { continue }
            addresses.insert(normalized); identities.insert(target.id)
            selected.append(WebsiteTarget(id: target.id, url: normalized, enabled: true))
        }
        return selected
    }

    private static func validIdentity(_ id: String) -> Bool {
        !id.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && id.utf8.count <= 128
            && !id.unicodeScalars.contains { CharacterSet.controlCharacters.contains($0) }
    }

    private static func normalizedAddress(_ input: String) throws -> String {
        let normalized = try WebsiteAddress.normalize(input)
        guard var components = URLComponents(url: normalized, resolvingAgainstBaseURL: false) else {
            throw WebsiteAddress.ValidationError.invalid
        }
        // Host root with/without slash and explicit default ports refer to the
        // same HTTP resource; they must not occupy duplicate selected slots.
        if components.path.isEmpty { components.path = "/" }
        if (components.scheme == "https" && components.port == 443)
            || (components.scheme == "http" && components.port == 80) { components.port = nil }
        guard let result = components.url?.absoluteString else { throw WebsiteAddress.ValidationError.invalid }
        guard result.utf8.count <= 2048 else { throw WebsiteAddress.ValidationError.tooLong }
        return result
    }
}

protocol WebsiteChecking: Sendable {
    func probe(_ input: String, strategy: WebsiteProbeStrategy) async -> WebsiteProbeResult
}

extension WebsiteProbe: WebsiteChecking {}

struct WebsiteCheckResult: Sendable {
    let targetID: String
    let displayName: String
    let result: WebsiteProbeResult
}

enum InternetCheckVerdict: String, CaseIterable, Sendable {
    case basicPassed, allPassed, partial, unavailable, cancelled
}

/// The base result remains the connectivity fact. Recovery evidence may request a bounded
/// attempt, but websites never supply campus identity or override authentication guards.
struct InternetCheckReport: Sendable {
    let base: ConnectivityReport
    let websites: [WebsiteCheckResult]
    /// Configured enabled count, retained even when quick mode skips all targets.
    let selectedCount: Int
    let wasCancelled: Bool
    let websitesDeferred: Bool

    init(base: ConnectivityReport, websites: [WebsiteCheckResult], selectedCount: Int,
         wasCancelled: Bool = false, websitesDeferred: Bool = false) {
        self.base = base; self.websites = websites
        self.selectedCount = max(0, selectedCount); self.wasCancelled = wasCancelled
        self.websitesDeferred = websitesDeferred
    }

    /// Number actually included in this round, including explicit cancellation.
    var includedCount: Int { websites.count }
    var websitePassedCount: Int { websites.filter { $0.result.state == .accessible && (200..<300).contains($0.result.statusCode ?? 0) }.count }

    /// Both base endpoints passed; any remaining failure or missing result belongs to websites.
    var hasOnlyWebsiteIssues: Bool {
        !websitesDeferred && base.mode == .complete && base.allPassed && verdict == .partial
    }

    /// Availability evidence can request a bounded recovery without identifying its cause.
    var hasRecoveryEvidence: Bool {
        guard base.mode == .complete, verdict == .partial, base.online else { return false }
        return !base.allPassed || websites.contains { $0.result.recoverySignal != .none }
    }

    /// Optional websites may traverse a different TUN/proxy path. Their generic
    /// failures cannot request automatic campus authentication on a working base path.
    var hasCampusRecoveryEvidence: Bool {
        guard base.mode == .complete, verdict == .partial, base.online else { return false }
        return !base.allPassed || websites.contains { $0.result.recoverySignal == .campusRedirect }
    }

    var completeBasePassed: Bool { !wasCancelled && base.mode == .complete && base.allPassed }

    /// Rearm only after complete rounds contain a response from every selected target.
    /// HTTP business errors can prove transport recovery without clearing authentication holds.
    var transportRecovered: Bool {
        base.mode == .complete && base.allPassed && verdict != .cancelled
            && includedCount == selectedCount && Set(websites.map(\.targetID)).count == selectedCount
            && websites.allSatisfy { $0.result.hasHTTPResponse && $0.result.recoverySignal != .campusRedirect }
    }

    var verdict: InternetCheckVerdict {
        if wasCancelled || (base.appleResult.state == .cancelled && base.microsoftResult.state == .cancelled) {
            return .cancelled
        }
        if base.mode == .quick { return base.online ? .basicPassed : .unavailable }
        if websitesDeferred && base.allPassed { return .basicPassed }
        if base.allPassed && includedCount == selectedCount && websitePassedCount == selectedCount {
            return .allPassed
        }
        return base.online || websitePassedCount > 0 ? .partial : .unavailable
    }
}

enum InternetChecker {
    private enum Completion: Sendable {
        case base(ConnectivityReport)
        case website(Int, WebsiteCheckResult)
    }

    static func check(mode: ConnectivityProbeMode, targets: [WebsiteTarget], checker: any WebsiteChecking, strategy: WebsiteProbeStrategy = .system,
                      prioritizeCampusRecovery: Bool = false, deferWebsites: Bool = false,
                      baseProbe: @escaping @Sendable (ConnectivityProbeMode) async -> ConnectivityReport) async -> InternetCheckReport {
        let selectedCount = targets.filter(\.enabled).count
        guard !Task.isCancelled else {
            return InternetCheckReport(base: cancelledBase(mode), websites: [], selectedCount: selectedCount, wasCancelled: true)
        }
        // Recovery acquires the independent campus base first. A broken proxy website
        // never delays an offline base or settlement of an uncertain submission.
        var acquiredBase: ConnectivityReport?
        if mode == .complete && (prioritizeCampusRecovery || deferWebsites) {
            let report = await baseProbe(mode)
            guard !Task.isCancelled else {
                return InternetCheckReport(base: cancelledBase(mode), websites: [], selectedCount: selectedCount, wasCancelled: true)
            }
            if deferWebsites || !report.allPassed {
                return InternetCheckReport(base: report, websites: [], selectedCount: selectedCount, websitesDeferred: true)
            }
            acquiredBase = report
        }
        let selected = mode == .complete ? WebsiteTarget.safeEnabledTargets(targets) : []
        let readyBase = acquiredBase
        return await withTaskGroup(of: Completion.self, returning: InternetCheckReport.self) { group in
            group.addTask {
                guard !Task.isCancelled else { return .base(cancelledBase(mode)) }
                if let readyBase { return .base(readyBase) }
                return .base(await baseProbe(mode))
            }
            for (index, target) in selected.enumerated() {
                group.addTask {
                    let result: WebsiteProbeResult
                    if Task.isCancelled { result = cancelledWebsite() }
                    else {
                        let response = await checker.probe(target.url, strategy: strategy)
                        result = Task.isCancelled ? cancelledWebsite() : response
                    }
                    return .website(index, WebsiteCheckResult(targetID: target.id, displayName: target.displayName, result: result))
                }
            }
            var base = cancelledBase(mode)
            var websiteResults: [Int: WebsiteCheckResult] = [:]
            for await completion in group {
                if Task.isCancelled { group.cancelAll() }
                switch completion {
                case .base(let report): base = report
                case .website(let index, let result): websiteResults[index] = result
                }
            }
            let ordered = selected.indices.compactMap { websiteResults[$0] }
            return InternetCheckReport(base: base, websites: ordered, selectedCount: selectedCount,
                                       wasCancelled: Task.isCancelled)
        }
    }

    private static func cancelledBase(_ mode: ConnectivityProbeMode) -> ConnectivityReport {
        ConnectivityReport(appleResult: .init(state: .cancelled, elapsed: 0),
                           microsoftResult: .init(state: .cancelled, elapsed: 0), mode: mode)
    }

    private static func cancelledWebsite() -> WebsiteProbeResult {
        WebsiteProbeResult(state: .cancelled, title: SharedCopy.t("website.state.cancelled.title"), detail: SharedCopy.t("website.state.cancelled.detail"),
                           statusCode: nil, elapsed: 0, displayURL: nil, strategy: .system)
    }
}
