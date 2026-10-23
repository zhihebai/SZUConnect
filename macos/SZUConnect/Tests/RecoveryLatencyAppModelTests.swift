import Foundation
import Darwin

/// In-memory transport only. Gates ignore cancellation to exercise stale operation rejection.
private actor LatencyService: ConnectionService {
    enum Stage: Hashable, Sendable { case probe, discovery }
    struct Snapshot: Sendable {
        let modes: [ConnectivityProbeMode]
        let discoveries: Int
        let attempts: [CampusArea]
    }
    private var reports: [ConnectivityReport] = []
    private var discoveries: [CampusDiscovery] = []
    private var replies: [LoginAttempt] = []
    private var modes: [ConnectivityProbeMode] = []
    private var discoveryCount = 0
    private var attempts: [CampusArea] = []
    private var pauses: Set<Stage> = []
    private var gates: [Stage: CheckedContinuation<Void, Never>] = [:]
    func configure(reports: [ConnectivityReport] = [], discoveries: [CampusDiscovery] = [], replies: [LoginAttempt] = []) {
        self.reports = reports; self.discoveries = discoveries; self.replies = replies
    }
    func pauseNext(_ stage: Stage) { pauses.insert(stage) }
    func waiting(_ stage: Stage) -> Bool { gates[stage] != nil }
    func release(_ stage: Stage) { gates.removeValue(forKey: stage)?.resume() }
    private func waitIfNeeded(_ stage: Stage) async {
        guard pauses.remove(stage) != nil else { return }
        await withCheckedContinuation { gates[stage] = $0 }
    }
    func setExpectedPhysicalRoute(_ route: CampusPhysicalRoute?) async -> Bool { true }
    func probe() async -> ConnectivityReport { await probe(mode: .quick) }
    func probe(mode: ConnectivityProbeMode) async -> ConnectivityReport {
        modes.append(mode)
        let report = reports.isEmpty ? ConnectivityReport(apple: false, microsoft: false) : reports.removeFirst()
        await waitIfNeeded(.probe)
        return ConnectivityReport(appleResult: report.appleResult, microsoftResult: report.microsoftResult, mode: mode)
    }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        discoveryCount += 1
        let discovery = discoveries.isEmpty ? CampusDiscovery(teaching: .confirmed, dormitory: .confirmed) : discoveries.removeFirst()
        await waitIfNeeded(.discovery)
        return discovery
    }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        attempts.append(area)
        return replies.isEmpty ? LoginAttempt(outcome: .unavailable) : replies.removeFirst()
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? { nil }
    func login(credentials: Credentials, area: CampusArea) async throws -> String { throw URLError(.cannotConnectToHost) }
    func snapshot() -> Snapshot { Snapshot(modes: modes, discoveries: discoveryCount, attempts: attempts) }
}

private actor LatencyWebsiteChecker: WebsiteChecking {
    private var calls = 0
    private var pause = false
    private var gate: CheckedContinuation<Void, Never>?
    func pauseNext() { pause = true }
    func waiting() -> Bool { gate != nil }
    func release() { gate?.resume(); gate = nil }
    func count() -> Int { calls }
    func probe(_ input: String, strategy: WebsiteProbeStrategy) async -> WebsiteProbeResult {
        calls += 1
        if pause {
            pause = false
            await withCheckedContinuation { gate = $0 }
        }
        return WebsiteProbeResult.failure(.timedOut, url: URL(string: input)!, strategy: strategy, elapsed: 20)
    }
}

/// Every retry wait is explicitly released. Only the injected clock advances time.
private actor LatencySleeper {
    struct Pending: Sendable { let id: UUID; let seconds: Int }
    private var waiting: [(Pending, CheckedContinuation<Void, Error>)] = []
    func sleep(_ seconds: Int) async throws {
        let id = UUID()
        try Task.checkCancellation()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                waiting.append((Pending(id: id, seconds: seconds), continuation))
            }
        } onCancel: { Task { await self.cancel(id) } }
        try Task.checkCancellation()
    }
    private func cancel(_ id: UUID) {
        guard let index = waiting.firstIndex(where: { $0.0.id == id }) else { return }
        waiting.remove(at: index).1.resume(throwing: CancellationError())
    }
    func pending() -> [Pending] { waiting.map { $0.0 } }
    func releaseFirst() { if !waiting.isEmpty { waiting.removeFirst().1.resume() } }
}

