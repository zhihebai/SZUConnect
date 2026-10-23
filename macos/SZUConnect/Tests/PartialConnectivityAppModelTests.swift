import Foundation
import Darwin

/// Real AppModel orchestration with fake transports. Queued replies and delayed callbacks
/// exercise operation ownership without contacting a portal or reading system credentials.
private actor PartialService: ConnectionService {
    enum Stage: Hashable { case probe, discovery, attempt }
    struct Snapshot { let probes: Int; let discoveries: [CampusArea]; let attempts: [CampusArea] }
    private var base: ConnectivityReport
    private var reports: [ConnectivityReport] = []
    private var discovery: CampusDiscovery
    private var discoveries: [CampusDiscovery] = []
    private var reply: LoginAttempt
    private var replies: [LoginAttempt] = []
    private var probes = 0
    private var discoveryCalls: [CampusArea] = []
    private var attempts: [CampusArea] = []
    private var paused: Set<Stage> = []
    private var gates: [Stage: CheckedContinuation<Void, Never>] = [:]
    init(base: ConnectivityReport, discovery: CampusDiscovery, reply: LoginAttempt) {
        self.base = base; self.discovery = discovery; self.reply = reply
    }
    func configure(base: ConnectivityReport? = nil, reports: [ConnectivityReport] = [],
                   discovery: CampusDiscovery? = nil, discoveries: [CampusDiscovery] = [],
                   reply: LoginAttempt? = nil, replies: [LoginAttempt] = []) {
        if let base { self.base = base }; self.reports = reports
        if let discovery { self.discovery = discovery }; self.discoveries = discoveries
        if let reply { self.reply = reply }; self.replies = replies
    }
    func pauseNext(_ stage: Stage) { paused.insert(stage) }
    func isWaiting(_ stage: Stage) -> Bool { gates[stage] != nil }
    func release(_ stage: Stage) { gates.removeValue(forKey: stage)?.resume() }
    private func gate(_ stage: Stage) async {
        guard paused.remove(stage) != nil else { return }
        // Deliberately ignores cancellation so AppModel must reject stale replies.
        await withCheckedContinuation { gates[stage] = $0 }
    }
    func setExpectedPhysicalRoute(_ route: CampusPhysicalRoute?) async -> Bool { true }
    func probe() async -> ConnectivityReport {
        probes += 1
        let value = reports.isEmpty ? base : reports.removeFirst()
        await gate(.probe)
        return value
    }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        discoveryCalls.append(area)
        let value = discoveries.isEmpty ? discovery : discoveries.removeFirst()
        await gate(.discovery)
        return value
    }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        attempts.append(area)
        let value = replies.isEmpty ? reply : replies.removeFirst()
        await gate(.attempt)
        return value
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? { fatalError("Use staged discovery") }
    func login(credentials: Credentials, area: CampusArea) async throws -> String { fatalError("Use classified attempt") }
    func snapshot() -> Snapshot { Snapshot(probes: probes, discoveries: discoveryCalls, attempts: attempts) }
}

private actor PartialWebsites: WebsiteChecking {
    private var passingHosts: Set<String> = ["one.fixture.invalid"]
    private var calls = 0
    private var failureCode: URLError.Code = .timedOut
    private var statusCode: Int?
    private var location: String?
    func fail(_ code: URLError.Code) { failureCode = code; statusCode = nil; location = nil }
    func respond(_ code: Int, location: String? = nil) { statusCode = code; self.location = location }
    func allPass(_ value: Bool) {
        passingHosts = value ? ["one.fixture.invalid", "two.fixture.invalid", "three.fixture.invalid"] : ["one.fixture.invalid"]
    }
    func probe(_ input: String, strategy: WebsiteProbeStrategy) async -> WebsiteProbeResult {
        calls += 1
        let passing = passingHosts.contains(URL(string: input)?.host ?? "")
        let url = URL(string: input)!
        if passing || statusCode != nil {
            return .response(passing ? 200 : statusCode!, url: url, strategy: strategy, elapsed: 0.01,
                             location: passing ? nil : location)
        }
        return .failure(failureCode, url: url, strategy: strategy, elapsed: 0.01)
    }
    func count() -> Int { calls }
}

private actor PartialSleeper {
    private var waiting: [(UUID, Int, CheckedContinuation<Void, Error>)] = []
    func sleep(_ seconds: Int) async throws {
        let id = UUID()
        try Task.checkCancellation()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { waiting.append((id, seconds, $0)) }
        } onCancel: { Task { await self.cancel(id) } }
        try Task.checkCancellation()
    }
    private func cancel(_ id: UUID) {
        guard let index = waiting.firstIndex(where: { $0.0 == id }) else { return }
        waiting.remove(at: index).2.resume(throwing: CancellationError())
    }
    func firstDelay() -> Int? { waiting.first?.1 }
    func release() { if !waiting.isEmpty { waiting.removeFirst().2.resume() } }
}

