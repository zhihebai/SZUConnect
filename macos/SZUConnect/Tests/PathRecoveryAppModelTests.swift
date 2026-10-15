import Foundation
import Darwin

/// Runs the real AppModel against scripted in-memory replies and private temporary files.
/// Gates intentionally ignore cancellation so stale completions must be rejected by AppModel.
private actor PathRecoveryService: ConnectionService {
    enum Stage: String, Hashable, Sendable { case probe, discovery, attempt }
    struct Snapshot: Sendable {
        let calls: [String]
        let probes: Int
        let discoveries: Int
        let attempts: [CampusArea]
        let resets: Int
    }
    private var reports: [ConnectivityReport] = []
    private var discoveries: [CampusDiscovery] = []
    private var replies: [LoginAttempt] = []
    private var calls: [String] = []
    private var probeCount = 0
    private var discoveryCount = 0
    private var attempts: [CampusArea] = []
    private var resetCount = 0
    private var pause: Set<Stage> = []
    private var gates: [Stage: CheckedContinuation<Void, Never>] = [:]

    func configure(reports: [ConnectivityReport] = [], discoveries: [CampusDiscovery] = [], attempts: [LoginAttempt] = []) {
        self.reports = reports; self.discoveries = discoveries; self.replies = attempts
    }
    func pauseNext(_ stage: Stage) { pause.insert(stage) }
    func isWaiting(_ stage: Stage) -> Bool { gates[stage] != nil }
    func release(_ stage: Stage) { gates.removeValue(forKey: stage)?.resume() }
    private func waitIfNeeded(_ stage: Stage) async {
        guard pause.remove(stage) != nil else { return }
        await withCheckedContinuation { gates[stage] = $0 }
    }
    func resetConnections() async { resetCount += 1; calls.append("reset") }
    func probe() async -> ConnectivityReport {
        probeCount += 1; calls.append("probe")
        let report = reports.isEmpty ? ConnectivityReport(apple: false, microsoft: false) : reports.removeFirst()
        await waitIfNeeded(.probe)
        return report
    }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        discoveryCount += 1; calls.append("discover")
        let reply = discoveries.isEmpty ? CampusDiscovery(teaching: .unconfirmed, dormitory: .unconfirmed) : discoveries.removeFirst()
        await waitIfNeeded(.discovery)
        return reply
    }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        attempts.append(area); calls.append("attempt")
        let reply = replies.isEmpty ? LoginAttempt(outcome: .unavailable) : replies.removeFirst()
        await waitIfNeeded(.attempt)
        return reply
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? { nil }
    func login(credentials: Credentials, area: CampusArea) async throws -> String { throw URLError(.cannotConnectToHost) }
    func snapshot() -> Snapshot {
        Snapshot(calls: calls, probes: probeCount, discoveries: discoveryCount, attempts: attempts, resets: resetCount)
    }
}

