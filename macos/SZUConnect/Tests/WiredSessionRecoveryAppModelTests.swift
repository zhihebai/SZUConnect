import Foundation
import Darwin

/// Exercises session expiry without disconnecting the physical cable. Every probe,
/// campus proof and credential result is scripted; no PortalClient is instantiated.
private actor WiredSessionService: ConnectionService {
    enum Stage: Hashable { case probe, discovery }
    struct Reply: Sendable {
        let result: LoginAttempt
        let submitted: Bool
    }
    struct Attempt: Sendable {
        let area: CampusArea
        let route: String?
        let submitted: Bool
    }
    struct Snapshot: Sendable {
        let bindings: [String?]
        let probes: [String?]
        let discoveries: [CampusArea]
        let discoveryRoutes: [String?]
        let attempts: [Attempt]
        let probeModes: [ConnectivityProbeMode]
        var submissions: [Attempt] { attempts.filter(\.submitted) }
    }
    private var binding: CampusPhysicalRoute?
    private var bindings: [String?] = []
    private var probes: [String?] = []
    private var discoveries: [CampusArea] = []
    private var discoveryRoutes: [String?] = []
    private var attempts: [Attempt] = []
    private var probeModes: [ConnectivityProbeMode] = []
    private var simulatedSession: CampusPortalHint?
    private var simulatedExpired = false
    private var simulatedQuickExposesPortal = false
    private var reports: [ConnectivityReport] = []
    private var discovery = CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
    private var replies: [Reply] = []
    private var paused: Set<Stage> = []
    private var waiting: [Stage: CheckedContinuation<Void, Never>] = [:]

    func configure(reports: [ConnectivityReport], discovery: CampusDiscovery? = nil, replies: [Reply] = []) {
        self.reports = reports
        if let discovery { self.discovery = discovery }
        self.replies = replies
    }
    func pauseNext(_ stage: Stage) { paused.insert(stage) }
    func isWaiting(_ stage: Stage) -> Bool { waiting[stage] != nil }
    func release(_ stage: Stage) {
        guard let pending = waiting.removeValue(forKey: stage) else { fatalError("No suspended fake operation") }
        pending.resume()
    }
    private func suspendIfRequested(_ stage: Stage) async {
        guard paused.remove(stage) != nil else { return }
        // Intentionally returns after cancellation, to test real operation ownership.
        await withCheckedContinuation { waiting[stage] = $0 }
    }
    func setExpectedPhysicalRoute(_ route: CampusPhysicalRoute?) -> Bool {
        binding = route; bindings.append(route?.interfaceName)
        return true
    }
    func resetConnections() { binding = nil }
    func simulateSession(_ area: CampusPortalHint, expired: Bool, quickExposesPortal: Bool = false) {
        simulatedSession = area; simulatedExpired = expired; simulatedQuickExposesPortal = quickExposesPortal
    }
    func probe(mode: ConnectivityProbeMode) async -> ConnectivityReport {
        probeModes.append(mode)
        guard let simulatedSession else { return await probe() }
        probes.append(binding?.interfaceName)
        let second: ConnectivityEndpointResult
        if mode == .quick && !(simulatedExpired && simulatedQuickExposesPortal) {
            second = .init(state: .notCompleted, elapsed: 0)
        } else if simulatedExpired {
            second = .init(state: .redirect, elapsed: 0, campusAreaHint: simulatedSession)
        } else { second = .init(state: .success, elapsed: 0) }
        let report = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0), microsoftResult: second, mode: mode)
        await suspendIfRequested(.probe)
        return report
    }
    func probe() async -> ConnectivityReport {
        probes.append(binding?.interfaceName)
        let report = reports.isEmpty ? ConnectivityReport(apple: false, microsoft: false) : reports.removeFirst()
        await suspendIfRequested(.probe)
        return report
    }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        discoveries.append(area); discoveryRoutes.append(binding?.interfaceName)
        await suspendIfRequested(.discovery)
        return discovery
    }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        guard area != .automatic, credentials.account == "wired-session-fixture-account",
              credentials.password == "wired-session-fixture-password" else {
            fatalError("Unexpected credential fixture or unresolved protocol")
        }
        guard !replies.isEmpty else { fatalError("Unexpected authentication attempt") }
        let reply = replies.removeFirst()
        attempts.append(Attempt(area: area, route: binding?.interfaceName, submitted: reply.submitted))
        if reply.result.outcome == .success { simulatedExpired = false }
        return reply.result
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? {
        fatalError("Session recovery must use fresh staged campus discovery")
    }
    func login(credentials: Credentials, area: CampusArea) async throws -> String {
        fatalError("Session recovery must use classified explicit-area attempts")
    }
    func snapshot() -> Snapshot {
        Snapshot(bindings: bindings, probes: probes, discoveries: discoveries,
                 discoveryRoutes: discoveryRoutes, attempts: attempts, probeModes: probeModes)
    }
}