@MainActor private final class LatencyClock {
    var date = Date(timeIntervalSince1970: 1_800_000_000)
    func advance(_ seconds: TimeInterval) { date.addTimeInterval(seconds) }
}

@main @MainActor
struct RecoveryLatencyAppModelTests {
    private static var passed = 0
    private static var failed = 0
    private static let offline = ConnectivityReport(apple: false, microsoft: false)
    private static let online = ConnectivityReport(apple: true, microsoft: true)
    private static let campus = CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
    private static let unconfirmed = CampusDiscovery(teaching: .unconfirmed, dormitory: .unconfirmed)
    private static let accepted = LoginAttempt.success("Fixture accepted")
    private static let date = Date(timeIntervalSince1970: 1_800_000_000)
    private nonisolated static let wiredIdentity = NetworkIdentity(fingerprint: "latency-wired")
    private nonisolated static let wifiIdentity = NetworkIdentity(fingerprint: "latency-wifi")
    private nonisolated static let nextIdentity = NetworkIdentity(fingerprint: "latency-next-wifi")
    private nonisolated static let wifi = NetworkDescriptor(kind: "wifi", raw: [
        "gw": "a4:5e:60:73:22:01", "gwip": "172.29.22.1", "net": "172.29.22.0/24"
    ])
    private nonisolated static let wired = NetworkDescriptor(kind: "ethernet", raw: [
        "gw": "a4:5e:60:73:22:02", "gwip": "172.28.22.1", "net": "172.28.22.0/24"
    ])
    private static func check(_ condition: Bool, _ description: String) {
        if condition { passed += 1; print("PASS: \(description)") }
        else { failed += 1; print("FAIL: \(description)") }
    }
    @discardableResult
    private static func waitUntil(_ description: String, _ condition: @MainActor () async -> Bool) async -> Bool {
        let deadline = ContinuousClock.now.advanced(by: .seconds(4))
        while ContinuousClock.now < deadline {
            if await condition() { return true }
            try? await Task.sleep(for: .milliseconds(1))
        }
        check(false, "Timed out: \(description)")
        return false
    }
    private static func attachWifi(_ model: AppModel, identity: NetworkIdentity = wifiIdentity) {
        model.receivePath(usable: true, interfaces: "Wi-Fi en0", identity: identity, descriptors: [wifi])
    }
    private static func fixture(complete: Bool = true, transition: Bool = true, readOnly: Bool = false, area: CampusArea = .teaching,
                                recoveryStore: URL? = nil, sleeper: LatencySleeper? = nil)
        -> (AppModel, LatencyService, LatencyWebsiteChecker, LatencyClock) {
        let service = LatencyService()
        let checker = LatencyWebsiteChecker()
        let clock = LatencyClock()
        var wait: (@Sendable (Int) async throws -> Void)?
        if let sleeper { wait = { @Sendable seconds in try await sleeper.sleep(seconds) } }
        let model = AppModel(client: service, isolated: true, readOnly: readOnly, now: { clock.date },
            recoveryStore: recoveryStore,
            isolatedCredentials: Credentials(account: "latency-fixture", password: "latency-fixture-password"),
            websiteChecker: checker, initialWebsiteTargets: [WebsiteTarget(id: "latency-website", url: "https://example.com")],
            runAutomaticLoopInIsolation: sleeper != nil, loopSleep: wait)
        model.area = area
        if complete { model.setProbeMode(.complete) }
        if transition { model.receivePath(usable: true, interfaces: "Ethernet en12", identity: wiredIdentity, descriptors: [wired]) }
        attachWifi(model)
        model.setMonitoringMode(.recover)
        return (model, service, checker, clock)
    }
    private static func folder() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("szu-recovery-latency-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    static func main() async throws {
        setbuf(stdout, nil)
        // The complete, interface-bound success on A must not delay fresh recovery on B.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("memory.json")
            var memory = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
            _ = memory.remember(wired, area: "teaching", proofTeaching: true, proofDormitory: false, now: date, newID: "1234ab22")
            _ = memory.remember(wifi, area: "teaching", proofTeaching: true, proofDormitory: false, now: date, newID: "5678ab22")
            try NetworkMemoryFile.write(memory.encoded(), to: store)
            let service = LatencyService(); let clock = LatencyClock()
            let journal = root.appendingPathComponent("recovery.json")
            let model = AppModel(client: service, isolated: true, now: { clock.date }, recoveryStore: journal,
                isolatedCredentials: Credentials(account: "fixture", password: "fixture-password"), memoryStore: store)
            model.area = .teaching; model.setMonitoringMode(.recover)
            func attach(_ wireless: Bool, identity: String? = nil, ip: String = "192.0.2.7", tunnel: Bool = true) {
                let name = wireless ? "en0" : "en12"
                let route = CampusPhysicalRoute(interfaceName: name, interfaceIndex: wireless ? 1 : 12,
                    addresses: [ip], dnsServers: ["192.0.2.53"], identity: identity ?? name,
                    descriptor: wireless ? wifi : wired)
                model.receivePath(usable: true, interfaces: wireless ? "Wi-Fi en0" : "Ethernet en12",
                    identity: NetworkIdentity(fingerprint: route.identity), tunnel: tunnel, descriptors: [wireless ? wifi : wired],
                    authenticationRoute: route, requiresPhysicalBinding: true)
            }
            attach(false)
            await service.configure(reports: [offline, offline, online], discoveries: [campus], replies: [accepted])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(model.isOnline && model.cooldownRemaining == 90, "Complete bound wired success keeps its own ninety-second protection")
            check(!model.recoverySummary.contains("等待登录"), "Online recovery summary never reports waiting for an already working login")
            attach(true)
            check(model.cooldownRemaining == nil, "Settled wired success permits independent Wi-Fi recovery before its old deadline")
            await service.configure(reports: [offline, offline], discoveries: [campus], replies: [LoginAttempt(outcome: .unavailable)])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(model.cooldownRemaining == nil, "Definitely-unsent B preflight restores A's settled interface scope")
            attach(false)
            check(model.cooldownRemaining == 90, "Unsent B preflight leaves A deadline unchanged")
            attach(true)
            await service.configure(reports: [offline, offline, online], discoveries: [campus], replies: [accepted])
            await model.runAutomaticCycle()
            check(await service.snapshot().attempts.count == 2 && model.recommendedCheckDelay == 1,
                  "New Wi-Fi gets one-second confirmation while still requiring two fresh failures")
            await model.runAutomaticCycle()
            check(await service.snapshot().attempts.count == 3 && model.isOnline,
                  "Reverified remembered Wi-Fi authenticates exactly once without inheriting the wired wait")
            check(await service.snapshot().modes.suffix(3) == [.quick, .quick, .complete],
                  "Even quick preference verifies accepted physical authentication with both complete base endpoints")
            let restoredService = LatencyService()
            let restored = AppModel(client: restoredService, isolated: true, now: { clock.date }, recoveryStore: journal,
                isolatedCredentials: Credentials(account: "fixture", password: "fixture-password"), memoryStore: store)
            restored.area = .teaching; restored.setMonitoringMode(.recover)
            let restoredRoute = CampusPhysicalRoute(interfaceName: "en12", interfaceIndex: 12,
                addresses: ["192.0.2.7"], dnsServers: ["192.0.2.53"], identity: "en12", descriptor: wired)
            restored.receivePath(usable: true, interfaces: "Ethernet en12", identity: wiredIdentity, descriptors: [wired],
                authenticationRoute: restoredRoute, requiresPhysicalBinding: true)
            await restoredService.configure(reports: [offline, offline])
            await restored.runAutomaticCycle(); await restored.runAutomaticCycle()
            check(await restoredService.snapshot().attempts.isEmpty && restored.cooldownRemaining == 90,
                  "Restart restores conservative global protection without reusing previous RAM-only interface proof")
            let journalText = try String(contentsOf: journal, encoding: .utf8)
            check(!journalText.contains("en0") && !journalText.contains("en12") && !journalText.contains("192.0.2"),
                  "Persistent recovery journal never includes raw physical interface, address or scope identity")
            attach(false)
            check(model.cooldownRemaining == 90, "A to B to A retains A's original ninety-second deadline")
            await service.configure(reports: [offline, offline], discoveries: [campus], replies: [accepted])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.snapshot().attempts.count == 3 && model.engineStatus == "coolingDown",
                  "Returning to A cannot submit again before its existing deadline")
            attach(true, identity: "same-radio-new-ssid-or-lease", ip: "192.0.2.8")
            check(model.cooldownRemaining == 90, "SSID, subnet or IP change on the same radio cannot create fresh submission permission")
            attach(true, identity: "same-radio-new-ssid-or-lease", ip: "192.0.2.8", tunnel: false)
            check(model.cooldownRemaining == 90, "TUN toggles cannot create an independent physical cooldown scope")
            model.receivePath(usable: false, interfaces: "", linkActive: false)
            attach(true)
            check(model.cooldownRemaining == 90, "Disconnecting and reattaching the same radio preserves protection")
            clock.advance(90)
            await service.configure(reports: [offline, offline, online], discoveries: [campus], replies: [accepted])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.snapshot().attempts.count == 4 && model.isOnline,
                  "Expired physical deadline allows one fresh-proven recovery")
        }
        for outcome in [LoginOutcome.success, .uncertain, .cancelled, .security, .rejected] {
            let service = LatencyService(); let clock = LatencyClock()
            let model = AppModel(client: service, isolated: true, now: { clock.date },
                isolatedCredentials: Credentials(account: "fixture", password: "fixture-password"))
            model.area = .teaching
            func attach(_ wireless: Bool) {
                let name = wireless ? "en0" : "en12"
                let route = CampusPhysicalRoute(interfaceName: name, interfaceIndex: wireless ? 1 : 12,
                    addresses: ["192.0.2.7"], dnsServers: ["192.0.2.53"], identity: name,
                    descriptor: wireless ? wifi : wired)
                model.receivePath(usable: true, interfaces: name, identity: NetworkIdentity(fingerprint: name),
                    descriptors: [wireless ? wifi : wired], authenticationRoute: route, requiresPhysicalBinding: true)
            }
            attach(false)
            await service.configure(reports: [offline, offline, offline, offline], replies: [LoginAttempt(outcome: outcome)])
            model.connect(); await waitUntil("owned unverified authentication result") { !model.busy }
            attach(true)
            check(model.cooldownRemaining == 90, "\(outcome) without settled connectivity retains global protection after physical switch")
        }

        // A wired-to-Wi-Fi transition uses the bounded fast confirmation interval.
        do {
            let (model, service, _, _) = fixture(complete: false)
            await service.configure(reports: [offline, online])
            await model.runAutomaticCycle()
            check(model.recommendedCheckDelay == 1, "Fresh path first failure schedules confirmation after one second")
            check(await service.snapshot().attempts.isEmpty, "First fresh failure never submits credentials")
            check(await service.snapshot().discoveries == 0, "First fresh failure does not skip the second probe gate")
            await model.runAutomaticCycle()
            check(model.isOnline, "A second fresh successful probe settles the path without login")
            check(await service.snapshot().attempts.isEmpty, "A second successful round sends no credentials")
            check(model.recommendedCheckDelay != 1, "Online result ends the fast confirmation phase")
        }
        // A slow website cannot hold fresh base checks or campus login. The separate full
        // follow-up must subsequently publish its actual website failure.
        do {
            let (model, service, checker, _) = fixture()
            await checker.pauseNext()
            await service.configure(reports: [offline, offline, online, online], discoveries: [campus], replies: [accepted])
            await model.runAutomaticCycle()
            check(await checker.count() == 0, "Fresh-path confirmation excludes optional website requests")
            check(model.lastInternetCheck?.includedCount == 0, "Quick recovery report has no invented website results")
            check(model.probeMode == .complete, "Recovery does not mutate the saved complete-check preference")
            await model.runAutomaticCycle()
            check(await service.snapshot().attempts == [.teaching], "Second fresh failure reaches one campus login despite a blocked website")
            check(await service.snapshot().modes == [.quick, .quick, .quick], "Fresh failures and immediate post-login confirmation use base-only quick mode")
            check(model.isOnline && model.lastInternetCheck?.verdict == .basicPassed, "Quick online recovery reports only basic connectivity")
            check(await checker.count() == 0, "Campus recovery completes before any optional website is started")
            check(model.recommendedCheckDelay == 2, "Complete preference gets a separate full check two seconds after recovery")
            let full = Task { await model.runAutomaticCycle() }
            if await waitUntil("full follow-up waits for selected website", { await checker.waiting() }) {
                check(await service.snapshot().attempts.count == 1, "Full follow-up does not submit credentials while the website is slow")
                check(await service.snapshot().modes.last == .complete, "Follow-up uses the unchanged complete preference")
                await checker.release(); await full.value
                check(model.lastInternetCheck?.includedCount == 1, "Full follow-up includes the selected website's actual response")
                check(model.lastInternetCheck?.websites.first?.result.state == .timeout, "Slow website retains its true timeout verdict")
                check(model.lastInternetCheck?.verdict == .partial, "Failed website cannot be reported as all passed")
                check(await service.snapshot().attempts.count == 1, "Full follow-up never duplicates the accepted login")
            } else { await checker.release(); full.cancel(); await full.value }
        }
        // Stable startup retains complete diagnostics. Repeated callbacks never extend the
        // bounded window, and reading the recommended cadence cannot restart it.
        do {
            let (stable, service, checker, _) = fixture(transition: false)
            await service.configure(reports: [online])
            await stable.runAutomaticCycle()
            check(await service.snapshot().modes == [.complete], "Initial stable attachment preserves normal complete checks")
            check(await checker.count() == 1, "Initial stable attachment still checks selected websites")
            let (model, retry, _, clock) = fixture(complete: false)
            await retry.configure(reports: [offline]); await model.runAutomaticCycle()
            clock.advance(44); attachWifi(model)
            check(model.recommendedCheckDelay == 1, "Unchanged path observation preserves the remaining recovery window")
            clock.advance(1)
            check(model.recommendedCheckDelay == 5, "At exactly forty-five seconds normal first-failure cadence resumes")
            clock.advance(90)
            check(model.recommendedCheckDelay == 5, "Reading recommended delay cannot restart an expired window")
        }
        // Changing the user's preference while the full follow-up is pending cannot
        // leave an unconsumable follow-up marker or a perpetual two-second schedule.
        do {
            let (model, service, checker, _) = fixture()
            await service.configure(reports: [online, online])
            await model.runAutomaticCycle()
            check(model.recommendedCheckDelay == 2 && model.prioritizingCampusRecovery,
                  "Fast healthy complete preference initially schedules its full follow-up")
            model.setProbeMode(.quick)
            check(model.recommendedCheckDelay >= 30 && !model.prioritizingCampusRecovery,
                  "Choosing quick clears the pending full follow-up and returns to normal cadence")
            await model.runAutomaticCycle()
            check(await service.snapshot().modes == [.quick, .quick], "Next cycle respects the new quick preference")
            check(model.recommendedCheckDelay >= 30 && !model.prioritizingCampusRecovery,
                  "Quick preference cannot become a perpetual two-second recovery schedule")
            check(await checker.count() == 0, "Choosing quick does not launch an obsolete complete website follow-up")
            check(await service.snapshot().attempts.isEmpty, "Changing check preference never submits credentials")
        }
        do {
            let (model, service, _, _) = fixture()
            await service.configure(reports: [online, offline])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.snapshot().modes == [.quick, .complete], "Quick healthy recovery restores complete preference without a prior login")
            check(await service.snapshot().attempts.isEmpty, "A failed full follow-up cannot bypass two fresh failed rounds or send credentials")
            check(model.authenticationSummary == SharedCopy.t("guard.detectionOnly"), "Scheduled full follow-up explicitly uses the read-only operation branch")
            check(model.lastInternetCheck?.verdict == .unavailable, "Failed full follow-up replaces quick success with the current real verdict")
            check(model.recommendedCheckDelay == 5 && !model.prioritizingCampusRecovery,
                  "Consumed full follow-up restores normal first-failure confirmation cadence")
        }

        // Transient pre-submission discovery retry still obtains fresh proof and expires.
        do {
            let (model, service, checker, clock) = fixture()
            await service.configure(reports: [offline, offline, offline], discoveries: [unconfirmed, campus], replies: [LoginAttempt(outcome: .unavailable)])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(model.engineStatus == "unconfirmed", "Unconfirmed campus proof remains a pre-submission failure")
            check(await service.snapshot().attempts.isEmpty, "Unconfirmed proof never sends a credential request")
            check(model.recommendedCheckDelay == 3, "Transient campus discovery retries after three seconds within the window")
            clock.advance(3); await model.runAutomaticCycle()
            check(await service.snapshot().discoveries == 2, "Prompt retry fetches a new campus proof")
            check(await service.snapshot().attempts == [.teaching], "Fresh confirmed retry makes at most one unsent preflight attempt")
            check(model.cooldownRemaining == nil, "Definitely unsent preflight does not invent a submission cooldown")
            check(await checker.count() == 0, "Transient discovery retry remains independent of selected websites")
            clock.advance(42)
            check(model.recommendedCheckDelay >= 30, "Expired discovery-retry window restores ordinary failure backoff")
        }
        // Holds and uncertain submissions always take precedence over latency.
        for reason in [HoldReason.userPaused, .security, .credentials, .uncertain] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let journal = root.appendingPathComponent("recovery.json")
            let hold = AuthenticationHold(reason: reason, area: reason == .credentials ? .teaching : nil,
                generation: reason == .credentials ? 1 : nil, origin: reason == .userPaused ? .user : .global,
                rejection: reason == .credentials ? "password" : "")
            try RecoveryJournal(generation: 1, holds: [hold], notBefore: nil, pendingSubmission: false).write(journal)
            let (model, service, _, _) = fixture(recoveryStore: journal)
            await service.configure(reports: [offline, offline]); await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.snapshot().attempts.isEmpty, "\(reason.key) hold prevents accelerated credential submission")
            check(await service.snapshot().modes == [.complete, .complete], "\(reason.key) hold preserves normal complete diagnostics")
            check(model.recommendedCheckDelay == 60, "\(reason.key) hold retains sixty-second checking cadence")
            check(model.holds == [hold], "Fast path never removes the \(reason.key) hold")
        }
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let journal = root.appendingPathComponent("cooldown.json")
            try RecoveryJournal(generation: 1, holds: [], notBefore: date.addingTimeInterval(42), pendingSubmission: false).write(journal)
            let (model, service, _, _) = fixture(recoveryStore: journal)
            await service.configure(reports: [offline, offline]); await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(model.recommendedCheckDelay == 15, "Submission cooldown retains fifteen-second read-only checks")
            check(model.cooldownRemaining == 42, "Fresh path never shortens an existing submission cooldown")
            check(await service.snapshot().attempts.isEmpty, "Cooldown blocks credentials despite a fresh recovery window")
            check(await service.snapshot().modes == [.complete, .complete], "Cooldown keeps normal diagnostics outside fast recovery")
        }
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let journal = root.appendingPathComponent("pending.json")
            try RecoveryJournal(generation: 1, holds: [], notBefore: date.addingTimeInterval(42), pendingSubmission: true).write(journal)
            let (model, service, checker, _) = fixture(complete: false, recoveryStore: journal)
            await service.configure(reports: [offline]); await model.runAutomaticCycle()
            check(await service.snapshot().modes == [.complete], "Uncertain submission forces complete proof even with quick preference")
            check(await checker.count() == 1, "Uncertain confirmation retains selected website evidence")
            check(await service.snapshot().attempts.isEmpty, "Uncertain pending submission is never duplicated")
            check(model.holds.contains { $0.reason == .uncertain }, "Failed pending confirmation keeps the uncertainty hold")
        }
        for readOnly in [false, true] {
            let (model, service, checker, _) = fixture(readOnly: readOnly)
            if !readOnly { model.setMonitoringMode(.observe) }
            await service.configure(reports: [offline, offline]); await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.snapshot().attempts.isEmpty, "\(readOnly ? "Read-only" : "Observe") mode cannot submit through recovery acceleration")
            check(await service.snapshot().modes == [.complete, .complete], "\(readOnly ? "Read-only" : "Observe") mode preserves complete diagnostics")
            check(await checker.count() == 2, "\(readOnly ? "Read-only" : "Observe") mode does not silently omit selected websites")
        }
        // Real scheduler, controlled waits. Trust only wakes an automatic round; no direct
        // action from granting trust may submit credentials or skip the fresh failure gate.
        do {
            let sleeper = LatencySleeper()
            let (model, service, _, _) = fixture(complete: false, area: .dormitory, sleeper: sleeper)
            await service.configure(reports: [offline, offline, online], discoveries: [campus], replies: [accepted])
            if await waitUntil("new path schedules immediate round", { await sleeper.pending().count == 1 }) {
                check(await sleeper.pending().first?.seconds == 0, "Observed path transition schedules first automatic check immediately")
                await sleeper.releaseFirst()
                if await waitUntil("first loop failure schedules confirmation", {
                    let waiting = await sleeper.pending()
                    return model.engineStatus == "confirming" && waiting.count == 1
                }) {
                    check(await sleeper.pending().first?.seconds == 1, "Actual loop waits one second between fresh failed probes")
                    check(await service.snapshot().attempts.isEmpty, "Actual loop never authenticates on its first failure")
                    model.authorizeCurrentNetwork()
                    check(model.currentNetworkAuthorized, "Explicit trust authorizes the identified dormitory attachment")
                    check(await service.snapshot().attempts.isEmpty, "Trust action itself never submits credentials")
                    if await waitUntil("trust cancels old wait and wakes loop", {
                        let waiting = await sleeper.pending()
                        return waiting.count == 1 && waiting.first?.seconds == 0
                    }) {
                        check(true, "Explicit trust wakes remaining recovery window immediately")
                        await sleeper.releaseFirst()
                        await waitUntil("trusted loop recovers", { model.isOnline && !model.busy })
                        check(await service.snapshot().attempts == [.dormitory], "Owned second failed round sends exactly one trusted dormitory login")
                    }
                }
            }
            model.setMonitoringMode(.manual)
            await waitUntil("controlled loop cancels remaining waits", { await sleeper.pending().isEmpty })
        }
        // A successful stale discovery cannot cross the shortened new-path window.
        do {
            let (model, service, _, _) = fixture(complete: false)
            await service.configure(reports: [offline, offline], discoveries: [campus], replies: [accepted])
            await model.runAutomaticCycle(); await service.pauseNext(.discovery)
            let old = Task { await model.runAutomaticCycle() }
            if await waitUntil("old path discovery remains gated", { await service.waiting(.discovery) }) {
                attachWifi(model, identity: nextIdentity)
                await service.release(.discovery); await old.value
                check(await service.snapshot().attempts.isEmpty, "Late old-path campus proof cannot submit on replacement path")
                check(!model.isOnline && !model.busy, "Late old-path result cannot publish or occupy replacement path")
                await service.configure(reports: [offline, online]); await model.runAutomaticCycle()
                check(model.recommendedCheckDelay == 1, "Replacement path starts fresh two-probe recovery sequence")
                check(await service.snapshot().attempts.isEmpty, "Replacement path cannot borrow old failed-probe count")
                await model.runAutomaticCycle()
                let attempts = await service.snapshot().attempts
                check(model.isOnline && attempts.isEmpty, "Replacement path online confirmation cannot reuse obsolete authentication proof")
            } else { await service.release(.discovery); old.cancel(); await old.value }
        }
        print("RecoveryLatencyAppModelTests: \(passed) passed, \(failed) failed; fake clock, controlled waits and in-memory transport only")
        if failed > 0 { exit(1) }
    }
}