/// Controlled scheduler: no wall-clock retry delays and no live NWPathMonitor.
private actor PathRecoverySleeper {
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

@MainActor private final class PathRecoveryClock {
    var date = Date(timeIntervalSince1970: 1_800_000_000)
    func advance(_ seconds: TimeInterval) { date.addTimeInterval(seconds) }
}

@main @MainActor
struct PathRecoveryAppModelTests {
    private static var passed = 0
    private static var failed = 0
    private static let offline = ConnectivityReport(apple: false, microsoft: false)
    private static let online = ConnectivityReport(apple: true, microsoft: true)
    private static let campus = CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
    private static let accepted = LoginAttempt.success("Fixture login accepted")
    private static let date = Date(timeIntervalSince1970: 1_800_000_000)
    private nonisolated static let wiredIdentity = NetworkIdentity(fingerprint: "fixture-wired")
    private nonisolated static let wifiIdentity = NetworkIdentity(fingerprint: "fixture-wifi")
    private nonisolated static let wifi = NetworkDescriptor(kind: "wifi", raw: [
        "gw": "a4:5e:60:73:22:01", "gwip": "172.29.22.1", "net": "172.29.22.0/24"
    ])
    private nonisolated static let wired = NetworkDescriptor(kind: "ethernet", raw: [
        "gw": "a4:5e:60:73:22:02", "gwip": "172.28.22.1", "net": "172.28.22.0/24"
    ])
    private nonisolated static let otherWifi = NetworkDescriptor(kind: "wifi", raw: [
        "gw": "a4:5e:60:73:22:03", "gwip": "172.27.22.1", "net": "172.27.22.0/24"
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
    private static func finish(_ model: AppModel) async { await waitUntil("manual operation completion") { !model.busy } }
    private static func folder() throws -> URL {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("szu-path-recovery-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        return root
    }
    private static func seedMemory(_ store: URL) throws {
        var memory = NetworkMemory(salt: "00112233445566778899aabbccddeeff")
        _ = memory.remember(wifi, area: "dormitory", proofTeaching: true, proofDormitory: true,
                            now: date.addingTimeInterval(-7_200), newID: "1234ab22")
        try NetworkMemoryFile.write(memory.encoded(), to: store)
    }
    private static func fixture(area: CampusArea = .dormitory, identity: NetworkIdentity? = wifiIdentity,
                                descriptors: [NetworkDescriptor] = [wifi], memoryStore: URL? = nil,
                                recoveryStore: URL? = nil) -> (AppModel, PathRecoveryService, PathRecoveryClock) {
        let service = PathRecoveryService()
        let clock = PathRecoveryClock()
        let model = AppModel(client: service, isolated: true, now: { clock.date }, recoveryStore: recoveryStore,
                             isolatedCredentials: Credentials(account: "fixture-account", password: "fixture-password"),
                             memoryStore: memoryStore)
        model.area = area
        model.receivePath(usable: true, interfaces: "Wi-Fi en0", identity: identity, descriptors: descriptors)
        model.setMonitoringMode(.recover)
        return (model, service, clock)
    }
    private static func attachWired(_ model: AppModel) {
        model.receivePath(usable: true, interfaces: "Ethernet en12", identity: wiredIdentity, descriptors: [wired])
    }
    private static func detach(_ model: AppModel) { model.receivePath(usable: false, interfaces: "", linkActive: false) }
    private static func attachWifi(_ model: AppModel, identity: NetworkIdentity? = wifiIdentity) {
        model.receivePath(usable: true, interfaces: "Wi-Fi en0", identity: identity, descriptors: [wifi])
    }

    private static func observation(interfaces: String = "Wi-Fi en0",
                                    descriptors: [NetworkDescriptor] = [wifi]) -> PathObservation {
        var continuity = NetworkContinuitySnapshot(identity: wifiIdentity, linkActive: true)
        continuity.physicalLabels = [interfaces]
        continuity.hasUsablePhysicalLink = true
        continuity.descriptors = descriptors
        return PathObservation(usable: true, interfaces: interfaces, constrained: false, continuity: continuity)
    }

    static func main() async throws {
        setbuf(stdout, nil)

        // Missing identity is never authorization. Repeating the exact observation must still
        // let a credential-free network check finish instead of cancelling it on each callback.
        do {
            let (model, service, _) = fixture(identity: nil)
            model.authorizeCurrentNetwork()
            check(!model.currentNetworkAuthorized, "Unknown identity cannot be explicitly trusted")
            await service.configure(reports: [online])
            await service.pauseNext(.probe)
            model.checkConnection()
            await waitUntil("unknown-identity check is in flight") { await service.isWaiting(.probe) }
            let revision = model.networkRevision
            for _ in 0..<3 { attachWifi(model, identity: nil) }
            check(model.busy && model.networkRevision == revision, "Repeated identical unknown identity does not cancel the active check")
            await service.release(.probe)
            await finish(model)
            check(model.isOnline && model.lastCheckedAt != nil, "Unknown-identity check can publish its own fresh result")
            check(!model.currentNetworkAuthorized, "A successful public check does not grant missing-identity trust")
        }

        // A known attachment may receive a new display label without changing networks.
        do {
            let (model, service, _) = fixture()
            await service.configure(reports: [online])
            await service.pauseNext(.probe)
            model.checkConnection()
            await waitUntil("known-identity check before label change") { await service.isWaiting(.probe) }
            let revision = model.networkRevision
            model.applyEnvironmentObservation(observation(interfaces: "Renamed Wi-Fi"))
            check(model.busy && model.networkRevision == revision, "Polled known identity display-label change preserves the active check")
            await service.release(.probe)
            await finish(model)
            check(model.isOnline, "Known identity display-label change accepts the owned result")
        }

        // A stale discovery may return success after a path change. It must never use memory
        // from the old attachment or send a credential request under that obsolete proof.
        for changed in ["descriptors", "tunnel", "interfaces"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let memoryStore = root.appendingPathComponent("network-memory.json")
            try seedMemory(memoryStore)
            let (model, service, _) = fixture(identity: changed == "interfaces" ? nil : wifiIdentity, memoryStore: memoryStore)
            await service.configure(reports: [offline, offline, online], discoveries: [campus], attempts: [accepted])
            await model.runAutomaticCycle()
            await service.pauseNext(.discovery)
            let stale = Task { await model.runAutomaticCycle() }
            await waitUntil("gated discovery before \(changed) change") { await service.isWaiting(.discovery) }
            let revision = model.networkRevision
            model.receivePath(usable: true, interfaces: changed == "interfaces" ? "Wi-Fi en1" : "Wi-Fi en0",
                              identity: changed == "interfaces" ? nil : wifiIdentity, tunnel: changed == "tunnel",
                              descriptors: changed == "descriptors" ? [otherWifi] : [wifi])
            check(model.networkRevision > revision && !model.busy, "Changed \(changed) invalidates the current operation without a new identity value")
            await service.release(.discovery)
            await stale.value
            check(await service.snapshot().attempts.isEmpty, "Late discovery after \(changed) change cannot submit credentials")
            check(!model.isOnline && model.lastAttemptOutcome == nil, "Late discovery after \(changed) change cannot publish old connection state")
        }

        // The 15-second interruption grace belongs to one unchanged attachment. A changed
        // descriptor or tunnel cannot inherit that trust merely because the identity is stable.
        for changed in ["none", "descriptors", "tunnel"] {
            let identity = NetworkIdentity(fingerprint: "grace-fixture", permitsBriefInterruption: true)
            let (model, _, clock) = fixture(identity: identity)
            model.authorizeCurrentNetwork()
            check(model.currentNetworkAuthorized, "Interruption-grace fixture begins trusted (change=\(changed))")
            let descriptors = changed == "descriptors" ? [otherWifi] : [wifi]
            let tunnel = changed == "tunnel"
            model.receivePath(usable: false, interfaces: "Wi-Fi en0", identity: identity,
                              linkActive: true, now: clock.date, tunnel: tunnel, descriptors: descriptors)
            check(model.currentNetworkAuthorized == (changed == "none"),
                  "Brief unavailability preserves trust only when descriptors and tunnel are unchanged (change=\(changed))")
            clock.advance(10)
            model.receivePath(usable: true, interfaces: "Wi-Fi en0", identity: identity,
                              linkActive: true, now: clock.date, tunnel: tunnel, descriptors: descriptors)
            check(model.currentNetworkAuthorized == (changed == "none"),
                  "Recovery within 15 seconds cannot inherit trust across a \(changed) change")
        }

        // The periodic environment poll has its own equality guard. Same identity/link/
        // tunnel values must not hide changed network descriptors from the memory matcher.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            try seedMemory(store)
            let (model, service, _) = fixture(memoryStore: store)
            check(model.memoryMatch.level == .strong, "Environment-poll fixture starts on its strongly remembered Wi-Fi")
            await service.configure(reports: [offline, offline, online], discoveries: [campus], attempts: [accepted])
            await model.runAutomaticCycle()
            await service.pauseNext(.discovery)
            let stale = Task { await model.runAutomaticCycle() }
            await waitUntil("old proof before descriptor-only environment poll") { await service.isWaiting(.discovery) }
            let revision = model.networkRevision
            model.applyEnvironmentObservation(observation(descriptors: [otherWifi]))
            check(model.networkRevision > revision && !model.busy,
                  "Descriptor-only environment poll invalidates in-flight proof with unchanged identity/link/tunnel")
            check(model.memoryMatch.level == .none && !model.networkRemembered,
                  "Descriptor-only environment poll updates memoryMatch immediately without restarting")
            await service.release(.discovery)
            await stale.value
            check(await service.snapshot().attempts.isEmpty && model.lastDiscovery == nil,
                  "Proof completed after descriptor-only poll cannot publish discovery or submit credentials")
            model.applyEnvironmentObservation(observation())
            check(model.memoryMatch.level == .strong && model.networkRemembered && !model.currentNetworkAuthorized,
                  "A later poll restores remembered eligibility while still requiring new campus proof")
        }

        // Interface switches start their own two-failure sequence. Saved Wi-Fi eligibility
        // permits recovery after new campus proof, while old session trust never carries over.
        for remembered in [false, true] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            if remembered { try seedMemory(store) }
            let (model, service, _) = fixture(memoryStore: store)
            attachWired(model); model.authorizeCurrentNetwork()
            await service.configure(reports: [offline])
            await model.runAutomaticCycle()
            detach(model); attachWifi(model)
            check(!model.currentNetworkAuthorized, "Switching wired to Wi-Fi discards wired session trust (remembered=\(remembered))")
            await service.configure(reports: [offline, offline, online], discoveries: [campus], attempts: [accepted])
            await model.runAutomaticCycle()
            check(await service.snapshot().attempts.isEmpty && model.engineStatus == "confirming",
                  "Wi-Fi waits for two fresh failed checks instead of reusing wired failure (remembered=\(remembered))")
            await model.runAutomaticCycle()
            let snapshot = await service.snapshot()
            if remembered {
                check(snapshot.attempts == [.dormitory] && model.isOnline,
                      "Remembered Wi-Fi automatically recovers exactly once after two fresh failures and campus proof")
            } else {
                check(snapshot.attempts.isEmpty && model.engineStatus == "needsTrust",
                      "Unremembered Wi-Fi cannot inherit wired trust or send credentials")
            }
        }

        // A matching saved descriptor alone cannot identify a live physical attachment.
        for missingFact in ["identity", "physicalLink"] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            try seedMemory(store)
            let (model, service, _) = fixture(memoryStore: store)
            model.receivePath(usable: true, interfaces: "Wi-Fi en0",
                              identity: missingFact == "identity" ? nil : wifiIdentity,
                              linkActive: missingFact != "physicalLink", descriptors: [wifi])
            await service.configure(reports: [offline, offline, online], discoveries: [campus], attempts: [accepted])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(await service.snapshot().attempts.isEmpty && !model.currentNetworkAuthorized,
                  "Saved descriptor without \(missingFact) cannot auto-authorize or submit credentials")
        }

        // Definitely pre-submission failures create no new duplicate-submission cooldown,
        // and may not extend or erase a deadline left by an earlier actual submission.
        for outcome in [LoginOutcome.unavailable, .challengeRefused] {
            for priorCooldown in [false, true] {
                let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
                let journalURL = root.appendingPathComponent("recovery.json")
                if priorCooldown {
                    try RecoveryJournal(generation: 1, holds: [], notBefore: date.addingTimeInterval(42), pendingSubmission: false).write(journalURL)
                }
                let (model, service, _) = fixture(area: .teaching, recoveryStore: journalURL)
                await service.configure(reports: [offline], attempts: [LoginAttempt(outcome: outcome)])
                model.connect(); await finish(model)
                check(model.lastAttemptOutcome == outcome, "\(outcome) publishes the owned pre-submission outcome")
                check(priorCooldown ? model.cooldownRemaining == 42 : model.cooldownRemaining == nil,
                      "\(outcome) preserves only the previous cooldown (prior=\(priorCooldown))")
                let journal = try RecoveryJournal.read(journalURL, now: date)
                check(journal?.holds.contains(where: { $0.reason == .uncertain }) == false && journal?.notBefore == (priorCooldown ? date.addingTimeInterval(42) : nil),
                      "\(outcome) persists the original deadline and clears the provisional submission marker (prior=\(priorCooldown))")
            }
        }

        // A cancelled credential request can already have reached the server. A later
        // preflight failure must not erase that earlier request's uncertain journal marker.
        for outcome in [LoginOutcome.unavailable, .challengeRefused] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let journalURL = root.appendingPathComponent("recovery.json")
            let (model, service, clock) = fixture(area: .teaching, recoveryStore: journalURL)
            await service.configure(reports: [offline, offline], discoveries: [campus], attempts: [accepted])
            await model.runAutomaticCycle()
            await service.pauseNext(.attempt)
            let submittedCycle = Task { await model.runAutomaticCycle() }
            await waitUntil("submitted request before cancellation") { await service.isWaiting(.attempt) }
            model.cancelCurrentOperation()
            clock.advance(7)
            await service.configure(reports: [offline], attempts: [LoginAttempt(outcome: outcome)])
            model.connect(); await finish(model)
            let journal = try RecoveryJournal.read(journalURL, now: clock.date)
            check(model.cooldownRemaining == 83, "\(outcome) after cancelled submission preserves the original 90-second deadline")
            check(journal?.holds.filter({ $0.reason == .uncertain }).count == 1,
                  "\(outcome) after cancelled submission preserves its existing uncertain journal marker")
            await service.release(.attempt)
            await submittedCycle.value
            check(model.lastAttemptOutcome == outcome && !model.isOnline && model.cooldownRemaining == 83,
                  "Late cancelled success cannot erase newer preflight outcome or old submission protection")
            let restored = AppModel(client: PathRecoveryService(), isolated: true, now: { clock.date },
                recoveryStore: journalURL, isolatedCredentials: Credentials(account: "fixture-account", password: "fixture-password"))
            check(restored.holds.contains(where: { $0.reason == .uncertain }) && restored.cooldownRemaining == 83,
                  "Reloading after \(outcome) still blocks duplicate submission from the cancelled request")
        }

        // Outcomes that may follow a submitted password retain cooldown protection.
        for outcome in [LoginOutcome.success, .uncertain, .cancelled] {
            let (model, service, _) = fixture(area: .teaching)
            let reply = outcome == .success ? accepted : LoginAttempt(outcome: outcome)
            await service.configure(reports: [offline, online], attempts: [reply])
            model.connect(); await finish(model)
            check(model.cooldownRemaining == 90, "\(outcome) retains the 90-second cooldown")
        }

        // Every new path and manual login starts with a clean client connection pool.
        // Cancellation ownership still belongs to AppModel even if the service returns late.
        do {
            let (model, service, _) = fixture(area: .teaching)
            await service.configure(reports: [online])
            await service.pauseNext(.probe)
            let oldCycle = Task { await model.runAutomaticCycle() }
            await waitUntil("old check before path reset") { await service.isWaiting(.probe) }
            let before = await service.snapshot()
            attachWired(model)
            await service.configure(reports: [offline], attempts: [LoginAttempt(outcome: .unavailable)])
            model.connect(); await finish(model)
            let after = await service.snapshot()
            let newCalls = Array(after.calls.dropFirst(before.calls.count))
            check(after.resets > before.resets, "A physical path change replaces stale client connections")
            check(newCalls.first == "reset" && (newCalls.firstIndex(of: "reset") ?? Int.max) < (newCalls.firstIndex(of: "probe") ?? -1),
                  "Client reset completes before probing or logging in on the new path")
            check(after.attempts == [.teaching], "Manual login can run after the interrupted old-path check")
            await service.release(.probe)
            await oldCycle.value
            check(!model.isOnline && model.lastAttemptOutcome == .unavailable,
                  "Late old-path success cannot replace the new manual attempt outcome")
            let resetCount = after.resets
            await service.configure(reports: [offline], attempts: [LoginAttempt(outcome: .unavailable)])
            model.connect(); await finish(model)
            check(await service.snapshot().resets > resetCount, "Another manual login replaces connections without restarting the app")
        }

        // Exercise the actual scheduling loop, including the abandoned old loop completing
        // after the new path has already scheduled its five-second confirmation check.
        do {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let store = root.appendingPathComponent("network-memory.json")
            try seedMemory(store)
            let service = PathRecoveryService()
            let clock = PathRecoveryClock()
            let sleeper = PathRecoverySleeper()
            let model = AppModel(client: service, isolated: true, now: { clock.date },
                isolatedCredentials: Credentials(account: "loop-fixture", password: "loop-fixture-password"), memoryStore: store,
                runAutomaticLoopInIsolation: true, loopSleep: { seconds in try await sleeper.sleep(seconds) })
            model.area = .dormitory
            attachWired(model)
            await service.configure(reports: [online])
            await service.pauseNext(.probe)
            model.setMonitoringMode(.recover)
            await waitUntil("initial wired loop sleep") { await sleeper.pending().count == 1 }
            await sleeper.releaseFirst()
            await waitUntil("old wired loop inside delayed probe") { await service.isWaiting(.probe) }
            let before = await service.snapshot()
            detach(model); attachWifi(model)
            await service.configure(reports: [offline, offline, online], discoveries: [campus], attempts: [accepted])
            await waitUntil("Wi-Fi loop starts after physical path recovery") { await sleeper.pending().count == 1 }
            await sleeper.releaseFirst()
            await waitUntil("Wi-Fi loop schedules its second confirming check") {
                (await sleeper.pending().count == 1) && model.engineStatus == "confirming"
            }
            check(await service.snapshot().attempts.isEmpty, "Real loop never authenticates after its first Wi-Fi failure")
            check(await sleeper.pending().first?.seconds == 1, "Real loop confirms a switched Wi-Fi path after one second")
            let newCalls = Array((await service.snapshot()).calls.dropFirst(before.calls.count))
            check(newCalls.first == "reset", "Real loop resets the old path before the first Wi-Fi probe")
            await service.release(.probe)
            await sleeper.releaseFirst()
            await waitUntil("remembered Wi-Fi loop recovers without app restart") { model.isOnline && !model.busy }
            check(await service.snapshot().attempts == [.dormitory], "Real loop sends exactly one remembered Wi-Fi login after fresh campus proof")
            check(model.monitoringMode == .recover && model.nextCheckAt != nil,
                  "Late old loop does not occupy or clear the replacement loop's schedule")
            model.setMonitoringMode(.manual)
            await waitUntil("controlled loop shuts down cleanly") { await sleeper.pending().isEmpty }
        }

        // Resetting dead sockets may not lift explicit user/security/credential/uncertain holds.
        for reason in [HoldReason.userPaused, .security, .credentials, .uncertain] {
            let root = try folder(); defer { try? FileManager.default.removeItem(at: root) }
            let journalURL = root.appendingPathComponent("recovery.json")
            let hold = AuthenticationHold(reason: reason, area: reason == .credentials ? .dormitory : nil,
                generation: reason == .credentials ? 1 : nil, origin: reason == .userPaused ? .user : .global,
                rejection: reason == .credentials ? "password" : "")
            try RecoveryJournal(generation: 1, holds: [hold], notBefore: date.addingTimeInterval(42), pendingSubmission: false).write(journalURL)
            let (model, service, _) = fixture(recoveryStore: journalURL)
            attachWired(model); detach(model); attachWifi(model)
            await service.configure(reports: [offline, offline])
            await model.runAutomaticCycle(); await model.runAutomaticCycle()
            check(model.holds == [hold] && model.automaticRecoveryPaused && model.cooldownRemaining == 42,
                  "Path recovery preserves \(reason.key) hold and existing submission cooldown")
            check(await service.snapshot().attempts.isEmpty, "Path recovery cannot submit through \(reason.key) hold")
            let resetCount = await service.snapshot().resets
            await service.configure(reports: [offline], attempts: [LoginAttempt(outcome: .unavailable)])
            model.connect(); await finish(model)
            check(await service.snapshot().resets > resetCount, "Explicit manual retry resets dead connections with \(reason.key) hold present")
            check(model.holds == [hold] && model.cooldownRemaining == 42,
                  "A manual retry that never submitted preserves \(reason.key) hold and the prior deadline")
        }

        print("PathRecoveryAppModelTests: \(passed) passed, \(failed) failed; fake transport and temporary files only")
        if failed > 0 { exit(1) }
    }
}