@MainActor private final class PartialClock {
    var date = Date(timeIntervalSince1970: 1_800_000_000)
    func advance(_ seconds: TimeInterval = 91) { date.addTimeInterval(seconds) }
}

@main @MainActor
struct PartialConnectivityAppModelTests {
    private static var passed = 0
    private static var failed = 0
    private static let date = Date(timeIntervalSince1970: 1_800_000_000)
    private static let campus = CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
    private static let unknown = CampusDiscovery(teaching: .unconfirmed, dormitory: .unconfirmed)
    private static let accepted = LoginAttempt.success("Fixture account accepted")
    private nonisolated static let partial = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.02),
        microsoftResult: .init(state: .redirect, elapsed: 0.02), mode: .complete)
    private static let full = ConnectivityReport(apple: true, microsoft: true)
    private static let offline = ConnectivityReport(apple: false, microsoft: false)
    private static let targets = ["one", "two", "three"].map { WebsiteTarget(id: $0, url: "https://\($0).fixture.invalid/") }
    private static let credentials = Credentials(account: "fixture-account", password: "fixture-password")
    private static let descriptor = NetworkDescriptor(kind: "wifi", raw: [
        "gw": "a4:5e:60:25:01:01", "gwip": "172.29.25.1", "net": "172.29.25.0/24"
    ])
    private static let memoryID = "01234567"

    private struct Fixture {
        let app: AppModel
        let service: PartialService
        let websites: PartialWebsites
        let clock: PartialClock
    }
    private static func check(_ condition: Bool, _ message: String) {
        if condition { passed += 1; print("PASS: \(message)") }
        else { failed += 1; print("FAIL: \(message)") }
    }
    @discardableResult
    private static func wait(_ message: String, _ condition: @MainActor () async -> Bool) async -> Bool {
        let deadline = ContinuousClock.now.advanced(by: .seconds(2))
        while ContinuousClock.now < deadline {
            if await condition() { return true }
            try? await Task.sleep(for: .milliseconds(1))
        }
        check(false, "Timed out: \(message)")
        return false
    }
    private static func finish(_ model: AppModel) async { await wait("manual completion") { !model.busy } }
    private static func fixture(base: ConnectivityReport = partial, trusted: Bool = true, readOnly: Bool = false,
                                saved: Bool = true, mode: MonitoringMode = .recover, area: CampusArea = .dormitory,
                                memory: URL? = nil, journal: URL? = nil, labels: URL? = nil,
                                sleeper: PartialSleeper? = nil, tunnel: Bool = false, bound: Bool = false) -> Fixture {
        let service = PartialService(base: base, discovery: campus, reply: accepted)
        let clock = PartialClock()
        let websites = PartialWebsites()
        let sleepLoop: (@Sendable (Int) async throws -> Void)?
        if let sleeper { sleepLoop = { seconds in try await sleeper.sleep(seconds) } }
        else { sleepLoop = nil }
        let app = AppModel(client: service, isolated: true, readOnly: readOnly, now: { clock.date },
            recoveryStore: journal, isolatedCredentials: saved ? credentials : nil, websiteChecker: websites,
            initialWebsiteTargets: targets, memoryStore: memory, runAutomaticLoopInIsolation: sleeper != nil,
            loopSleep: sleepLoop, labelsStore: labels)
        app.area = area
        app.setProbeMode(.complete)
        let route = bound ? CampusPhysicalRoute(interfaceName: "en0", interfaceIndex: 1, addresses: ["172.29.25.42"],
            dnsServers: ["172.29.25.2"], identity: "fixture-bound-route", descriptor: descriptor) : nil
        app.receivePath(usable: true, interfaces: "Wi-Fi en0", identity: NetworkIdentity(fingerprint: "fixture-wifi"),
            tunnel: tunnel, descriptors: [descriptor], authenticationRoute: route, requiresPhysicalBinding: bound)
        app.setMonitoringMode(mode)
        if trusted { app.authorizeCurrentNetwork() }
        return Fixture(app: app, service: service, websites: websites, clock: clock)
    }
    private static func cycle(_ fixture: Fixture, count: Int = 2) async {
        for _ in 0..<count { await fixture.app.runAutomaticCycle() }
    }
    private static func folder() throws -> URL {
        let value = FileManager.default.temporaryDirectory.appendingPathComponent("szu-partial-model-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: value, withIntermediateDirectories: true)
        return value
    }
    private static func seedMemory(_ url: URL) throws {
        var memory = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
        _ = memory.remember(descriptor, area: "dormitory", proofTeaching: true, proofDormitory: true,
            now: date.addingTimeInterval(-3_600), newID: memoryID)
        try NetworkMemoryFile.write(memory.encoded(), to: url)
    }

    static func main() async throws {
        setbuf(stdout, nil)

        do {
            let f = fixture(tunnel: true, bound: true)
            f.clock.advance(46)
            await cycle(f, count: 1)
            check(f.app.partialRecoveryAwaitingConfirmation && f.app.recommendedCheckDelay == 5,
                  "A real partial physical failure keeps its five-second authentication confirmation")
            check(await f.websites.count() == 0,
                  "A partial physical failure does not wait for TUN websites")
            await cycle(f, count: 1)
            check(await f.service.snapshot().attempts == [.dormitory],
                  "Second partial physical check can authenticate without a read-only supplement stealing the round")
        }

        // Campus connectivity can settle uncertainty even while a separate TUN website is broken.
        for other in [HoldReason.userPaused, .credentials, .security] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let url = root.appendingPathComponent("recovery.json")
            let uncertain = AuthenticationHold(reason: .uncertain, origin: .global)
            let preserved = AuthenticationHold(reason: other, origin: .global)
            let deadline = date.addingTimeInterval(90)
            try RecoveryJournal(generation: 1, holds: [uncertain, preserved], notBefore: deadline, pendingSubmission: true).write(url)
            let f = fixture(base: full, journal: url, tunnel: true, bound: true)
            await cycle(f, count: 1)
            check(f.app.holds == [preserved] && f.app.cooldownRemaining == 90,
                  "Complete bound base proof clears only uncertainty and preserves \(other.rawValue) and cooldown")
            let saved = try RecoveryJournal.read(url, now: date)
            check(saved?.holds == [preserved] && saved?.notBefore == deadline,
                  "Bound base proof settles the pending journal without resetting the submission deadline")
            check(await f.websites.count() == 0 && f.app.lastInternetCheck?.verdict == .basicPassed,
                  "Submission settlement skips unrelated TUN websites and does not label them passed")
            check(await f.service.snapshot().attempts.isEmpty,
                  "Submission settlement never sends credentials")
        }

        // A user-forced login may fail even though this same round proves full Internet access.
        do {
            let complete = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.02),
                microsoftResult: .init(state: .success, elapsed: 0.02), mode: .complete)
            let f = fixture(base: complete)
            await f.websites.allPass(true)
            f.app.connect(); await finish(f.app)
            check(f.app.offerForcedLogin && f.app.lastInternetCheck?.verdict == .allPassed,
                  "A complete successful check offers only explicit forced login")

            await f.service.configure(reply: LoginAttempt(outcome: .rejected, rejection: .password, error: .rejected))
            f.app.connectEvenIfOnline(confirmation: f.app.forcedLoginConfirmation()); await finish(f.app)
            let presentation = f.app.presentation
            check(await f.service.snapshot().attempts == [.dormitory],
                  "Forced login submits once after a complete successful check")
            check(f.app.lastAttemptOutcome == .rejected && f.app.lastInternetCheck?.verdict == .allPassed,
                  "Rejected login leaves the current complete connectivity result intact")
            check(f.app.presentationStatus == "online" && presentation.headline == "internet.allPassed.title"
                  && presentation.tone == "success" && presentation.primary == "check",
                  "Current online connectivity remains the main status after login rejection")
            check(f.app.pausedForAutomatic && presentation.banner?.key == "hold.credentials"
                  && presentation.banner?.tone == "danger" && f.app.holds.count == 1,
                  "Credential rejection stays visible as an independent pause warning")

            f.app.receivePath(usable: true, interfaces: "Ethernet en12",
                              identity: NetworkIdentity(fingerprint: "different-fixture-network"))
            check(!f.app.isOnline && f.app.lastInternetCheck == nil && f.app.presentationStatus != "online",
                  "A new network cannot reuse the prior online result or headline")
        }

        do {
            let complete = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.02),
                microsoftResult: .init(state: .success, elapsed: 0.02), mode: .complete)
            let f = fixture(base: complete, mode: .manual)
            await f.websites.allPass(true)
            f.app.connect(); await finish(f.app)
            await f.service.configure(reply: LoginAttempt(outcome: .rejected, rejection: .password, error: .rejected))
            f.app.connectEvenIfOnline(confirmation: f.app.forcedLoginConfirmation()); await finish(f.app)
            check(f.app.presentationStatus == "online" && f.app.presentation.banner?.key == "banner.loginRejected"
                  && f.app.presentation.banner?.tone == "warning",
                  "Manual mode retains a visible rejection warning beside the online headline")
        }

        // One base endpoint succeeds, one redirects, and one of three websites responds.
        do {
            let f = fixture()
            await cycle(f, count: 1)
            check(f.app.isOnline && f.app.lastInternetCheck?.base.online == true, "Partial detection preserves the base online fact")
            check(f.app.lastInternetCheck?.websitePassedCount == 1 && f.app.lastInternetCheck?.selectedCount == 3,
                  "Partial report retains one of three selected websites passing")
            check(await f.service.snapshot().attempts.isEmpty, "The first complete partial check never authenticates")
            check(f.app.presentation.primary == "partialLogin" && f.app.presentation.secondary == "check",
                  "Partial result offers explicit login and a separate read-only check")
            await cycle(f, count: 1)
            let calls = await f.service.snapshot()
            check(calls.discoveries == [.dormitory] && calls.attempts == [.dormitory], "Two complete partial checks discover fresh campus proof and log in once")
            check(calls.probes == 5, "Accepted login with persistent partial result runs all three read-only confirmations")
            check(f.app.isOnline && f.app.lastInternetCheck?.verdict == .partial, "Failed optional results keep partial visible after login without erasing online")
            f.clock.advance(600)
            await cycle(f, count: 6)
            check(await f.service.snapshot().attempts.count == 1, "Persistent partial results never spend a second authentication round after cooldown")
            check(f.app.presentation.primary == "partialLogin", "A spent automatic partial budget keeps explicit manual retry available")
        }

        // Website availability may depend on a separate proxy/TUN path.
        for tunnel in [false, true] {
            let f = fixture(base: full, tunnel: tunnel)
            await cycle(f, count: 1)
            check(!f.app.partialRecoveryAwaitingConfirmation,
                  "Website-only failures do not arm automatic campus recovery with tunnel=\(tunnel)")
            f.clock.advance(600); await cycle(f, count: 8)
            let calls = await f.service.snapshot()
            check(calls.attempts.isEmpty && calls.discoveries.isEmpty,
                  "Persistent proxy or website failures submit zero campus credentials with tunnel=\(tunnel)")
            check(f.app.isOnline && f.app.lastInternetCheck?.hasOnlyWebsiteIssues == true,
                  "Website-only failures preserve both passing base endpoints and their diagnostics")
            check(f.app.canOfferPartialLogin && !f.app.partialRecoveryAttempted,
                  "Explicit manual recovery remains available without spending an automatic recovery budget")
        }

        for code in [401, 403, 404, 429, 500, 302] {
            let f = fixture(base: full)
            await f.websites.respond(code, location: "https://ordinary-redirect.invalid/")
            await cycle(f, count: 4)
            check(await f.service.snapshot().attempts.isEmpty && !f.app.partialRecoveryAttempted,
                  "HTTP \(code) alone never starts automatic campus authentication")
            check(!f.app.canOfferPartialLogin && f.app.canOfferForcedLogin && f.app.presentation.secondary == "forceLogin",
                  "HTTP-only partial results retain a separately confirmed manual login entry")
        }

        for code: URLError.Code in [.badURL, .unsupportedURL, .badServerResponse, .secureConnectionFailed,
                                    .appTransportSecurityRequiresSecureConnection, .cancelled] {
            let f = fixture(base: full)
            await f.websites.fail(code)
            await cycle(f, count: 4)
            check(await f.service.snapshot().attempts.isEmpty && !f.app.partialRecoveryAwaitingConfirmation,
                  "Non-availability website error \(code.rawValue) cannot trigger automatic recovery")
        }

        for code: URLError.Code in [.cannotFindHost, .dnsLookupFailed, .cannotConnectToHost,
                                    .networkConnectionLost, .notConnectedToInternet] {
            let f = fixture(base: full)
            await f.websites.fail(code); await cycle(f)
            check(await f.service.snapshot().attempts.isEmpty,
                  "Website connection error \(code.rawValue) cannot request automatic campus recovery")
        }

        do {
            let f = fixture(base: full)
            await f.websites.respond(302, location: "https://net.szu.edu.cn/srun_portal_pc?fixture=1")
            await cycle(f)
            check(await f.service.snapshot().attempts == [.dormitory],
                  "An exact campus-homepage redirect can request one guarded recovery")
        }
        do {
            let f = fixture(base: full)
            await f.websites.respond(302, location: "https://net.szu.edu.cn/srun_portal_pc")
            await f.service.configure(discovery: unknown)
            await cycle(f)
            check(await f.service.snapshot().attempts.isEmpty,
                  "A campus redirect never replaces fresh confirmed campus discovery")
        }

        // A normal action always rechecks evidence before deciding to authenticate.
        for recoveredCode in [200, 403] {
            let f = fixture(base: full)
            f.app.checkConnection(); await finish(f.app)
            check(f.app.canOfferPartialLogin, "A current timeout exposes normal manual recovery")
            await f.websites.respond(recoveredCode)
            f.app.connect(); await finish(f.app)
            check(await f.service.snapshot().attempts.isEmpty && f.app.canOfferForcedLogin,
                  "A fresh HTTP \(recoveredCode) response stops normal recovery before submission")
            f.app.connectEvenIfOnline(confirmation: f.app.forcedLoginConfirmation()); await finish(f.app)
            check(await f.service.snapshot().attempts == [.dormitory],
                  "Confirmed force after HTTP \(recoveredCode) still submits exactly once")
        }
        do {
            let f = fixture(base: full)
            f.app.checkConnection(); await finish(f.app)
            f.app.connect(); await finish(f.app)
            check(await f.service.snapshot().attempts == [.dormitory] && !f.app.offerForcedLogin,
                  "A fresh persistent connection failure permits normal manual recovery without a force offer")
        }

        // A persistent connection failure cannot rearm a spent recovery budget.
        do {
            let f = fixture()
            await cycle(f)
            f.clock.advance(600)
            await f.service.configure(base: full); await cycle(f, count: 4)
            check(await f.service.snapshot().attempts.count == 1 && f.app.partialRecoveryAttempted,
                  "Website-only failures preserve a previously consumed recovery budget")
            await f.service.configure(base: partial); await cycle(f)
            check(await f.service.snapshot().attempts.count == 1,
                  "Later base failures cannot reuse the consumed recovery round")
            await f.service.configure(base: full); await f.websites.allPass(true); await cycle(f, count: 1)
            await f.service.configure(base: partial); await f.websites.allPass(false); await cycle(f)
            check(await f.service.snapshot().attempts.count == 1, "One complete healthy result cannot rearm a spent partial episode")
            await f.service.configure(base: full); await f.websites.allPass(true); await cycle(f)
            await f.service.configure(base: partial); await f.websites.allPass(false); await cycle(f)
            check(await f.service.snapshot().attempts.count == 2, "Two consecutive completely healthy results rearm exactly one later partial recovery")
        }

        do {
            let f = fixture()
            await cycle(f, count: 1)
            await f.service.configure(base: full); await f.websites.respond(403); await cycle(f, count: 1)
            await f.service.configure(base: partial); await cycle(f, count: 1)
            check(await f.service.snapshot().attempts.isEmpty,
                  "An HTTP-only result interrupts consecutive base-failure confirmation")
            await cycle(f, count: 1)
            check(await f.service.snapshot().attempts.count == 1,
                  "Two new base-failure checks can start one recovery round")
        }

        do {
            let f = fixture()
            await cycle(f, count: 1)
            await f.service.configure(base: full); await f.websites.allPass(true)
            await cycle(f, count: 1)
            await f.service.configure(base: partial); await f.websites.allPass(false)
            await cycle(f, count: 1)
            check(await f.service.snapshot().attempts.isEmpty, "A complete healthy check interrupts the initial consecutive-partial confirmation")
            await cycle(f, count: 1)
            check(await f.service.snapshot().attempts.count == 1, "Two consecutive partial checks after recovery start the first recovery round")
        }

        // Real HTTP responses rearm transport recovery independently of aggregate success.
        do {
            let f = fixture()
            await cycle(f)
            f.clock.advance(600)
            await f.service.configure(base: full); await f.websites.respond(403); await cycle(f, count: 1)
            check(f.app.partialRecoveryAttempted, "One complete HTTP response round cannot rearm the budget")
            await f.websites.fail(.timedOut); await cycle(f, count: 3)
            check(await f.service.snapshot().attempts.count == 1,
                  "An intervening timeout discards the single transport-recovery round")
            await f.websites.respond(403); await cycle(f)
            check(!f.app.partialRecoveryAttempted && f.app.lastInternetCheck?.verdict == .partial,
                  "Two complete HTTP 403 rounds rearm recovery while the aggregate remains partial")
            await f.service.configure(base: partial); await f.websites.fail(.timedOut); await cycle(f)
            check(await f.service.snapshot().attempts.count == 2,
                  "A newly confirmed outage can use the rearmed budget once")
        }
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let url = root.appendingPathComponent("recovery.json")
            let hold = AuthenticationHold(reason: .uncertain, origin: .global)
            try RecoveryJournal(generation: 1, holds: [hold], notBefore: nil, pendingSubmission: true).write(url)
            let f = fixture(base: full, journal: url)
            await f.websites.respond(403); await cycle(f, count: 4)
            check(f.app.holds == [hold] && f.app.lastInternetCheck?.transportRecovered == true,
                  "HTTP transport recovery never clears an uncertain authentication hold")
            await f.websites.fail(.timedOut); await cycle(f)
            check(await f.service.snapshot().attempts.isEmpty,
                  "The uncertain hold still prevents a later availability recovery")
        }

        do {
            let f = fixture()
            await cycle(f); f.clock.advance(600)
            await f.service.configure(base: full); await f.websites.respond(403); await cycle(f, count: 1)
            f.app.setWebsiteStrategy(.direct)
            await cycle(f, count: 1)
            check(f.app.partialRecoveryAttempted,
                  "A configuration change discards the previous transport-recovery confirmation")
            await cycle(f, count: 1)
            check(!f.app.partialRecoveryAttempted,
                  "Only two full HTTP-response rounds under the new configuration rearm recovery")
        }

        // The post-login check ends early only when the full report has actually recovered.
        do {
            let f = fixture()
            await f.websites.allPass(true)
            await f.service.configure(reports: [partial, partial, partial, full])
            await cycle(f)
            check(await f.service.snapshot().probes == 4, "Verification continues past one partial result and stops at the first complete pass")
            check(f.app.lastInternetCheck?.verdict == .allPassed && f.app.isOnline, "A complete post-login pass replaces the partial result")
        }

        // Confirmation delay is measured from the real scheduler, without waiting five seconds.
        for base in [partial, full] {
            let sleeper = PartialSleeper()
            let f = fixture(base: base, sleeper: sleeper)
            await wait("initial automatic schedule") { await sleeper.firstDelay() != nil }
            await sleeper.release()
            await wait("first partial check scheduled its confirmation") {
                let probes = await f.service.snapshot().probes
                let delay = await sleeper.firstDelay()
                return probes == 1 && delay != nil
            }
            check(await sleeper.firstDelay() == (base.allPassed ? 30 : 5), "The automatic loop confirms base failures quickly and monitors website-only failures normally")
            check(await f.service.snapshot().attempts.isEmpty, "Scheduling partial confirmation itself sends no credentials")
            f.app.setMonitoringMode(.manual)
            await wait("scheduler cancellation") { await sleeper.firstDelay() == nil }
        }

        // A fresh path and a newly saved credential generation independently rearm the budget.
        for reset in ["path", "saved credentials"] {
            let f = fixture()
            await cycle(f)
            f.clock.advance()
            if reset == "path" {
                f.app.receivePath(usable: true, interfaces: "Ethernet en12", identity: NetworkIdentity(fingerprint: "fixture-ethernet"))
                f.app.authorizeCurrentNetwork()
            } else {
                f.app.password = "changed-fixture-password"
                f.app.saveCredentials()
            }
            if reset == "path" {
                await cycle(f, count: 1)
                let attemptedAfterBase = await f.service.snapshot().attempts.count
                check(f.app.lastProbeMode == .quick && attemptedAfterBase == 1,
                      "New-path base-only priority does not count as a full website confirmation")
            }
            await cycle(f, count: 1)
            check(await f.service.snapshot().attempts.count == 1, "\(reset) still requires the first new partial confirmation")
            await cycle(f, count: 1)
            check(await f.service.snapshot().attempts.count == 2, "\(reset) rearms one recovery round without restarting the app")
        }

        // User-triggered checks/settings interrupt consecutive evidence, not the spent budget.
        for reset in ["manual check", "monitoring mode", "probe mode", "website strategy"] {
            let f = fixture()
            func interruptConfirmation() async {
                switch reset {
                case "manual check": f.app.checkConnection(); await finish(f.app)
                case "monitoring mode": f.app.setMonitoringMode(.manual); f.app.setMonitoringMode(.recover)
                case "website strategy": f.app.setWebsiteStrategy(f.app.customWebsiteStrategy == .system ? .direct : .system)
                default: f.app.setProbeMode(.quick); f.app.setProbeMode(.complete)
                }
            }
            await cycle(f, count: 1)
            await interruptConfirmation()
            await cycle(f, count: 1)
            check(await f.service.snapshot().attempts.isEmpty, "\(reset) discards the prior partial confirmation without authenticating")
            await cycle(f, count: 1)
            check(await f.service.snapshot().attempts.count == 1, "\(reset) permits recovery only after two new automatic partial checks")
            f.clock.advance(600)
            await interruptConfirmation()
            await cycle(f, count: 4)
            check(await f.service.snapshot().attempts.count == 1, "\(reset) never resets an already consumed partial recovery budget")
        }

        // An attempt on another entry path consumes the same partial episode budget.
        for entry in ["manual", "offline", "unsubmitted preflight"] {
            let f = fixture()
            if entry == "offline" {
                await f.service.configure(base: offline)
                await cycle(f)
            } else {
                if entry == "unsubmitted preflight" { await f.service.configure(reply: LoginAttempt(outcome: .unavailable)) }
                f.app.checkConnection(); await finish(f.app)
                f.app.connect(); await finish(f.app)
            }
            check(await f.service.snapshot().attempts.count == 1, "\(entry) enters exactly one attempt API before subsequent partial checks")
            f.clock.advance()
            await f.service.configure(base: partial)
            await cycle(f, count: 4)
            check(await f.service.snapshot().attempts.count == 1, "\(entry) also consumes the automatic partial recovery budget")
        }

        // Existing pre-submission teaching fallback remains in the same bounded round.
        do {
            let f = fixture(area: .automatic)
            await f.service.configure(discoveries: [CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed), campus],
                                      replies: [LoginAttempt(outcome: .unavailable), accepted])
            await cycle(f)
            check(await f.service.snapshot().attempts == [.teaching, .dormitory], "An unsent teaching preflight can use the existing separately confirmed dormitory fallback")
            f.clock.advance(); await cycle(f, count: 3)
            check(await f.service.snapshot().attempts.count == 2, "Same-round unsent fallback never grants an additional partial recovery round")
        }

        // Both partial base reports and website-only issues preserve every persisted hold.
        for reason in [HoldReason.userPaused, .credentials, .security, .uncertain] {
            for base in [partial, full] {
                let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
                let url = root.appendingPathComponent("recovery.json")
                let hold = AuthenticationHold(reason: reason, area: reason == .credentials ? .dormitory : nil,
                    generation: reason == .credentials ? 1 : nil, origin: reason == .userPaused ? .user : .global,
                    rejection: reason == .credentials ? "password" : "")
                try RecoveryJournal(generation: 1, holds: [hold], notBefore: nil, pendingSubmission: false).write(url)
                let f = fixture(base: base, journal: url)
                await cycle(f, count: 4)
                let context = base.allPassed ? "website-only" : "base partial"
                check(await f.service.snapshot().attempts.isEmpty, "\(context) results cannot bypass the \(reason.key) hold")
                check(f.app.holds == [hold], "\(context) results preserve the \(reason.key) hold itself")
            }
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let url = root.appendingPathComponent("recovery.json")
            let uncertain = AuthenticationHold(reason: .uncertain, area: nil, generation: nil, origin: .global, rejection: "")
            try RecoveryJournal(generation: 1, holds: [uncertain], notBefore: nil, pendingSubmission: true).write(url)
            let weak = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.01),
                                          microsoftResult: .init(state: .notCompleted, elapsed: 0), mode: .quick)
            let f = fixture(base: weak, journal: url)
            f.app.setProbeMode(.quick)
            await cycle(f)
            check(f.app.probeMode == .quick && f.app.lastProbeMode == .complete && f.app.holds == [uncertain],
                  "Uncertain recovery temporarily checks completely; one reachable endpoint keeps the hold")
            check(await f.service.snapshot().attempts.isEmpty, "Uncertain read-only check never submits credentials")
            await f.websites.allPass(true)
            await f.service.configure(base: full)
            await cycle(f, count: 1)
            check(f.app.lastInternetCheck?.verdict == .allPassed && f.app.holds.isEmpty && f.app.probeMode == .quick,
                  "Complete success settles uncertainty while preserving the quick preference")
            check(await f.service.snapshot().attempts.isEmpty, "Settling uncertainty sends no credentials")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let url = root.appendingPathComponent("recovery.json")
            try RecoveryJournal(generation: 1, holds: [], notBefore: date.addingTimeInterval(90), pendingSubmission: false).write(url)
            let f = fixture(journal: url)
            await cycle(f)
            f.clock.advance(89); await cycle(f, count: 1)
            check(await f.service.snapshot().attempts.isEmpty && f.app.cooldownRemaining == 1, "Partial recovery respects the entire preexisting 90-second cooldown")
            f.clock.advance(1); await cycle(f, count: 1)
            check(await f.service.snapshot().attempts.count == 1, "A confirmed partial episode can try once after the existing cooldown expires")
        }

        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let url = root.appendingPathComponent("recovery.json")
            let deadline = date.addingTimeInterval(90)
            let journal = RecoveryJournal(generation: 1, holds: [], notBefore: deadline, pendingSubmission: true)
            try journal.write(url)
            let f = fixture(base: full, journal: url)
            await cycle(f)
            let saved = try RecoveryJournal.read(url, now: f.clock.date)
            check(saved?.notBefore == deadline && saved?.holds == journal.holds
                  && f.app.cooldownRemaining == 90,
                  "Website-only issues preserve the pending-submission hold and its cooldown")
            f.clock.advance(89); await cycle(f)
            check(await f.service.snapshot().attempts.isEmpty && f.app.cooldownRemaining == 1,
                  "Website-only checks neither reset nor extend the existing cooldown")
            f.clock.advance(1); await cycle(f)
            check(await f.service.snapshot().attempts.isEmpty && !f.app.partialRecoveryAttempted,
                  "Cooldown expiry cannot bypass the pending-submission hold for website failures")
        }

        for blocked in ["read-only", "observe", "missing credentials", "unknown campus", "untrusted"] {
            let f = fixture(trusted: blocked != "untrusted", readOnly: blocked == "read-only", saved: blocked != "missing credentials",
                            mode: blocked == "observe" ? .observe : .recover)
            if blocked == "unknown campus" { await f.service.configure(discovery: unknown) }
            await cycle(f, count: 4)
            check(await f.service.snapshot().attempts.isEmpty, "\(blocked) still blocks automatic partial authentication")
            if blocked == "read-only" || blocked == "observe" {
                check(f.app.presentation.primary != "partialLogin", "\(blocked) presentation offers no partial-login action")
                f.app.connectEvenIfOnline(confirmation: f.app.forcedLoginConfirmation()); await finish(f.app)
                check(await f.service.snapshot().attempts.isEmpty, "\(blocked) also rejects programmatic forced-login action")
            }
        }

        // A stored network is only eligibility; fresh portal proof is still required each round.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let memory = root.appendingPathComponent("network-memory.json"); try seedMemory(memory)
            let f = fixture(trusted: false, memory: memory)
            await f.service.configure(discovery: unknown); await cycle(f)
            check(await f.service.snapshot().attempts.isEmpty, "Remembered hash match alone cannot authenticate on partial connectivity")
            await f.service.configure(discovery: campus); await cycle(f, count: 1)
            check(await f.service.snapshot().attempts == [.dormitory], "Remembered network plus fresh campus proof can recover partial connectivity")
            check(!f.app.currentNetworkAuthorized, "Automatic memory-backed recovery never fabricates manual session trust")
        }

        // Revocation and network changes invalidate delayed discovery before credentials are sent.
        for invalidation in ["path", "memory revocation"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let memory = root.appendingPathComponent("network-memory.json"); try seedMemory(memory)
            let f = fixture(trusted: false, memory: memory)
            await cycle(f, count: 1)
            await f.service.pauseNext(.discovery)
            let task = Task { await f.app.runAutomaticCycle() }
            if await wait("delayed discovery before \(invalidation)", { await f.service.isWaiting(.discovery) }) {
                if invalidation == "path" {
                    f.app.receivePath(usable: true, interfaces: "Ethernet en12", identity: NetworkIdentity(fingerprint: "new-path"))
                } else { f.app.forgetAllNetworks() }
                await f.service.release(.discovery)
            }
            await task.value
            check(await f.service.snapshot().attempts.isEmpty, "A stale discovery after \(invalidation) cannot send credentials")
            await cycle(f)
            check(await f.service.snapshot().attempts.isEmpty, "\(invalidation) leaves no stale trust for subsequent partial recovery")
        }

        // A cancelled first probe cannot become a fresh partial confirmation on the new path.
        do {
            let f = fixture()
            await f.service.pauseNext(.probe)
            let old = Task { await f.app.runAutomaticCycle() }
            await wait("delayed public probe") { await f.service.isWaiting(.probe) }
            f.app.receivePath(usable: true, interfaces: "Ethernet en12", identity: NetworkIdentity(fingerprint: "probe-new-path"))
            f.app.authorizeCurrentNetwork()
            await f.service.release(.probe); await old.value
            await cycle(f, count: 1)
            check(await f.service.snapshot().attempts.isEmpty, "Late old-path probe cannot count toward a new-path partial confirmation")
            check(f.app.lastProbeMode == .quick, "The first new-path round prioritizes only the base connection")
            await cycle(f, count: 1)
            check(await f.service.snapshot().attempts.isEmpty, "The read-only full follow-up supplies only the first fresh website confirmation")
            await cycle(f, count: 1)
            check(await f.service.snapshot().attempts.count == 1, "Only two owned full new-path website checks can initiate recovery")
        }

        // Quick early success is intentional, including the unfinished second base endpoint.
        do {
            let f = fixture(base: ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.01),
                microsoftResult: .init(state: .notCompleted, elapsed: 0), mode: .quick))
            f.app.setProbeMode(.quick)
            await cycle(f, count: 5)
            let calls = await f.service.snapshot()
            check(calls.attempts.isEmpty && calls.discoveries.isEmpty,
                  "Quick success with a notCompleted second endpoint never invokes partial authentication")
            check(await f.websites.count() == 0 && f.app.lastInternetCheck?.verdict == .basicPassed,
                  "Quick checks continue to skip selected websites and publish basicPassed")
        }

        // Same identity=nil/link=false/usable=false path must still notice the physical flag.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let memory = root.appendingPathComponent("network-memory.json"); try seedMemory(memory)
            let labels = root.appendingPathComponent("network-labels.json")
            var names = NetworkLabels()
            names.updateAutomatic(id: memoryID, displayName: "Fixture-Campus", kind: .wifi, ssid: "Fixture-Campus")
            try names.encoded().write(to: labels)
            let f = fixture(memory: memory, labels: labels)
            f.app.wifiRecovery.requestWiFiNamePermission() // Isolated model grants a simulated permission only.
            f.app.wifiRecovery.setAutoJoinEnabled(true)
            f.app.wifiRecovery.setPowerOnWhenDisconnected(true)
            check(f.app.wifiRecovery.selectedNetworkIDs == [memoryID], "An already remembered campus SSID needs no second individual selection after enabling automatic Wi-Fi")
            var continuity = NetworkContinuitySnapshot(identity: nil, linkActive: false)
            continuity.hasUsablePhysicalLink = true
            let attached = PathObservation(usable: false, interfaces: "", constrained: false, continuity: continuity)
            f.app.applyEnvironmentObservation(attached)
            check(f.app.wifiRecovery.status == .connected, "Unknown but physically attached path suppresses automatic Wi-Fi association")
            continuity.hasUsablePhysicalLink = false
            f.app.applyEnvironmentObservation(PathObservation(usable: false, interfaces: "", constrained: false, continuity: continuity))
            check(f.app.wifiRecovery.status == .waiting, "Physical-link-only true-to-false observation reaches Wi-Fi recovery instead of being discarded")
            await f.app.wifiRecovery.refreshOnce()
            check(f.app.wifiRecovery.status == .waiting, "Isolated Wi-Fi recovery executes only its fake notFound service after physical disconnection")
        }

        print("PartialConnectivityAppModelTests: \(passed) passed, \(failed) failed")
        if failed > 0 { exit(1) }
    }
}