private actor WiredSessionWaiter {
    private var serial = 0
    private var pending: [Int: (Int, CheckedContinuation<Void, Error>)] = [:]
    func sleep(_ seconds: Int) async throws {
        serial += 1
        let id = serial
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                pending[id] = (seconds, continuation)
            }
        } onCancel: { Task { await self.cancel(id) } }
    }
    private func cancel(_ id: Int) { pending.removeValue(forKey: id)?.1.resume(throwing: CancellationError()) }
    func next() -> (id: Int, delay: Int)? {
        guard let id = pending.keys.min(), let value = pending[id] else { return nil }
        return (id, value.0)
    }
    func release(_ id: Int) {
        guard let value = pending.removeValue(forKey: id) else { fatalError("Missing fake session monitoring wait") }
        value.1.resume()
    }
}

@MainActor private final class WiredSessionClock {
    var date = Date(timeIntervalSince1970: 1_800_000_000)
    func advance(_ seconds: TimeInterval) { date.addTimeInterval(seconds) }
}

@main @MainActor
struct WiredSessionRecoveryAppModelTests {
    private static var assertions = 0
    private static let date = Date(timeIntervalSince1970: 1_800_000_000)
    private static let recordID = "b184abcd"
    private nonisolated static let descriptor = NetworkDescriptor(kind: "ethernet", raw: [
        "gw": "a4:5e:60:91:19:01", "gwip": "172.28.91.1", "net": "172.28.91.0/24",
        "dhcp": "172.28.91.2", "dns": "172.28.91.3"
    ])
    private nonisolated static let otherDescriptor = NetworkDescriptor(kind: "ethernet", raw: [
        "gw": "a4:5e:60:91:19:02", "gwip": "172.27.92.1", "net": "172.27.92.0/24"
    ])
    private static let both = CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
    private static let noIdentity = CampusDiscovery(teaching: .unconfirmed, dormitory: .confirmed)
    private static let online = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.01),
        microsoftResult: .init(state: .success, elapsed: 0.01), mode: .complete)
    private static let offline = ConnectivityReport(apple: false, microsoft: false)
    private static let accepted = WiredSessionService.Reply(result: .success("Fixture accepted"), submitted: true)

    private static func hinted(_ hint: CampusPortalHint, other: CampusPortalHint? = nil) -> ConnectivityReport {
        ConnectivityReport(appleResult: .init(state: other == nil ? .network : .redirect, elapsed: 0.01,
                                              campusAreaHint: other),
            microsoftResult: .init(state: .redirect, elapsed: 0.01, campusAreaHint: hint), mode: .complete)
    }
    private static func check(_ condition: Bool, _ description: String) {
        guard condition else { fatalError("FAIL: \(description)") }
        assertions += 1
        print("PASS: \(description)")
    }
    private static func folder() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("szu-wired-session-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }
    private static func route(_ descriptor: NetworkDescriptor = descriptor, lease: String = "A") -> CampusPhysicalRoute {
        CampusPhysicalRoute(interfaceName: "en12", interfaceIndex: 12, addresses: ["172.28.91.42"],
            dnsServers: ["172.28.91.3"], identity: "fake-wired-route-\(lease)", serviceID: "fake-wired-service",
            descriptor: descriptor, isSystemPrimary: true)
    }
    private static func attach(_ model: AppModel, descriptor: NetworkDescriptor = descriptor, lease: String = "A") {
        var continuity = NetworkContinuitySnapshot(identity: NetworkIdentity(fingerprint: "fake-wired-topology-\(lease)"), linkActive: true)
        continuity.tunnel = true
        continuity.physicalLabels = ["有线 en12"]
        continuity.systemPhysicalLabel = "有线 en12"
        continuity.connectedPhysicalLabels = ["有线 en12"]
        continuity.hasUsablePhysicalLink = true
        continuity.sessionOnlyTrust = true
        continuity.requiresPhysicalBinding = true
        model.applyEnvironmentObservation(PathObservation(usable: true, interfaces: "fixture-wired-tun", constrained: false,
            continuity: continuity, physicalCandidates: [route(descriptor, lease: lease)]))
    }
    @discardableResult private static func seed(_ store: URL, area: String = "dormitory", dormitoryProof: Bool = true) throws -> MemoryRecord {
        var memory = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
        guard memory.remember(descriptor, area: area, proofTeaching: true, proofDormitory: dormitoryProof,
            now: date.addingTimeInterval(-7_200), newID: recordID) != nil,
            let record = memory.records.first else { fatalError("Invalid fake memory") }
        try NetworkMemoryFile.write(memory.encoded(), to: store)
        return record
    }
    private static func fixture(_ root: URL, service: WiredSessionService, clock: WiredSessionClock,
                                area: CampusArea = .automatic, readOnly: Bool = false,
                                hasCredentials: Bool = true, waiter: WiredSessionWaiter? = nil,
                                websites: [WebsiteTarget] = []) -> AppModel {
        let sleep: (@Sendable (Int) async throws -> Void)?
        if let waiter { sleep = { try await waiter.sleep($0) } } else { sleep = nil }
        let model = AppModel(client: service, isolated: true, readOnly: readOnly, now: { clock.date },
            monotonicNow: { clock.date.timeIntervalSince1970 },
            recoveryStore: root.appendingPathComponent("recovery.json"),
            isolatedCredentials: hasCredentials ? Credentials(account: "wired-session-fixture-account", password: "wired-session-fixture-password") : nil,
            initialWebsiteTargets: websites,
            memoryStore: root.appendingPathComponent("network-memory.json"),
            runAutomaticLoopInIsolation: waiter != nil, loopSleep: sleep)
        model.area = area
        model.setProbeMode(.complete)
        model.setMonitoringMode(.recover)
        attach(model)
        return model
    }
    private static func recover(_ model: AppModel) async {
        await model.runAutomaticCycle(); await model.runAutomaticCycle()
    }
    private static func waitUntil(_ description: String, _ condition: @MainActor () async -> Bool) async throws {
        let deadline = ContinuousClock.now.advanced(by: .seconds(4))
        while ContinuousClock.now < deadline {
            if await condition() { return }
            try await Task.sleep(for: .milliseconds(2))
        }
        fatalError("Timed out waiting for \(description)")
    }

    static func main() async throws {
        setbuf(stdout, nil)
        let teachingHint = hinted(.teaching)
        let dormitoryHint = hinted(.dormitory)
        let conflicting = hinted(.teaching, other: .dormitory)

        // The resolver changes a candidate only, not authentication eligibility.
        check(CampusRoutingResolution.resolve(choice: .automatic, report: teachingHint, discovery: both, physicallyBound: true)
              == .selected(.teaching, allowUnsentFallback: false), "A fresh bound teaching redirect selects one teaching candidate")
        check(CampusRoutingResolution.resolve(choice: .automatic, report: teachingHint, discovery: noIdentity, physicallyBound: true)
              == .unconfirmed, "A redirect cannot replace independent HTTPS campus identity")
        check(CampusRoutingResolution.resolve(choice: .automatic, report: dormitoryHint,
            discovery: CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed), physicallyBound: true)
              == .unconfirmed, "A dormitory hint still requires its separate homepage proof")
        check(CampusRoutingResolution.resolve(choice: .automatic, report: conflicting, discovery: both, physicallyBound: true)
              == .conflictingHints, "Conflicting current portal hints never guess a protocol")
        check(CampusRoutingResolution.resolve(choice: .automatic, report: teachingHint, discovery: both, physicallyBound: false)
              == .selected(.automatic, allowUnsentFallback: true), "A system-path hint cannot select the physical authentication protocol")
        check(CampusRoutingResolution.resolve(choice: .automatic, report: offline, discovery: both, physicallyBound: true)
              == .needsSelection, "Two reachable services without a physical hint never default to dormitory submission")
        check(CampusRoutingResolution.resolve(choice: .automatic, report: offline, discovery: both,
            physicallyBound: true, rememberedArea: .teaching) == .selected(.teaching, allowUnsentFallback: false),
              "A remembered teaching candidate remains usable without a conflicting current hint")
        check(CampusRoutingResolution.resolve(choice: .automatic, report: dormitoryHint, discovery: both,
            physicallyBound: true, rememberedArea: .teaching) == .selected(.dormitory, allowUnsentFallback: false),
              "Fresh applicable physical hints take priority over a remembered teaching candidate")
        check(CampusRoutingResolution.resolve(choice: .automatic, report: conflicting, discovery: both,
            physicallyBound: true, rememberedArea: .teaching) == .conflictingHints,
              "Remembered teaching preference cannot hide conflicting current physical hints")
        check(CampusRoutingResolution.resolve(choice: .automatic, report: offline, discovery: both,
            physicallyBound: true, rememberedArea: .dormitory) == .selected(.dormitory, allowUnsentFallback: false),
              "A verified remembered dormitory entry resolves dual reachability without a new user choice")
        check(CampusRoutingResolution.resolve(choice: .automatic, report: teachingHint, discovery: both,
            physicallyBound: true, rememberedArea: .dormitory) == .selected(.teaching, allowUnsentFallback: false),
              "A fresh teaching hint supersedes the historical dormitory candidate")
        check(CampusRoutingResolution.resolve(choice: .automatic, report: conflicting, discovery: both,
            physicallyBound: true, rememberedArea: .dormitory) == .conflictingHints,
              "A remembered dormitory candidate cannot conceal conflicting fresh hints")
        for unconfirmed in [CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed), noIdentity] {
            check(CampusRoutingResolution.resolve(choice: .automatic, report: offline, discovery: unconfirmed,
                physicallyBound: true, rememberedArea: .dormitory) == .unconfirmed,
                  "Remembered dormitory recovery requires fresh applicable proofs and cannot fall through to teaching")
        }
        check(CampusRoutingResolution.resolve(choice: .automatic, report: offline,
            discovery: CampusDiscovery(teaching: .confirmed, dormitory: .confirmed, teachingClientBound: true),
            physicallyBound: true, rememberedArea: .dormitory) == .selected(.teaching, allowUnsentFallback: false),
              "A fresh source-bound controller takes precedence over a remembered dormitory entry")
        for choice in [CampusArea.teaching, .dormitory] {
            check(CampusRoutingResolution.resolve(choice: choice, report: conflicting, discovery: both, physicallyBound: true)
                  == .selected(choice, allowUnsentFallback: false), "An explicit \(choice.rawValue) choice retains priority over redirect hints")
        }

        // A remembered dormitory entry is usable even after a session expires
        // without exposing a redirect. The remembered entry is not fresh proof.
        for dormitoryProof in [false, true] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store, area: "dormitory", dormitoryProof: dormitoryProof)
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock)
            check(model.rememberedCampusArea == (dormitoryProof ? .dormitory : nil),
                  "Dormitory candidate requires the matched record's complete saved proof scope")
            await service.configure(reports: [offline, offline, online], replies: [accepted])
            await model.runAutomaticCycle()
            check(await service.snapshot().submissions.isEmpty,
                  "A remembered entry does not skip the first failed-probe confirmation")
            await model.runAutomaticCycle()
            let snapshot = await service.snapshot()
            check(dormitoryProof ? snapshot.submissions.map(\.area) == [.dormitory] && model.isOnline
                                 : snapshot.attempts.isEmpty && model.entrySelectionNeeded,
                  "Only a qualified dormitory memory resolves and recovers the unchanged attachment automatically")
            if dormitoryProof {
                check(snapshot.discoveryRoutes == ["en12"] && snapshot.attempts.first?.route == "en12",
                      "Remembered dormitory proof and one submission stay on the current bound physical route")
                check(model.diagnosticSummary.contains("source=rememberedDormitory")
                      && !model.currentNetworkAuthorized,
                      "Candidate diagnostics identify the dormitory memory without inventing current trust")
            }
        }

        for outcome in [LoginOutcome.rejected, .uncertain, .security] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root.appendingPathComponent("network-memory.json"))
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock)
            let result = outcome == .rejected
                ? LoginAttempt(outcome: outcome, rejection: .password, error: .rejected)
                : LoginAttempt(outcome: outcome)
            await service.configure(reports: [offline, offline], replies: [
                .init(result: result, submitted: outcome != .security)
            ])
            await recover(model)
            check(await service.snapshot().attempts.map(\.area) == [.dormitory] && !model.holds.isEmpty,
                  "\(outcome): remembered dormitory recovery retains protective holds after one attempt")
            clock.advance(300)
            await service.configure(reports: [offline, offline])
            await recover(model)
            check(await service.snapshot().attempts.count == 1,
                  "\(outcome): remembered preference and elapsed time cannot bypass a protective hold")
        }

        for rememberedArea in [CampusArea.teaching, .dormitory] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root.appendingPathComponent("network-memory.json"), area: rememberedArea.rawValue)
            let service = WiredSessionService(), clock = WiredSessionClock()
            await service.configure(reports: [offline, online], replies: [accepted])
            let model = fixture(root, service: service, clock: clock)
            model.connect()
            try await waitUntil("manual remembered entry") { !model.busy }
            let snapshot = await service.snapshot()
            check(snapshot.submissions.map(\.area) == [rememberedArea] && model.isOnline,
                  "Manual automatic-area login uses the same remembered \(rememberedArea.rawValue) candidate")
            check(!model.entrySelectionNeeded && model.currentNetworkAuthorized,
                  "Only the accepted manual login grants session trust after fresh service proof")
        }

        // A public allowlist may keep Apple reachable after the campus session
        // expires. Quick mode must periodically observe the endpoint it skips.
        for rememberedArea in [CampusArea.teaching, .dormitory] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root.appendingPathComponent("network-memory.json"), area: rememberedArea.rawValue)
            let service = WiredSessionService(), clock = WiredSessionClock()
            let website = WebsiteTarget(id: "session-audit-site", url: "https://session.fixture.invalid/", enabled: true)
            await service.configure(reports: [], replies: Array(repeating: accepted, count: 3))
            let hint: CampusPortalHint = rememberedArea == .teaching ? .teaching : .dormitory
            await service.simulateSession(hint, expired: false)
            let model = fixture(root, service: service, clock: clock, websites: [website])
            model.setProbeMode(.quick)
            await model.runAutomaticCycle()
            let initial = await service.snapshot()
            check(model.isOnline && initial.probeModes == [.quick],
                  "Healthy quick monitoring starts with the user's chosen scope")
            for episode in 1...3 {
                clock.advance(100)
                await service.simulateSession(hint, expired: true)
                await model.runAutomaticCycle()
                let first = await service.snapshot()
                check(first.probeModes.last == .complete && first.submissions.count == episode - 1,
                      "\(rememberedArea.rawValue) episode \(episode): a due base audit reveals the skipped campus redirect without submitting")
                await model.runAutomaticCycle()
                let recovered = await service.snapshot()
                check(recovered.submissions.map(\.area) == Array(repeating: rememberedArea, count: episode)
                      && model.isOnline && model.cooldownRemaining != nil,
                      "\(rememberedArea.rawValue) episode \(episode): two fresh observations allow one bound login and retain cooldown")
                check(model.probeMode == .quick && model.customWebsiteTargets == [website]
                      && model.lastInternetCheck?.websitesDeferred == true && model.lastInternetCheck?.includedCount == 0,
                      "Base audits preserve quick mode and configured websites without requesting optional targets")
                check(model.recommendedCheckDelay == 2,
                      "A verified recovery promptly schedules the second read-only base confirmation")
                await model.runAutomaticCycle()
                let confirmed = await service.snapshot()
                check(!model.partialRecoveryAttempted && confirmed.submissions.count == episode,
                      "Two complete base confirmations rearm future session recovery without another credential submission")
                await model.runAutomaticCycle()
                check((await service.snapshot()).probeModes.last == .quick,
                      "A healthy audit returns later monitoring to quick mode")
            }
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root.appendingPathComponent("network-memory.json"))
            let service = WiredSessionService(), clock = WiredSessionClock()
            await service.configure(reports: [], replies: [accepted])
            await service.simulateSession(.dormitory, expired: true, quickExposesPortal: true)
            let model = fixture(root, service: service, clock: clock)
            model.setProbeMode(.quick)
            await recover(model)
            let snapshot = await service.snapshot()
            check(snapshot.submissions.map(\.area) == [.dormitory] && model.isOnline,
                  "A quick probe's genuine campus redirect cannot be hidden by its passing public peer")
            check(Array(snapshot.probeModes.prefix(2)) == [.quick, .complete] && model.probeMode == .quick,
                  "Observed portal evidence triggers confirmation without waiting for the periodic audit deadline")
        }

        for condition in ["paused", "readOnly", "missingCredentials"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root.appendingPathComponent("network-memory.json"))
            let service = WiredSessionService(), clock = WiredSessionClock()
            await service.simulateSession(.dormitory, expired: false)
            let model = fixture(root, service: service, clock: clock, readOnly: condition == "readOnly",
                                hasCredentials: condition != "missingCredentials")
            model.setProbeMode(.quick)
            await model.runAutomaticCycle()
            if condition == "paused" { model.pauseAutomaticRecovery() }
            clock.advance(100)
            await service.simulateSession(.dormitory, expired: true, quickExposesPortal: true)
            await recover(model)
            check((await service.snapshot()).attempts.isEmpty,
                  "Periodic session checks and portal hints cannot bypass \(condition)")
        }

        for boundary in ["cancel", "forget", "observe", "pause", "change"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root.appendingPathComponent("network-memory.json"))
            let service = WiredSessionService(), clock = WiredSessionClock()
            await service.simulateSession(.dormitory, expired: false)
            let model = fixture(root, service: service, clock: clock)
            model.setProbeMode(.quick)
            await model.runAutomaticCycle()
            clock.advance(100)
            await service.simulateSession(.dormitory, expired: true)
            await model.runAutomaticCycle()
            await service.pauseNext(.discovery)
            let task = Task { await model.runAutomaticCycle() }
            try await waitUntil("in-flight session audit proof") { await service.isWaiting(.discovery) }
            switch boundary {
            case "cancel": model.cancelCurrentOperation()
            case "forget": model.forgetCurrentNetwork()
            case "observe": model.setMonitoringMode(.observe)
            case "pause": model.pauseAutomaticRecovery()
            default: attach(model, descriptor: otherDescriptor, lease: "session-audit-changed")
            }
            await service.release(.discovery); await task.value
            check((await service.snapshot()).attempts.isEmpty,
                  "A delayed session-audit proof cannot authenticate after \(boundary)")
        }

        // Drive the actual monitoring loop with a virtual waiter. Session loss is
        // scripted after a healthy round, without another physical observation.
        for rememberedArea in [CampusArea.teaching, .dormitory] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root.appendingPathComponent("network-memory.json"), area: rememberedArea.rawValue)
            let service = WiredSessionService(), clock = WiredSessionClock(), waiter = WiredSessionWaiter()
            await service.configure(reports: [online, offline, offline, online], replies: [accepted])
            let model = fixture(root, service: service, clock: clock, waiter: waiter)
            var previousWait = 0
            for round in 1...3 {
                try await waitUntil("scheduled session round") { await waiter.next().map { $0.id > previousWait } ?? false }
                guard let next = await waiter.next() else { fatalError("Missing scheduled fake wait") }
                previousWait = next.id
                clock.advance(TimeInterval(next.delay)); await waiter.release(next.id)
                try await waitUntil("scheduled session round completion") {
                    let pending = await waiter.next()
                    return !model.busy && (pending.map { $0.id > previousWait } ?? false)
                }
                let snapshot = await service.snapshot()
                if round == 1 {
                    check(model.isOnline && snapshot.attempts.isEmpty,
                          "Scheduled \(rememberedArea.rawValue) monitoring starts online without authentication")
                } else if round == 2 {
                    check(snapshot.attempts.isEmpty,
                          "One scheduled session failure remains a read-only confirmation round")
                } else {
                    check(snapshot.submissions.map(\.area) == [rememberedArea] && model.isOnline,
                          "Scheduled \(rememberedArea.rawValue) session recovery runs without a link event or user action")
                    check(snapshot.probes.allSatisfy { $0 == "en12" } && snapshot.discoveryRoutes == ["en12", "en12"],
                          "Scheduled recovery obtains new bound campus proof rather than reusing its healthy-round proof")
                    check(model.nextCheckAt != nil && !model.currentNetworkAuthorized,
                          "Successful automatic session recovery rearms monitoring without inventing session trust")
                }
            }
            check(await model.prepareForAppMaintenance(),
                  "The simulated scheduled recovery loop cancels and drains cleanly")
        }

        // A still-connected wired link can lose its authenticated session. No path event,
        // Wi-Fi scan, power change or manual trust action is needed for recovery.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store)
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock)
            model.wifiRecovery.setAutoJoinEnabled(true)
            model.wifiRecovery.setPowerOnWhenDisconnected(true)
            await service.configure(reports: [online, teachingHint, teachingHint, online], replies: [accepted])
            await model.runAutomaticCycle()
            check(model.isOnline && model.wifiRecovery.hasExistingConnection,
                  "A wired link is physically connected while its authenticated session is initially online")
            let initial = await service.snapshot()
            check(initial.attempts.isEmpty, "A working wired session never submits authentication")
            check(initial.discoveries == [.automatic] && initial.discoveryRoutes == ["en12"],
                  "The working wired session confirms its remembered campus service on the same physical binding")
            await model.runAutomaticCycle()
            check(await service.snapshot().attempts.isEmpty, "One expired-session probe does not submit credentials")
            await model.runAutomaticCycle()
            let snapshot = await service.snapshot()
            check(snapshot.attempts.count == 1 && snapshot.submissions.count == 1 && snapshot.attempts.first?.area == .teaching,
                  "Two failures on the unchanged wired link use one teaching submission despite both homepages being reachable")
            check(snapshot.probes.allSatisfy { $0 == "en12" }
                  && snapshot.discoveries == [.automatic, .automatic] && snapshot.discoveryRoutes == ["en12", "en12"]
                  && snapshot.attempts.first?.route == "en12", "Probe, independent proof and submission retain the same physical binding")
            check(snapshot.discoveries.count == initial.discoveries.count + 1,
                  "Recovery obtains new campus proof instead of reusing the initial online confirmation")
            check(model.isOnline && !model.currentNetworkAuthorized && model.holds.isEmpty,
                  "A successful verification probe restores service without inventing session trust")
            let saved = NetworkMemory.decode(try Data(contentsOf: store), now: clock.date)?.records.first
            check(saved?.id == original.id && saved?.features == original.features,
                  "Successful teaching recovery preserves the existing physical memory identity and features")
            check(saved?.area == "teaching" && saved?.proofTeaching == true && saved?.proofDormitory == true,
                  "Only the successful authentication updates its entry label while retaining both old service proofs")
            check(saved?.verified == clock.date.timeIntervalSince1970,
                  "Fresh verification of both campus homepages can renew their shared proof age")
            check(model.wifiRecovery.autoJoinEnabled && model.wifiRecovery.hasExistingConnection
                  && model.wifiRecovery.status == .connected,
                  "Even enabled Wi-Fi finding remains stopped while the wired session is reauthenticated")
        }

        // A teaching-only current proof must not extend a historical dormitory
        // proof merely because teaching authentication was accepted afterwards.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store)
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock)
            await service.configure(reports: [teachingHint, teachingHint, online],
                discovery: CampusDiscovery(teaching: .confirmed, dormitory: .notChecked), replies: [accepted])
            await recover(model)
            let snapshot = await service.snapshot()
            let saved = NetworkMemory.decode(try Data(contentsOf: store), now: clock.date)?.records.first
            check(snapshot.submissions.count == 1 && snapshot.submissions.first?.area == .teaching && model.isOnline,
                  "A fresh teaching proof can complete one teaching recovery without pretending the dormant portal was checked")
            check(saved?.area == "teaching" && saved?.proofTeaching == true && saved?.proofDormitory == true,
                  "Accepted teaching authentication updates the entry preference without discarding historical proof flags")
            check(saved?.verified == original.verified && saved?.created == original.created,
                  "Teaching success alone cannot renew a historical dormitory proof's verification or creation time")
            check(saved?.id == original.id && saved?.features == original.features,
                  "Updating an accepted entry preference preserves the same physical memory record")
        }

        // A genuine pre-submit failure is not permission to disregard the observed portal.
        for outcome in [LoginOutcome.unavailable, .challengeRefused] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store)
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock)
            await service.configure(reports: [teachingHint, teachingHint, online], replies: [
                .init(result: LoginAttempt(outcome: outcome), submitted: false), accepted
            ])
            await recover(model)
            let snapshot = await service.snapshot()
            check(snapshot.attempts.map(\.area) == [.teaching] && snapshot.submissions.isEmpty,
                  "\(outcome.rawValue): the hinted teaching preflight never tries a second protocol")
            check(snapshot.discoveries == [.automatic], "\(outcome.rawValue): no fallback dormitory discovery is launched")
            let saved = NetworkMemory.decode(try Data(contentsOf: store), now: clock.date)?.records.first
            check(saved?.area == original.area && saved?.proofDormitory == original.proofDormitory,
                  "\(outcome.rawValue): discovery and an unsent attempt do not rewrite the remembered authentication entry")
        }

        for variant in ["conflicting", "unconfirmed", "readOnly", "unknown", "missingCredentials"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            if variant != "unknown" { _ = try seed(store) }
            let before = try? Data(contentsOf: store)
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock,
                readOnly: variant == "readOnly", hasCredentials: variant != "missingCredentials")
            let report = variant == "conflicting" ? conflicting : teachingHint
            await service.configure(reports: [report, report], discovery: variant == "unconfirmed" ? noIdentity : both)
            await recover(model)
            let snapshot = await service.snapshot()
            check(snapshot.attempts.isEmpty, "\(variant): a portal hint never bypasses authentication protection")
            if variant == "readOnly" {
                check((try? Data(contentsOf: store)) == before, "Read-only session checks preserve exact network-memory bytes")
            }
            if variant == "unknown" || variant == "missingCredentials" {
                check(snapshot.discoveries.isEmpty, "\(variant): ineligible recovery stops before campus discovery")
            }
        }

        // A user explicitly choosing a protocol still owns that choice.
        for variant in ["explicitTeaching", "explicitDormitory"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store)
            let service = WiredSessionService(), clock = WiredSessionClock()
            let choice: CampusArea = variant == "explicitTeaching" ? .teaching : variant == "explicitDormitory" ? .dormitory : .automatic
            let expected: CampusArea = variant == "explicitDormitory" ? .dormitory : .teaching
            let model = fixture(root, service: service, clock: clock, area: choice)
            await service.configure(reports: [conflicting, conflicting, online], replies: [accepted])
            await recover(model)
            let snapshot = await service.snapshot()
            check(snapshot.attempts.map(\.area) == [expected] && snapshot.submissions.count == 1,
                  "\(variant): an explicit protocol is not overridden by probe conflicts")
        }

        // A remembered network with no stored entry still cannot choose a protocol
        // from dual reachability alone. Keep the actual selection action available.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root.appendingPathComponent("network-memory.json"), area: "")
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock)
            await service.configure(reports: [offline, offline, offline])
            await recover(model)
            check(await service.snapshot().attempts.isEmpty && model.entrySelectionNeeded,
                  "Ambiguous wired protocol selection requests a choice without attempting either protocol")
            check(model.holds.isEmpty && model.cooldownRemaining == nil,
                  "An unanswered area choice creates neither an uncertain hold nor a submission cooldown")
            check(model.presentation.primary == "area" && model.presentation.detail == "campus.entry.bothDetected",
                  "The visible action opens login-area selection instead of presenting unrelated error actions")
            let summary = model.diagnosticSummary
            check(summary.contains("source=needsSelection") && summary.contains("teaching=confirmed")
                  && summary.contains("dormitory=confirmed") && !summary.contains("wired-session-fixture"),
                  "Selection evidence is exported as typed protocol states without fixture credentials")
            model.area = .teaching
            await service.configure(reports: [offline, offline, online], replies: [accepted])
            await recover(model)
            check(await service.snapshot().submissions.map(\.area) == [.teaching] && model.isOnline,
                  "An explicit teaching selection can recover the unchanged wire with one accepted submission")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root.appendingPathComponent("network-memory.json"), area: "")
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock)
            let partial = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.01),
                microsoftResult: .init(state: .timeout, elapsed: 0.01), mode: .complete)
            await service.configure(reports: Array(repeating: partial, count: 4))
            // A prioritized quick round can schedule a read-only full supplement;
            // allow both complete confirmations before expecting recovery planning.
            for _ in 0..<4 {
                await model.runAutomaticCycle()
                if model.entrySelectionNeeded { break }
            }
            check(await service.snapshot().attempts.isEmpty && model.isOnline && model.entrySelectionNeeded
                  && model.presentation.primary == "area",
                  "A partial base success does not hide the unresolved login-area control")
            await service.configure(reports: [online])
            await model.runAutomaticCycle()
            check(!model.entrySelectionNeeded && model.presentation.primary == "check",
                  "A later complete connectivity success clears the stale area-choice presentation")
        }

        for conflictingHints in [false, true] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root.appendingPathComponent("network-memory.json"), area: "teaching")
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock)
            await service.configure(reports: conflictingHints ? [conflicting, conflicting] : [offline, offline, online],
                replies: conflictingHints ? [] : [accepted])
            await recover(model)
            let snapshot = await service.snapshot()
            check(snapshot.discoveries == [conflictingHints ? .automatic : .teaching],
                  "Only a contradictory current hint broadens a teaching memory's HTTPS-only discovery")
            check(conflictingHints ? snapshot.attempts.isEmpty : snapshot.submissions.map(\.area) == [.teaching],
                  "A teaching preference is used only when current physical hints do not conflict")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            _ = try seed(store, area: "teaching", dormitoryProof: false)
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock)
            await service.configure(reports: [dormitoryHint, dormitoryHint])
            await recover(model)
            check(await service.snapshot().attempts.isEmpty && model.engineStatus == "needsTrust",
                  "A fresh dormitory hint cannot upgrade a teaching-only memory into plaintext credential consent")
            let saved = NetworkMemory.decode(try Data(contentsOf: store), now: clock.date)?.records.first
            check(saved?.area == "teaching" && saved?.proofDormitory == false,
                  "Discovering a different entry never silently upgrades the stored proof scope")
        }

        // Freshly seeing both homepages is not evidence that an already successful
        // teaching attachment should be relabelled as dormitory authentication.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            let original = try seed(store, area: "teaching")
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock)
            await service.configure(reports: [dormitoryHint, dormitoryHint], replies: [
                .init(result: LoginAttempt(outcome: .challengeRefused), submitted: false)
            ])
            await recover(model)
            let saved = NetworkMemory.decode(try Data(contentsOf: store), now: clock.date)?.records.first
            check(saved?.area == "teaching" && saved?.proofTeaching == true && saved?.proofDormitory == true,
                  "Read-only dual-homepage discovery never rewrites an existing teaching entry or its proofs")
            check(saved?.id == original.id && saved?.features == original.features,
                  "An unsent candidate preflight preserves the physical attachment's durable memory")
            check(await service.snapshot().attempts.map(\.area) == [.dormitory],
                  "A fresh dormitory hint can select its independently verified service without rewriting teaching memory")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root.appendingPathComponent("network-memory.json"), area: "teaching")
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock)
            await service.configure(reports: [teachingHint, teachingHint], replies: [
                .init(result: LoginAttempt(outcome: .rejected, rejection: .areaMismatch, error: .rejected), submitted: true)
            ])
            await recover(model)
            check(model.holds.first?.origin == .candidate && model.holds.first?.sticky == false,
                  "A remembered teaching pin does not turn automatic area mismatch into a sticky explicit-choice hold")
            check(await service.snapshot().submissions.count == 1,
                  "A remembered teaching mismatch never triggers a second protocol submission")
            attach(model, descriptor: otherDescriptor, lease: "after-mismatch")
            check(model.holds.isEmpty, "A network-scoped teaching candidate mismatch is released after changing the physical attachment")
        }

        // Selecting an account's teaching entry cannot stand in for remembering a
        // bound cable. The setup checklist uses the same physical eligibility.
        for remembered in [false, true] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            if remembered { _ = try seed(root.appendingPathComponent("network-memory.json"), area: "teaching") }
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock, area: .teaching)
            check(model.recoverySetupHasNetwork == remembered,
                  "Bound teaching setup \(remembered ? "accepts an actual physical memory" : "does not substitute area selection for physical memory")")
            check(await service.snapshot().attempts.isEmpty,
                  "Reading bound teaching setup readiness never submits authentication")
        }

        // The redirect is still an automatic candidate. It cannot change rejection
        // origin, bypass the submission cooldown, or clear sticky safety holds.
        for variant in ["password", "areaMismatch", "uncertain", "security"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root.appendingPathComponent("network-memory.json"))
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock)
            let outcome: LoginOutcome = variant == "uncertain" ? .uncertain : variant == "security" ? .security : .rejected
            let attempt = outcome == .rejected
                ? LoginAttempt(outcome: .rejected, rejection: variant == "password" ? .password : .areaMismatch, error: .rejected)
                : LoginAttempt(outcome: outcome)
            await service.configure(reports: [teachingHint, teachingHint], replies: [.init(result: attempt, submitted: outcome != .security)])
            await recover(model)
            let first = await service.snapshot()
            check(first.attempts.count == 1 && first.attempts.first?.area == .teaching,
                  "\(variant): the initial candidate is attempted only once")
            if outcome == .rejected {
                check(model.holds.first?.origin == .candidate && model.holds.first?.sticky == (variant == "password"),
                      "\(variant): a redirect-selected rejection retains the existing automatic candidate hold scope")
            }
            await service.configure(reports: [teachingHint, teachingHint])
            await recover(model)
            check(await service.snapshot().attempts.count == 1,
                  "\(variant): a fresh redirect cannot bypass the submission cooldown or hold")
            clock.advance(300)
            await service.configure(reports: [teachingHint, teachingHint])
            await recover(model)
            check(await service.snapshot().attempts.count == 1 && !model.holds.isEmpty,
                  "\(variant): time alone and a portal hint cannot release an unresolved protective hold")
        }

        // A result-unknown hold is global even when the user corrects the entry.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root.appendingPathComponent("network-memory.json"))
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock)
            await service.configure(reports: [dormitoryHint, dormitoryHint], replies: [
                .init(result: LoginAttempt(outcome: .uncertain), submitted: true)
            ])
            await recover(model)
            model.area = .teaching
            clock.advance(300)
            await service.configure(reports: [teachingHint, teachingHint])
            await recover(model)
            check(await service.snapshot().submissions.map(\.area) == [.dormitory]
                  && model.holds.contains(where: { $0.reason == .uncertain }),
                  "Choosing a different entry after an uncertain request never creates a second automatic submission")
        }

        // Late reports cannot borrow the previous attachment's route hint or proof.
        for stage in [WiredSessionService.Stage.probe, .discovery] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            _ = try seed(root.appendingPathComponent("network-memory.json"))
            let service = WiredSessionService(), clock = WiredSessionClock()
            let model = fixture(root, service: service, clock: clock)
            await service.configure(reports: [teachingHint, teachingHint])
            await model.runAutomaticCycle()
            await service.pauseNext(stage)
            let pending = Task { await model.runAutomaticCycle() }
            try await waitUntil("gated \(stage)") { await service.isWaiting(stage) }
            attach(model, descriptor: otherDescriptor, lease: "changed")
            await service.release(stage)
            await pending.value
            check(await service.snapshot().attempts.isEmpty, "A late \(stage) result cannot authenticate a changed physical attachment")
            check(!model.networkRemembered, "A changed physical attachment cannot inherit the old memory's eligibility")
        }

        print("Wired session recovery AppModel tests: \(assertions) assertions passed")
    }
}
