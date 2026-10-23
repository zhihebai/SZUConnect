import Foundation
import Darwin

private actor DiagnosticFixtureService: ConnectionService {
    struct Probe: Sendable {
        let apple: ConnectivityEndpointState
        let microsoft: ConnectivityEndpointState
        let appleElapsed: TimeInterval
        let microsoftElapsed: TimeInterval
    }
    private var probes: [Probe] = []
    private var discovery = CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked, issue: .dns)
    private var gateDiscovery = false
    private var pending: CheckedContinuation<Void, Never>?
    private var observers: [CheckedContinuation<Void, Never>] = []
    private(set) var probeCalls = 0
    private(set) var discoveryCalls = 0
    private(set) var submissions = 0
    func setExpectedPhysicalRoute(_ route: CampusPhysicalRoute?) -> Bool { true }
    func enqueue(_ probe: Probe) { probes.append(probe) }
    func configureDiscovery(_ discovery: CampusDiscovery, gated: Bool = false) {
        self.discovery = discovery; gateDiscovery = gated
    }
    func probe() async -> ConnectivityReport { await probe(mode: .quick) }
    func probe(mode: ConnectivityProbeMode) async -> ConnectivityReport {
        probeCalls += 1
        let result = probes.isEmpty ? Probe(apple: .success, microsoft: .success, appleElapsed: 0.05, microsoftElapsed: 0.08)
            : probes.removeFirst()
        return .init(appleResult: .init(state: result.apple, elapsed: result.appleElapsed),
            microsoftResult: .init(state: mode == .quick && result.apple == .success ? .notCompleted : result.microsoft,
                                   elapsed: result.microsoftElapsed), mode: mode)
    }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        discoveryCalls += 1
        let result = discovery
        if gateDiscovery {
            gateDiscovery = false
            await withCheckedContinuation { continuation in
                pending = continuation
                let current = observers; observers.removeAll(); current.forEach { $0.resume() }
            }
        }
        return result
    }
    func waitForDiscovery() async {
        if pending != nil { return }
        await withCheckedContinuation { observers.append($0) }
    }
    func releaseDiscovery() {
        guard let current = pending else { fatalError("Missing fake discovery gate") }
        pending = nil; current.resume()
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? { fatalError("Use staged fake discovery") }
    func login(credentials: Credentials, area: CampusArea) async throws -> String { fatalError("No diagnostic test may log in") }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        submissions += 1; fatalError("No diagnostic test may submit credentials")
    }
}

private actor DiagnosticObservationGate {
    private var pending: CheckedContinuation<PathObservation, Error>?
    private var observers: [CheckedContinuation<Void, Never>] = []
    func read() async throws -> PathObservation {
        try await withCheckedThrowingContinuation { continuation in
            pending = continuation
            let current = observers; observers.removeAll(); current.forEach { $0.resume() }
        }
    }
    func waitUntilReading() async {
        if pending != nil { return }
        await withCheckedContinuation { observers.append($0) }
    }
    func release(_ observation: PathObservation) {
        guard let current = pending else { fatalError("Missing fake observation gate") }
        pending = nil; current.resume(returning: observation)
    }
}

/// The check has already stamped completion. Only delivery to the model is delayed.
private actor DiagnosticDeliveryGate {
    private var first = true
    private var pending: CheckedContinuation<Void, Never>?
    private var report: InternetCheckReport?
    private var observers: [CheckedContinuation<InternetCheckReport, Never>] = []
    func consume(_ value: InternetCheckReport) async {
        guard first else { return }
        first = false; report = value
        await withCheckedContinuation { continuation in
            pending = continuation
            let current = observers; observers.removeAll(); current.forEach { $0.resume(returning: value) }
        }
    }
    func waitForCompletion() async -> InternetCheckReport {
        if let report { return report }
        return await withCheckedContinuation { observers.append($0) }
    }
    func release() {
        guard let current = pending else { fatalError("Missing completed-report delivery gate") }
        pending = nil; current.resume()
    }
}

@MainActor private final class DiagnosticTestClock {
    var date = Date(timeIntervalSinceNow: -1)
    func advance(_ seconds: TimeInterval) { date.addTimeInterval(seconds) }
}

@main @MainActor
struct DiagnosticHistoryAppModelTests {
    private static var assertions = 0
    private static let credentials = Credentials(account: "diagnostic-fixture-account", password: "diagnostic-fixture-password")
    private static let descriptor = NetworkDescriptor(kind: "ethernet", raw: [
        "gw": "02:00:00:00:ab:01", "gwip": "192.0.2.1", "net": "192.0.2.0/24",
        "dhcp": "192.0.2.2", "dns": "192.0.2.3"
    ])
    private static func check(_ value: Bool, _ text: String) {
        precondition(value, "FAIL: " + text); assertions += 1; print("PASS: " + text)
    }
    private static func waitUntil(_ text: String, _ predicate: @MainActor () -> Bool) async throws {
        let deadline = ContinuousClock.now.advanced(by: .seconds(5))
        while ContinuousClock.now < deadline {
            if predicate() { return }
            await Task.yield()
        }
        fatalError("Timed out waiting for " + text)
    }
    private static func folder() throws -> URL {
        let result = FileManager.default.temporaryDirectory.appendingPathComponent("szu-diagnostic-history-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: result, withIntermediateDirectories: true)
        return result
    }
    private static func seedMemory(_ folder: URL, now: Date) throws {
        var memory = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
        precondition(memory.remember(descriptor, area: "teaching", proofTeaching: true, proofDormitory: false,
            now: now.addingTimeInterval(-60), newID: "01ab23cd") != nil)
        try NetworkMemoryFile.write(memory.encoded(), to: folder.appendingPathComponent("network-memory.json"))
    }
    private static func attach(_ model: AppModel, name: String = "original", tunnel: Bool = true) {
        model.receivePath(usable: true, interfaces: "有线 en99", identity: .init(fingerprint: "diagnostic-fixture-" + name),
            tunnel: tunnel, descriptors: [descriptor], physicalConnected: true,
            systemPhysicalLabel: "有线 en99", connectedPhysicalLabels: ["有线 en99"])
    }
    private static func observation() -> PathObservation {
        var continuity = NetworkContinuitySnapshot(identity: .init(fingerprint: "diagnostic-fixture-original"), linkActive: true)
        continuity.physicalLabels = ["有线 en99"]; continuity.systemPhysicalLabel = "有线 en99"
        continuity.connectedPhysicalLabels = ["有线 en99"]; continuity.hasUsablePhysicalLink = true
        continuity.descriptors = [descriptor]; continuity.tunnel = true
        return .init(usable: true, interfaces: "有线 en99", constrained: false, continuity: continuity)
    }
    private static func fixture(_ folder: URL, _ service: DiagnosticFixtureService, _ clock: DiagnosticTestClock,
                                reader: DiagnosticObservationGate? = nil, delivery: DiagnosticDeliveryGate? = nil) -> AppModel {
        let model = AppModel(client: service, isolated: true, now: { clock.date },
            isolatedCredentials: credentials, recordStoreDirectory: folder,
            networkInformationReader: reader.map { gate in { try await gate.read() } },
            internetCheckDelivery: delivery.map { gate in { await gate.consume($0) } })
        model.area = .teaching; model.setMonitoringMode(.observe); model.setProbeMode(.complete)
        attach(model)
        return model
    }
    private static func object(_ model: AppModel) throws -> [String: Any] {
        try JSONSerialization.jsonObject(with: model.diagnosticExportData) as! [String: Any]
    }
    private static func finish(_ model: AppModel) async throws {
        try await waitUntil("diagnostic model operation completion") { !model.busy }
    }
    private static func reportURL() -> URL? {
        guard let index = CommandLine.arguments.firstIndex(of: "--report"), CommandLine.arguments.indices.contains(index + 1) else { return nil }
        return URL(fileURLWithPath: CommandLine.arguments[index + 1])
    }
    static func main() async throws {
        setbuf(stdout, nil)
        var reportToExport: Data?
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = DiagnosticFixtureService(), clock = DiagnosticTestClock()
            let model = fixture(root, service, clock)
            await service.enqueue(.init(apple: .success, microsoft: .timeout, appleElapsed: 0.123, microsoftElapsed: 1.456))
            model.checkConnection(); try await finish(model)
            guard let failure = model.currentHealth.samples.first else { fatalError("Missing diagnostic sample") }
            check(failure.context?.area == "teaching" && failure.context?.monitoringMode == "observe"
                  && failure.context?.operation == "manual", "Manual failure fixes the original operation, mode and area")
            check(failure.appleMilliseconds == 123 && failure.microsoftMilliseconds == 1456
                  && failure.completeResult == "partial" && failure.isHistoricalFailure,
                  "A complete partial failure stores both actual timings and does not disappear behind base success")
            check(model.lastCheckedAt == failure.date, "Current checked time uses the report completion time")
            let failureEvents = model.events.filter { $0.context != nil }
            check(failureEvents.count == 1 && failureEvents[0].context?.kind == "connection",
                  "A completed failed connection appends a structured failure event")
            check(failureEvents[0].context?.campusVerification.state == "notChecked",
                  "A connection-only round cannot borrow an earlier campus verification")
            await service.enqueue(.init(apple: .success, microsoft: .success, appleElapsed: 0.021, microsoftElapsed: 0.037))
            model.checkConnection(); try await finish(model)
            check(model.isOnline && model.currentHealth.samples.count == 2 && model.currentHealth.samples.filter(\.isHistoricalFailure).count == 1,
                  "A later success appends without replacing the original failure")
            check(model.events.filter { $0.context != nil } == failureEvents, "Recovery success does not overwrite or duplicate the earlier failure event")
            model.area = .dormitory; model.setMonitoringMode(.manual)
            check(model.currentHealth.samples.first(where: { $0.id == failure.id }) == failure,
                  "Later mode and area edits never rewrite a historical sample")
            let exported = try object(model)
            let exportedSamples = exported["samples"] as! [[String: Any]]
            check(exportedSamples.count == 2 && exportedSamples.allSatisfy { ($0["interface"] as? String) == "有线" },
                  "Export keeps both original observations while removing raw adapter identifiers")
            check(model.diagnosticSummary.contains("Microsoft timeout 1456 ms"), "The summary includes actual elapsed time for a recent failed endpoint")
            let savedSamples = try JSONDecoder().decode(ConnectionHealth.self, from: Data(contentsOf: root.appendingPathComponent("health.json")))
            check(savedSamples.samples.first(where: { $0.id == failure.id }) == failure,
                  "The persisted health file preserves failure time and original context")
            let savedEvents = try EventLogFile.read(from: root.appendingPathComponent("events.json"))
            check(savedEvents.contains { $0.context == failureEvents.first?.context }, "The event file retains structured failure evidence after recovery")
            let forbidden = [credentials.account, credentials.password, "en99", "192.0.2.1", "diagnostic-fixture-original"]
            let json = String(decoding: model.diagnosticExportData, as: UTF8.self)
            check(forbidden.allSatisfy { !json.contains($0) }, "Reports contain no fake credential, adapter, source identity or address")
            reportToExport = model.diagnosticExportData
            clock.date = model.health.samples.map(\.date).max()!.addingTimeInterval(86_401)
            check((try object(model)["samples"] as! [Any]).isEmpty && !model.health.samples.isEmpty,
                  "Long idle export expires 24-hour samples without mutating stored evidence")
            check(!model.diagnosticSummary.contains("Microsoft timeout 1456 ms"), "Expired samples do not remain in the recent-failure summary")
            check(await service.submissions == 0, "Read-only diagnostic checks never submit credentials")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = DiagnosticFixtureService(), clock = DiagnosticTestClock()
            try seedMemory(root, now: clock.date)
            let model = fixture(root, service, clock)
            check(model.canVerifyCurrentCampusService, "An isolated remembered physical network can run independent service verification")
            await service.configureDiscovery(.init(teaching: .unconfirmed, dormitory: .notChecked, issue: .dnsTls))
            model.verifyCurrentCampusService(); try await finish(model)
            guard let failed = model.events.first(where: { $0.context?.kind == "campusVerification" }) else {
                fatalError("Missing independent service failure event")
            }
            check(failed.context?.campusVerification.issue == "dnsTls" && failed.context?.campusVerification.state == "unconfirmed",
                  "Independent verification failure records its own fixed campus issue")
            await service.configureDiscovery(.init(teaching: .confirmed, dormitory: .notChecked))
            model.verifyCurrentCampusService(); try await finish(model)
            check(model.requiredCampusVerificationState == "confirmed" && model.events.contains { $0 == failed },
                  "Later service confirmation preserves the original verification failure event")
            let eventBytes = try Data(contentsOf: root.appendingPathComponent("events.json"))
            let restarted = AppModel(client: DiagnosticFixtureService(), isolated: true, now: { clock.date }, recordStoreDirectory: root)
            check(restarted.events.contains { $0 == failed }, "Restart reloads the original failed service evidence")
            check(try Data(contentsOf: root.appendingPathComponent("events.json")) == eventBytes,
                  "Read-only history export and fixture reconstruction do not rewrite stored event evidence")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = DiagnosticFixtureService(), clock = DiagnosticTestClock(), delivery = DiagnosticDeliveryGate()
            let model = fixture(root, service, clock, delivery: delivery)
            await service.enqueue(.init(apple: .timeout, microsoft: .redirect, appleElapsed: 2.321, microsoftElapsed: 0.045))
            model.checkConnection()
            let completed = await delivery.waitForCompletion()
            check(completed.verdict == .unavailable && model.health.samples.isEmpty && model.lastCheckedAt == nil,
                  "A completed fake check can wait for actor delivery without updating current model state")
            attach(model, name: "new-current")
            await service.enqueue(.init(apple: .success, microsoft: .success, appleElapsed: 0.07, microsoftElapsed: 0.09))
            model.checkConnection(); try await finish(model)
            let currentDate = model.lastCheckedAt, currentProbe = model.lastInternetCheck
            check(currentDate != nil && model.isOnline && model.currentHealth.samples.count == 1,
                  "The replacement network can finish its new successful check while old delivery remains blocked")
            await delivery.release()
            try await waitUntil("late completed history append") { model.currentHealth.samples.count == 2 }
            guard let old = model.currentHealth.samples.first(where: { $0.date == completed.completedAt }) else {
                fatalError("Missing original completed sample")
            }
            check(old.appleState == "timeout" && old.microsoftState == "redirect" && old.appleMilliseconds == 2321
                  && old.microsoftMilliseconds == 45 && old.completeResult == "unavailable",
                  "Late delivery preserves actual pre-cancellation endpoint failures and elapsed times")
            check(old.context?.area == "teaching" && old.context?.monitoringMode == "observe" && old.date == completed.completedAt,
                  "Late history keeps the original operation context and worker completion date")
            check(model.lastCheckedAt == currentDate && model.lastInternetCheck?.completedAt == currentProbe?.completedAt && model.isOnline,
                  "An old-network completed result never overwrites new current time, result or availability")
            check(model.events.allSatisfy { $0.context == nil },
                  "A superseded round cannot synthesize an operation failure from the replacement network")
        }

        for change in ["area", "mode", "cancel", "sleep"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = DiagnosticFixtureService(), clock = DiagnosticTestClock(), delivery = DiagnosticDeliveryGate()
            let model = fixture(root, service, clock, delivery: delivery)
            await service.enqueue(.init(apple: .success, microsoft: .timeout, appleElapsed: 0.1, microsoftElapsed: 2))
            model.checkConnection()
            let completed = await delivery.waitForCompletion()
            switch change {
            case "area": model.area = .dormitory
            case "mode": model.setMonitoringMode(.manual)
            case "cancel": model.cancelCurrentOperation()
            default: model.setSleeping(true)
            }
            await delivery.release()
            try await waitUntil("completed history after " + change) { model.currentHealth.samples.count == 1 }
            let sample = model.currentHealth.samples[0]
            check(sample.context?.area == "teaching" && sample.context?.monitoringMode == "observe"
                  && sample.context?.operation == "manual" && sample.date == completed.completedAt,
                  "A completed report delivered after \(change) retains its original immutable context and time")
            check(model.events.allSatisfy { $0.context == nil },
                  "A completed old round after \(change) cannot use newer state to build a structured failure event")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = DiagnosticFixtureService(), clock = DiagnosticTestClock(), delivery = DiagnosticDeliveryGate()
            let model = fixture(root, service, clock, delivery: delivery)
            model.setMonitoringMode(.manual)
            await service.enqueue(.init(apple: .success, microsoft: .timeout, appleElapsed: 0.02, microsoftElapsed: 1))
            model.checkConnection(); _ = await delivery.waitForCompletion()
            model.setMonitoringMode(.recover); model.setMonitoringMode(.manual)
            await delivery.release(); try await finish(model)
            check(model.currentHealth.samples.count == 1 && model.currentHealth.samples[0].context?.monitoringMode == "manual",
                  "A mode round trip preserves the actual completed sample's original context")
            check(model.events.allSatisfy { $0.context == nil },
                  "Changing mode away and back cannot revive the old round's failure-context ownership")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = DiagnosticFixtureService(), clock = DiagnosticTestClock(), delivery = DiagnosticDeliveryGate()
            let model = fixture(root, service, clock, delivery: delivery)
            let route = CampusPhysicalRoute(interfaceName: "en99", interfaceIndex: 99,
                addresses: ["192.0.2.10"], dnsServers: ["192.0.2.3"], identity: "diagnostic-bound-fixture",
                descriptor: descriptor, isSystemPrimary: true)
            func boundObservation(tunnel: Bool) {
                model.receivePath(usable: true, interfaces: "有线 en99", identity: .init(fingerprint: "same-physical-fixture"),
                    tunnel: tunnel, descriptors: [descriptor], physicalConnected: true,
                    systemPhysicalLabel: "有线 en99", connectedPhysicalLabels: ["有线 en99"],
                    authenticationRoute: route, requiresPhysicalBinding: true)
            }
            boundObservation(tunnel: true)
            await service.enqueue(.init(apple: .success, microsoft: .timeout, appleElapsed: 0.03, microsoftElapsed: 3))
            model.checkConnection(); _ = await delivery.waitForCompletion()
            boundObservation(tunnel: false)
            await delivery.release(); try await finish(model)
            check(model.events.filter { $0.context != nil }.count == 1,
                  "A TUN-only notification on the same complete physical binding cannot swallow a real completed failure")
            check(model.currentHealth.samples[0].context?.environment.tun == .detected,
                  "The historical sample keeps its original TUN snapshot after a same-binding notification")
            let firstEvent = model.events.first { $0.context != nil }!
            await service.enqueue(.init(apple: .timeout, microsoft: .network, appleElapsed: 2, microsoftElapsed: 0.1))
            model.checkConnection(); try await finish(model)
            let failures = model.events.filter { $0.context != nil }
            check(failures.count == 2 && failures.contains(firstEvent),
                  "Two same-text failure events with different outcomes append independently instead of five-minute deduplication")
            check(failures[0].message == failures[1].message && failures[0].context?.status != failures[1].context?.status,
                  "Failure event identity retains the changed actual status behind a shared reviewed message")
        }

        for invalidation in ["cancel", "path", "mode", "area", "sleep"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let service = DiagnosticFixtureService(), clock = DiagnosticTestClock()
            try seedMemory(root, now: clock.date)
            let model = fixture(root, service, clock)
            await service.configureDiscovery(.init(teaching: .unconfirmed, dormitory: .notChecked, issue: .tls), gated: true)
            model.verifyCurrentCampusService(); await service.waitForDiscovery()
            switch invalidation {
            case "cancel": model.cancelCurrentOperation()
            case "path": attach(model, name: "replacement")
            case "mode": model.setMonitoringMode(.manual)
            case "area": model.area = .dormitory
            default: model.setSleeping(true)
            }
            let before = model.events.filter { $0.context != nil }
            await service.releaseDiscovery()
            try await finish(model)
            await Task.yield()
            check(model.events.filter { $0.context != nil } == before,
                  "A late independent verification after \(invalidation) cannot synthesize a failure from newer state")
            check(model.requiredCampusVerificationState != "confirmed", "The late failed \(invalidation) round cannot confirm the current campus service")
            check(await service.submissions == 0, "The \(invalidation) diagnostic fixture submits no credentials")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let clock = DiagnosticTestClock(), service = DiagnosticFixtureService(), reader = DiagnosticObservationGate()
            let holds = [AuthenticationHold(reason: .credentials, area: .teaching, generation: 7, origin: .explicitArea),
                         AuthenticationHold(reason: .uncertain, area: .dormitory, generation: 7, origin: .explicitArea),
                         AuthenticationHold(reason: .security, area: .teaching, generation: 8, origin: .explicitArea)]
            try RecoveryJournal(generation: 7, holds: holds, notBefore: clock.date.addingTimeInterval(45), pendingSubmission: false)
                .write(root.appendingPathComponent("recovery.json"))
            let model = fixture(root, service, clock, reader: reader)
            model.setMonitoringMode(.recover)
            var projection = try object(model)
            var recovery = projection["recovery"] as! [String: Any]
            check(recovery["holdReasons"] as? [String] == ["credentials"] && recovery["paused"] as? Bool == true,
                  "Explicit teaching diagnostics include only holds for the original area and saved credential generation")
            model.area = .dormitory
            recovery = try object(model)["recovery"] as! [String: Any]
            check(recovery["holdReasons"] as? [String] == ["uncertain"], "Explicit dormitory diagnostics do not borrow a derived teaching hold")
            model.area = .automatic
            recovery = try object(model)["recovery"] as! [String: Any]
            check(recovery["holdReasons"] as? [String] == ["uncertain", "credentials"] && recovery["paused"] as? Bool == true,
                  "Automatic area keeps both applicable reasons and the original all-candidates pause rule")
            let previousHolds = model.holds, previousCooldown = model.cooldownRemaining
            model.refreshDisplayedNetwork(); await reader.waitUntilReading()
            projection = try object(model); recovery = projection["recovery"] as! [String: Any]
            let environment = projection["environment"] as! [String: Any]
            check(environment["refreshing"] as? Bool == true && environment["connection"] as? String == "unknown"
                  && environment["systemProxy"] as? String == "unknown" && environment["tun"] as? String == "unknown",
                  "A pending snapshot reports unknown current environment instead of old physical or TUN state")
            check(recovery["networkAvailable"] as? Bool == false && recovery["networkRemembered"] as? Bool == false
                  && recovery["sessionTrusted"] as? Bool == false && projection["physicalNetwork"] == nil,
                  "Pending environment cannot export old physical readiness, memory eligibility or trust")
            check((projection["campusVerification"] as! [String: Any])["state"] as? String == "notChecked",
                  "Pending environment cannot export an old campus confirmation")
            check(recovery["cooldownRemainingSeconds"] as? Int == 45 && model.holds == previousHolds && model.cooldownRemaining == previousCooldown,
                  "Diagnostic refresh preserves applicable holds and remaining cooldown without mutating the plan")
            await reader.release(observation())
            try await waitUntil("fake snapshot application") { !model.networkInformationRefreshing }
            check((try object(model)["environment"] as! [String: Any])["refreshing"] as? Bool == false,
                  "A completed current observation restores diagnostic environment freshness")
            model.setSleeping(true)
            projection = try object(model); recovery = projection["recovery"] as! [String: Any]
            check((projection["environment"] as! [String: Any])["refreshing"] as? Bool == true
                  && recovery["networkAvailable"] as? Bool == false, "Sleep invalidates live diagnostic network readiness")
            model.setSleeping(false)
            check((try object(model)["environment"] as! [String: Any])["refreshing"] as? Bool == true,
                  "Wake without a new observation does not relabel the old network as current")
            check(model.holds == previousHolds && model.cooldownRemaining == previousCooldown,
                  "Sleeping and waking diagnostics preserve sticky protection and cooldown")
            model.pauseAutomaticRecovery()
            let paused = try object(model)["recovery"] as! [String: Any]
            check((paused["holdReasons"] as? [String])?.contains("userPaused") == true
                  && paused["paused"] as? Bool == true && paused["cooldownRemainingSeconds"] as? Int == 45,
                  "A user pause remains explicit in stale-environment diagnostics without shortening cooldown")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let model = fixture(root, DiagnosticFixtureService(), DiagnosticTestClock())
            let raw = "legacy-private-SSID en99 192.0.2.90 account=fixture-only https://private.invalid/secret"
            let event = ConnectionEvent(date: Date(), message: raw)
            model.events = [event]
            let exported = try object(model), exportedEvent = (exported["events"] as! [[String: Any]])[0]
            check(exportedEvent["message"] as? String == SharedCopy.t("diagnostic.legacyEvent") && exportedEvent["context"] == nil,
                  "Legacy arbitrary text exports a reviewed generic event without fabricating a past context")
            check(!String(decoding: model.diagnosticExportData, as: UTF8.self).contains("legacy-private-SSID"),
                  "Legacy free text cannot leak private fixture data through JSON")
            check(model.events == [event], "Export anonymization does not erase the original local legacy event")
            model.events = (0..<305).map { ConnectionEvent(date: Date(), message: "private-event-\($0)") }
            check((try object(model)["events"] as! [Any]).count == 300,
                  "Diagnostic export remains bounded to at most 300 local operation events")
        }
        if let url = reportURL(), let reportToExport {
            try reportToExport.write(to: url, options: .atomic)
            print("Synthetic diagnostic report saved for schema validation.")
        }
        print("DiagnosticHistoryAppModel: \(assertions) assertions passed (fake services, temporary stores, no network or credentials).")
    }
}
